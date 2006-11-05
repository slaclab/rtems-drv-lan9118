/* $Id$ */
#ifndef UDPCOMM_LAYER_H
#define UDPCOMM_LAYER_H

/* Glue for simple UDP communication over
 * either BSD sockets or simple 'udpSock'ets and
 * the lanIpBasic 'stack'.
 */

#ifdef BSDSOCKET
#include <sys/socket.h>
#include <netinet/in.h>
#define STATICINLINE
#else
#include <lanIpBasic.h>
#define STATICINLINE static inline
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void * UdpCommPkt;

/* Create */
STATICINLINE int
udpCommSocket(int port);

/* Close  */
static inline int
udpCommClose(int sd);

/* Connect socket to a peer */
STATICINLINE int
udpCommConnect(int sd, uint32_t diaddr, int port);


/* Receive a packet */
STATICINLINE UdpCommPkt
udpCommRecv(int sd, int timeout_ms);

/* Obtain pointer to data area in buffer (UDP payload) */
static inline void *
udpCommBufPtr(UdpCommPkt p);

/* Release packet (obtained from Recv) when done */
static inline void
udpCommFreePacket(UdpCommPkt p);

/* Send packet to connected peer */
static inline int
udpCommSend(int sd, void *buf, int len);

/* Return packet to sender (similar to 'send'; 
 * this interface exists for efficiency reasons
 * [coldfire/lan9118]).
 */
static inline void
udpCommReturnPacket(int sd, UdpCommPkt p, int len);

/* Inline implementations for both BSD and udpSocks */
static inline int
udpCommClose(int sd)
{
#ifdef BSDSOCKET
	return close(sd);
#else
	return udpSockDestroy(sd);
#endif
}

static inline void *
udpCommBufPtr(UdpCommPkt p)
{
#ifdef BSDSOCKET
	return (void*)p;
#else
	return (void*)((LanIpPacket)p)->p_u.udp_s.pld;
#endif
}

static inline void
udpCommFreePacket(UdpCommPkt p)
{
#ifdef BSDSOCKET
	free(p);
#else
	udpSockFreeBuf(p);
#endif
}

static inline int
udpCommSend(int sd, void *buf, int len)
{
#ifdef BSDSOCKET
	return send(sd, buf, len, 0);
#else
	return udpSockSend(sd, buf, len);
#endif
}

static inline void
udpCommReturnPacket(int sd, UdpCommPkt p, int len)
{
#ifdef BSDSOCKET
	/* Subtle differences exist between sending to the peer
     * and just swapping the headers around (as done for
     * udpSocks) -- guaranteed no ARP is necessary in the
     * latter case. For BSDSOCKET we don't care about ARP.
     */
	send(sd, p, len, 0);
#else
	udpSockHdrsReflect(p);	/* point headers back to sender */
	udpSockSendBufRawIp(p); /* send off                     */
#endif
}

#ifndef BSDSOCKET
/* Inline implementation for udpSocks */
static inline int
udpCommSocket(int port)
{
	return udpSockCreate(port);
}

static inline UdpCommPkt
udpCommRecv(int sd, int timeout_ms)
{
	if ( ! (timeout_ms/=20) )
		timeout_ms = 1;
	return udpSockRecv(sd, timeout_ms/20); /* FIXME: use system clock rate */
}

static inline int
udpCommConnect(int sd, uint32_t diaddr, int port)
{
	return udpSockConnect(sd, diaddr, port);
}

#endif

#ifdef __cplusplus
}
#endif

#endif
