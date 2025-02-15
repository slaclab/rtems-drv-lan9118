#ifndef DRV_LAN_PROTO_DEFS_H
#define DRV_LAN_PROTO_DEFS_H

/* $Id$ */

/* Raw packet driver; Protocol Header Definitions             */

/* Author: Till Straumann <strauman@slac.stanford.edu>, 2006 */

#include <stdint.h>

#define EH_PAD_BYTES 2

/* Guarantee packet alignment to 32 bytes;
 * (particular chip driver may have stricter
 * requirements and the buffer memory will
 * then be aligned accordingly)
 */
#define LAN_IP_BASIC_PACKET_ALIGNMENT 32

/* uint32_t aligned ethernet header */
typedef struct EthHeaderRec_ {
	uint8_t		pad[EH_PAD_BYTES];
	uint8_t		dst[6];
	uint8_t		src[6];
	uint16_t	type;
} __attribute__((may_alias)) EthHeaderRec;

typedef struct IpHeaderRec_ {
	uint8_t		vhl;
	uint8_t		tos;
	uint16_t	len;
	uint16_t	id;
	uint16_t	off;
	uint8_t		ttl;
	uint8_t		prot;
	uint16_t	csum;
	uint32_t	src;
	uint32_t	dst;
	uint32_t    opts[];
} __attribute__((may_alias)) IpHeaderRec;

typedef struct UdpHeaderRec_ {
	uint16_t	sport;
	uint16_t	dport;
	uint16_t	len;
	uint16_t	csum;
} __attribute__((may_alias)) UdpHeaderRec;

typedef struct IpArpRec_ {
	uint16_t	htype;
	uint16_t	ptype;
	uint8_t		hlen;
	uint8_t		plen;
	uint16_t	oper;
	uint8_t		sha[6];
	uint8_t		spa[4];
	uint8_t		tha[6];
	uint8_t		tpa[4];
} IpArpRec;

typedef struct IcmpHeaderRec_ {
	uint8_t		type;
	uint8_t		code;
	uint16_t	csum;
	uint16_t	ident;
	uint16_t	seq;
} __attribute__((may_alias)) IcmpHeaderRec;

typedef struct IgmpV2HeaderRec_ {
	uint8_t     type;
    uint8_t     max_rtime;
	uint16_t    csum;
	uint32_t    gaddr;
} __attribute__((may_alias)) IgmpV2HeaderRec;

/*
 * Max. packet size incl. header, FCS-space and 2-byte padding (which is
 * never transmitted on the wire).
 */
#define LANPKTMAX			(1500+14+2+4)

#define IPPAYLOADSIZE		(LANPKTMAX - sizeof(EthHeaderRec) - sizeof(IpHeaderRec) - 4)
#define UDPPAYLOADSIZE		(IPPAYLOADSIZE - sizeof(UdpHeaderRec))
#define ICMPPAYLOADSIZE		(IPPAYLOADSIZE - sizeof(IcmpHeaderRec))

typedef struct LanEtherPktRec_ {
	EthHeaderRec    ll;
	uint8_t			pld[];
} LanEtherPktRec, *LanEtherPkt;

typedef struct LanArpPktRec_ {
	EthHeaderRec    ll;
	IpArpRec		arp;
} LanArpPktRec, *LanArpPkt;

typedef struct LanIpPartRec_ {
	EthHeaderRec    ll;
	IpHeaderRec     ip;
} LanIpPartRec, *LanIpPart;

typedef struct LanIpPktRec_ {
	LanIpPartRec    ip_part;
	uint8_t         pld[];
} LanIpPktRec, *LanIpPkt;

typedef struct LanIgmpV2PktRec_ {
	LanIpPartRec    ip_part;
	union {
		struct {
			uint32_t		ra_opt; /* router-alert IP option; logically part of IpHeaderRec */
			IgmpV2HeaderRec igmp;
		}           ip46;
		struct {
			IgmpV2HeaderRec igmp;
		}           ip45;
	}               igmp_u;
} LanIgmpV2PktRec, *LanIgmpV2Pkt;

typedef struct LanIcmpPktRec_ {
	LanIpPartRec    ip_part;
	IcmpHeaderRec	icmp;
	uint8_t			pld[];
} LanIcmpPktRec, *LanIcmpPkt;

/* sizeof(LanUdp) is 44 */
typedef struct LanUdpPktRec_ {
	LanIpPartRec    ip_part;
	UdpHeaderRec	udp;
	uint8_t			pld[];
} LanUdpPktRec, *LanUdpPkt;

typedef union LanIpPacketHeaderRec_ {
	LanEtherPktRec     eth_S;
	LanArpPktRec       arp_S;
	LanIpPktRec        ip_S;
	LanIcmpPktRec      icmp_S;
	LanUdpPktRec       udp_S;
	LanIgmpV2PktRec    igmpv2_S;
} LanIpPacketHeaderRec, *LanIpPacketHeader;

typedef union LanIpPacketRec_ {
	LanIpPacketHeaderRec	p_u;
	char					raw[LANPKTMAX];
} LanIpPacketRec, *LanIpPacket;

#define lpkt_eth(p)				(p)->p_u.eth_S.ll
#define lpkt_arp(p)				(p)->p_u.arp_S.arp
#define lpkt_ip(p)				(p)->p_u.ip_S.ip_part.ip
#define lpkt_udp(p)				(p)->p_u.udp_S.udp
#define lpkt_icmp(p)			(p)->p_u.icmp_S.icmp
#define lpkt_igmpv2_ra(p)		(p)->p_u.igmpv2_S.igmp_u.ip46.igmp
#define lpkt_igmpv2_nora(p)		(p)->p_u.igmpv2_S.igmp_u.ip45.igmp

#define lpkt_arp_pkt(p)         (p)->p_u.arp_S
#define lpkt_ip_hdrs(p)			(p)->p_u.ip_S.ip_part
#define lpkt_udp_hdrs(p)		(p)->p_u.udp_S
#define lpkt_igmpv2hdr(p)		(p)->p_u.igmpv2_S

#define lpkt_eth_pld(p,type)	(((union { char c[sizeof(type)]; type x; } __attribute__((may_alias)) *)(p)->p_u.eth_S.pld)->x)
#define lpkt_icmp_pld(p,type)	(((union { char c[sizeof(type)]; type x; } __attribute__((may_alias)) *)(p)->p_u.icmp_S.pld)->x)
#define lpkt_ip_pld(p,type)	    (((union { char c[sizeof(type)]; type x; } __attribute__((may_alias)) *)(p)->p_u.ip_S.pld)->x)
#define lpkt_udp_pld(p,type)	(((union { char c[sizeof(type)]; type x; } __attribute__((may_alias)) *)(p)->p_u.udp_S.pld)->x)

#define ETHPKTSZ(eth_payload_sz)	((eth_payload_sz) + sizeof(EthHeaderRec))
#define IPPKTSZ(ip_payload_sz)      ETHPKTSZ((ip_payload_sz) + sizeof(IpHeaderRec))
#define	UDPPKTSZ(udp_payload_sz)	IPPKTSZ((udp_payload_sz) + sizeof(UdpHeaderRec))

#endif
