/* Host tests for the LPM-10C replacement UI. No tester hardware required.
 *
 * Text assertions use a recording display that maps each drawn glyph bitmap
 * back to its character, so tests check what words reach the screen. */
#include <stdio.h>
#include <string.h>

#include "lpm_app.h"
#include "lpm_assets.h"
#include "lpm_demo.h"
#include "lpm_fb.h"
#include "lpm_screens.h"
#include "lpm_theme.h"

static int failures;
static int checks;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(cond)) {                                                                                                 \
            failures++;                                                                                                \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                   \
        }                                                                                                              \
    } while (0)

#define RUN(test)                                                                                                      \
    do {                                                                                                               \
        int before = failures;                                                                                         \
        test();                                                                                                        \
        printf("%s %s\n", failures == before ? "ok  " : "FAIL", #test);                                                \
    } while (0)

/* ---- Recording display -------------------------------------------------- */

typedef struct {
    char text[4096];
    size_t len;
    int next_x, y;
    bool out_of_bounds;
} recorder;

static void rec_fill(void *ctx, int16_t x, int16_t y, int16_t w, int16_t h, lpm_color color)
{
    recorder *r = ctx;
    (void)color;
    if (x < 0 || y < 0 || x + w > LPM_SCREEN_W || y + h > LPM_SCREEN_H) {
        r->out_of_bounds = true;
    }
}

static bool glyph_of(const lpm_font *font, const uint8_t *bits, char *out, int *advance)
{
    size_t size = (size_t)(font->width + 7) / 8 * font->height;
    size_t index;
    if (bits < font->bits || bits >= font->bits + size * font->count) {
        return false;
    }
    index = (size_t)(bits - font->bits) / size;
    *out = (char)(font->first + index);
    *advance = font->advances ? font->advances[index] : font->advance;
    return true;
}

static void rec_mono(void *ctx, int16_t x, int16_t y, int16_t w, int16_t h, const uint8_t *bits, uint16_t stride,
                     lpm_color fg, lpm_color bg)
{
    recorder *r = ctx;
    char c;
    int advance;
    (void)stride;
    (void)fg;
    (void)bg;
    rec_fill(ctx, x, y, w, h, 0);
    if (!glyph_of(&lpm_font_small, bits, &c, &advance) && !glyph_of(&lpm_font_body, bits, &c, &advance) &&
        !glyph_of(&lpm_font_digits, bits, &c, &advance)) {
        return;
    }
    if ((x != r->next_x || y != r->y) && r->len + 1 < sizeof r->text) {
        r->text[r->len++] = '|';
    }
    if (r->len + 1 < sizeof r->text) {
        r->text[r->len++] = c;
        r->text[r->len] = '\0';
    }
    r->next_x = x + advance;
    r->y = y;
}

static lpm_display recording(recorder *r)
{
    lpm_display d;
    memset(r, 0, sizeof *r);
    d.ctx = r;
    d.fill_rect = rec_fill;
    d.draw_mono = rec_mono;
    return d;
}

/* Words are drawn glyph by glyph, so spaces inside one call stay intact. */
static bool shows(const recorder *r, const char *text)
{
    return strstr(r->text, text) != NULL;
}

/* ---- Helpers ------------------------------------------------------------ */

static lpm_key_event key(lpm_key k)
{
    lpm_key_event e = {k, 0};
    return e;
}

static lpm_key_event held(lpm_key k)
{
    lpm_key_event e = {k, 3};
    return e;
}

static lpm_fb fb;
static lpm_data data;

/* ---- Menu and navigation ------------------------------------------------ */

static void test_menu_stops_at_boundaries_by_default(void)
{
    lpm_menu m;
    lpm_menu_change c;
    lpm_menu_init(&m, 9, 6, false);
    c = lpm_menu_step(&m, -1);
    CHECK(m.selected == 0 && !c.moved);
    lpm_menu_select(&m, 8);
    c = lpm_menu_step(&m, 1);
    CHECK(m.selected == 8 && !c.moved);
    CHECK(m.top == 3);
}

static void test_menu_scrolls_window_to_keep_selection_visible(void)
{
    lpm_menu m;
    lpm_menu_change c;
    int i;
    lpm_menu_init(&m, 9, 6, false);
    for (i = 0; i < 5; i++) {
        c = lpm_menu_step(&m, 1);
        CHECK(!c.scrolled);
    }
    c = lpm_menu_step(&m, 1);
    CHECK(m.selected == 6 && m.top == 1 && c.scrolled);
    lpm_menu_set_count(&m, 3);
    CHECK(m.selected == 2 && m.top == 0);
    lpm_menu_set_count(&m, 0);
    CHECK(m.selected == 0 && m.top == 0);
    c = lpm_menu_step(&m, 1);
    CHECK(!c.moved);
}

static void test_wrap_applies_to_fresh_presses_only(void)
{
    lpm_app app;
    lpm_demo_base(&data);
    data.status.menu_wrap = true;
    lpm_app_init(&app, &data);
    lpm_app_key(&app, held(LPM_KEY_UP));
    CHECK(app.home.selected == 0);
    lpm_app_key(&app, key(LPM_KEY_UP));
    CHECK(app.home.selected == LPM_ITEM_COUNT - 1);
    lpm_app_key(&app, held(LPM_KEY_DOWN));
    CHECK(app.home.selected == LPM_ITEM_COUNT - 1);
    lpm_app_key(&app, key(LPM_KEY_DOWN));
    CHECK(app.home.selected == 0);
}

static void test_home_has_proposed_order_and_native_items(void)
{
    static const char *const expected[] = {"Cable map",   "Cable length", "Cable trace", "Port flash", "PoE voltage",
                                           "Link status", "Ping monitor", "IP scan",     "Settings"};
    int i;
    for (i = 0; i < LPM_ITEM_COUNT; i++) {
        CHECK(strcmp(lpm_item_get((lpm_item)i)->name, expected[i]) == 0);
        CHECK(lpm_item_get((lpm_item)i)->icon->width == 24 && lpm_item_get((lpm_item)i)->icon->height == 24);
    }
    CHECK(lpm_item_get(LPM_ITEM_TRACE)->native && lpm_item_get(LPM_ITEM_PORT_FLASH)->native &&
          lpm_item_get(LPM_ITEM_SETTINGS)->native && !lpm_item_get(LPM_ITEM_PING)->native);
}

static void test_selection_move_redraws_two_rows_and_title_only(void)
{
    lpm_app app;
    lpm_display d = lpm_fb_display(&fb);
    lpm_demo_base(&data);
    lpm_app_init(&app, &data);
    lpm_app_render(&app, &d);
    lpm_fb_reset_stats(&fb);
    lpm_app_key(&app, key(LPM_KEY_DOWN));
    lpm_app_render(&app, &d);
    CHECK(fb.min_y >= LPM_TITLE_Y);
    CHECK(fb.max_y < LPM_HOME_LIST_Y + 2 * LPM_ROW_H);
    /* Two rows, plus label/icon overdraw (~4.5k) and the "n/9" counter: under 30% of a frame. */
    CHECK(fb.pixels < 2 * LPM_ROW_H * LPM_SCREEN_W + 6000);
    printf("     selection move: %u pixels written, %u port calls\n", (unsigned)fb.pixels, (unsigned)fb.calls);
    CHECK(fb.px[(LPM_HOME_LIST_Y + LPM_ROW_H + 18) * LPM_SCREEN_W + 6] == LPM_C_ACCENT);
    CHECK(fb.px[(LPM_HOME_LIST_Y + 18) * LPM_SCREEN_W + 6] == LPM_C_BG);
    CHECK(!fb.out_of_bounds);
}

static void test_two_moves_before_render_leave_one_highlight(void)
{
    lpm_app app;
    lpm_display d = lpm_fb_display(&fb);
    int row, highlighted = 0;
    lpm_demo_base(&data);
    lpm_app_init(&app, &data);
    lpm_app_render(&app, &d);
    lpm_app_key(&app, key(LPM_KEY_DOWN));
    lpm_app_key(&app, key(LPM_KEY_DOWN));
    lpm_app_render(&app, &d);
    for (row = 0; row < LPM_HOME_ROWS; row++) {
        highlighted += fb.px[(LPM_HOME_LIST_Y + row * LPM_ROW_H + 18) * LPM_SCREEN_W + 6] == LPM_C_ACCENT;
    }
    CHECK(highlighted == 1);
}

static void test_selection_survives_tool_and_native_screens(void)
{
    lpm_app app;
    lpm_action a;
    lpm_demo_base(&data);
    lpm_app_init(&app, &data);
    lpm_menu_select(&app.home, LPM_ITEM_TRACE);
    a = lpm_app_key(&app, key(LPM_KEY_ENTER));
    CHECK(a.kind == LPM_ACTION_OPEN_NATIVE && a.item == LPM_ITEM_TRACE);
    lpm_app_resume(&app);
    CHECK(app.screen == LPM_HOME && app.home.selected == LPM_ITEM_TRACE);

    lpm_menu_select(&app.home, LPM_ITEM_POE);
    a = lpm_app_key(&app, key(LPM_KEY_ENTER));
    CHECK(a.kind == LPM_ACTION_START && a.item == LPM_ITEM_POE && app.screen == LPM_ITEM_POE);
    a = lpm_app_key(&app, key(LPM_KEY_ENTER));
    CHECK(a.kind == LPM_ACTION_START && app.screen == LPM_ITEM_POE);
    a = lpm_app_key(&app, key(LPM_KEY_BACK));
    CHECK(a.kind == LPM_ACTION_STOP && a.item == LPM_ITEM_POE);
    CHECK(app.screen == LPM_HOME && app.home.selected == LPM_ITEM_POE);
}

static void test_back_on_home_is_left_to_stock_firmware(void)
{
    lpm_app app;
    lpm_demo_base(&data);
    lpm_app_init(&app, &data);
    CHECK(lpm_app_key(&app, key(LPM_KEY_BACK)).kind == LPM_ACTION_UNHANDLED);
    CHECK(lpm_app_key(&app, key(LPM_KEY_LEFT)).kind == LPM_ACTION_NONE);
}

static void test_scan_cancel_then_ping_host_and_back_to_list(void)
{
    lpm_app app;
    lpm_action a;
    lpm_demo_base(&data);
    lpm_demo_scan(&data.scan, true);
    lpm_app_init(&app, &data);
    lpm_menu_select(&app.home, LPM_ITEM_IP_SCAN);
    a = lpm_app_key(&app, key(LPM_KEY_ENTER));
    CHECK(a.kind == LPM_ACTION_START && a.item == LPM_ITEM_IP_SCAN);
    a = lpm_app_key(&app, key(LPM_KEY_BACK));
    CHECK(a.kind == LPM_ACTION_CANCEL_SCAN && app.screen == LPM_ITEM_IP_SCAN);
    data.scan.running = false;
    data.scan.cancelled = true;
    lpm_app_key(&app, key(LPM_KEY_DOWN));
    lpm_app_key(&app, key(LPM_KEY_DOWN));
    a = lpm_app_key(&app, key(LPM_KEY_ENTER));
    CHECK(a.kind == LPM_ACTION_PING_HOST && a.ip == data.scan.hosts[2].ip && app.screen == LPM_ITEM_PING);
    a = lpm_app_key(&app, key(LPM_KEY_BACK));
    CHECK(a.kind == LPM_ACTION_STOP && a.item == LPM_ITEM_PING && app.screen == LPM_ITEM_IP_SCAN);
    CHECK(app.hosts.selected == 2);
    a = lpm_app_key(&app, key(LPM_KEY_BACK));
    CHECK(a.kind == LPM_ACTION_STOP && app.screen == LPM_HOME && app.home.selected == LPM_ITEM_IP_SCAN);
}

static void test_link_tabs_switch_and_start_carries_tab(void)
{
    lpm_app app;
    lpm_action a;
    lpm_demo_base(&data);
    lpm_app_init(&app, &data);
    lpm_menu_select(&app.home, LPM_ITEM_LINK);
    a = lpm_app_key(&app, key(LPM_KEY_ENTER));
    CHECK(a.kind == LPM_ACTION_START && a.tab == 0);
    lpm_app_key(&app, key(LPM_KEY_RIGHT));
    lpm_app_key(&app, held(LPM_KEY_RIGHT));
    CHECK(app.link_tab == 1);
    a = lpm_app_key(&app, key(LPM_KEY_ENTER));
    CHECK(a.kind == LPM_ACTION_START && a.item == LPM_ITEM_LINK && a.tab == 1);
}

/* ---- Input -------------------------------------------------------------- */

static void test_only_direction_keys_repeat_with_acceleration(void)
{
    lpm_repeater r;
    lpm_key_event e;
    uint32_t t = 0xfffffe00u; /* exercises counter wrap-around */
    uint32_t last = t, gap = 0;
    int repeats = 0;
    lpm_repeater_init(&r);
    e = lpm_repeater_press(&r, LPM_KEY_ENTER, t);
    CHECK(e.key == LPM_KEY_ENTER && e.repeat == 0);
    CHECK(!lpm_repeater_poll(&r, t + 5000u, &e));
    lpm_repeater_press(&r, LPM_KEY_BACK, t);
    CHECK(!lpm_repeater_poll(&r, t + 5000u, &e));

    lpm_repeater_press(&r, LPM_KEY_DOWN, t);
    CHECK(!lpm_repeater_poll(&r, t + LPM_REPEAT_DELAY_MS - 1, &e));
    for (; repeats < 30; t += 10) {
        if (lpm_repeater_poll(&r, t, &e)) {
            CHECK(e.key == LPM_KEY_DOWN && e.repeat == repeats + 1);
            gap = t - last;
            last = t;
            repeats++;
        }
    }
    CHECK(gap == LPM_REPEAT_FASTEST_MS);
    lpm_repeater_release(&r);
    CHECK(!lpm_repeater_poll(&r, t + 1000u, &e));
}

/* ---- Data --------------------------------------------------------------- */

static void test_ping_statistics_and_timeouts(void)
{
    lpm_ping_log log;
    lpm_ping_reset(&log, LPM_IP(10, 0, 0, 1));
    CHECK(lpm_ping_add_reply(&log, 1, 1000));
    CHECK(lpm_ping_add_reply(&log, 2, 3000));
    CHECK(!lpm_ping_add_reply(&log, 2, 3000)); /* duplicate */
    CHECK(lpm_ping_add_timeout(&log, 3));
    CHECK(lpm_ping_add_reply(&log, 4, 8000)); /* no variation across the timeout */
    CHECK(lpm_ping_add_reply(&log, 5, 6000));
    CHECK(log.sent == 5 && log.received == 4 && lpm_ping_lost(&log) == 1);
    CHECK(lpm_ping_loss_permille(&log) == 200);
    CHECK(log.min_us == 1000 && log.max_us == 8000);
    CHECK(lpm_ping_mean_us(&log) == 4500);
    CHECK(log.variation_count == 2 && lpm_ping_variation_us(&log) == 2000);
    CHECK(lpm_ping_at(&log, 2)->status == LPM_PING_TIMEOUT);
}

static void test_ping_ring_keeps_newest_window(void)
{
    lpm_ping_log log;
    uint16_t seq;
    lpm_ping_reset(&log, 0);
    for (seq = 1; seq <= 300; seq++) {
        lpm_ping_add_reply(&log, seq, seq);
    }
    CHECK(log.count == LPM_PING_HISTORY && log.sent == 300);
    CHECK(lpm_ping_at(&log, 0)->seq == 300 - LPM_PING_HISTORY + 1);
    CHECK(lpm_ping_at(&log, LPM_PING_HISTORY - 1)->seq == 300);
    CHECK(log.min_us == 1);
}

static void test_memory_budget(void)
{
    CHECK(sizeof(lpm_ping_sample) == 8);
    CHECK(sizeof(((lpm_ping_log *)0)->samples) == 960);
    CHECK(sizeof(lpm_ping_log) <= 1024);
    CHECK(sizeof(lpm_data) <= 2560);
    CHECK(sizeof(lpm_app) <= 64);
    printf("     lpm_data %u bytes, lpm_app %u bytes\n", (unsigned)sizeof(lpm_data), (unsigned)sizeof(lpm_app));
}

static void test_wiremap_classification(void)
{
    lpm_wiremap map;
    lpm_wiremap_summary s;
    memset(&map, 0, sizeof map);
    CHECK(lpm_wiremap_summarize(&map).wiring == LPM_WIRING_NO_RESULT);
    lpm_demo_wiremap_straight(&map);
    CHECK(lpm_wiremap_summarize(&map).wiring == LPM_WIRING_STRAIGHT);
    lpm_demo_wiremap_crossover(&map);
    CHECK(lpm_wiremap_summarize(&map).wiring == LPM_WIRING_CROSSOVER);
    map.remote[3] = 7;
    map.remote[6] = 4;
    map.remote[4] = 8;
    map.remote[7] = 5;
    CHECK(lpm_wiremap_summarize(&map).wiring == LPM_WIRING_CROSSOVER);
    map.remote[7] = 8; /* partial gigabit crossover is a miswire */
    CHECK(lpm_wiremap_summarize(&map).wiring == LPM_WIRING_FAULT);
    lpm_demo_wiremap_fault(&map);
    s = lpm_wiremap_summarize(&map);
    CHECK(s.wiring == LPM_WIRING_FAULT && s.open == 1 && s.shorted == 2 && s.crossed == 2);
    CHECK(lpm_wiremap_pin(&map, 3) == LPM_PIN_OPEN && lpm_wiremap_pin(&map, 6) == LPM_PIN_SHORT);
}

static void test_scan_keeps_order_flags_duplicates_and_overflow(void)
{
    lpm_scan scan;
    uint8_t mac_a[6] = {1, 2, 3, 4, 5, 6};
    uint8_t mac_b[6] = {1, 2, 3, 4, 5, 7};
    int i;
    lpm_scan_reset(&scan, LPM_IP(10, 0, 0, 1), LPM_IP(10, 0, 0, 254), LPM_IP(10, 0, 0, 1));
    CHECK(scan.total == 254 && scan.running);
    lpm_scan_add(&scan, LPM_IP(10, 0, 0, 9), mac_a);
    lpm_scan_add(&scan, LPM_IP(10, 0, 0, 1), mac_a);
    lpm_scan_add(&scan, LPM_IP(10, 0, 0, 9), mac_a); /* repeat reply */
    CHECK(scan.count == 2 && scan.hosts[0].ip == LPM_IP(10, 0, 0, 1));
    CHECK(scan.hosts[0].flags == LPM_HOST_GATEWAY && scan.hosts[1].flags == 0);
    lpm_scan_add(&scan, LPM_IP(10, 0, 0, 9), mac_b);
    CHECK(scan.count == 3 && (scan.hosts[1].flags & LPM_HOST_DUPLICATE) && (scan.hosts[2].flags & LPM_HOST_DUPLICATE));
    for (i = 0; i < 100; i++) {
        lpm_scan_add(&scan, LPM_IP(10, 0, 1, i), mac_a);
    }
    CHECK(scan.count == LPM_SCAN_MAX_HOSTS && scan.overflow);
    for (i = 1; i < scan.count; i++) {
        CHECK(scan.hosts[i - 1].ip <= scan.hosts[i].ip);
    }
}

/* ---- Formatting and drawing --------------------------------------------- */

static void test_formatting(void)
{
    char buf[32];
    char tiny[5];
    lpm_str s;
    static const uint8_t mac[6] = {0x00, 0x1a, 0xff, 0x09, 0xa0, 0x5c};
    lpm_str_init(&s, buf, sizeof buf);
    lpm_str_fixed(&s, 234, 1);
    lpm_str_char(&s, ' ');
    lpm_str_fixed(&s, -5, 2);
    lpm_str_char(&s, ' ');
    lpm_str_fixed(&s, 7, 0);
    CHECK(strcmp(buf, "23.4 -0.05 7") == 0);
    lpm_str_init(&s, buf, sizeof buf);
    lpm_str_ms(&s, 3149);
    lpm_str_char(&s, ' ');
    lpm_str_ms(&s, 99949);
    lpm_str_char(&s, ' ');
    lpm_str_ms(&s, 99950);
    lpm_str_char(&s, ' ');
    lpm_str_ms(&s, 4294967295u);
    CHECK(strcmp(buf, "3.1 99.9 100 4294967") == 0);
    lpm_str_init(&s, buf, sizeof buf);
    lpm_str_ipv4(&s, LPM_IP(192, 168, 0, 254));
    CHECK(strcmp(buf, "192.168.0.254") == 0);
    lpm_str_init(&s, buf, sizeof buf);
    lpm_str_mac(&s, mac);
    CHECK(strcmp(buf, "00:1A:FF:09:A0:5C") == 0);
    lpm_str_init(&s, tiny, sizeof tiny);
    lpm_str_add(&s, "truncated");
    CHECK(strcmp(tiny, "trun") == 0 && s.len == 4);
}

static void test_line_and_clipping(void)
{
    lpm_display d = lpm_fb_display(&fb);
    int x, count = 0;
    lpm_fill(&d, 0, 0, LPM_SCREEN_W, LPM_SCREEN_H, 0);
    lpm_line(&d, 10, 10, 30, 17, 0xffff);
    for (x = 0; x < LPM_SCREEN_W * LPM_SCREEN_H; x++) {
        count += fb.px[x] == 0xffff;
    }
    CHECK(count == 21);
    CHECK(fb.px[10 * LPM_SCREEN_W + 10] == 0xffff && fb.px[17 * LPM_SCREEN_W + 30] == 0xffff);
    lpm_fill(&d, -20, -20, 30, 30, 1);
    lpm_fill(&d, 230, 310, 50, 50, 1);
    lpm_line(&d, -50, -50, 300, 400, 1);
    lpm_text(&d, &lpm_font_body, 236, 316, "clipped", 1, 0);
    CHECK(!fb.out_of_bounds);
    CHECK(fb.px[0] == 1 && fb.px[LPM_SCREEN_W * LPM_SCREEN_H - 1] == 1);
}

static void test_fonts_cover_printable_ascii(void)
{
    CHECK(lpm_font_small.first == ' ' && lpm_font_small.count == 95);
    CHECK(lpm_font_body.first == ' ' && lpm_font_body.count == 95);
    CHECK(lpm_font_body.line_height == 16 && lpm_font_small.line_height == 12);
    CHECK(lpm_font_digits.line_height == 32);
    CHECK(lpm_text_width(&lpm_font_body, "Ping monitor") == 12 * 8);
    CHECK(lpm_text_width(&lpm_font_digits, "12.3") == 3 * 18 + 8);
}

/* ---- Screen content ----------------------------------------------------- */

static void test_dhcp_offer_only_is_not_reported_as_lease(void)
{
    recorder r;
    lpm_display d = recording(&r);
    lpm_dhcp dhcp;
    lpm_demo_dhcp_offer_only(&dhcp);
    lpm_screen_dhcp(&d, &dhcp);
    CHECK(shows(&r, "OFFER only, no lease") && shows(&r, "WARN"));
    CHECK(!shows(&r, "Lease confirmed") && !shows(&r, "PASS"));
    CHECK(shows(&r, "no reply"));
    lpm_demo_dhcp_acked(&dhcp);
    d = recording(&r);
    lpm_screen_dhcp(&d, &dhcp);
    CHECK(shows(&r, "Lease confirmed") && shows(&r, "24 h 0 min") && shows(&r, "38 ms"));
}

static void test_length_never_invents_pair_values(void)
{
    recorder r;
    lpm_display d = recording(&r);
    lpm_length len = {true, false, 2347, {0, 0, 0, 0}};
    lpm_screen_length(&d, &len, false);
    CHECK(shows(&r, "23.5") && shows(&r, "Calibrated estimate"));
    CHECK(!shows(&r, "PER PAIR") && shows(&r, "not"));
    len.per_pair = true;
    len.pair_cm[3] = 1210;
    d = recording(&r);
    lpm_screen_length(&d, &len, true);
    CHECK(shows(&r, "PER PAIR") && shows(&r, "77.0") && shows(&r, "ft") && shows(&r, "39.7"));
}

static void test_status_words_accompany_colour(void)
{
    recorder r;
    lpm_display d = recording(&r);
    lpm_health h;
    lpm_wiremap map;
    lpm_ping_log log;
    lpm_poe_log poe;

    lpm_demo_health(&h);
    lpm_screen_health(&d, &h);
    CHECK(shows(&r, "FAIL") && shows(&r, "may be filtered") && shows(&r, "not tested"));

    lpm_demo_wiremap_fault(&map);
    d = recording(&r);
    lpm_screen_wiremap(&d, &map);
    CHECK(shows(&r, "OPEN") && shows(&r, "SHORT 8") && shows(&r, "CROSS 6") && shows(&r, "FAULT"));

    lpm_ping_reset(&log, LPM_IP(10, 0, 0, 1));
    lpm_ping_add_reply(&log, 1, 2500);
    lpm_ping_add_timeout(&log, 2);
    d = recording(&r);
    lpm_screen_ping(&d, &log);
    CHECK(shows(&r, "TIMEOUT") && shows(&r, "1 of 2") && shows(&r, "50.0% lost") && shows(&r, "10.0.0.1"));

    memset(&poe, 0, sizeof poe);
    lpm_poe_add(&poe, 48150);
    d = recording(&r);
    lpm_screen_poe(&d, &poe);
    CHECK(shows(&r, "48.2") && shows(&r, "no load") && shows(&r, "Pair mode not reported"));
}

static void test_content_update_leaves_chrome_alone(void)
{
    lpm_app app;
    lpm_display d = lpm_fb_display(&fb);
    lpm_demo_base(&data);
    lpm_demo_ping(&data.ping);
    lpm_app_init(&app, &data);
    lpm_menu_select(&app.home, LPM_ITEM_PING);
    lpm_app_key(&app, key(LPM_KEY_ENTER));
    lpm_app_render(&app, &d);
    lpm_fb_reset_stats(&fb);
    lpm_ping_add_reply(&data.ping, 999, 4200);
    lpm_app_data_changed(&app);
    lpm_app_render(&app, &d);
    CHECK(fb.min_y >= LPM_CONTENT_Y && fb.max_y < LPM_FOOTER_Y);
    lpm_fb_reset_stats(&fb);
    data.status.battery_pct = 40;
    lpm_app_status_changed(&app);
    lpm_app_render(&app, &d);
    CHECK(fb.min_y == 0 && fb.max_y < LPM_STATUS_H);
}

static void test_every_screen_stays_on_screen_with_extreme_data(void)
{
    lpm_app app;
    lpm_display d = lpm_fb_display(&fb);
    uint8_t mac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    int item, pass, i;

    for (pass = 0; pass < 2; pass++) {
        lpm_demo_base(&data);
        data.status.battery_pct = pass ? 100 : -1;
        data.status.charging = pass != 0;
        if (pass) {
            lpm_ping_reset(&data.ping, 0xffffffffu);
            for (i = 0; i < 200; i++) {
                lpm_ping_add_reply(&data.ping, (uint16_t)i, i % 3 ? 4294967295u : 1u);
            }
            for (i = 0; i < 100; i++) {
                lpm_poe_add(&data.poe, (uint16_t)(i % 2 ? 65535 : 0));
            }
            data.length.valid = data.length.per_pair = true;
            data.length.total_cm = 4294967295u;
            for (i = 0; i < LPM_PAIRS; i++) {
                data.length.pair_cm[i] = 4294967295u / (uint32_t)(i + 1);
            }
            data.status.feet = true;
            lpm_demo_wiremap_fault(&data.wiremap);
            lpm_scan_reset(&data.scan, 0, 0xffffffffu, 0xffffffffu);
            for (i = 0; i < 60; i++) {
                lpm_scan_add(&data.scan, 0xffffff00u + (uint32_t)i, mac);
            }
            data.scan.probed = 60000;
            data.health.link = LPM_STEP_PASS;
            data.health.source = LPM_ADDR_OFFER_ONLY;
            data.health.ip = data.health.gateway = 0xffffffffu;
            data.health.address = data.health.gateway_arp = data.health.gateway_ping = LPM_STEP_PASS;
            data.health.gateway_rtt_us = 4294967295u;
            lpm_demo_dhcp_acked(&data.dhcp);
            data.dhcp.offer_ms = data.dhcp.ack_ms = 65535;
            data.dhcp.lease_s = 4294967295u;
        }
        lpm_app_init(&app, &data);
        lpm_app_render(&app, &d);
        for (item = 0; item < LPM_ITEM_COUNT; item++) {
            lpm_app_resume(&app);
            lpm_menu_select(&app.home, (uint8_t)item);
            lpm_app_key(&app, key(LPM_KEY_ENTER));
            lpm_app_render(&app, &d);
            for (i = 0; i < 70; i++) {
                lpm_app_key(&app, key(LPM_KEY_DOWN));
                lpm_app_render(&app, &d);
            }
            lpm_app_key(&app, key(LPM_KEY_RIGHT));
            lpm_app_render(&app, &d);
        }
        CHECK(!fb.out_of_bounds);
    }
}

int main(void)
{
    RUN(test_menu_stops_at_boundaries_by_default);
    RUN(test_menu_scrolls_window_to_keep_selection_visible);
    RUN(test_wrap_applies_to_fresh_presses_only);
    RUN(test_home_has_proposed_order_and_native_items);
    RUN(test_selection_move_redraws_two_rows_and_title_only);
    RUN(test_two_moves_before_render_leave_one_highlight);
    RUN(test_selection_survives_tool_and_native_screens);
    RUN(test_back_on_home_is_left_to_stock_firmware);
    RUN(test_scan_cancel_then_ping_host_and_back_to_list);
    RUN(test_link_tabs_switch_and_start_carries_tab);
    RUN(test_only_direction_keys_repeat_with_acceleration);
    RUN(test_ping_statistics_and_timeouts);
    RUN(test_ping_ring_keeps_newest_window);
    RUN(test_memory_budget);
    RUN(test_wiremap_classification);
    RUN(test_scan_keeps_order_flags_duplicates_and_overflow);
    RUN(test_formatting);
    RUN(test_line_and_clipping);
    RUN(test_fonts_cover_printable_ascii);
    RUN(test_dhcp_offer_only_is_not_reported_as_lease);
    RUN(test_length_never_invents_pair_values);
    RUN(test_status_words_accompany_colour);
    RUN(test_content_update_leaves_chrome_alone);
    RUN(test_every_screen_stays_on_screen_with_extreme_data);
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
