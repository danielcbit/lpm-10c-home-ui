/*
 * Home screen in the working design (output/screens/01-home.png) for the vendor
 * LPM-10C TX firmware V2.0.7.
 *
 * The update file may not grow beyond the vendor's 434,176 bytes (a larger
 * home-ui build installed only partly and crashed), so this has about 3 KB for
 * code after the payload; its fonts, icons and strings live in the freed CJK
 * glyph table (home_ui.ld). It keeps no RAM state: everything is derived from
 * the vendor selection byte.
 *
 * Linked by scripts/build_patched_image.py with home_ui.ld at flash 0x0807244C,
 * after the vendor payload. The vendor menu calls are redirected here:
 *   0x0800FA7E, 0x0800FA9E  grid draw 0x0800F648(partial) -> lpm_home_draw
 *   0x0800D802              grid keys 0x080180C0(action)  -> lpm_home_key
 *                           (UP and DOWN in Link status too: lpm_link_key)
 * The vendor still owns the selection byte, so M opens the same function, and
 * still draws its battery icon (x 176-215, y 4-20) after every full draw.
 *
 * Vendor interface (docs/lpm-10c-bootloader-analysis.md; drawing in draw.c):
 *   0x20000150[1]   menu state: 4 + vendor index of the selected item
 *   0x0800E960      post_message(id, a, b); id 7 redraws the menu
 */
#include <stdint.h>

#include "draw.h"
#include "lpm_assets.h"
#include "lpm_theme.h"

#define MENU_STATE ((volatile uint8_t *)0x20000150u)

#define post_message ((void (*)(int, int, int))0x0800E961u)
#define vendor_draw_string ((void (*)(uint16_t, uint16_t, uint16_t, uint16_t, uint16_t, const char *))0x08018D49u)

#define LINK_SYS_STATE 8 /* Link status, whose UP and DOWN arrive here */
#define FIRST_STATE 4
#define REDRAW_MENU 7
#define ITEMS 10
#define ROW_H 36
#define ROWS 6
#define LIST_W 232 /* the scroll bar sits to the right */
#define BAR_X 234

typedef struct {
    const char *name;
    const lpm_icon *icon;
    uint8_t vendor; /* index in the vendor menu: state byte - 4 */
} home_item;

/* Design order and names. Link status (link_status.c) replaces the vendor QC
 * test in its slot; Link speed is kept, as only it can see a gigabit link. */
static const home_item items[ITEMS] = {
    {"Cable map", &lpm_icon_cable_map, 0},  {"Cable length", &lpm_icon_ruler, 3},
    {"Cable trace", &lpm_icon_trace, 1},    {"Port flash", &lpm_icon_port_flash, 2},
    {"PoE voltage", &lpm_icon_bolt, 6},     {"Link status", &lpm_icon_ethernet, 4},
    {"Link speed", &lpm_icon_speed, 5},     {"Ping monitor", &lpm_icon_latency, 7},
    {"IP scan", &lpm_icon_hosts, 8},        {"Settings", &lpm_icon_settings, 9},
};

void lpm_cust_info_pin_english(void); /* cust_info.c */

static int selected_position(void)
{
    int vendor = MENU_STATE[1] - FIRST_STATE;
    for (int i = 0; i < ITEMS; i++)
        if (items[i].vendor == vendor)
            return i;
    return 0;
}

static void paint_row(int index, int y, int selected)
{
    uint16_t bg = selected ? LPM_C_ACCENT : LPM_C_BG;
    uint16_t fg = selected ? LPM_C_ON_ACCENT : LPM_C_FG;
    fill(0, y, LIST_W, ROW_H, LPM_C_BG);
    if (selected)
        fill(LPM_MARGIN - 2, y + 1, LIST_W - 2 * (LPM_MARGIN - 2), ROW_H - 3, bg);
    else
        fill(LPM_MARGIN, y + ROW_H - 1, LIST_W - 2 * LPM_MARGIN, 1, LPM_C_RULE);
    draw_icon(items[index].icon, 14, y + 6, fg, bg);
    text(&font_body, 48, y + 10, items[index].name, fg, bg);
}

/* Replaces 0x0800F648 for both the full draw (0) and the redraw after a key (1). */
void lpm_home_draw(int partial)
{
    char count[8];
    int n = 0, selected = selected_position();
    int top = selected - 2;
    if (top > ITEMS - ROWS)
        top = ITEMS - ROWS;
    if (top < 0)
        top = 0;
    lpm_cust_info_pin_english();

    /* The status bar holds the vendor battery icon, which is repainted only after a full
     * draw (0x0800FA82). Clearing it on every key press made the icon flash, so the bar
     * and the static chrome are drawn only when the vendor cleared the screen. */
    if (!partial) {
        draw_header("Tools", 10);
        fill(0, LPM_CONTENT_Y, LPM_SCREEN_W, LPM_HOME_LIST_Y - LPM_CONTENT_Y, LPM_C_BG);
    } else {
        fill(LPM_SCREEN_W - 70, LPM_TITLE_Y + 12 + font_small.dy, 60, font_small.rows, LPM_C_BG);
    }
    if (selected + 1 >= 10)
        count[n++] = '1';
    count[n++] = (char)('0' + (selected + 1) % 10);
    count[n++] = '/';
    count[n++] = '1';
    count[n++] = '0';
    count[n] = 0;
    text_right(&font_small, LPM_SCREEN_W - 10, LPM_TITLE_Y + 12, count, LPM_C_DIM, LPM_C_BG);

    for (int row = 0; row < ROWS; row++)
        paint_row(top + row, LPM_HOME_LIST_Y + row * ROW_H, top + row == selected);

    int track = ROWS * ROW_H, thumb = track * ROWS / ITEMS;
    fill(BAR_X, LPM_HOME_LIST_Y, 3, track, LPM_C_RULE);
    fill(BAR_X, LPM_HOME_LIST_Y + (track - thumb) * top / (ITEMS - ROWS), 3, thumb, LPM_C_DIM);

    if (!partial) {
        int end = LPM_HOME_LIST_Y + ROWS * ROW_H;
        fill(0, end, LPM_SCREEN_W, LPM_FOOTER_Y - end, LPM_C_BG);
        draw_footer("M open", "UP/DOWN select");
    }
}

/* Replaces the title draw_string(x, y, w, h, size, label) at 0x0800F3F8 in the vendor tool
 * frame: the tool's name as the home list shows it, in a field wide enough for it (the
 * vendor's 88 pixels hold 11 characters). Other states keep the vendor label. */
#define TITLE_W 120

void lpm_tool_title(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t size, const char *label)
{
    int vendor = MENU_STATE[0] - FIRST_STATE;
    for (int i = 0; i < ITEMS; i++)
        if (items[i].vendor == vendor) {
            label = items[i].name;
            w = TITLE_W;
        }
    vendor_draw_string(x, y, w, h, size, label);
}

/* Replaces 0x080180C0. Actions 2 (up) and 4 (left) select the previous item,
 * 3 (down) and 5 (right) the next; the list stops at both ends. The key table
 * also sends UP and DOWN here from Link status, which scrolls its own list. */
void lpm_link_key(int action);

void lpm_home_key(int action)
{
    if (MENU_STATE[0] == LINK_SYS_STATE) {
        lpm_link_key(action);
        return;
    }
    int position = selected_position();
    if ((action == 2 || action == 4) && position > 0)
        position--;
    else if ((action == 3 || action == 5) && position < ITEMS - 1)
        position++;
    MENU_STATE[1] = (uint8_t)(FIRST_STATE + items[position].vendor);
    post_message(REDRAW_MENU, 0, 0);
}
