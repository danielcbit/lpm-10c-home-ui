/*
 * The added tools that share Link status's system state 8: Link status itself
 * and VLAN test (link_status.c). Their home items both select the vendor QC
 * slot, so M on either
 * of them reaches lpm_link_open, which opens the one the home list shows; from
 * then on every key, draw and frame message of state 8 is passed to that tool.
 *
 * Each draws as a list of rows (row_list) through the Link status list code:
 * a name, a verdict on the right, and detail lines, scrolled by UP and DOWN.
 */
#ifndef LPM_PATCH_TOOLS_H
#define LPM_PATCH_TOOLS_H

#include <stdint.h>

enum { TOOL_LINK, TOOL_VLAN };

#define TOOLS_MAGIC 0x4C504D54u   /* "LPMT": the RAM no longer holds its power-up pattern */
#define TAGGING_MAGIC 0x564C414Eu /* "VLAN": frames are tagged */

typedef struct {
    uint32_t magic;   /* TOOLS_MAGIC once the home list has set position */
    uint32_t tagging; /* TAGGING_MAGIC while a VLAN test runs; anything else, untagged */
    uint16_t tag;     /* the 802.1Q tag control: priority 0 and the VLAN */
    uint16_t vlan;    /* the VLAN test's VLAN, kept until power off */
    uint8_t position; /* the home list position, when the vendor selection matches it */
    uint8_t tool;     /* TOOL_*, the one open in state 8 */
    uint8_t digit;    /* the VLAN digit edited, 0-3 from the left */
    uint8_t running;  /* reserved: a run that polls keys */
    uint8_t stop;     /* reserved */
} tools_state;

/* After ping_state (0x20020200, 832 bytes), in the RAM the vendor never uses. */
#define TOOLS ((tools_state *)0x20020580u)

/* A tool's rows. row() returns the name and gives the verdict, its colour and
 * the detail, in a buffer of 128 of which the detail takes at most the first 96
 * (a name made on the spot may go after it); lines(row) is the number of detail
 * lines, 0 for a heading. */
typedef struct {
    int (*count)(void);
    int (*lines)(int row);
    const char *(*row)(int row, const char **verdict, uint16_t *colour, char *detail);
    uint8_t *(*top)(void);
} row_list;

/* home_ui.c */
int lpm_home_tool(void);

/* link_status.c */
void lpm_vlan_transmit(void); /* the vendor transmit and receive, tagged while a VLAN test runs */
int lpm_vlan_receive(void);
const char *lpm_verdict(unsigned step, uint16_t *colour);
const char *lpm_link_row(int row, const char **verdict, uint16_t *colour, char *detail);

#endif
