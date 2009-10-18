/* $Id$ */


/* Wrapper program to send padProto requests */

#ifndef BSDSOCKET
#define  BSDSOCKET
#endif

#include <padProto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>

#include "hostStream.h"

static int verbose    = 0;

static int theChannel = 0;

void
pdump(UdpCommPkt p)
{
int i,sz;
	if (p) {
		printf("Packet Dump:");
		sz = ntohs(((PadReply)p)->nBytes);
		for (i=0; i<sz; i++) {
			if ( i%16 == 0 )
				printf("\n");
			printf("%02X ",((uint8_t*)p)[i]);
		}
		printf("\n");
	}
}

void
dumpReply(PadReply p)
{
int16_t tmp;
	printf("Got Reply (proto version 0x%02x):\n", p->version);
	printf("     Type: 0x%02x\n",           (uint8_t)p->type);
	printf("  Channel: %i\n",               p->chnl);
	tmp = ntohs(p->nBytes);
	printf("   nBytes: %i [payload %i]\n",  tmp, tmp-(int16_t)sizeof(*p));
	printf("      XID: 0x%08x\n",           ntohl(p->xid)); 
	tmp = ntohs(p->status);
	printf("   status: %i (%s)\n",          tmp, strerror(-tmp));
	printf("     spec: 0x%02x, 0x%02x\n",   p->spec[0], p->spec[1]);
}

static int
isbe()
{
union {
	uint8_t	 xx[2];
	uint16_t tst;
} endian = { xx : {0xbe, 00} };
	return (0xbe00 == endian.tst);
}

void
usage(char *nm)
{
	fprintf(stderr,"Usage: %s [-bhvec] [-C channel] [-l port] [-n nsamples] [-s srvr_port] [-S sdds_file:col,col,col,col] [-P server_xmit_period] [-d dbg_flgs] ip:port <msg_type_int>\n",nm);
	fprintf(stderr,"          -b bcast to all channels\n");
	fprintf(stderr,"          -h print this help\n");
	fprintf(stderr,"          -v be verbose\n");
	fprintf(stderr,"          -e request wrong endianness (for testing; STRM command only)\n");
	fprintf(stderr,"          -c request col-major data   (for testing; STRM command only)\n");
	fprintf(stderr,"          -n <nsamples> request <nsamples> (STRM command only)\n");
	fprintf(stderr,"          -C <channel>  send to PAD # <channel>\n");
	fprintf(stderr,"          -l <port>     operate in STRM listener mode on <port>\n");
	fprintf(stderr,"          -n <port>     operate in STRM listener mode on <port>\n");
	fprintf(stderr,"          <msg_type_int> (forced to STRM if any of -[nec] options present:\n");
	fprintf(stderr,"                      0 = NOP\n");
	fprintf(stderr,"                      1 = ECHO\n");
	fprintf(stderr,"                      2 = STRM (start stream)\n");
	fprintf(stderr,"                      3 = SPET (pet stream)\n");
	fprintf(stderr,"                      4 = STOP (stop stream)\n");
	fprintf(stderr,"                      5 = SIM  (simulate BPM)\n");
	fprintf(stderr,"                     15 = KILL (terminate padUdpHandler on target)\n");
	fprintf(stderr,"          -s srvr_port  run as a padProto server\n");
	fprintf(stderr,"          -S sdds_file:col,col,col,col SDDS file to 'play'\n");
	fprintf(stderr,"                                       specified columns.\n");
	fprintf(stderr,"          -P period_ms  transmit simulated or playback\n");
	fprintf(stderr,"                        waveforms every 'period_ms'\n");
	fprintf(stderr,"          -p start:end  SDDS page range (1-based) to 'play'\n");
}

static void
client(int sd, char *ip, int port, PadStrmCommand scmd)
{
int               err;
UdpCommPkt        p = 0;

	if ( (err = udpCommConnect(sd, inet_addr(ip), port)) ) {
		fprintf(stderr,"udpCommConnect: %s",strerror(-err));
		exit(1);
	}

	if ( (err=padRequest(sd, theChannel, scmd->type, 0xdead, scmd, &p, 1000)) ) {
		fprintf(stderr,">>> Sending request failed: %s <<<\n",strerror(-err));
	}

	if ( p )
		dumpReply((PadReply)p);

	udpCommFreePacket(p);
}

static int padudpkilled = 0;

static int
server(int port, int chan, int timeout_ms)
{
int         err = -1;
int         sd;
UdpCommPkt  p;
uint32_t	peerip;
int         T = timeout_ms <= 0 ? 1000 : timeout_ms;

	if ( (sd = udpCommSocket(port)) < 0 ) {
		return sd;
	}

	while ( !padudpkilled ) {
		if ( (p = udpCommRecvFrom(sd, T, &peerip, 0)) ) {
			err = padProtoHandler((PadRequest)udpCommBufPtr(p), chan, (int*)&padudpkilled, peerip);
			if ( 0 < err ) {
				/* OK to send packet */
				udpCommReturnPacket(p, ntohs(((PadReply)udpCommBufPtr(p))->nBytes));
			} else {
				/* release buffer */
				udpCommFreePacket(p);
				if ( err < 0 ) {
					fprintf(stderr,"padProtoHandler returned error %i\n",err);
				}
			}
		} else {
			if ( timeout_ms > 0 ) {
				/* server period expired; try to send stream */
				padStreamSimulated();
			}
		}
	}

	padudpkilled = 0;
	err          = 0;
	
	udpCommClose(sd);

	return 0;
}

static void
streamdump(int sd)
{
UdpCommPkt        p;
PadReply          rply;
int16_t           err;
int               i;
int               sz;
int16_t           *buf;

	while ( (p = udpCommRecv(sd, 1000000000)) ) {
		rply = (PadReply)p;
		if ( verbose ) 
			dumpReply(rply);
		if ( rply->type != (PADCMD_STRM | PADCMD_RPLY) ) {
			fprintf(stderr,"Listener: received invalid reply 0x%02x\n", (uint8_t)rply->type);
			break;
		}
		if ( (err=ntohs(rply->status)) ) {
			fprintf(stderr,"Listener: received bad reply (%i -> %s)\n", err, strerror(-err));
			break;
		}
		/* Check matching endianness */
		if ( isbe() != !(rply->strm_cmd_flags & PADCMD_STRM_FLAG_LE) ) {
			fprintf(stderr,"Listener: endianness mismatch\n");
			break;
		}

		sz = ntohs(rply->nBytes) - sizeof(*rply);

		sz/=sizeof(int16_t)*4; /* four channels */

		/* Dump packet */
		buf = (int16_t*)rply->data;

		printf("// IDX: %i, TYPE %i\n",
			rply->strm_cmd_idx,
			PADRPLY_STRM_FLAG_TYPE_GET(rply->strm_cmd_flags));
		/* always write out in fortran (scilab) format with
		 * the samples going down the columns
		 */
		if ( rply->strm_cmd_flags & PADCMD_STRM_FLAG_CM ) {
			for (i=0; i<sz*4; i+=4) {
				printf("%5i %5i %5i %5i\n",
					buf[i+0], buf[i+1], buf[i+2], buf[i+3]);
			}
		} else {
			for (i=0; i<sz; i++) {
				printf("%5i %5i %5i %5i\n",
					buf[i+0*sz], buf[i+1*sz], buf[i+2*sz], buf[i+3*sz]);
			}
		}
		printf("\n\n");
		fflush(stdout);
		udpCommFreePacket(p);
	}

	if ( p ) {
		dumpReply((PadReply)p);
		udpCommFreePacket(p);
	}
}

int
main(int argc, char **argv)
{
int               sd;
int               type = -1;
int               ch;
PadStrmCommandRec scmd;
int               port      = 0;
int               listener  = 0;
char               *col     = 0;
int               nsamples  = 8;
int               badEndian = 0;
int               colMajor  = 0;
int               srvrMode  = 0;
unsigned          dbg       = 0;
int               srvrPerMs = 0;
#ifdef USE_SDDS
const char        *sddsnam  = 0;
int               pgFst     = 0;
int               pgLst     = -1;
#endif

	while ( (ch = getopt(argc, argv, "bvcehl:n:C:s:d:S:P:p:")) > 0 ) {
		switch (ch) {
			default:
				fprintf(stderr,"Unknown option '%c'\n",ch);
				usage(argv[0]);
				exit(1);

			case 'h':
				usage(argv[0]);
				exit(0);

			case 'd':
				if ( 1!=sscanf(optarg,"%i",&dbg) ) {
					fprintf(stderr,"invalid 'debug' value: '%s'\n", optarg);
					usage(argv[0]);
					exit(1);
				}
			break;

			case 'l':
			case 's':
				if ( 1!= sscanf(optarg,"%i",&port) || port < 0 || port > 65535 ) {
					fprintf(stderr,"invalid port number: '%s'\n", optarg);
					usage(argv[0]);
					exit(1);
				}
				if ( 'l' == ch )
					listener = 1;	
				else
					srvrMode = 1;
				break;

			case 'S':
#ifdef USE_SDDS
				sddsnam = optarg;
#else
				fprintf(stderr,"program was built w/o SDDS support; recompile with -DUSE_SDDS\n");
#endif
			break;

			case 'p':
#ifdef USE_SDDS
				if ( 2 != sscanf(optarg,"%i:%i",&pgFst,&pgLst) ) {
					fprintf(stderr,"need <start-page>:<end-page> range\n");
					exit(1);
				}
				if ( --pgFst < 0 ) 
					pgFst = 0;
				/* last page < 0 means -> end */
				pgLst--;

#else
				fprintf(stderr,"program was built w/o SDDS support; recompile with -DUSE_SDDS\n");
#endif
			break;

			case 'b': theChannel = -128; break;

			case 'v': verbose   = 1; break;
			case 'c': colMajor  = 1; type = PADCMD_STRM; break;
			case 'e': badEndian = 1; type = PADCMD_STRM; break;

			case 'C':
				if ( 1 != sscanf(optarg,"%i",&theChannel) || theChannel > 255 || (theChannel < 0 && -128 != theChannel) ) {
					fprintf(stderr,"invalid channel #: '%s'\n",optarg);
					usage(argv[0]);
					exit(1);
				}
			break;

			case 'P':
				if ( 1 != sscanf(optarg,"%i",&srvrPerMs) ) {
					fprintf(stderr,"invalid period (number expected): '%s'\n",optarg);
					usage(argv[0]);
					exit(1);
				}
			break;

			case 'n':
				if ( 1 != sscanf(optarg,"%i",&nsamples) ) {
					fprintf(stderr,"invalid number of samples: '%s'\n",optarg);
					usage(argv[0]);
					exit(1);
				}
				type = PADCMD_STRM;
			break;

		}
	}

#ifdef USE_SDDS
	if ( sddsnam &&  padStreamSddsSetup(sddsnam,pgFst,pgLst) ) {
		fprintf(stderr,"invalid SDDS filespec\n");
		exit(1);
	}
#endif

	if ( !listener && !srvrMode ) {
		if ( argc - optind < 2 || !(col=strchr(argv[optind],':')) || (*col++=0, INADDR_NONE == inet_addr(argv[optind])) || 1 != sscanf(col,"%i",&port) || (-1 == type && 1 != sscanf(argv[optind+1],"%i",&type)) ) {
			usage(argv[0]);
			exit(1);
		}
	}

	
	if ( 0 == port )
		port = listener ? PADPROTO_STRM_PORT : PADPROTO_PORT;

	if ( srvrMode ) {
		extern int padProtoDebug;
		padProtoDebug = dbg;
		server(port, theChannel, srvrPerMs);
		/* should never return here */
		perror("padUdpHandler failed");
		exit(1);
	}

	sd = udpCommSocket(listener ? port : 0);

	if ( sd < 0 ) {
		fprintf(stderr,"udpCommSocket: %s",strerror(-sd));
		exit(1);
	}

	if ( listener ) {
		streamdump(sd);
	} else {
		if ( PADCMD_STRM == (scmd.type = type) ) {
			scmd.flags    = (!isbe() ^ (badEndian == 1)) ? PADCMD_STRM_FLAG_LE : 0;
			scmd.port     = htons(port+1); /* should be PADPROTO_STRM_PORT */
			scmd.nsamples = htonl(nsamples);

			if ( colMajor )
				scmd.flags |= PADCMD_STRM_FLAG_CM;
		}
		client(sd, argv[optind], port, &scmd);
	}

	udpCommClose(sd);
	return 0;
}
