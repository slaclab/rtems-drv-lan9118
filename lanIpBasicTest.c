#include <rtems.h>
#include <lanIpBasic.h>
#include <string.h>

#include <netinet/in.h>

#define PAYLDLEN 1024

#ifdef DRVLAN9118

#include <drvLan9118.h>

static inline void
shutdown(void *drv)
{
	drvLan9118Shutdown((DrvLan9118_tps)drv);
}

static void *
setup(IpCbData cbd)
{
	return drvLan9118Setup(0,0);
}

static int
start(void *drv, IpCbData cbd, int pri)
{
	return drvLan9118Start(drv, pri, 0,
				drvLan9118IpRxCb, cbd,
				0, 0,
				0, 0,
				0, 0);
}

/* Must be initialized! */
extern uint32_t Read_timer();

#define READ_TIMER()	Read_timer()

#elif defined(DRVMVE)

#include <bsp/if_mve_pub.h>

static rtems_id mve_tid = 0;

extern void
drvMveIpBasicShutdown(rtems_id tid);

extern void
drvMveIpBasicTask(rtems_task_argument arg);

extern rtems_id
drvMveIpBasicSetup(IpCbData cbd);

extern void *
drvMveIpBasicGetDrv(rtems_id tid);

static void *
setup(IpCbData cbd)
{
	mve_tid = drvMveIpBasicSetup(cbd);
	if ( mve_tid ) {
		return drvMveIpBasicGetDrv(mve_tid);
	}
	return 0;
}

static int
start(void *drv, IpCbData cbd, int pri)
{
rtems_task_priority op;

	if ( pri > 0 )
		rtems_task_set_priority(mve_tid, (rtems_task_priority)pri, &op);
	return RTEMS_SUCCESSFUL == rtems_task_start(mve_tid, drvMveIpBasicTask, (rtems_task_argument)cbd) ? 0 : -1;
}

static inline void
shutdown(void *drv)
{
	drvMveIpBasicShutdown(mve_tid);
	mve_tid = 0;
}

static inline uint32_t READ_TIMER()
{
uint32_t t;
	asm volatile("mftb %0":"=r"(t));
	return t;
}

#else
#error "unsupported driver"
#endif

void           *plan = 0;
IpCbData       cbdta = 0;
int			   udpsd = -1;

void
lanIpTakedown()
{
	if ( udpsd >= 0 ) {
		udpSockDestroy(udpsd);
		udpsd = -1;
	}
	if ( cbdta ) {
		lanIpCbDataDestroy( cbdta );
		cbdta = 0;
	}
	if ( plan ) {
		shutdown(plan);
		plan = 0;
	}
}

int
lanIpSetup(char *ip, char *nmsk, int port)
{
	if ( !ip || !nmsk || !port ) {
		fprintf(stderr,"Usage: lanIpSetup(char *ip, char *netmask, int port)\n");
		return -1;
	}
	if ( plan ) {
		fprintf(stderr,"Can call setup only once\n");
		return -1;
	}

	if ( ! (cbdta = lanIpCbDataCreate()) ) {
		fprintf(stderr,"Unable to create callback data\n");
		goto egress;
	}

	plan = setup(cbdta);

	if ( !plan )
		goto egress;

	lanIpCbDataInit(cbdta, plan, ip, nmsk);

	/* Start driver */
	if ( start(plan, cbdta, 0) ) {
		fprintf(stderr,"Unable to start driver\n");
		goto egress;
	}

	if ( (udpsd = udpSockCreate(port)) < 0 ) {
		fprintf(stderr,"Unable to create UDPSOCK: %s\n", strerror(-udpsd));
		goto egress;
	}

	return (udpsd);

egress:
	lanIpTakedown();
	return -1;
}

uint32_t mintrip         = -1;
uint32_t maxtrip         =  0;
uint32_t avgtrip128      =  0;
uint32_t pktlost         =  0;
uint32_t pktsent         =  0;
volatile int keeprunning =  1;

/* bounce a UDP packet back to the caller */

int
udpSocketEcho(int sd, int raw, int idx, int timeout)
{
LanIpPacket p;
uint16_t    tmp;
int         len = -1, rval;
uint32_t	now, then;

static LanIpPacketRec dummy = {{{0}}};

	if ( !dummy.ip.src ) {
		if ( (rval = udpSockInitHdrs(sd, &dummy, 0, 0, 0)) ) {
			fprintf(stderr,"udpSocketEcho - Unable to initialize headers: %s\n", strerror(-rval));
			return rval;
		}
	}

	p = udpSockRecv(sd, timeout);

	if ( p ) {

		if ( raw ) {
			/* user manages headers */
			memcpy(p->ll.dst, p->ll.src, sizeof(p->ll.dst));
			p->ip.dst              = p->ip.src;	
			p->p_u.udp_s.hdr.dport = p->p_u.udp_s.hdr.sport;
			{
				/* fill source ll address, IP address and UDP port */
				memcpy(p->ll.src, &dummy.ll.src, sizeof(dummy.ll.src));
				p->ip.src              = dummy.ip.src;

				tmp                    = p->p_u.udp_s.hdr.dport;
				p->p_u.udp_s.hdr.dport = p->p_u.udp_s.hdr.sport;
				p->p_u.udp_s.hdr.sport = tmp;

				p->ip.csum             = 0;
				/*
				   p->ip.csum             = htons(in_cksum_hdr((void*)&p->ip));
				 */

				p->p_u.udp_s.hdr.csum = 0;

				len = IPPKTSZ(ntohs(p->p_u.udp_s.hdr.len));
			}
			now  = READ_TIMER();
			then = ((uint32_t*)p->p_u.udp_s.pld)[idx];
			((uint32_t*)p->p_u.udp_s.pld)[idx] = now;
			udpSockSendBufRaw(p, len);

		} else {
			now  = READ_TIMER();
			then = ((uint32_t*)p->p_u.udp_s.pld)[idx];
			((uint32_t*)p->p_u.udp_s.pld)[idx] = now;
			len = ntohs(p->p_u.udp_s.hdr.len) - sizeof(UdpHeaderRec);
			udpSockSend(sd, &p->p_u.udp_s.pld, len);
			udpSockFreeBuf(p);
		}

		pktsent++;

		/* If this was not the first packet then measure roundtrip */
		if ( then != 0 ) {
			now-=then;	
			if ( now < mintrip )
				mintrip = now;
			if ( now > maxtrip )
				maxtrip = now;
			/* avgtrip = (127*avgtrip + now)/128;
             * 128 * a = 127 * a + n = 127/128 * (128*a) + n
             */
            avgtrip128 = (avgtrip128 * 127)/128 + now;
			/* When reading avgtrip it must be divided by 128 */
		}
	}
	return len;
}

int
udpBouncer(int master, int raw, uint32_t dipaddr, uint16_t dport)
{
LanIpPacket p;
int         err = -1;
	if ( !raw ) {
		if ( (err=udpSockConnect(udpsd, dipaddr, dport)) ) {
			fprintf(stderr,"bouncer: Unable to connect socket: %s\n", strerror(-err));
			return err;
		}
	}

	if ( master ) {
		do {
			/* create a packet */
			if ( !(p=udpSockGetBuf()) ) {
				fprintf(stderr,"bouncer: Unable to allocate buffer\n");
				goto egress;		
			}
			if ( raw ) {
				/* fillin headers */
				udpSockInitHdrs(udpsd, p, dipaddr, dport, 0);
				lanIpUdpHdrSetlen(p, PAYLDLEN);

				/* initialize timestamps */
				((uint32_t*)p->p_u.udp_s.pld)[0] = 0;
				((uint32_t*)p->p_u.udp_s.pld)[1] = READ_TIMER();
				udpSockSendBufRaw(p, UDPPKTSZ(PAYLDLEN) );	
			} else {
				/* initialize timestamps */
				((uint32_t*)p->p_u.udp_s.pld)[0] = 0;
				((uint32_t*)p->p_u.udp_s.pld)[1] = READ_TIMER();
				udpSockSend(udpsd, p, PAYLDLEN);
				udpSockFreeBuf(p);
			}

			while ( udpSocketEcho(udpsd, raw, 1, 20) > 0 ) {
				if ( !keeprunning ) {
					pktlost--;
					break;
				}
			}

			pktlost++;

		} while ( keeprunning );
			fprintf(stderr,"Master terminated.\n");
			/* in case they want to start again */
			keeprunning=1;
	} else {
			/* make sure socket is flushed */
			while ( (p = udpSockRecv(udpsd,0)) )		
				udpSockFreeBuf(p);
			while (udpSocketEcho(udpsd, raw, 0, 100) > 0)
					;
			fprintf(stderr,"Slave timed out; terminating\n");
	}
	err = 0;

egress:
	if ( !raw ) {
		if ( (err=udpSockConnect(udpsd, 0, 0)) ) {
			fprintf(stderr,"bouncer: Unable to disconnect socket: %s\n", strerror(-err));
		}
	}
	return(err);
}


int
_cexpModuleFinalize(void* unused)
{
	if ( plan || udpsd>=0 || cbdta ) {
		fprintf(stderr,"Module still in use, use 'lanIpTakedown()'\n");
		return -1;
	}
	return 0;
}
