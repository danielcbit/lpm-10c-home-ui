/*
 * Drawing helpers shared by the added screens; see draw.h.
 *
 * Vendor interface (docs/lpm-10c-bootloader-analysis.md):
 *   0x08018568      fill_rect(x0, y0, x1, y1, colour), inclusive corners
 *   0x080189E8      lcd_set_cursor(x, y); 0x08018FFC lcd_write_start()
 *   0x60020000      LCD data register; the cursor advances along x
 */
#include "draw.h"

#include "lpm_theme.h"
#include "subset_font.h"

#define LCD_DATA (*(volatile uint16_t *)0x60020000u)

#define vendor_fill_rect ((void (*)(uint16_t, uint16_t, uint16_t, uint16_t, uint16_t))0x08018569u)
#define lcd_set_cursor ((void (*)(uint16_t, uint16_t))0x080189E9u)
#define lcd_write_start ((void (*)(void))0x08018FFDu)

const sub_font font_body = {&lpm_sub_body[0][0], LPM_SUB_BODY_H, LPM_SUB_BODY_W, LPM_SUB_BODY_ADVANCE, LPM_SUB_BODY_DY};
const sub_font font_small = {&lpm_sub_small[0][0], LPM_SUB_SMALL_H, LPM_SUB_SMALL_W, LPM_SUB_SMALL_ADVANCE,
                             LPM_SUB_SMALL_DY};

void fill(int x, int y, int w, int h, uint16_t colour)
{
    vendor_fill_rect((uint16_t)x, (uint16_t)y, (uint16_t)(x + w - 1), (uint16_t)(y + h - 1), colour);
}

int text_width(const sub_font *f, const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    return n * f->advance;
}

/* One LCD window per row of the whole string, rather than per glyph, so the
 * cursor is set once per row and the pixels stream out. The background is
 * written too, which is what every caller here wants. */
void text(const sub_font *f, int x, int y, const char *s, uint16_t fg, uint16_t bg)
{
    y += f->dy;
    for (int row = 0; row < f->rows; row++) {
        lcd_set_cursor((uint16_t)x, (uint16_t)(y + row));
        lcd_write_start();
        for (const char *c = s; *c; c++) {
            unsigned code = (unsigned char)*c - LPM_SUB_FIRST;
            uint8_t slot = code < LPM_SUB_COUNT ? lpm_sub_index[code] : LPM_SUB_BLANK;
            uint8_t bits = slot == LPM_SUB_BLANK ? 0 : f->glyphs[slot * f->rows + row];
            for (int col = 0; col < f->advance; col++)
                LCD_DATA = (col < f->width && (bits & (0x80u >> col))) ? fg : bg;
        }
    }
}

void text_right(const sub_font *f, int right, int y, const char *s, uint16_t fg, uint16_t bg)
{
    text(f, right - text_width(f, s), y, s, fg, bg);
}

void text_field(const sub_font *f, int x, int y, int w, int right, const char *s, uint16_t fg, uint16_t bg)
{
    int gap = w - text_width(f, s);
    if (gap < 0)
        gap = 0;
    /* fill_rect takes inclusive corners, so a zero-width fill must not reach it. */
    if (right && gap)
        fill(x, y + f->dy, gap, f->rows, bg);
    text(f, right ? x + gap : x, y, s, fg, bg);
    if (!right && gap)
        fill(x + w - gap, y + f->dy, gap, f->rows, bg);
}

void draw_icon(const lpm_icon *icon, int x, int y, uint16_t fg, uint16_t bg)
{
    int stride = (icon->width + 7) / 8;
    for (int row = 0; row < icon->height; row++) {
        const uint8_t *line = icon->bits + row * stride;
        lcd_set_cursor((uint16_t)x, (uint16_t)(y + row));
        lcd_write_start();
        for (int col = 0; col < icon->width; col++)
            LCD_DATA = (line[col >> 3] & (0x80u >> (col & 7))) ? fg : bg;
    }
}

void big_number(int x, int y, int w, const char *s, uint16_t fg, uint16_t bg)
{
    const lpm_font *f = &lpm_font_digits;
    int stride = (f->width + 7) / 8, top = (f->line_height - f->height) / 2, width = 0;
    for (const char *c = s; *c; c++)
        if ((unsigned char)*c - f->first < f->count)
            width += f->advances[(unsigned char)*c - f->first];
    int gap = w > width ? w - width : 0;
    fill(x, y, w, top, bg);
    fill(x, y + top + f->height, w, f->line_height - top - f->height, bg);
    if (gap)
        fill(x, y + top, gap, f->height, bg);
    for (int row = 0; row < f->height; row++) {
        lcd_set_cursor((uint16_t)(x + gap), (uint16_t)(y + top + row));
        lcd_write_start();
        for (const char *c = s; *c; c++) {
            unsigned code = (unsigned char)*c - f->first;
            if (code >= f->count)
                continue;
            const uint8_t *line = f->bits + (code * f->height + row) * stride;
            for (int col = 0; col < f->advances[code]; col++)
                LCD_DATA = (col < f->width && (line[col >> 3] & (0x80u >> (col & 7)))) ? fg : bg;
        }
    }
}

void draw_header(const char *title, int status_x)
{
    fill(0, LPM_STATUS_Y, LPM_SCREEN_W, LPM_STATUS_H - 1, LPM_C_BG);
    text(&font_small, status_x, 4, "LPM-10C", LPM_C_DIM, LPM_C_BG);
    fill(0, LPM_STATUS_H - 1, LPM_SCREEN_W, 1, LPM_C_RULE);
    fill(0, LPM_TITLE_Y, LPM_SCREEN_W, LPM_TITLE_H - 1, LPM_C_BG);
    text(&font_body, 10, LPM_TITLE_Y + 10, title, LPM_C_FG, LPM_C_BG);
    fill(0, LPM_CONTENT_Y - 1, LPM_SCREEN_W, 1, LPM_C_RULE);
}

void draw_footer(const char *left, const char *right)
{
    fill(0, LPM_FOOTER_Y, LPM_SCREEN_W, 1, LPM_C_RULE);
    fill(0, LPM_FOOTER_Y + 1, LPM_SCREEN_W, LPM_FOOTER_H - 1, LPM_C_BG);
    text(&font_small, 10, LPM_FOOTER_Y + 8, left, LPM_C_DIM, LPM_C_BG);
    text_right(&font_small, LPM_SCREEN_W - 10, LPM_FOOTER_Y + 8, right, LPM_C_DIM, LPM_C_BG);
}
