/*
 * Switch port, the second tab of Link status (switch_port.c). The key task
 * fills switch_state; the GUI task reads it through lpm_switch_detail.
 */
#ifndef LPM_PATCH_SWITCH_PORT_H
#define LPM_PATCH_SWITCH_PORT_H

#include <stdint.h>

#include "lpm_data.h"

#define STEP_NONE (LPM_STEP_SKIPPED + 1) /* not offered by this network, which is no fault */

enum { SW_LINK, SW_SWITCH, SW_PORT, SW_VLAN, SW_ADDRESS, SW_POE, SW_DOT1X, SWITCH_ROWS };
enum { HEARD_LLDP = 1, HEARD_CDP = 2 };
enum { DOT1X_REQUESTED = 1 };

#define SWITCH_TEXT 32 /* a name, port or model, cut to what one detail line shows */
#define SWITCH_TAGGED_MAX 6

typedef struct {
    uint8_t step[SWITCH_ROWS]; /* lpm_step, or STEP_NONE */
    uint8_t tab;               /* the Link status tab shown: 0 checklist, 1 switch port */
    uint8_t drawn_tab;         /* the tab on screen; the GUI task's own */
    uint8_t top;               /* the first row shown */
    uint8_t heard;             /* HEARD_* */
    uint8_t dot1x;
    uint8_t ten_mbps, full_duplex;
    uint8_t seconds;     /* listened so far */
    uint8_t poe_class;   /* 1 + the class announced, or 0 */
    uint8_t tagged_count;
    uint8_t address[4];  /* management IPv4 address */
    uint16_t vlan, voice_vlan;
    uint16_t poe_dw;     /* power, in 0.1 W */
    uint16_t tagged[SWITCH_TAGGED_MAX];
    char name[SWITCH_TEXT], platform[SWITCH_TEXT], port[SWITCH_TEXT], port_desc[SWITCH_TEXT];
} switch_state;

/* Above link_state (0x20020000, 200 bytes), in the RAM the vendor never uses. */
#define SWITCH ((switch_state *)0x20020100u)

/* scripts/preview_link_status.py reads this layout. */
_Static_assert(sizeof(switch_state) == 168 && __builtin_offsetof(switch_state, name) == 40,
               "the layout the emulator reads");

extern const char *const lpm_switch_names[SWITCH_ROWS];
extern const uint8_t lpm_switch_lines[SWITCH_ROWS];

void lpm_switch_run(void);
/* All zero, on the first tab: on entering Link status. */
void lpm_switch_reset(void);
void lpm_switch_detail(int row, char *p);

#endif
