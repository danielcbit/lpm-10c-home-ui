/*
 * English in place of the Chinese pictures in two vendor screens.
 *
 * Picture 0x15 is the reset warning's title, a warning sign and "注意"
 * (70 x 21 at 78, 126), drawn by 0x0800F412 from the warning 0x08011CC8.
 * That call is redirected here, and the title is drawn as text in the vendor
 * font, in the picture's orange. The warning sets its own colours again
 * straight after (0x08011D40), so changing them here leaks nowhere.
 *
 * Picture 0x16, the company name in About (136 x 10 at 120, 211), has no
 * replacement: its two calls are simply removed (build_patched_image.py).
 *
 *   0x200001A0/A2   text foreground / background colour
 *   0x08018D48      draw_string(x, y, w, h, size, text), size 12 (6 px) or 16 (8 px)
 */
#include <stdint.h>

#define TEXT_FG (*(volatile uint16_t *)0x200001A0u)
#define TEXT_BG (*(volatile uint16_t *)0x200001A2u)

#define vendor_draw_string ((void (*)(int, int, int, int, int, const char *))0x08018D49u)

#define WARNING_ORANGE 0xFCC0 /* the picture's own colour */
#define TITLE "Warning"
#define TITLE_W (7 * 8)

void lpm_reset_warning_title(void)
{
    TEXT_FG = WARNING_ORANGE;
    TEXT_BG = 0x0000; /* the dialog's text background, set again after this call */
    vendor_draw_string(120 - TITLE_W / 2, 128, TITLE_W, 16, 16, TITLE);
}
