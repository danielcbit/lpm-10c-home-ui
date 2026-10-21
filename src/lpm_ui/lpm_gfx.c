#include "lpm_gfx.h"

static int clip_span(int *start, int *length, int limit)
{
    if (*start < 0) {
        *length += *start;
        *start = 0;
    }
    if (*start + *length > limit) {
        *length = limit - *start;
    }
    return *length > 0;
}

void lpm_fill(const lpm_display *d, int x, int y, int w, int h, lpm_color color)
{
    if (clip_span(&x, &w, LPM_SCREEN_W) && clip_span(&y, &h, LPM_SCREEN_H)) {
        d->fill_rect(d->ctx, (int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, color);
    }
}

void lpm_hline(const lpm_display *d, int x, int y, int w, lpm_color color)
{
    lpm_fill(d, x, y, w, 1, color);
}

void lpm_vline(const lpm_display *d, int x, int y, int h, lpm_color color)
{
    lpm_fill(d, x, y, 1, h, color);
}

void lpm_frame(const lpm_display *d, int x, int y, int w, int h, lpm_color color)
{
    lpm_hline(d, x, y, w, color);
    lpm_hline(d, x, y + h - 1, w, color);
    lpm_vline(d, x, y + 1, h - 2, color);
    lpm_vline(d, x + w - 1, y + 1, h - 2, color);
}

void lpm_line(const lpm_display *d, int x0, int y0, int x1, int y1, lpm_color color)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int run_x = x0;

    for (;;) {
        int at_end = x0 == x1 && y0 == y1;
        int e2 = 2 * err;
        int step_y = !at_end && e2 <= dx;
        if (at_end || step_y) {
            int left = run_x < x0 ? run_x : x0;
            int width = (run_x < x0 ? x0 - run_x : run_x - x0) + 1;
            lpm_hline(d, left, y0, width, color);
        }
        if (at_end) {
            return;
        }
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (step_y) {
            err += dx;
            y0 += sy;
            run_x = x0;
        }
    }
}

void lpm_draw_icon(const lpm_display *d, const lpm_icon *icon, int x, int y, lpm_color fg, lpm_color bg)
{
    if (x < 0 || y < 0 || x + icon->width > LPM_SCREEN_W || y + icon->height > LPM_SCREEN_H) {
        return;
    }
    d->draw_mono(d->ctx, (int16_t)x, (int16_t)y, icon->width, icon->height, icon->bits,
                 (uint16_t)((icon->width + 7) / 8), fg, bg);
}

static int glyph_advance(const lpm_font *font, unsigned char c)
{
    if (c < font->first || c >= font->first + font->count) {
        c = '?';
        if (c < font->first || c >= font->first + font->count) {
            return font->advance;
        }
    }
    return font->advances ? font->advances[c - font->first] : font->advance;
}

int lpm_text_width(const lpm_font *font, const char *text)
{
    int width = 0;
    while (*text) {
        width += glyph_advance(font, (unsigned char)*text++);
    }
    return width;
}

static int draw_text(const lpm_display *d, const lpm_font *font, int x, int y, const char *text, int limit_x,
                     lpm_color fg, lpm_color bg)
{
    int stride = (font->width + 7) / 8;
    int top = y + (font->line_height - font->height) / 2;

    while (*text) {
        unsigned char c = (unsigned char)*text++;
        int advance = glyph_advance(font, c);
        if (x + advance > limit_x) {
            break;
        }
        if (c < font->first || c >= font->first + font->count) {
            c = '?';
        }
        if (x >= 0 && y >= 0 && x + advance <= LPM_SCREEN_W && y + font->line_height <= LPM_SCREEN_H) {
            int index = c - font->first;
            int glyph_w = font->width < advance ? font->width : advance;
            lpm_fill(d, x, y, advance, top - y, bg);
            lpm_fill(d, x, top + font->height, advance, y + font->line_height - top - font->height, bg);
            lpm_fill(d, x + glyph_w, top, advance - glyph_w, font->height, bg);
            if (index >= 0 && index < font->count) {
                d->draw_mono(d->ctx, (int16_t)x, (int16_t)top, (int16_t)glyph_w, font->height,
                             font->bits + (size_t)index * stride * font->height, (uint16_t)stride, fg, bg);
            }
        }
        x += advance;
    }
    return x;
}

int lpm_text(const lpm_display *d, const lpm_font *font, int x, int y, const char *text, lpm_color fg, lpm_color bg)
{
    return draw_text(d, font, x, y, text, LPM_SCREEN_W, fg, bg);
}

void lpm_text_box(const lpm_display *d, const lpm_font *font, int x, int y, int w, lpm_align align, const char *text,
                  lpm_color fg, lpm_color bg)
{
    int width = lpm_text_width(font, text);
    int start = x;
    if (width < w && align == LPM_ALIGN_RIGHT) {
        start = x + w - width;
    } else if (width < w && align == LPM_ALIGN_CENTER) {
        start = x + (w - width) / 2;
    }
    lpm_fill(d, x, y, start - x, font->line_height, bg);
    start = draw_text(d, font, start, y, text, x + w, fg, bg);
    lpm_fill(d, start, y, x + w - start, font->line_height, bg);
}

void lpm_str_init(lpm_str *s, char *buf, size_t cap)
{
    s->data = buf;
    s->cap = cap;
    s->len = 0;
    if (cap) {
        buf[0] = '\0';
    }
}

void lpm_str_char(lpm_str *s, char c)
{
    if (s->len + 1 < s->cap) {
        s->data[s->len++] = c;
        s->data[s->len] = '\0';
    }
}

void lpm_str_add(lpm_str *s, const char *text)
{
    while (*text) {
        lpm_str_char(s, *text++);
    }
}

static void add_digits(lpm_str *s, uint32_t value, int min_digits)
{
    char digits[10];
    int n = 0;
    do {
        digits[n++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value || n < min_digits);
    while (n) {
        lpm_str_char(s, digits[--n]);
    }
}

void lpm_str_u32(lpm_str *s, uint32_t value)
{
    add_digits(s, value, 1);
}

void lpm_str_fixed(lpm_str *s, int32_t value, uint8_t decimals)
{
    uint32_t magnitude = value < 0 ? 0u - (uint32_t)value : (uint32_t)value;
    uint32_t scale = 1;
    uint8_t i;
    for (i = 0; i < decimals; i++) {
        scale *= 10u;
    }
    if (value < 0) {
        lpm_str_char(s, '-');
    }
    add_digits(s, magnitude / scale, 1);
    if (decimals) {
        lpm_str_char(s, '.');
        add_digits(s, magnitude % scale, decimals);
    }
}

void lpm_str_ipv4(lpm_str *s, uint32_t ip)
{
    int shift;
    for (shift = 24; shift >= 0; shift -= 8) {
        lpm_str_u32(s, (ip >> shift) & 0xffu);
        if (shift) {
            lpm_str_char(s, '.');
        }
    }
}

void lpm_str_mac(lpm_str *s, const uint8_t mac[6])
{
    static const char hex[] = "0123456789ABCDEF";
    int i;
    for (i = 0; i < 6; i++) {
        if (i) {
            lpm_str_char(s, ':');
        }
        lpm_str_char(s, hex[mac[i] >> 4]);
        lpm_str_char(s, hex[mac[i] & 15]);
    }
}

void lpm_str_ms(lpm_str *s, uint32_t microseconds)
{
    if (microseconds < 99950u) {
        lpm_str_fixed(s, (int32_t)((microseconds + 50u) / 100u), 1);
    } else {
        lpm_str_u32(s, microseconds / 1000u + (microseconds % 1000u >= 500u));
    }
}
