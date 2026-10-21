/* Vertical list selection with a scrolling window and minimal redraws. */
#ifndef LPM_MENU_H
#define LPM_MENU_H

#include "lpm_gfx.h"

typedef struct {
    uint8_t count;
    uint8_t visible; /* rows on screen */
    uint8_t selected;
    uint8_t top; /* first visible index */
    bool wrap;   /* false: stop at the first/last item */
} lpm_menu;

typedef struct {
    uint8_t previous;
    uint8_t current;
    bool moved;
    bool scrolled; /* window moved: every visible row needs redrawing */
} lpm_menu_change;

void lpm_menu_init(lpm_menu *menu, uint8_t count, uint8_t visible, bool wrap);
/* Changes the item count (e.g. hosts found), keeping the selection valid. */
void lpm_menu_set_count(lpm_menu *menu, uint8_t count);
void lpm_menu_select(lpm_menu *menu, uint8_t index);
lpm_menu_change lpm_menu_step(lpm_menu *menu, int delta);

/* Row background with each pixel written once: an inset highlight when
 * selected, otherwise plain background with a separator rule. Leaves the
 * scroll bar column alone. */
void lpm_row_background(const lpm_display *d, int y, int row_h, bool selected);

typedef void (*lpm_row_painter)(const lpm_display *d, void *ctx, uint8_t index, int y, bool selected);

/* Paints all visible rows, blank space below the last item and a scroll bar
 * at the right edge when the list is longer than the window. */
void lpm_list_draw(const lpm_display *d, const lpm_menu *menu, int y, int row_h, lpm_row_painter paint, void *ctx);
/* After a step: repaints only the two affected rows unless the window scrolled. */
void lpm_list_redraw(const lpm_display *d, const lpm_menu *menu, lpm_menu_change change, int y, int row_h,
                     lpm_row_painter paint, void *ctx);

#endif
