/*
 * Skip the settings write when nothing changed, to spare the flash page.
 *
 * The vendor stores Cust_Info at every power-off, including Auto Off: the routine
 * at 0x080104B8 erases page 0x0807F800 and programs 204 bytes, with no check for
 * whether the record differs from what is already there. The page therefore takes
 * one erase cycle per power-off (docs/lpm-10c-bootloader-analysis.md, "Flash wear").
 *
 * scripts/build_patched_image.py redirects all eight calls to that routine here.
 * The comparison is the vendor's own definition of a good write: after programming,
 * 0x08010560 verifies the first 0xC8 bytes with the same memcmp, and treats equal
 * as success. If they are already equal, erasing and reprogramming cannot change
 * the stored bytes, so the write is skipped.
 *
 * The vendor logs "APP_Home_Cust_Info_Storage" inside the routine, so a power-off
 * with no change now prints no storage line at all. That absence is the evidence
 * the skip worked.
 */
#include <stddef.h>
#include <stdint.h>

#define CUST_INFO_RAM ((const void *)0x20002CF0u)
#define CUST_INFO_FLASH ((const void *)0x0807F800u)
#define CUST_INFO_VERIFIED 0xC8 /* bytes the vendor compares after programming */

#define vendor_memcmp ((int (*)(const void *, const void *, uint32_t))0x0800A6A1u)
#define vendor_cust_info_store ((void (*)(void))0x080104B9u)

void lpm_cust_info_store_if_changed(void)
{
    if (vendor_memcmp(CUST_INFO_RAM, CUST_INFO_FLASH, CUST_INFO_VERIFIED) == 0)
        return;
    vendor_cust_info_store();
}

/*
 * Keep the language English, because this build has no Chinese glyphs.
 *
 * Cust_Info +0xA5 is the language: 1 English, 2 Chinese. With 2, nearly every
 * vendor screen draws its labels as 16x16 glyphs from the table at 0x080708F8.
 * That table is reused for added data and its blitter 0x08018DB0 returns at once,
 * so in Chinese those labels would be blank. Chinese is also the vendor default:
 * 0x0801B340 fills a fresh record with 2, and the boot load 0x08010288 forces 2
 * for any value it does not recognise.
 *
 * So the boot load and the factory reset go through wrappers that set English
 * after the vendor code runs, the two "switch to Chinese" stores (0x0801142A,
 * 0x080117D2) are patched to store English, and the home screen checks again
 * on every draw in case some path was missed. A record that held 2 then
 * differs from flash, so the next power-off stores English once.
 */
#define CUST_INFO_LANGUAGE (((volatile uint8_t *)0x20002CF0u)[0xA5])
#define LANGUAGE_ENGLISH 1

#define vendor_cust_info_load ((int (*)(void))0x08010289u)
#define vendor_cust_info_defaults ((void (*)(void))0x0801B341u)

void lpm_cust_info_pin_english(void)
{
    if (CUST_INFO_LANGUAGE != LANGUAGE_ENGLISH)
        CUST_INFO_LANGUAGE = LANGUAGE_ENGLISH;
}

/* Replaces the two boot calls 0x0800FE78 and 0x0800FE82, which retry while it returns 0. */
int lpm_cust_info_load_english(void)
{
    int loaded = vendor_cust_info_load();
    lpm_cust_info_pin_english();
    return loaded;
}

/* Replaces the factory-reset call 0x080106F4. */
void lpm_cust_info_defaults_english(void)
{
    vendor_cust_info_defaults();
    lpm_cust_info_pin_english();
}
