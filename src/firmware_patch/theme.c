/*
 * The home screen's theme for every vendor screen.
 *
 * Every vendor screen reaches the panel through five small routines. Their
 * entries are patched to branch here (scripts/build_patched_image.py,
 * THEME_ROUTINES), so the vendor screens keep their layout and logic but take
 * the theme's colours and the Terminus font:
 *   0x080182A4  clear(colour)                     -> lpm_theme_clear
 *   0x08018450  draw_point(x, y), colour at POINT -> lpm_theme_point
 *   0x08018568  fill_rect(x0, y0, x1, y1, colour) -> lpm_theme_fill_rect
 *   0x080188A4  plot(x, y, colour), at y + 40     -> lpm_theme_plot
 *   0x08018A34  draw_char(x, y, c, size, mode)    -> lpm_theme_char
 * Lines, frames and the vendor number and string drawers are built on these.
 * Pictures (0x080183EC) keep their own pixels.
 *
 * Colours are mapped by value: the theme's own colours pass unchanged (the added
 * screens draw through fill_rect too), each vendor colour in lpm_theme_map
 * becomes its theme colour, and the anti-aliased yellows and greys of the vendor
 * pictures are moved onto the accent and the background-to-foreground ramp.
 * Anything else passes unchanged.
 *
 * Vendor interface (docs/lpm-10c-bootloader-analysis.md, "Theme"):
 *   0x200001A0/A2   foreground (also draw_point's colour) / background
 *   0x080189E8      lcd_set_cursor(x, y); 0x08018FFC lcd_write_start()
 *   0x60020000      LCD data register; the cursor advances along x
 */
#include <stdint.h>

#include "lpm_theme.h"
#include "subset_font.h"

#define LCD_DATA (*(volatile uint16_t *)0x60020000u)
#define POINT_COLOUR (*(volatile uint16_t *)0x200001A0u)
#define BACK_COLOUR (*(volatile uint16_t *)0x200001A2u)

#define lcd_set_cursor ((void (*)(uint16_t, uint16_t))0x080189E9u)
#define lcd_write_start ((void (*)(void))0x08018FFDu)

#define SCREEN_W 240
#define SCREEN_H 320
#define PLOT_DY 40 /* the vendor plot routine draws 40 rows below the y it is given */

/* Vendor colour -> theme colour. Kept in step with docs/lpm-10c-bootloader-analysis.md
 * ("Theme"), which says where each vendor colour is used. */
const uint16_t lpm_theme_map[17][2] = {
    {0x0000, LPM_C_BG},        /* screen background */
    {0xFFFF, LPM_C_FG},        /* text */
    {0x31A7, LPM_C_FIELD},     /* input boxes, frame edges, the cable map body */
    {0x2105, LPM_C_PANEL},     /* the frame body around each tool */
    {0x4A69, LPM_C_RAISED},    /* label boxes, disabled buttons and their text */
    {0x7304, LPM_C_ACCENT_DIM}, /* selection fill */
    {0xFE60, LPM_C_ACCENT},    /* selection outline and text */
    {0x3AFD, LPM_C_ACCENT},    /* blue labels */
    {0x249E, LPM_C_ACCENT},    /* blue values */
    {0xFFE0, LPM_C_WARN},      /* bright yellow */
    {0xDB61, LPM_C_WARN},      /* orange */
    {0xC503, LPM_C_WARN},      /* gold */
    {0x07E0, LPM_C_PASS},      /* green: pass, the port arrow */
    {0xDFF3, LPM_C_PASS},      /* pale green */
    {0xF800, LPM_C_FAULT},     /* red: fault */
    {0xB140, LPM_C_FAULT},     /* dark red */
    {0x0927, LPM_C_PANEL},     /* dark blue panels */
};
#define THEME_MAP_SIZE (sizeof lpm_theme_map / sizeof lpm_theme_map[0])

/* The vendor's selection yellow, as drawn into its pictures with anti-aliasing:
 * red and green high, blue low. Such a pixel becomes the accent at the same
 * brightness, so the icons follow the selection colour. The orange of the
 * scan dot (green below 40) is kept. */
static int yellow(uint16_t c)
{
    unsigned r = c >> 11, g = (c >> 5) & 63, b = c & 31;
    return r >= 20 && g >= 40 && b <= 8;
}

static uint16_t scale(uint16_t c, unsigned level)
{
    unsigned r = (c >> 11) * level / 63, g = ((c >> 5) & 63) * level / 63, b = (c & 31) * level / 63;
    return (uint16_t)(r << 11 | g << 5 | b);
}

/* A grey from the vendor pictures (all three channels within a few steps of each
 * other) is placed on the theme's ramp from background to foreground at the same
 * brightness, so dark picture parts sit on the navy like they sat on black. */
static int neutral(uint16_t c, unsigned *level)
{
    unsigned r = (c >> 11) * 2, g = (c >> 5) & 63, b = (c & 31) * 2;
    unsigned hi = r > g ? (r > b ? r : b) : (g > b ? g : b), lo = r < g ? (r < b ? r : b) : (g < b ? g : b);
    *level = g;
    return hi - lo <= 4;
}

static uint16_t ramp(unsigned level)
{
    unsigned r0 = LPM_C_BG >> 11, g0 = (LPM_C_BG >> 5) & 63, b0 = LPM_C_BG & 31;
    unsigned r1 = LPM_C_FG >> 11, g1 = (LPM_C_FG >> 5) & 63, b1 = LPM_C_FG & 31;
    return (uint16_t)((r0 + (r1 - r0) * level / 63) << 11 | (g0 + (g1 - g0) * level / 63) << 5
                      | (b0 + (b1 - b0) * level / 63));
}

/* The theme's own colours pass unchanged, so a colour mapped twice stays put. */
static const uint16_t palette[] = {LPM_C_BG,    LPM_C_FG,     LPM_C_DIM,   LPM_C_RULE,       LPM_C_ACCENT,
                                   LPM_C_ON_ACCENT, LPM_C_PANEL, LPM_C_FIELD, LPM_C_RAISED,  LPM_C_ACCENT_DIM,
                                   LPM_C_PASS,  LPM_C_WARN,   LPM_C_FAULT};

uint16_t lpm_theme_colour(uint16_t colour)
{
    unsigned level;
    for (unsigned i = 0; i < sizeof palette / sizeof palette[0]; i++)
        if (palette[i] == colour)
            return colour;
    for (unsigned i = 0; i < THEME_MAP_SIZE; i++)
        if (lpm_theme_map[i][0] == colour)
            return lpm_theme_map[i][1];
    if (yellow(colour))
        return scale(LPM_C_ACCENT, (colour >> 5) & 63);
    if (neutral(colour, &level))
        return ramp(level);
    return colour;
}

static void span(int x, int y, int w, uint16_t colour)
{
    lcd_set_cursor((uint16_t)x, (uint16_t)y);
    lcd_write_start();
    while (w-- > 0)
        LCD_DATA = colour;
}

void lpm_theme_clear(uint16_t colour)
{
    colour = lpm_theme_colour(colour);
    for (int y = 0; y < SCREEN_H; y++)
        span(0, y, SCREEN_W, colour);
}

void lpm_theme_fill_rect(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t colour)
{
    colour = lpm_theme_colour(colour);
    for (int y = y0; y <= y1; y++)
        span(x0, y, x1 - x0 + 1, colour);
}

void lpm_theme_point(uint16_t x, uint16_t y)
{
    span(x, y, 1, lpm_theme_colour(POINT_COLOUR));
}

void lpm_theme_plot(uint16_t x, uint16_t y, uint16_t colour)
{
    span(x, y + PLOT_DY, 1, lpm_theme_colour(colour));
}

/* draw_char(x, y, c, size, mode): size 12 draws a 6x12 cell, anything else 8x16
 * (the vendor reads its 16-row table for any size but 12); mode 0 paints the
 * background, mode 1 only the glyph. Characters outside 0x20-0x7E draw nothing,
 * where the vendor read past its table. Clipped to the screen like the vendor's. */
void lpm_theme_char(uint16_t x, uint16_t y, uint8_t c, uint8_t size, uint8_t mode)
{
    int small = size == 12;
    int rows = small ? LPM_SUB_SMALL_H : LPM_SUB_BODY_H, width = small ? LPM_SUB_SMALL_W : LPM_SUB_BODY_W;
    unsigned code = (unsigned)c - LPM_SUB_FIRST;
    if (code >= LPM_SUB_COUNT || lpm_sub_index[code] == LPM_SUB_BLANK || x >= SCREEN_W)
        return;
    const uint8_t *glyph = small ? lpm_sub_small[lpm_sub_index[code]] : lpm_sub_body[lpm_sub_index[code]];
    uint16_t fg = lpm_theme_colour(POINT_COLOUR), bg = lpm_theme_colour(BACK_COLOUR);
    if (x + width > SCREEN_W)
        width = SCREEN_W - x;
    for (int row = 0; row < rows && y + row < SCREEN_H; row++) {
        uint8_t bits = glyph[row];
        if (mode == 0) {
            lcd_set_cursor(x, (uint16_t)(y + row));
            lcd_write_start();
            for (int col = 0; col < width; col++)
                LCD_DATA = (bits & (0x80u >> col)) ? fg : bg;
        } else {
            for (int col = 0; col < width; col++)
                if (bits & (0x80u >> col))
                    span(x + col, y + row, 1, fg);
        }
    }
}
