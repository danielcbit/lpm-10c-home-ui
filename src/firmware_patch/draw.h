/*
 * Drawing helpers shared by the added screens (home_ui.c, link_status.c).
 *
 * Everything goes straight to the vendor LCD routines, so these may only run
 * in the vendor GUI task, which owns the display.
 */
#ifndef LPM_PATCH_DRAW_H
#define LPM_PATCH_DRAW_H

#include <stdint.h>

#include "lpm_assets.h"

/* Terminus 6x12 and 8x16, cut down to the characters these screens draw
 * (scripts/gen_subset_font.py). The vendor draw_string is not used. */
typedef struct {
    const uint8_t *glyphs; /* LPM_SUB_GLYPHS entries of `rows` bytes, one byte per row, MSB left */
    uint8_t rows;
    uint8_t width;
    uint8_t advance;
    int8_t dy; /* cell top relative to the y asked for, so every font shares the design baseline */
} sub_font;

extern const sub_font font_body;
extern const sub_font font_small;

void fill(int x, int y, int w, int h, uint16_t colour);
int text_width(const sub_font *f, const char *s);
void text(const sub_font *f, int x, int y, const char *s, uint16_t fg, uint16_t bg);
void text_right(const sub_font *f, int right, int y, const char *s, uint16_t fg, uint16_t bg);
/* Text in a field w wide, left or right aligned, with the rest of the field cleared
 * to bg. Each pixel is written once, so a changing value does not flicker. */
void text_field(const sub_font *f, int x, int y, int w, int right, const char *s, uint16_t fg, uint16_t bg);
void draw_icon(const lpm_icon *icon, int x, int y, uint16_t fg, uint16_t bg);

/* The status bar (left of the vendor battery icon), the title and their rules. The
 * status text starts at status_x, so a screen can keep the bar's left end free. */
void draw_header(const char *title, int status_x);
void draw_footer(const char *left, const char *right);

#endif
