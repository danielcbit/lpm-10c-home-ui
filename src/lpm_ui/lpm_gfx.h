/* Drawing primitives, text and allocation-free string formatting.
 *
 * Everything here is freestanding C99: no heap, no libc, no framebuffer. The
 * firmware supplies an lpm_display port that writes directly to the LCD. */
#ifndef LPM_GFX_H
#define LPM_GFX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LPM_SCREEN_W 240
#define LPM_SCREEN_H 320

typedef uint16_t lpm_color; /* RGB565, as used by the stock firmware */
#define LPM_RGB565(r, g, b) ((lpm_color)((((r) & 0xf8u) << 8) | (((g) & 0xfcu) << 3) | ((b) >> 3)))

/* Display port. Calls are clipped to the screen before reaching it, so an
 * implementation may assume 0 <= x, x + w <= LPM_SCREEN_W (and likewise y). */
typedef struct {
    void *ctx;
    void (*fill_rect)(void *ctx, int16_t x, int16_t y, int16_t w, int16_t h, lpm_color color);
    /* 1-bit bitmap, MSB first, `stride` bytes per row: set bits use fg, clear bits bg. */
    void (*draw_mono)(void *ctx, int16_t x, int16_t y, int16_t w, int16_t h, const uint8_t *bits, uint16_t stride,
                      lpm_color fg, lpm_color bg);
} lpm_display;

/* Monospace bitmap font unless `advances` gives a per-glyph advance. */
typedef struct {
    uint8_t first;
    uint8_t count;
    uint8_t width;
    uint8_t height;
    uint8_t advance;
    uint8_t line_height;
    const uint8_t *advances;
    const uint8_t *bits;
} lpm_font;

typedef struct {
    uint8_t width;
    uint8_t height;
    const uint8_t *bits;
} lpm_icon;

typedef enum { LPM_ALIGN_LEFT, LPM_ALIGN_CENTER, LPM_ALIGN_RIGHT } lpm_align;

void lpm_fill(const lpm_display *d, int x, int y, int w, int h, lpm_color color);
void lpm_hline(const lpm_display *d, int x, int y, int w, lpm_color color);
void lpm_vline(const lpm_display *d, int x, int y, int h, lpm_color color);
/* 1 px line; horizontal runs are merged into single fill_rect calls. */
void lpm_line(const lpm_display *d, int x0, int y0, int x1, int y1, lpm_color color);
void lpm_frame(const lpm_display *d, int x, int y, int w, int h, lpm_color color);
void lpm_draw_icon(const lpm_display *d, const lpm_icon *icon, int x, int y, lpm_color fg, lpm_color bg);

int lpm_text_width(const lpm_font *font, const char *text);
/* Draws one line with an opaque background; `y` is the top of the line box.
 * Glyphs that would not fit on screen are skipped. Returns the end x. */
int lpm_text(const lpm_display *d, const lpm_font *font, int x, int y, const char *text, lpm_color fg, lpm_color bg);
/* Clears the box [x, x + w) x line_height, then draws text aligned inside it,
 * dropping glyphs that would cross the right edge. */
void lpm_text_box(const lpm_display *d, const lpm_font *font, int x, int y, int w, lpm_align align, const char *text,
                  lpm_color fg, lpm_color bg);

/* Bounded string builder; the buffer is always NUL-terminated. */
typedef struct {
    char *data;
    size_t cap;
    size_t len;
} lpm_str;

void lpm_str_init(lpm_str *s, char *buf, size_t cap);
void lpm_str_add(lpm_str *s, const char *text);
void lpm_str_char(lpm_str *s, char c);
void lpm_str_u32(lpm_str *s, uint32_t value);
/* value / 10^decimals, e.g. (234, 1) -> "23.4", (-5, 1) -> "-0.5". */
void lpm_str_fixed(lpm_str *s, int32_t value, uint8_t decimals);
/* IPv4 held as a.b.c.d -> (a << 24) | (b << 16) | (c << 8) | d. */
void lpm_str_ipv4(lpm_str *s, uint32_t ip);
void lpm_str_mac(lpm_str *s, const uint8_t mac[6]);
/* Round-trip time: one decimal below 100 ms, whole milliseconds above. */
void lpm_str_ms(lpm_str *s, uint32_t microseconds);

#endif
