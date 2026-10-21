/* Palette and fixed layout for the 240 x 320 portrait working design. */
#ifndef LPM_THEME_H
#define LPM_THEME_H

#include "lpm_gfx.h"

#define LPM_C_BG LPM_RGB565(0x10, 0x18, 0x20)
#define LPM_C_FG LPM_RGB565(0xee, 0xf5, 0xfa)
#define LPM_C_DIM LPM_RGB565(0xa2, 0xb7, 0xc7)
#define LPM_C_RULE LPM_RGB565(0x2a, 0x3b, 0x48)
#define LPM_C_ACCENT LPM_RGB565(0x15, 0x98, 0xca) /* the only selection accent */
#define LPM_C_ON_ACCENT LPM_RGB565(0x07, 0x19, 0x21)
/* Semantic colours always accompany a word such as PASS, OPEN or FAIL. */
#define LPM_C_PASS LPM_RGB565(0x5c, 0xc8, 0x7e)
#define LPM_C_WARN LPM_RGB565(0xf2, 0xb3, 0x3d)
#define LPM_C_FAULT LPM_RGB565(0xff, 0x6b, 0x5e)

#define LPM_STATUS_Y 0
#define LPM_STATUS_H 20
#define LPM_TITLE_Y 20
#define LPM_TITLE_H 36
#define LPM_CONTENT_Y 56
#define LPM_CONTENT_H 236
#define LPM_FOOTER_Y 292
#define LPM_FOOTER_H 28

#define LPM_MARGIN 8
#define LPM_ROW_H 36
#define LPM_HOME_ROWS 6
#define LPM_HOME_LIST_Y (LPM_CONTENT_Y + 8)

#endif
