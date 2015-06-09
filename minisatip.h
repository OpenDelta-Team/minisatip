#ifndef MINISATIP_H
#define MINISATIP_H

#define _GNU_SOURCE 

#include "stream.h"
#include "socketworks.h"
#include "utils.h"


#define VERSION_BUILD "36"
#define CC(a,b,c) #a b #c
#define VERSION CC(0.3.,VERSION_BUILD,)

void set_options (int argc, char *argv[]);

#define RRTP_OPT 'r'
#define DEVICEID_OPT 'd'
#define HTTPSERVER_OPT 'w'
#define HTTPPORT_OPT 'x'
#define LOG_OPT 'l'
#define HELP_OPT 'h'
#define SCAN_OPT 'z'
#define PLAYLIST_OPT 'p'
#define DVBS2_ADAPTERS_OPT 'a'
#define CLEANPSI_OPT 't'
#define MAC_OPT 'm'
#define FOREGROUND_OPT 'f'
#define BW_OPT 'c'
#define DVRBUFFER_OPT 'b'
#define ENABLE_ADAPTERS_OPT 'e'
#define UNICABLE_OPT 'u'
#define JESS_OPT 'j'
#define DVBAPI_OPT 'o'
#define SYSLOG_OPT 'g'
#define RTSPPORT_OPT 'y'
#define SATIPCLIENT_OPT 's'


#define PID_FILE "/var/run/minisatip.pid"

#define copy32(a,i,v) { a[i] = ((v)>>24) & 0xFF;\
			a[i+1] = ((v)>>16) & 0xFF;\
			a[i+2] = ((v)>>8) & 0xFF;\
			a[i+3] = (v) & 0xFF; }
#define copy16(a,i,v) { a[i] = ((v)>>8) & 0xFF; a[i+1] = (v) & 0xFF; }

#define copy16r(v, a, i) { v = ((a[i] & 0xFF) << 8) | a[i+1]; }
#define copy16rr(v, a, i) { v = ((a[i+1] & 0xFF) << 8) | a[i]; }

#define copy32r(v, a, i) { v = ((a[i] & 0xFF) << 24) | ((a[i+1] & 0xFF) << 16) | ((a[i+2] & 0xFF) << 8)| (a[i+3] & 0xFF);   }
#define copy32rr(v, a, i) { v = ((a[i+3] & 0xFF) << 24) | ((a[i+2] & 0xFF) << 16) | ((a[i+1] & 0xFF) << 8)| (a[i] & 0xFF);   }

struct struct_opts
{
	char *rrtp;
	char *http_host;			 //http-server host
	char *disc_host;			 //discover host
	char mac[13];
	unsigned int log, slog, start_rtp, http_port;
	int timeout_sec;
	int force_sadapter, force_tadapter, force_cadapter;
	int daemon;
	int device_id;
	int bootid;
	int bw;	
	int dvr_buffer;
	int adapter_buffer;
	int output_buffer;
	int force_scan;
	int clean_psi;
	int file_line;
	char *last_log;	
	int dvbapi_port;
	char *dvbapi_host;
	char *satipc;	
	char playlist[200];
	int drop_encrypted;
	int rtsp_port;
};


int ssdp_discovery (sockets * s);
int becomeDaemon ();
int readBootID();
char * http_response (sockets *s, int rc, char *ah, char *desc, int cseq, int lr);

#endif
