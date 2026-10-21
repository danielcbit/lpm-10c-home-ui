/* Simulated measurement data for the host simulator and tests. None of these
 * values come from a tester. */
#ifndef LPM_DEMO_H
#define LPM_DEMO_H

#include "lpm_data.h"

#define LPM_IP(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

void lpm_demo_base(lpm_data *data);
void lpm_demo_ping(lpm_ping_log *log);
void lpm_demo_wiremap_straight(lpm_wiremap *map);
void lpm_demo_wiremap_fault(lpm_wiremap *map);
void lpm_demo_wiremap_crossover(lpm_wiremap *map);
void lpm_demo_poe(lpm_poe_log *log);
void lpm_demo_scan(lpm_scan *scan, bool running);
void lpm_demo_health(lpm_health *health);
void lpm_demo_dhcp_acked(lpm_dhcp *dhcp);
void lpm_demo_dhcp_offer_only(lpm_dhcp *dhcp);

#endif
