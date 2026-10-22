/*
 * The Link status network checks, for Ping monitor (ping_monitor.c). Key task
 * only; the state they work on is Link status's own (LINK in link_status.c),
 * which is cleared first, so each tool starts from nothing.
 */
#ifndef LPM_PATCH_NET_H
#define LPM_PATCH_NET_H

#include <stdint.h>

#define LINK_SYS_STATE 8  /* Link status, in the vendor QC test */
#define PING_SYS_STATE 11 /* Ping monitor, in the vendor PING tool */

enum { NET_NO_LINK, NET_NO_ADDRESS, NET_NO_GATEWAY, NET_READY };

/* Link, a DHCP address (a lease, or an OFFER only) and the gateway's MAC by ARP,
 * as the first three Link status checks: how far it got. */
int lpm_net_prepare(void);
const uint8_t *lpm_net_ip(void);
const uint8_t *lpm_net_mask(void);
const uint8_t *lpm_net_gateway(void);
const uint8_t *lpm_net_gateway_mac(void);
/* The MAC of ip on our subnet: 1 when it answered within 1 s. */
int lpm_net_arp(const uint8_t *ip, uint8_t *mac);
/* One ICMP echo to target through mac, numbered seq; 1 with *rtt_us at the reply.
 * poll is called while waiting; when it returns non-zero the wait ends and 0 is
 * returned. ARP requests for our address are answered meanwhile. */
int lpm_net_echo(const uint8_t *mac, const uint8_t *target, unsigned seq, uint32_t *rtt_us, int (*poll)(void));

/* ping_monitor.c: draws the Ping monitor frame for message 0x36 in state 11
 * and returns 1, or returns 0 when the vendor PING screen is in use. */
int lpm_ping_frame(void);

#endif
