#include "lpm_demo.h"

#include <string.h>

void lpm_demo_base(lpm_data *data)
{
    memset(data, 0, sizeof *data);
    data->status.battery_pct = 82;
    lpm_ping_reset(&data->ping, LPM_IP(192, 168, 1, 1));
    lpm_poe_reset(&data->poe);
}

void lpm_demo_ping(lpm_ping_log *log)
{
    /* RTTs in tenths of a millisecond; 0 marks a timeout. */
    static const uint16_t pattern[] = {31, 42, 35, 48, 39, 33, 61, 44, 52, 38, 72, 121, 183, 94, 51,
                                       43, 34, 41, 62, 47, 40, 36, 0,  44, 52, 41, 63,  50,  43, 37};
    uint16_t seq;
    lpm_ping_reset(log, LPM_IP(192, 168, 1, 1));
    for (seq = 1; seq <= 150; seq++) {
        uint16_t v = pattern[seq % (sizeof pattern / sizeof pattern[0])];
        if (v == 0 || seq == 131) {
            lpm_ping_add_timeout(log, seq);
        } else {
            lpm_ping_add_reply(log, seq, v * 100u + (seq * 37u) % 90u);
        }
    }
}

static void straight(lpm_wiremap *map)
{
    uint8_t i;
    memset(map, 0, sizeof *map);
    map->valid = true;
    for (i = 0; i < LPM_PINS; i++) {
        map->remote[i] = (uint8_t)(i + 1);
    }
}

void lpm_demo_wiremap_straight(lpm_wiremap *map)
{
    straight(map);
}

void lpm_demo_wiremap_fault(lpm_wiremap *map)
{
    straight(map);
    map->remote[3] = 0; /* pin 4 open */
    map->remote[2] = 6; /* 3 and 6 swapped */
    map->remote[5] = 3;
    map->shorted_to[6] = 8; /* 7-8 shorted */
    map->shorted_to[7] = 7;
}

void lpm_demo_wiremap_crossover(lpm_wiremap *map)
{
    straight(map);
    map->remote[0] = 3;
    map->remote[2] = 1;
    map->remote[1] = 6;
    map->remote[5] = 2;
}

void lpm_demo_poe(lpm_poe_log *log)
{
    uint8_t i;
    lpm_poe_reset(log);
    log->mode = NULL;
    for (i = 0; i < 45; i++) {
        lpm_poe_add(log, (uint16_t)(47800u + (i * 53u) % 700u - (i == 30 ? 1900u : 0u)));
    }
}

void lpm_demo_scan(lpm_scan *scan, bool running)
{
    static const uint8_t last_octets[] = {1, 2, 10, 23, 24, 50, 77, 101, 102, 140, 200};
    uint8_t mac[6] = {0x00, 0x1a, 0x2b, 0x3c, 0x00, 0x00};
    uint8_t i;
    lpm_scan_reset(scan, LPM_IP(192, 168, 1, 1), LPM_IP(192, 168, 1, 254), LPM_IP(192, 168, 1, 1));
    for (i = 0; i < sizeof last_octets; i++) {
        mac[4] = (uint8_t)(i * 17);
        mac[5] = last_octets[i];
        lpm_scan_add(scan, LPM_IP(192, 168, 1, last_octets[i]), mac);
    }
    mac[3] = 0x99; /* a second device answering for .50 */
    lpm_scan_add(scan, LPM_IP(192, 168, 1, 50), mac);
    scan->probed = running ? 118 : 254;
    scan->running = running;
}

void lpm_demo_health(lpm_health *h)
{
    static const uint8_t gw_mac[6] = {0x00, 0x1a, 0x2b, 0x3c, 0x00, 0x01};
    memset(h, 0, sizeof *h);
    h->link = LPM_STEP_PASS;
    h->link_mbps = 100;
    h->full_duplex = true;
    h->address = LPM_STEP_PASS;
    h->source = LPM_ADDR_DHCP;
    h->ip = LPM_IP(192, 168, 1, 23);
    h->gateway = LPM_IP(192, 168, 1, 1);
    h->gateway_arp = LPM_STEP_PASS;
    memcpy(h->gateway_mac, gw_mac, 6);
    h->gateway_ping = LPM_STEP_FAIL;
}

void lpm_demo_dhcp_acked(lpm_dhcp *dhcp)
{
    memset(dhcp, 0, sizeof *dhcp);
    dhcp->state = LPM_DHCP_ACKED;
    dhcp->offer_ms = 142;
    dhcp->ack_ms = 38;
    dhcp->server = LPM_IP(192, 168, 1, 1);
    dhcp->offered_ip = LPM_IP(192, 168, 1, 23);
    dhcp->mask = LPM_IP(255, 255, 255, 0);
    dhcp->router = LPM_IP(192, 168, 1, 1);
    dhcp->lease_known = true;
    dhcp->lease_s = 86400;
}

void lpm_demo_dhcp_offer_only(lpm_dhcp *dhcp)
{
    lpm_demo_dhcp_acked(dhcp);
    dhcp->state = LPM_DHCP_OFFER_ONLY;
    dhcp->ack_ms = 0;
    dhcp->lease_known = false;
}
