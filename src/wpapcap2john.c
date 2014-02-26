/*
 * This software is Copyright (c) 2013 Jim Fougeron jfoug AT cox dot net,
 * Copyright (c) 2013 Dhiru Kholia <dhiru.kholia at gmail.com>
 * and Copyright (c) 2014 magnum, and it is hereby released
 * to the general public under the following terms:  Redistribution and use in
 * source and binary forms, with or without modification, are permitted.
 *
 * magnum's notes:
 * We seem to use WPA4way_s struct like this:
 *   ssid[36]  is used for ESSID (network name)
 *   essid[18] is used for BSSID (AP MAC address)
 *   bssid[18] unused
 *
 * I find that very confusing. I would think we should rather ditch the
 * ssid member and use the others like this:
 *   essid[36] ESSID (network name)
 *   bssid[18] BSSID (AP MAC address)
 *
 * ...then again, maybe I'm just confused?
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "wpapcap2john.h"
#include "memdbg.h"

/*
 * Max. number of handshakes we can collect from all files combined.
 * Just bump this if you need more.
 */
#define MAX_HANDSHAKES	1000

int GetNextPacket(FILE *in);
int ProcessPacket();
void HandleBeacon();
void Handle4Way(int bIsQOS);
void DumpKey(int idx, int one_three, int bIsQOS);

uint32 start_t, start_u, cur_t, cur_u;
pcaprec_hdr_t pkt_hdr;
uint8 full_packet[65535];
uint8 *packet;
static int bROT;
WPA4way_t wpa[MAX_HANDSHAKES];
int nwpa = 0;
const char cpItoa64[64] =
	"./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
const char *InFName;
unsigned int link_type;

int Process(FILE *in)
{
	pcap_hdr_t main_hdr;

	if (fread(&main_hdr, 1, sizeof(pcap_hdr_t), in) != sizeof(pcap_hdr_t)) {
		fprintf(stderr, "%s: Error, could not read enough bytes to get a common 'main' pcap header\n", InFName);
		return 0;
	}
	if (main_hdr.magic_number ==  0xa1b2c3d4) bROT = 0;
	else if (main_hdr.magic_number ==  0xd4c3b2a1) bROT = 1;
	else { fprintf(stderr, "%s: Error, Invalid pcap magic number, (not a pcap file!)\n", InFName);  return 0; }
	if (bROT) {
		main_hdr.magic_number = swap32u(main_hdr.magic_number);
		main_hdr.version_major = swap16u(main_hdr.version_major);
		main_hdr.version_minor = swap16u(main_hdr.version_minor);
		main_hdr.sigfigs = swap32u(main_hdr.sigfigs);
		main_hdr.snaplen = swap32u(main_hdr.snaplen);
		main_hdr.network = swap32u(main_hdr.network);
	}
	link_type = main_hdr.network;
	if (link_type != LINKTYPE_IEEE802_11 &&
	    link_type != LINKTYPE_PRISM_HEADER &&
	    link_type != LINKTYPE_RADIOTAP_HDR &&
	    link_type != LINKTYPE_PPI_HDR) {
		fprintf(stderr, "%s: No 802.11 wireless traffic data (network %d)\n", InFName, link_type);
		return 0;
	}

	while (GetNextPacket(in)) {
		if (!ProcessPacket())
			return 1;
	}
	return 1;
}

int GetNextPacket(FILE *in)
{
	size_t read_size;

	if (fread(&pkt_hdr, 1, sizeof(pkt_hdr), in) != sizeof(pkt_hdr)) return 0;

	if (bROT) {
		pkt_hdr.ts_sec = swap32u(pkt_hdr.ts_sec);
		pkt_hdr.ts_usec = swap32u(pkt_hdr.ts_usec);
		pkt_hdr.incl_len = swap32u(pkt_hdr.incl_len);
		pkt_hdr.orig_len = swap32u(pkt_hdr.orig_len);
	}
	if (!start_t) {
		start_t = pkt_hdr.ts_sec;
		start_u = pkt_hdr.ts_usec;
	}
	cur_t = pkt_hdr.ts_sec-start_t;
	if (start_u > pkt_hdr.ts_usec) {
		--cur_t;
		cur_u = 1000000-(start_u-pkt_hdr.ts_usec);
	} else
		cur_u = pkt_hdr.ts_usec-start_u;

	read_size = fread(full_packet, 1, pkt_hdr.incl_len, in);
	if (read_size < pkt_hdr.incl_len)
		fprintf(stderr, "%s: truncated last packet\n", InFName);

	return (read_size == pkt_hdr.incl_len);
}

// Ok, this function is the main packet processor.  NOTE, when we are done
// reading packets (i.e. we have done what we want), we return 0, and
// the program will exit gracefully.  It is not an error, it is just an
// indication we have completed (or that the data we want is not here).
int ProcessPacket()
{
	ether_frame_hdr_t *pkt;
	ether_frame_ctl_t *ctl;
	unsigned int frame_skip = 0;

	packet = full_packet;

	// Skip Prism frame if present
	if (link_type == LINKTYPE_PRISM_HEADER) {
		if (packet[7] == 0x40)
			frame_skip = 64;
		else {
			frame_skip = *(unsigned int*)&packet[4];
#if !ARCH_LITTLE_ENDIAN
			JOHNSWAP(frame_skip);
#endif
		}
		if (frame_skip < 8 || frame_skip >= pkt_hdr.incl_len)
			return 0;
		packet += frame_skip;
		pkt_hdr.incl_len -= frame_skip;
	}

	// Skip Radiotap frame if present
	if (link_type == LINKTYPE_RADIOTAP_HDR) {
		frame_skip = *(unsigned short*)&packet[2];
#if !ARCH_LITTLE_ENDIAN
		JOHNSWAP(frame_skip);
#endif
		if (frame_skip == 0 || frame_skip >= pkt_hdr.incl_len)
			return 0;
		packet += frame_skip;
		pkt_hdr.incl_len -= frame_skip;
	}

	// Skip PPI frame if present
	if (link_type == LINKTYPE_PPI_HDR) {
		frame_skip = *(unsigned short*)&packet[2];
#if !ARCH_LITTLE_ENDIAN
		JOHNSWAP(frame_skip);
#endif
		if( frame_skip <= 0 || frame_skip>= (int) pkt_hdr.incl_len )
			return 0;

		// Kismet logged broken PPI frames for a period
		if (frame_skip == 24 && *(unsigned short*)&packet[8] == 2)
			frame_skip = 32;

		if (frame_skip == 0 || frame_skip >= pkt_hdr.incl_len)
			return 0;
		packet += frame_skip;
		pkt_hdr.incl_len -= frame_skip;
	}

	// our data is in *packet with pkt_hdr being the pcap packet header for this packet.
	pkt = (ether_frame_hdr_t*)packet;
	ctl = (ether_frame_ctl_t *)&pkt->frame_ctl;

	if (ctl->type == 0 && ctl->subtype == 8) { // beacon  Type 0 is management, subtype 8 is beacon
		HandleBeacon();
		return 1;
	}
	// if not beacon, then only look data, looking for EAPOL 'type'
	if (ctl->type == 2) { // type 2 is data
		uint8 *p = packet;
		int bQOS = (ctl->subtype & 8) != 0;
		if ( (ctl->toDS ^ ctl->fromDS) != 1)// eapol will ONLY be direct toDS or direct fromDS.
			return 1;
		// Ok, find out if this is a EAPOL packet or not.

		p += sizeof(ether_frame_hdr_t);
		if (bQOS)
			p += 2;
		// p now points to the start of the LLC (logical link control) structure.
		// this is 8 bytes long, and the last 2 bytes are the 'type' field.  What
		// we are looking for is 802.11X authentication packets. These are 0x888e
		// in value.  We are running from an LE system, so should look for 0x8e88
		p += 6;
		if (*((uint16*)p) == 0x8e88)
			Handle4Way(bQOS);	// this packet was a eapol packet.
	}

	return 1;
}

void to_bssid(char ssid[18], uint8 *p)
{
	sprintf(ssid, "%02X:%02X:%02X:%02X:%02X:%02X",p[0],p[1],p[2],p[3],p[4],p[5]);
}

void to_dashed(char ssid[18], uint8 *p)
{
	sprintf(ssid, "%02x-%02x-%02x-%02x-%02x-%02x",p[0],p[1],p[2],p[3],p[4],p[5]);
}

void to_compact(char ssid[18], uint8 *p)
{
	sprintf(ssid, "%02x%02x%02x%02x%02x%02x",p[0],p[1],p[2],p[3],p[4],p[5]);
}

void HandleBeacon()
{
	ether_frame_hdr_t *pkt = (ether_frame_hdr_t*)packet;
	int i;

	ether_beacon_data_t *pDat = (ether_beacon_data_t*)&packet[sizeof(ether_frame_hdr_t)];
	ether_beacon_tag_t *tag = pDat->tags;
	uint8 *pFinal = &packet[pkt_hdr.incl_len];
	char ssid[36] = { 0 };
	char essid[18];

	// addr1 should be broadcast
	// addr2 is source addr (should be same as BSSID)
	// addr3 is BSSID (routers MAC)

	// ok, walk the tags

	while (((uint8*)tag) < pFinal) {
		char *x = (char*)tag;
		if (tag->tagtype == 0 && tag->taglen < sizeof(ssid)) // ESSID
			memcpy(ssid, tag->tag, tag->taglen);
		x += tag->taglen + 2;
		tag = (ether_beacon_tag_t *)x;
	}
	to_bssid(essid, pkt->addr3);
	for (i = 0; i < nwpa; ++i) {
		if (!strcmp(essid, wpa[i].essid) && !strcmp(ssid, wpa[i].ssid))
			return;
	}
	strcpy(wpa[nwpa].ssid, ssid);
	strcpy(wpa[nwpa].essid, essid);
	if (++nwpa >= MAX_HANDSHAKES) {
		fprintf(stderr, "ERROR: Too many handshakes (%d)\n", MAX_HANDSHAKES);
		exit(EXIT_FAILURE);
	}
}

void Handle4Way(int bIsQOS)
{
	ether_frame_hdr_t *pkt = (ether_frame_hdr_t*)packet;
	int i, ess=-1;
	uint8 orig_2[512];
	uint8 *p = (uint8*)&packet[sizeof(ether_frame_hdr_t)];
	ether_auto_802_1x_t *auth;
	int msg = 0;
	char essid[18];

	// ok, first thing, find the beacon.  If we can NOT find the beacon, then
	// do not proceed.  Also, if we find the becon, we may determine that
	// we already HAVE fully cracked this

	to_bssid(essid, pkt->addr3);
	for (i = 0; i < nwpa; ++i) {
		if (!strcmp(essid, wpa[i].essid)) {
			ess=i;
			break;
		}
	}
	if (ess==-1) return;
	if (wpa[ess].fully_cracked)
		return;  // no reason to go on.

	memcpy(orig_2, packet, pkt_hdr.orig_len);

	// Ok, after pkt,  uint16 QOS control (should be 00 00)
	if (bIsQOS)
		p += 2;
	// we are now at Logical-Link Control. (8 bytes long).
	// LLC check not needed here any more.  We do it in the packet cracker section, b4
	// calling this function.  We just need to skip the 8 byte LLC.
	//if (memcmp(p, "\xaa\xaa\x3\0\0\0\x88\x8e", 8)) return; // not a 4way
	p += 8;
	// p now points to the 802.1X Authentication structure.
	auth = (ether_auto_802_1x_t*)p;
	auth->length = swap16u(auth->length);
	//*(uint16*)&(auth->key_info) = swap16u(*(uint16*)&(auth->key_info));
	auth->key_info_u16 = swap16u(auth->key_info_u16);
	auth->key_len  = swap16u(auth->key_len);
	auth->replay_cnt  = swap64u(auth->replay_cnt);
	auth->wpa_keydatlen  = swap16u(auth->wpa_keydatlen);

	if (!auth->key_info.KeyACK) {
		// msg 2 or 4
		if (auth->key_info.Secure) {
			// msg = 4;
			// is this useful?
			return;
		}
		else
			msg = 2;
	} else {
		if (auth->key_info.Install)
			msg = 3;
		else
			msg = 1;
	}

	// Ok, we look for a 1 followed immediately by a 2 which have exact same replay_cnt, we have
	// a 'likely' key. Or we want a 2 followed by a 3 that are 1 replay count apart)  which means
	// we DO have a key.  The 3 is not returned unless the 2 (which came from the client), IS
	// valid. So, we get the anonce from either the 1 or the 3 packet.

	// for our first run, we output ALL valid keys found in the file. That way, I can validate that
	// any keys which were produced by aircrack-ng are 'valid' or not.  aircrack-ng WILL generate some
	// invalid keys.  Also, I want to flag "unknown" keys as just that, unk.  These are 1-2's which
	// do not have valid 3 4's.  They 'may' be valid, but may also be a client with the wrong password.

	if (msg == 1) {
		if (wpa[ess].packet1) free(wpa[ess].packet1);
		wpa[ess].packet1 = (uint8 *)malloc(sizeof(uint8) * pkt_hdr.orig_len);
		memcpy(wpa[ess].packet1, packet, pkt_hdr.orig_len);
		if (wpa[ess].packet2) free(wpa[ess].packet2);  wpa[ess].packet2 = NULL;
		if (wpa[ess].orig_2)  free(wpa[ess].orig_2);   wpa[ess].orig_2 = NULL;
		if (wpa[ess].packet3) free(wpa[ess].packet3);  wpa[ess].packet3 = NULL;
		if (wpa[ess].packet4) free(wpa[ess].packet4);  wpa[ess].packet4 = NULL;
	}
	if (msg == 2) {
		// Some sanitiy checks
		if (pkt_hdr.orig_len < sizeof(ether_frame_hdr_t) + (bIsQOS ? 10 : 8)) {
			fprintf(stderr, "%s: header len %u, wanted to subtract %zu, skipping packet\n", InFName, pkt_hdr.orig_len, sizeof(ether_frame_hdr_t) + (bIsQOS ? 10 : 8));
			return;
		}

		// see if we have a msg1 that 'matches'.
		if (wpa[ess].packet3) free(wpa[ess].packet3);  wpa[ess].packet3 = NULL;
		if (wpa[ess].packet4) free(wpa[ess].packet4);  wpa[ess].packet4 = NULL;
		wpa[ess].packet2 = (uint8 *)malloc(sizeof(uint8) * pkt_hdr.orig_len);
		wpa[ess].orig_2  = (uint8 *)malloc(sizeof(uint8) * pkt_hdr.orig_len);
		memcpy(wpa[ess].packet2, packet, pkt_hdr.orig_len);
		memcpy(wpa[ess].orig_2, orig_2, pkt_hdr.orig_len);

		wpa[ess].eapol_sz = pkt_hdr.orig_len - sizeof(ether_frame_hdr_t) - 8;

		if (bIsQOS)
			wpa[ess].eapol_sz -= 2;

		if (wpa[ess].packet1) {
			ether_auto_802_1x_t *auth2 = auth, *auth1;
			p = (uint8*)wpa[ess].packet1;
			if (bIsQOS)
				p += 2;
			p += 8;
			p += sizeof(ether_frame_hdr_t);
			auth1 = (ether_auto_802_1x_t*)p;
			if (auth1->replay_cnt == auth2->replay_cnt) {
				fprintf (stderr, "\nKey1/Key2 hit (hopeful hit), for SSID:%s (%s)\n", wpa[ess].ssid, InFName);
				DumpKey(ess, 1, bIsQOS);
			}
			// we no longer want to know about this packet 1.
			if (wpa[ess].packet1) free(wpa[ess].packet1);  wpa[ess].packet1 = NULL;
		}
	}
	if (msg == 3) {
		// see if we have a msg2 that 'matches',  which is 1 less than our replay count.
		if (wpa[ess].packet1) free(wpa[ess].packet1);  wpa[ess].packet1 = NULL;
		if (wpa[ess].packet4) free(wpa[ess].packet4);  wpa[ess].packet4 = NULL;
		wpa[ess].packet3 = (uint8 *)malloc(sizeof(uint8) * pkt_hdr.orig_len);
		memcpy(wpa[ess].packet3, packet, pkt_hdr.orig_len);
		if (wpa[ess].packet2) {
			ether_auto_802_1x_t *auth3 = auth, *auth2;
			p = (uint8*)wpa[ess].packet2;
			if (bIsQOS)
				p += 2;
			p += 8;
			p += sizeof(ether_frame_hdr_t);
			auth2 = (ether_auto_802_1x_t*)p;
			if (auth2->replay_cnt+1 == auth3->replay_cnt) {
				fprintf (stderr, "\nKey2/Key3 hit (SURE hit), for SSID:%s (%s)\n", wpa[ess].ssid, InFName);
				DumpKey(ess, 3, bIsQOS);
				wpa[ess].fully_cracked = 1;
			}
		}
		// clear this, so we do not hit the same 3 packet and output exact same 2/3 combo.
		if (wpa[ess].packet3) free(wpa[ess].packet3);  wpa[ess].packet3 = NULL;
		if (wpa[ess].packet2) free(wpa[ess].packet2);  wpa[ess].packet2 = NULL;
		if (wpa[ess].orig_2)  free(wpa[ess].orig_2);   wpa[ess].orig_2 = NULL;
	}
}

// These 2 functions output data properly for JtR, in base-64 format. These
// were taken from hccap2john.c source, and modified for this project.
static void code_block(unsigned char *in, unsigned char b)
{
	putchar(cpItoa64[in[0] >> 2]);
	putchar(cpItoa64[((in[0] & 0x03) << 4) | (in[1] >> 4)]);
	if (b) {
		putchar(cpItoa64[((in[1] & 0x0f) << 2) | (in[2] >> 6)]);
		putchar(cpItoa64[in[2] & 0x3f]);
	} else
		putchar(cpItoa64[((in[1] & 0x0f) << 2)]);
}

void DumpKey(int ess, int one_three, int bIsQOS)
{
	ether_auto_802_1x_t *auth13, *auth2;
	uint8 *p = (uint8*)wpa[ess].packet2;
	uint8 *pkt2 = p;
	uint8 *p13;
	hccap_t	hccap;
	int i;
	uint8 *w;
	char sta_mac[18], ap_mac[18], gecos[13];

	fprintf (stderr, "Dumping key %d at time:  %d.%d BSSID %s\n", one_three, cur_t, cur_u, wpa[ess].essid);
	printf ("%s:$WPAPSK$%s#", wpa[ess].ssid, wpa[ess].ssid);
	if (!wpa[ess].packet2) { printf ("ERROR, msg2 null\n"); return; }
	if (bIsQOS)
		p += 2;
	p += 8;
	p += sizeof(ether_frame_hdr_t);
	auth2 = (ether_auto_802_1x_t*)p;
	if (one_three==1) {
		if (!wpa[ess].packet1) { printf ("ERROR, msg1 null\n"); return; }
		p = wpa[ess].packet1;
	} else  {
		if (!wpa[ess].packet3) { printf ("ERROR, msg3 null\n"); return; }
		p = wpa[ess].packet3;
	}
	p13 = p;
	if (bIsQOS)
		p += 2;
	p += 8;
	p += sizeof(ether_frame_hdr_t);
	auth13 = (ether_auto_802_1x_t*)p;

	memset(&hccap, 0, sizeof(hccap_t));
	hccap.keyver = auth2->key_info.KeyDescr;
	memcpy(hccap.mac1, ((ether_frame_hdr_t*)pkt2)->addr1, 6);
	memcpy(hccap.mac2, ((ether_frame_hdr_t*)(p13))->addr1, 6);
	memcpy(hccap.nonce1, auth2->wpa_nonce,32);
	memcpy(hccap.nonce2, auth13->wpa_nonce,32);
	memcpy(hccap.keymic, auth2->wpa_keymic, 16);
	p = wpa[ess].orig_2;
	if (bIsQOS)
		p += 2;
	p += 8;
	p += sizeof(ether_frame_hdr_t);
	auth2 = (ether_auto_802_1x_t*)p;
	memset(auth2->wpa_keymic, 0, 16);
	memcpy(hccap.eapol, auth2, wpa[ess].eapol_sz);
	hccap.eapol_size = wpa[ess].eapol_sz;

	w = (uint8 *)&hccap;
	for (i = 36; i + 3 < sizeof(hccap_t); i += 3)
		code_block(&w[i], 1);
	code_block(&w[i], 0);
	to_compact(gecos, hccap.mac1);
	to_dashed(ap_mac, hccap.mac1);
	to_dashed(sta_mac, hccap.mac2);
	printf(":%s:%s:%s::WPA", sta_mac, ap_mac, gecos);
	if (hccap.keyver > 1)
		printf("%d", hccap.keyver);
	printf(":password %sverified:%s\n", (one_three == 1) ? "not " : "", InFName);
	fflush(stdout);
	fprintf(stderr, "\n");
}

int main(int argc, char **argv)
{
	FILE *in;
	int i;
	char *base;

	if (argc < 2)
		return !!fprintf(stderr,
		                 "Usage: %s <pcap_file[s]>\n", argv[0]);

	for (i = 1; i < argc; i++) {
		in = fopen(InFName = argv[i], "rb");
		if (in) {
			if ((base = strrchr(InFName, '/')))
				InFName = ++base;
			Process(in);
			fclose(in);
		} else
			fprintf(stderr, "Error, file %s not found\n", argv[i]);
	}
	return 0;
}
