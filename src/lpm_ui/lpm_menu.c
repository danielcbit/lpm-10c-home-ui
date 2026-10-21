#include "lpm_menu.h"

#include "lpm_theme.h"

#define LPM_SCROLLBAR_X (LPM_SCREEN_W - 4)

static void keep_visible(lpm_menu *menu)
{
    if (menu->selected < menu->top) {
        menu->top = menu->selected;
    } else if (menu->selected >= menu->top + menu->visible) {
        menu->top = (uint8_t)(menu->selected - menu->visible + 1);
    }
    if (menu->count <= menu->visible) {
        menu->top = 0;
    } else if (menu->top > menu->count - menu->visible) {
        menu->top = (uint8_t)(menu->count - menu->visible);
    }
}

void lpm_menu_init(lpm_menu *menu, uint8_t count, uint8_t visible, bool wrap)
{
    menu->count = count;
    menu->visible = visible ? visible : 1;
    menu->selected = 0;
    menu->top = 0;
    menu->wrap = wrap;
}

void lpm_menu_set_count(lpm_menu *menu, uint8_t count)
{
    menu->count = count;
    if (menu->selected >= count) {
        menu->selected = count ? (uint8_t)(count - 1) : 0;
    }
    keep_visible(menu);
}

void lpm_menu_select(lpm_menu *menu, uint8_t index)
{
    if (index < menu->count) {
        menu->selected = index;
        keep_visible(menu);
    }
}

lpm_menu_change lpm_menu_step(lpm_menu *menu, int delta)
{
    lpm_menu_change change;
    uint8_t old_top = menu->top;
    int target = menu->selected + delta;

    change.previous = menu->selected;
    if (menu->count == 0) {
        target = 0;
    } else if (menu->wrap) {
        target %= menu->count;
        if (target < 0) {
            target += menu->count;
        }
    } else if (target < 0) {
        target = 0;
    } else if (target >= menu->count) {
        target = menu->count - 1;
    }
    menu->selected = (uint8_t)target;
    keep_visible(menu);
    change.current = menu->selected;
    change.moved = change.current != change.previous;
    change.scrolled = menu->top != old_top;
    return change;
}

void lpm_row_background(const lpm_display *d, int y, int row_h, bool selected)
{
    const int inset = 4;
    const int width = LPM_SCROLLBAR_X - 2 * inset;
    if (selected) {
        lpm_fill(d, 0, y, inset, row_h, LPM_C_BG);
        lpm_fill(d, inset + width, y, inset, row_h, LPM_C_BG);
        lpm_hline(d, inset, y, width, LPM_C_BG);
        lpm_hline(d, inset, y + row_h - 1, width, LPM_C_BG);
        lpm_fill(d, inset, y + 1, width, row_h - 2, LPM_C_ACCENT);
    } else {
        lpm_fill(d, 0, y, LPM_SCROLLBAR_X, row_h - 1, LPM_C_BG);
        lpm_hline(d, 0, y + row_h - 1, 2 * inset, LPM_C_BG);
        lpm_hline(d, 2 * inset, y + row_h - 1, width - 2 * inset, LPM_C_RULE);
        lpm_hline(d, width, y + row_h - 1, 2 * inset, LPM_C_BG);
    }
}

static void draw_scrollbar(const lpm_display *d, const lpm_menu *menu, int y, int row_h)
{
    int track = menu->visible * row_h;
    int thumb;
    int offset;
    if (menu->count <= menu->visible) {
        return;
    }
    thumb = track * menu->visible / menu->count;
    offset = track * menu->top / menu->count;
    lpm_fill(d, LPM_SCROLLBAR_X, y, 3, track, LPM_C_RULE);
    lpm_fill(d, LPM_SCROLLBAR_X, y + offset, 3, thumb, LPM_C_DIM);
}

void lpm_list_draw(const lpm_display *d, const lpm_menu *menu, int y, int row_h, lpm_row_painter paint, void *ctx)
{
    uint8_t row;
    for (row = 0; row < menu->visible; row++) {
        uint8_t index = (uint8_t)(menu->top + row);
        if (index < menu->count) {
            paint(d, ctx, index, y + row * row_h, index == menu->selected);
        } else {
            lpm_fill(d, 0, y + row * row_h, LPM_SCROLLBAR_X, row_h, LPM_C_BG);
        }
    }
    draw_scrollbar(d, menu, y, row_h);
}

void lpm_list_redraw(const lpm_display *d, const lpm_menu *menu, lpm_menu_change change, int y, int row_h,
                     lpm_row_painter paint, void *ctx)
{
    if (change.scrolled) {
        lpm_list_draw(d, menu, y, row_h, paint, ctx);
    } else if (change.moved) {
        paint(d, ctx, change.previous, y + (change.previous - menu->top) * row_h, false);
        paint(d, ctx, change.current, y + (change.current - menu->top) * row_h, true);
    }
}
