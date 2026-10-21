/*
 * List-style main menu for the vendor LPM-10C TX firmware V2.0.7.
 *
 * Replaces the 2x2 grid (3 pages of 4) with one scrolling list of the same ten
 * items. Built freestanding and copied, unlinked, to flash 0x0807244C after the
 * vendor payload (scripts/build_patched_image.py). It therefore must not need
 * relocations: vendor routines and RAM are reached through absolute addresses,
 * and there is no read-only data or switch table.
 *
 * Vendor interface (docs/lpm-10c-bootloader-analysis.md):
 *   0x20000150[1]   menu state: 4 + index of the selected item (4..13)
 *   0x20000018      table of the ten item label pointers (RAM)
 *   0x200001A0/A2   foreground/background colour used by the text routines
 *   0x08018568      fill_rect(x0, y0, x1, y1, colour), inclusive corners
 *   0x08018D48      draw_string(x, y, w, h, size, text), size 12 or 16, opaque
 *   0x0800E960      post_message(id, a, b); id 7 redraws the menu
 */
#include <stdint.h>

#define MENU_STATE ((volatile uint8_t *)0x20000150u)
#define MENU_LABELS ((const char *const *)0x20000018u)
#define TEXT_FG (*(volatile uint16_t *)0x200001A0u)
#define TEXT_BG (*(volatile uint16_t *)0x200001A2u)

#define fill_rect ((void (*)(uint16_t, uint16_t, uint16_t, uint16_t, uint16_t))0x08018569u)
#define draw_string ((void (*)(uint16_t, uint16_t, uint16_t, uint16_t, uint8_t, const char *))0x08018D49u)
#define post_message ((void (*)(int, int, int))0x0800E961u)

#define ITEMS 10
#define FIRST_STATE 4
#define REDRAW_MENU 7

/* The status bar and the header tab above y 40 stay vendor-drawn. */
#define LIST_X0 4
#define LIST_X1 231
#define LIST_Y 44
#define ROW_H 38
#define ROWS 7
#define BAR_X0 235
#define BAR_X1 237

/* Vendor palette: black screen, 0x7304 selection, 0x31A7 frames, white text. */
#define C_BG 0x0000u
#define C_SELECT 0x7304u
#define C_RULE 0x31A7u
#define C_TEXT 0xFFFFu

static int selected_index(void)
{
    int index = MENU_STATE[1] - FIRST_STATE;
    if (index < 0 || index >= ITEMS)
        index = 0;
    return index;
}

static void draw_row(int index, int y, int selected)
{
    uint16_t colour = selected ? C_SELECT : C_BG;
    fill_rect(LIST_X0, (uint16_t)y, LIST_X1, (uint16_t)(y + ROW_H - 3), colour);
    fill_rect(LIST_X0, (uint16_t)(y + ROW_H - 2), LIST_X1, (uint16_t)(y + ROW_H - 2), C_RULE);
    TEXT_FG = C_TEXT;
    TEXT_BG = colour;
    draw_string(LIST_X0 + 12, (uint16_t)(y + 10), LIST_X1 - LIST_X0 - 24, 16, 16, MENU_LABELS[index]);
}

/* Replaces 0x0800F648 at both call sites. The argument (0 full, 1 after a key) is
 * ignored: every visible row is repainted, row by row, so nothing flashes black. */
void lpm_menu_draw(int partial)
{
    (void)partial;
    int selected = selected_index();
    int first = selected - ROWS / 2;
    if (first > ITEMS - ROWS)
        first = ITEMS - ROWS;
    if (first < 0)
        first = 0;

    for (int row = 0; row < ROWS; row++)
        draw_row(first + row, LIST_Y + row * ROW_H, first + row == selected);

    int track = ROWS * ROW_H;
    int thumb = track * ROWS / ITEMS;
    int top = LIST_Y + (track - thumb) * first / (ITEMS - ROWS);
    fill_rect(BAR_X0, LIST_Y, BAR_X1, (uint16_t)(LIST_Y + track - 1), C_RULE);
    fill_rect(BAR_X0, (uint16_t)top, BAR_X1, (uint16_t)(top + thumb - 1), C_TEXT);
    TEXT_FG = C_TEXT;
    TEXT_BG = C_BG;
}

/* Replaces 0x080180C0. Actions 2 (up) and 4 (left) select the previous item,
 * 3 (down) and 5 (right) the next one, wrapping at both ends. */
void lpm_menu_nav(int action)
{
    int index = selected_index();
    if (action == 2 || action == 4)
        index = index > 0 ? index - 1 : ITEMS - 1;
    else if (action == 3 || action == 5)
        index = index < ITEMS - 1 ? index + 1 : 0;
    MENU_STATE[1] = (uint8_t)(FIRST_STATE + index);
    post_message(REDRAW_MENU, 0, 0);
}
