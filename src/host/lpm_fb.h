/* Host-only framebuffer implementation of lpm_display, for the simulator and
 * tests. The firmware port draws straight to the LCD instead. */
#ifndef LPM_FB_H
#define LPM_FB_H

#include "lpm_gfx.h"

typedef struct {
    lpm_color px[LPM_SCREEN_W * LPM_SCREEN_H];
    uint32_t calls;
    uint32_t pixels; /* pixels written since the last lpm_fb_reset_stats() */
    bool out_of_bounds;
    int min_y, max_y; /* rows touched since the last reset; min_y > max_y if none */
} lpm_fb;

lpm_display lpm_fb_display(lpm_fb *fb);
void lpm_fb_reset_stats(lpm_fb *fb);
/* RGB PNG, each panel pixel drawn as scale x scale. Returns 0 on success. */
int lpm_fb_write_png(const lpm_fb *fb, const char *path, int scale);
/* Writes several framebuffers side by side in rows of `columns`. */
int lpm_fb_write_sheet(const lpm_fb *const *fbs, int count, int columns, const char *path, int scale);

#endif
