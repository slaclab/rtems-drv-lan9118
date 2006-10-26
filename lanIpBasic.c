/* $Id$ */

/* Rudimentary IP protocol:
 *
 *  - trivial ARP
 *  - trivial IP (only 20byte header)
 *  - ICMP echo request (ping) handling
 *
 *  - LAN only (no IP routing)
 *
 * NOTE: daemon using the 'lanIpProcessBuffer()' must have enough stack
 *       allocated. The Callback uses ~2k itself.
 */

/* T. Straumann <strauman@slac.stanford.edu> */
#define __RTEMS_VIOLATE_KERNEL_VISIBILITY__
#include <rtems.h>
#include <rtems/error.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include <netinet/in_systm.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <machine/in_cksum.h>

#include <lanIpProto.h>
#include <lanIpBasic.h>
/* include netdriver AFTER defining VIOLATE_KERNEL_VISIBILITY (in case it uses rtems.h already) */
#include NETDRV_INCLUDE

#define DEBUG_IP	1

#define DEBUG		0

#ifdef DEBUG
int	lanIpDebug = DEBUG;
#endif

#ifndef CACHE_OVERLAP
#define CACHE_OVERLAP 10
#endif

/* Trivial RX buffers */
#ifndef RBUFSZ
#define RBUFSZ		1500
#endif
#ifndef NRBUFS
#define NRBUFS		50	/* Total number of RX buffers */
#endif
#ifndef NSOCKS
#define NSOCKS		5
#endif
#ifndef QDEPTH
#define QDEPTH		10	/* RX socket queue depth      */
#endif

typedef LanIpPacketRec rbuf_t;

static rbuf_t		rbufs[NRBUFS] = {{{{0}}}};

/* lazy init of tbuf facility */
static int    ravail = NRBUFS;

static rbuf_t *frb = 0; /* free list */

static rbuf_t *getrbuf()
{
rbuf_t                *rval;
rtems_interrupt_level key;

	rtems_interrupt_disable(key);
	if ( (rval = frb) ) {
		frb = *(rbuf_t **)rval;
	} else {
		/* get from pool */
		if ( ravail ) {
			rval = &rbufs[--ravail];
		}
	}
	rtems_interrupt_enable(key);
	return rval;
}

static void relrbuf(rbuf_t *b)
{
rtems_interrupt_level key;

	if ( b ) {
		rtems_interrupt_disable(key);
			*(rbuf_t**)b = frb;
			frb = b;
		rtems_interrupt_enable(key);
	}
}

typedef struct UdpSockRec_ {
	volatile int	port;
	rtems_id		msgq;
} UdpSockRec, *UdpSock;

static UdpSockRec	socks[NSOCKS] = {{0}};

/* socks array is protected by disabling thread dispatching */

/* Find socket for 'port' and return (dispatching disabled on success) */
static int
sockget(int port)
{
int i;
	_Thread_Disable_dispatch();
	for (i=0; i<NSOCKS; i++) {
		if ( socks[i].port == port )
			return i;
	}
	_Thread_Enable_dispatch();
	return -1;
}


int
udpSockCreate(uint16_t port)
{
int       rval, i;
rtems_id  q = 0;

	/* 0 is invalid (we use it as a marker) */
	if ( 0 == port )
		return -1;

	if ( (rval = sockget(0)) < 0 ) {
		/* no free slot */
		goto egress;
	}

	if ( RTEMS_SUCCESSFUL != rtems_message_queue_create(
			rtems_build_name('u','d','p','q'),
			QDEPTH,
			sizeof(rbuf_t*),
			RTEMS_FIFO | RTEMS_LOCAL,
			&q) ) {
		return -2;
	}

	/* found free slot, array is locked */
	for ( i=0; i<NSOCKS; i++ ) {
		if ( socks[i].port == port ) {
			/* duplicate port */
			_Thread_Enable_dispatch();
			rval = -3;
			goto egress;
		}
	}

	/* everything OK */
	socks[rval].port = port;
	socks[rval].msgq = q;

	_Thread_Enable_dispatch();

	q             = 0;

egress:
	if ( q )
		rtems_message_queue_delete(q);
	return rval;
}

/* destroying a sock somebody is blocking on is BAD */
int
udpSockDestroy(int sd)
{
rtems_id q = 0;
	if ( sd < 0 || sd >= NSOCKS )
		return -1;

	_Thread_Disable_dispatch();
		if (socks[sd].port) {
			socks[sd].port = 0;
			q = socks[sd].msgq;
			socks[sd].msgq = 0;
		}
	_Thread_Enable_dispatch();
	if (q) {
		/* drain queue */
		void     *p;
		uint32_t sz;
		while ( RTEMS_SUCCESSFUL == rtems_message_queue_receive(
										q,
										&p,
										&sz,
										RTEMS_NO_WAIT,
										0) ) {
			relrbuf(p);
		}
		rtems_message_queue_delete(q);
		return 0;
	}
	return -2;
}

LanIpPacketRec *
udpSockRcv(int sd, int timeout_ticks)
{
LanIpPacketRec *rval = 0;
uint32_t		sz = sizeof(rval);
	if ( sd < 0 || sd >= NSOCKS ) {
		return 0;
	}
	if ( RTEMS_SUCCESSFUL != rtems_message_queue_receive(
								socks[sd].msgq,
								&rval,
								&sz,
								timeout_ticks ? RTEMS_WAIT : RTEMS_NO_WAIT,
								timeout_ticks < 0 ? RTEMS_NO_TIMEOUT : timeout_ticks) )
		return 0;

	return rval;
}

void
udpSockFreeBuf(LanIpPacketRec *b)
{
	relrbuf(b);
}

typedef struct ArpEntryRec_ {
	uint32_t			ipaddr;
	uint8_t				hwaddr[6];
	Watchdog_Interval	ctime;
} ArpEntryRec, *ArpEntry;

/* NOTES: On class C networks (or equivalent A/B subnets)
 *        there will never be collisions in the cache.
 *
 *        Dont change size (256) w/o adapting hashing
 *        algorithm
 *
 */
static ArpEntry arpcache[256] = {0};

/* don't bother about MSBs; assume we're on a LAN anyways */
#define ARPHASH(h,ipaddr)			\
	do {							\
		uint32_t ipaddr_h_;			\
		ipaddr_h_ = ntohl(ipaddr);	\
		h  = ipaddr_h_;				\
		h += ipaddr_h_>>8;			\
	} while (0)

typedef struct IpCbDataRec_ {
	void			*drv_p;
	rtems_id		mutx;
	uint32_t		ipaddr;
	uint32_t		nmask;
	struct {
		EtherHeaderRec  ll;
		IpArpRec		arp;
	} arpreq;
	struct {
		EtherHeaderRec  ll;
		IpArpRec		arp;
	} arprep;
} IpCbDataRec;

#define ARPLOCK(pd)		rtems_semaphore_obtain((pd)->mutx, RTEMS_WAIT, RTEMS_NO_TIMEOUT)
#define ARPUNLOCK(pd) 	rtems_semaphore_release((pd)->mutx)

int arpLookup(IpCbData pd, uint32_t ipaddr, uint8_t *enaddr)
{
ArpEntry rval;
int      i,n;
uint8_t  hh;
uint8_t  h;

		ARPHASH(h, ipaddr);

		for ( n = 0; n < CACHE_OVERLAP; n++ ) {

			
			/* Abuse the TX lock */
			ARPLOCK(pd);
			for ( i = 0, hh=h; i<CACHE_OVERLAP && (rval = arpcache[hh]); i++, hh++ ) {
				if ( ipaddr == rval->ipaddr ) {
					memcpy(enaddr, rval->hwaddr, 6);
					ARPUNLOCK(pd);
					return 0;
				}
			}
			ARPUNLOCK(pd);

			/* must do a new lookup */
			NETDRV_ATOMIC_SEND_ARPREQ(pd, ipaddr);

			/* should synchronize but it's easier to just delay and try again */
			rtems_task_wake_after(1);
		}
		
		return -1;
}

void
arpPutEntry(IpCbData pd, uint8_t *enaddr, uint32_t ipaddr)
{
ArpEntry rval;
ArpEntry newe = malloc(sizeof(*newe));	/* pre-allocate */
int      i;
uint8_t  oh;
uint8_t  h;

		ARPHASH(h, ipaddr);

		/* Abuse the TX lock */
		ARPLOCK(pd);
		for ( i = 0, oh=h; i<CACHE_OVERLAP && (rval = arpcache[h]); i++, h++ ) {
			if ( ipaddr == rval->ipaddr ) {
				memcpy(rval->hwaddr, enaddr, 6);
				rtems_clock_get(RTEMS_CLOCK_GET_SECONDS_SINCE_EPOCH, &rval->ctime);
				ARPUNLOCK(pd);
				return;
			}
			if ( rval->ctime < arpcache[oh]->ctime )
				oh = h;
				
		}

		if ( rval ) {
			/* all slots full; must evict oldest entry */
			h = oh;
		} else {
			/* use new entry */
			arpcache[h] = newe;
			newe        = 0;
		}
		rval = arpcache[h];
		rval->ipaddr = ipaddr;
		memcpy(rval->hwaddr, enaddr, 6);
		rtems_clock_get(RTEMS_CLOCK_GET_SECONDS_SINCE_EPOCH, &rval->ctime);
		ARPUNLOCK(pd);

		free(newe);
}

void
arpDelEntry(IpCbData pd, uint32_t ipaddr)
{
ArpEntry rval, found = 0;
int      i;
uint8_t  h;

		ARPHASH(h, ipaddr);

		/* Abuse the TX lock */
		ARPLOCK(pd);
		for ( i = 0; i<CACHE_OVERLAP && (rval = arpcache[h]); i++, h++ ) {
				if ( ipaddr == rval->ipaddr ) {
					arpcache[h] = 0;
					found = rval;
					break;
				}
		}
		ARPUNLOCK(pd);
		free(found);
}

#ifdef DEBUG
static void
prether(FILE *f, const unsigned char *ea)
{
int i;
	for (i=0; i<5; i++)
		fprintf(f,"%02X:",*ea++);
	fprintf(f,"%02X",*ea);
}
#endif

static int
handleArp(rbuf_t **ppbuf, IpCbData pd)
{
int			isreq = 0;
rbuf_t		*p    = *ppbuf;
IpArpRec	*pipa = (IpArpRec*)&p->ip;


	 /* 0x0001 == Ethernet, 0x0800 == IP */
	NETDRV_READ_INCREMENTAL(pd, pipa, 8);
	if ( ntohl(0x00010800) != *(uint32_t*)pipa )
		return 8;


	switch ( *(((uint32_t*)pipa) + 1) ) {
		default:
			return 8;

		/* 0x06 hw addr len, 0x04 proto len, 0x0001 ARP REQUEST */
		case ntohl(0x06040001):
			isreq = 1;
		/* 0x06 hw addr len, 0x04 proto len, 0x0002 ARP REPLY   */
		case ntohl(0x06040002):
			break;
	}

	/* Fill rest of ARP packet            */
	NETDRV_READ_INCREMENTAL(pd, pipa->sha, 5*4);

	if ( isreq ) {
#ifdef DEBUG
		if ( lanIpDebug & DEBUG_IP )
			printf("got ARP request for %d.%d.%d.%d\n",pipa->tpa[0],pipa->tpa[1],pipa->tpa[2],pipa->tpa[3]); 
#endif
		if ( *(uint32_t*)pipa->tpa != pd->ipaddr )
			return sizeof(*pipa);

		/* they mean us; send reply */
		memcpy( pd->arprep.ll.dst,  pipa->sha, 6);
		memcpy( pd->arprep.arp.tha, pipa->sha, 10);

#ifdef DEBUG
		if ( lanIpDebug & DEBUG_IP ) {
			extern void md(void*,int);
			printf("MATCH -> sending\n");
			md(&pd->arprep, sizeof(pd->arprep));
		}
#endif

		NETDRV_ENQ_PACKET(pd, &pd->arprep, sizeof(pd->arprep));
	} else {
		/* a reply to our request */
#ifdef DEBUG
		if ( lanIpDebug & DEBUG_IP ) {
			printf("got ARP reply from "); prether(stdout, pipa->sha);
			printf("\n");
		}
#endif
		arpPutEntry(pd, pipa->sha, *(uint32_t*)pipa->spa);
	}

	return sizeof(*pipa);
}

static int
handleIP(rbuf_t **ppbuf, IpCbData pd)
{
int         rval = 0, l, nbytes, i;
rbuf_t		*p = *ppbuf;
uint16_t	dport;
int			isbcst = 0;

	NETDRV_READ_INCREMENTAL(pd, &p->ip, sizeof(p->ip));
	rval += sizeof(p->ip);

	/* accept IP unicast and broadcast */
	if ( p->ip.dst != pd->ipaddr && ! (isbcst = ((p->ip.dst & ~pd->nmask) == ~pd->nmask)) )
		return rval;

#ifdef DEBUG
	if ( (lanIpDebug & DEBUG_IP) && p->ip.dst == pd->ipaddr ) {
		printf("accepting IP unicast, proto %i\n", p->ip.prot);
	}
#endif

	/* reject non-trivial headers (version != 4, header length > 5, fragmented,
     * i.e., MF (more fragments) or the offset are set
	 */
	if ( p->ip.vhl != 0x45 || ntohs(p->ip.off) & 0x9fff ) {
#ifdef DEBUG
		if ( (lanIpDebug & DEBUG_IP) )
			printf("dropping IP packet, vhl %i, off %i\n", p->ip.vhl, ntohs(p->ip.off));
#endif
		return rval;
	}

	nbytes = ntohs(p->ip.len);
	l = ((nbytes - sizeof(p->ip)) + 3) & ~3;
	switch ( p->ip.prot ) {
		case 1 /* ICMP */:
		if ( sizeof(p)-sizeof(p->ip) - sizeof(p->ll) >= l ) {
			NETDRV_READ_INCREMENTAL(pd, &p->p_u.icmp_s, l);
			rval += l;
			if ( p->p_u.icmp_s.hdr.type == 8 /* ICMP REQUEST */ && p->p_u.icmp_s.hdr.code == 0 ) {
#ifdef DEBUG
				if ( lanIpDebug & DEBUG_IP )
					printf("handling ICMP ECHO request\n");
#endif
				p->p_u.icmp_s.hdr.type = 0; /* ICMP REPLY */
				p->p_u.icmp_s.hdr.csum = 0;
				memcpy( p->ll.dst, p->ll.src, 6 );
				memcpy( p->ll.src, pd->arprep.ll.src, 6);
				p->ll.type = htons(0x800); /* IP */
				p->ip.dst  = p->ip.src;
				p->ip.src  = pd->ipaddr; 
				p->ip.csum = 0;
				p->ip.csum = htons(in_cksum_hdr((void*)&p->ip));
				NETDRV_ENQ_PACKET(pd, p, sizeof(EtherHeaderRec) + nbytes);
			}
		}
		break;

		case 17 /* UDP */:
			/* UDP header is word aligned -> OK */
			NETDRV_READ_INCREMENTAL(pd, &p->p_u.udp_s, sizeof(p->p_u.udp_s.hdr));
			rval += sizeof(p->p_u.udp_s.hdr);
			l    -= sizeof(p->p_u.udp_s.hdr);
			dport = ntohs(p->p_u.udp_s.hdr.dport);
#ifdef DEBUG
			if ( lanIpDebug & DEBUG_IP )
				printf("handling UDP packet (dport %i%s)\n", dport, isbcst ? ", BCST":"");
#endif
			_Thread_Disable_dispatch();
			for ( i=0; i<NSOCKS; i++ ) {
				if ( socks[i].port == dport ) {
					_Thread_Enable_dispatch();
					/* slurp data */
					NETDRV_READ_INCREMENTAL(pd, p->p_u.udp_s.pld, l);
					rval += l;
					_Thread_Disable_dispatch();
					/* see if socket is still alive */
					if ( socks[i].port == dport ) {
						if ( RTEMS_SUCCESSFUL == rtems_message_queue_send(socks[i].msgq, &p, sizeof(p)) ) {
							/* they now own the buffer */
							*ppbuf = 0;
						}
					}
					break;
				}
			}
			_Thread_Enable_dispatch();
		break;

		default:
		break;
	}

	return rval;
}

/* Handle ARP and ICMP echo (ping) requests
 * Dispatch UDP packets to trivial 'sockets'
 *
 * Need to pass a pointer to the buffer pointer;
 * (*pprb) is set to NULL if the buffer was handed
 * on to a 'socket'.
 */

int
lanIpProcessBuffer(IpCbData pd, rbuf_t **pprb, int len)
{
rbuf_t *prb = *pprb;

	NETDRV_READ_INCREMENTAL(pd, prb, sizeof((prb)->ll));
	len -= sizeof((prb)->ll);

	switch ( prb->ll.type ) {
		case htons(0x806) /* ARP */:
			len -= handleArp(pprb, pd);
			break;

		case htons(0x800) /* IP  */:
			len -= handleIP(pprb, pd);
			break;

		default:
			break;
	}

	return len;
}


IpCbData
lanIpCbDataCreate()
{
IpCbData          rval = malloc(sizeof(*rval));
rtems_status_code sc;

	if ( !rval )
		return 0;

	memset( rval, 0, sizeof(*rval) );

	sc = rtems_semaphore_create(
			rtems_build_name('i','p','m','x'), 
			1,
			RTEMS_SIMPLE_BINARY_SEMAPHORE | RTEMS_PRIORITY | RTEMS_INHERIT_PRIORITY,
			0,
			&rval->mutx);

	if ( RTEMS_SUCCESSFUL != sc ) {
		rtems_error(sc, "lanIpCb: unable to create mutex\n");
		free(rval);
		return 0;
	}
	return rval;
}

void
lanIpCbDataInit(IpCbData cbd_p, void *drv_p, char *ipaddr, char *netmask)
{
uint8_t		      (*enaddr)[6];

	cbd_p->drv_p  = drv_p;

	cbd_p->ipaddr = inet_addr(ipaddr);
	cbd_p->nmask  = inet_addr(ipaddr);


	/* convenience variable */
	enaddr = &cbd_p->arpreq.ll.src;
	
	/* Setup ARP templates for request and reply */

	/* LL HEADERS FIRST */

	/* REQUEST */
		/* DST: bcast address            */
		memset(&cbd_p->arpreq.ll.dst, 0xff, sizeof(*enaddr));
		/* SRC: drv_p's ethernet address  */
		NETDRV_READ_ENADDR(drv_p, (uint8_t*)enaddr);
		/* TYPE/LEN is ARP (0x806)       */
		cbd_p->arpreq.ll.type = htons(0x806);
	/* REPLY   */
		/* DST: ??? filled by daemon     */

		/* SRC: drv_p's ethernet address  */
		memcpy(cbd_p->arprep.ll.src, enaddr, sizeof(*enaddr));
		/* TYPE/LEN is ARP (0x806)       */
		cbd_p->arprep.ll.type = htons(0x806);

	/* ARP PORTION */
	/* HW and PROTO type for both */
	cbd_p->arprep.arp.htype = cbd_p->arpreq.arp.htype = htons(1);     /* Ethernet */
	cbd_p->arprep.arp.ptype = cbd_p->arpreq.arp.ptype = htons(0x800); /* IP       */
	cbd_p->arprep.arp.hlen  = cbd_p->arpreq.arp.hlen  = 6; 
	cbd_p->arprep.arp.plen  = cbd_p->arpreq.arp.plen  = 4; 

	cbd_p->arprep.arp.oper  = htons(2); /* ARP REPLY   */
	cbd_p->arpreq.arp.oper  = htons(1); /* ARP REQUEST */

	/* REQUEST */
		/* TARGET HW ADDR: bcst                       */ 
		memset(cbd_p->arpreq.arp.tha, 0xff,   sizeof(*enaddr));
		/* TARGET IP ADDR: ??? (filled by requestor)  */

		/* SOURCE HW ADDR: drv_p's ethernet address    */
		memcpy(&cbd_p->arpreq.arp.sha, enaddr, sizeof(*enaddr));
		/* SOURCE IP ADDR: our IP                     */
		memcpy(&cbd_p->arpreq.arp.spa, &cbd_p->ipaddr, 4);

	/* REPLY */
		/* TARGET HW ADDR: ??? (filled by daemon)     */ 

		/* TARGET IP ADDR: ??? (filled by daemon)     */

		/* SOURCE HW ADDR: drv_p's ethernet address    */
		memcpy(cbd_p->arprep.arp.sha, enaddr, sizeof(*enaddr));
		/* SOURCE IP ADDR: our IP                     */
		memcpy(cbd_p->arprep.arp.spa, &cbd_p->ipaddr, 4);
}

void *
lanIpCbDataGetDrv(IpCbData cbd_p)
{
	return cbd_p->drv_p;
}

void
lanIpCbDataDestroy(IpCbData pd)
{
	if ( pd ) {
		rtems_semaphore_delete(pd->mutx);
		free(pd);
	}
}

#ifdef DEBUG
const uint8_t dstenaddr[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }; /* { 0x00,0x30,0x65,0xC9,0x9D,0xF8 }; */
const uint8_t srcenaddr[6] = { 0x08,0x00,0x56,0x00,0x01,0x00 };

uint16_t csum(uint16_t *d, int n)
{
uint32_t s = 0;
	while (n--)
		s+=*d++;
	while ( s > 0xffff )
		s = (s & 0xffff) + (s >> 16);
	return ~s & 0xffff;
}

void
drvLan9118InitUdpPacket(UdpPacketRec *p, int payload_len)
{
	memcpy(p->eh.dst, dstenaddr, 6);
	memcpy(p->eh.src, srcenaddr, 6);
	p->eh.type  = htons(0x0800);	/* IP */

	p->ih.vhl   = 0x45;	/* version 4, 5words length */
	p->ih.tos   = 0x30; 	/* priority, minimize delay */
	p->ih.len   = htons(payload_len + sizeof(UdpHeaderRec) + sizeof(IpHeaderRec));
	p->ih.id    = htons(0);	/* ? */
	p->ih.off   = htons(0);
	p->ih.ttl   = 4;
	p->ih.prot  = 17;	/* UDP */
	p->ih.src   = inet_addr("134.79.219.35"); 
	p->ih.dst   = inet_addr("134.79.216.68"); 
	p->ih.csum  = 0;
	
	p->ih.csum  = htons(in_cksum_hdr((void*)&p->ih));

	p->uh.sport = 0xabcd; 
	p->uh.dport = 0xabcd; 
	p->uh.len   = htons(payload_len + sizeof(UdpHeaderRec));
	p->uh.csum  = 0; /* csum disabled */
}
#endif
