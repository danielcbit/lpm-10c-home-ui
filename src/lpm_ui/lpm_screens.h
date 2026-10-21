/* Screen renderers. Each element paints its own background box, so calling a
 * renderer again after new data redraws in place without clearing the screen
 * (no framebuffer is needed). */
#ifndef LPM_SCREENS_H
#define LPM_SCREENS_H

#include "lpm_data.h"
#include "lpm_gfx.h"
#include "lpm_menu.h"

void lpm_draw_status(const lpm_display *d, const lpm_status *status);
void lpm_draw_title(const lpm_display *d, const char *title, const char *right);
/* Repaints only the small right-hand title text, e.g. the list position. */
void lpm_draw_title_right(const lpm_display *d, const char *right);
void lpm_draw_footer(const lpm_display *d, const char *left, const char *right);
void lpm_draw_tabs(const lpm_display *d, const char *const *labels, uint8_t count, uint8_t active);
void lpm_clear_content(const lpm_display *d);

/* Large numeral with a unit in body text; the number is right-aligned in a
 * fixed box so decimal places stay aligned between updates. */
void lpm_draw_reading(const lpm_display *d, int x, int y, int number_w, const char *number, const char *unit,
                      lpm_color color);

void lpm_screen_ping(const lpm_display *d, const lpm_ping_log *log);
void lpm_screen_wiremap(const lpm_display *d, const lpm_wiremap *map);
void lpm_screen_length(const lpm_display *d, const lpm_length *length, bool feet);
void lpm_screen_poe(const lpm_display *d, const lpm_poe_log *log);
void lpm_screen_health(const lpm_display *d, const lpm_health *health);
void lpm_screen_dhcp(const lpm_display *d, const lpm_dhcp *dhcp);

/* Host list: scan header plus the list window held in `list`. */
#define LPM_SCAN_ROWS 5
#define LPM_SCAN_LIST_Y 88
void lpm_screen_scan(const lpm_display *d, const lpm_scan *scan, const lpm_menu *list);
void lpm_scan_paint_row(const lpm_display *d, void *scan, uint8_t index, int y, bool selected);

#endif
