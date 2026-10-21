/* Renders every screen with simulated data to PNG files, driving the real
 * lpm_app state machine with key events.
 *
 * usage: lpm_sim [output-dir] [scale] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lpm_app.h"
#include "lpm_demo.h"
#include "lpm_fb.h"

#define MAX_SHOTS 16

typedef struct {
    const char *dir;
    int scale;
    lpm_fb *shots[MAX_SHOTS];
    int count;
    int failures;
} capture;

static lpm_data data;
static lpm_app app;
static lpm_fb fb;
static lpm_display display;

static void press(lpm_key key)
{
    lpm_key_event event = {key, 0};
    lpm_app_key(&app, event);
    lpm_app_render(&app, &display);
}

static void go_home(void)
{
    lpm_app_resume(&app);
    lpm_app_render(&app, &display);
}

static void open_item(lpm_item item)
{
    go_home();
    while (app.home.selected < item) {
        press(LPM_KEY_DOWN);
    }
    while (app.home.selected > item) {
        press(LPM_KEY_UP);
    }
    press(LPM_KEY_ENTER);
}

static void shoot(capture *c, const char *name)
{
    char path[512];
    lpm_fb *copy;
    snprintf(path, sizeof path, "%s/%s.png", c->dir, name);
    if (fb.out_of_bounds || lpm_fb_write_png(&fb, path, c->scale) != 0) {
        fprintf(stderr, "failed: %s%s\n", path, fb.out_of_bounds ? " (out-of-bounds draw)" : "");
        c->failures++;
        return;
    }
    printf("%s\n", path);
    if (c->count < MAX_SHOTS && (copy = malloc(sizeof *copy)) != NULL) {
        *copy = fb;
        c->shots[c->count++] = copy;
    }
}

int main(int argc, char **argv)
{
    capture c = {argc > 1 ? argv[1] : "output/screens", argc > 2 ? atoi(argv[2]) : 2, {0}, 0, 0};
    char path[512];
    int i;

    if (c.scale < 1 || c.scale > 4) {
        c.scale = 2;
    }
    lpm_demo_base(&data);
    lpm_demo_ping(&data.ping);
    lpm_demo_wiremap_fault(&data.wiremap);
    lpm_demo_poe(&data.poe);
    lpm_demo_scan(&data.scan, false);
    lpm_demo_health(&data.health);
    lpm_demo_dhcp_acked(&data.dhcp);
    data.length.valid = true;
    data.length.total_cm = 2347;

    display = lpm_fb_display(&fb);
    lpm_app_init(&app, &data);
    lpm_app_render(&app, &display);
    shoot(&c, "01-home");
    open_item(LPM_ITEM_IP_SCAN);
    press(LPM_KEY_BACK);
    shoot(&c, "02-home-scrolled");

    open_item(LPM_ITEM_PING);
    shoot(&c, "03-ping");

    open_item(LPM_ITEM_CABLE_MAP);
    shoot(&c, "04-cable-map-fault");
    lpm_demo_wiremap_crossover(&data.wiremap);
    lpm_app_data_changed(&app);
    lpm_app_render(&app, &display);
    shoot(&c, "05-cable-map-crossover");

    open_item(LPM_ITEM_LENGTH);
    shoot(&c, "06-length");
    data.length.per_pair = true;
    data.length.pair_cm[0] = 2351;
    data.length.pair_cm[1] = 2338;
    data.length.pair_cm[2] = 2344;
    data.length.pair_cm[3] = 1210;
    lpm_app_data_changed(&app);
    lpm_app_render(&app, &display);
    shoot(&c, "07-length-per-pair");

    open_item(LPM_ITEM_POE);
    shoot(&c, "08-poe");

    open_item(LPM_ITEM_IP_SCAN);
    press(LPM_KEY_DOWN);
    press(LPM_KEY_DOWN);
    press(LPM_KEY_DOWN);
    press(LPM_KEY_DOWN);
    press(LPM_KEY_DOWN);
    shoot(&c, "09-ip-scan");
    lpm_demo_scan(&data.scan, true);
    lpm_app_data_changed(&app);
    lpm_app_render(&app, &display);
    shoot(&c, "10-ip-scan-running");

    open_item(LPM_ITEM_LINK);
    shoot(&c, "11-link-checklist");
    press(LPM_KEY_RIGHT);
    shoot(&c, "12-link-dhcp");
    lpm_demo_dhcp_offer_only(&data.dhcp);
    lpm_app_data_changed(&app);
    lpm_app_render(&app, &display);
    shoot(&c, "13-link-dhcp-offer-only");

    snprintf(path, sizeof path, "%s/contact-sheet.png", c.dir);
    if (lpm_fb_write_sheet((const lpm_fb *const *)c.shots, c.count, 5, path, 1) != 0) {
        c.failures++;
    } else {
        printf("%s\n", path);
    }
    for (i = 0; i < c.count; i++) {
        free(c.shots[i]);
    }
    return c.failures ? 1 : 0;
}
