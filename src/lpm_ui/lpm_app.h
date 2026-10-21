/* Home list and tool screens.
 *
 * Integration contract: the firmware feeds key events to lpm_app_key(),
 * performs the returned action with its existing test routines, updates the
 * shared lpm_data, calls lpm_app_data_changed(), and calls lpm_app_render()
 * from its UI loop. The UI never starts a measurement or touches the network
 * itself. */
#ifndef LPM_APP_H
#define LPM_APP_H

#include "lpm_data.h"
#include "lpm_gfx.h"
#include "lpm_input.h"
#include "lpm_menu.h"

typedef enum {
    LPM_ITEM_CABLE_MAP,
    LPM_ITEM_LENGTH,
    LPM_ITEM_TRACE,      /* stock screen */
    LPM_ITEM_PORT_FLASH, /* stock screen */
    LPM_ITEM_POE,
    LPM_ITEM_LINK,
    LPM_ITEM_PING,
    LPM_ITEM_IP_SCAN,
    LPM_ITEM_SETTINGS, /* stock screen */
    LPM_ITEM_COUNT
} lpm_item;

#define LPM_HOME LPM_ITEM_COUNT

typedef enum {
    LPM_ACTION_NONE,
    LPM_ACTION_START,       /* start or re-run `item` (for LINK, `tab` 0 = checklist, 1 = DHCP) */
    LPM_ACTION_STOP,        /* stop `item`; the UI has already left its screen */
    LPM_ACTION_OPEN_NATIVE, /* run the stock screen for `item`, then call lpm_app_resume() */
    LPM_ACTION_CANCEL_SCAN, /* stop probing; results stay on screen */
    LPM_ACTION_PING_HOST,   /* stop any scan, then ping `ip` (packet buffers are shared) */
    LPM_ACTION_UNHANDLED    /* not consumed (Back on the home list): apply stock behaviour */
} lpm_action_kind;

typedef struct {
    lpm_action_kind kind;
    lpm_item item;
    uint8_t tab;
    uint32_t ip;
} lpm_action;

typedef struct {
    const char *name;
    const lpm_icon *icon;
    bool native;
} lpm_item_info;

const lpm_item_info *lpm_item_get(lpm_item item);

#define LPM_DIRTY_ALL 0x01u
#define LPM_DIRTY_STATUS 0x02u
#define LPM_DIRTY_CONTENT 0x04u
#define LPM_DIRTY_LIST 0x08u

typedef struct {
    const lpm_data *data;
    lpm_menu home;  /* selection survives visits to tool and stock screens */
    lpm_menu hosts; /* IP scan result list */
    lpm_item screen;
    uint8_t link_tab;
    bool ping_from_scan;
    uint8_t dirty;
    lpm_menu_change change;
} lpm_app;

void lpm_app_init(lpm_app *app, const lpm_data *data);
lpm_action lpm_app_key(lpm_app *app, lpm_key_event event);
void lpm_app_resume(lpm_app *app);
void lpm_app_data_changed(lpm_app *app);
void lpm_app_status_changed(lpm_app *app);
/* Draws only what changed since the last call. */
void lpm_app_render(lpm_app *app, const lpm_display *d);

#endif
