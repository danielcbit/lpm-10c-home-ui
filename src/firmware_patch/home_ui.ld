/* Places the added code (home screen, Link status). The whole update file must
 * stay within the vendor's 434,176 bytes, so code after the vendor payload must
 * end by flash 0x08073000 (2,996 bytes after 0x0807244C). Nothing here uses RAM:
 * .data and .bss are refused. */
MEMORY
{
    FLASH (rx)    : ORIGIN = 0x0807244C, LENGTH = 0x08073000 - 0x0807244C
    /* Vendor bytes the patches strand, confirmed unreachable by
       scripts/check_dead_code.py and listed in build_patched_image.DEAD_RANGES.
       DEAD1 and DEAD2 are the replaced grid-menu routines, BLIT the body of the
       CJK glyph blitter (its entry 0x08018DB0 is patched to return), CJK its
       glyph table. All four are free at present. */
    DEAD1 (r)     : ORIGIN = 0x0800F648, LENGTH = 0x0800F808 - 0x0800F648
    DEAD2 (r)     : ORIGIN = 0x080180C0, LENGTH = 0x080181AA - 0x080180C0
    BLIT (r)      : ORIGIN = 0x08018DB4, LENGTH = 0x08018F0C - 0x08018DB4
    CJK (rx)      : ORIGIN = 0x080708F8, LENGTH = 0x08071E58 - 0x080708F8
    /* Pictures 9, 5, 22 and 23 (0x080584F9-0x0806E2A7), which the picture getter
       now refuses, kept clear of picture 11 after them and aligned. Pictures 4,
       0 and 2, 8 and 3, 21, 7 and 6, and 1 are freed too and not used yet. */
    PICTURES (rx) : ORIGIN = 0x080584FC, LENGTH = 0x0806E2A4 - 0x080584FC
    /* Four more of the freed pictures (DEAD_RANGES), for Discovery's MAC prefix table. */
    OUI1 (r)      : ORIGIN = 0x08026B34, LENGTH = 0x0802A8FC - 0x08026B34
    OUI2 (r)      : ORIGIN = 0x0802C4AC, LENGTH = 0x0802EAD0 - 0x0802C4AC
    OUI3 (r)      : ORIGIN = 0x0802FA0C, LENGTH = 0x08031EBD - 0x0802FA0C
    OUI4 (r)      : ORIGIN = 0x080323D4, LENGTH = 0x08034359 - 0x080323D4
    RAM (rw)      : ORIGIN = 0x20020000, LENGTH = 0
}

SECTIONS
{
    /* After the payload: the routines the vendor calls into, small and first.
       lpm_home_draw must start the section (build_patched_image.py checks it). */
    .text : {
        *(.text.lpm_home_draw)
        *(.text.lpm_home_key)
        *home_ui.o(.text*)
        *draw.o(.text*)
        *cust_info.o(.text*)
        *vendor_text.o(.text*)
    } > FLASH

    /* Discovery's MAC prefix table, in chunks sized for these (scripts/gen_oui.py). */
    .oui1 : { *(.rodata.lpm_oui_1) } > OUI1
    .oui2 : { *(.rodata.lpm_oui_2) } > OUI2
    .oui3 : { *(.rodata.lpm_oui_3) } > OUI3
    .oui4 : { *(.rodata.lpm_oui_4) } > OUI4

    /* Everything else: Link status, fonts, icons, strings and tables. Any flash
       address is in BL range of any other. */
    .pictures : {
        *(.text*)
        *(.rodata*)
    } > PICTURES

    /* Nothing copies or clears RAM for this code, so initialised and zeroed data are refused. */
    .data : { *(.data*) } > RAM
    .bss : { *(.bss*) *(COMMON) } > RAM

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) *(.ARM.attributes) }
}

ASSERT(SIZEOF(.data) == 0, "home_ui: initialised data is not supported")
ASSERT(SIZEOF(.bss) == 0, "home_ui: zero-initialised data is not supported")
