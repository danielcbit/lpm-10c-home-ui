#include "lpm_app.h"

#include "lpm_assets.h"
#include "lpm_screens.h"
#include "lpm_theme.h"

static const lpm_item_info items[LPM_ITEM_COUNT] = {
    {"Cable map", &lpm_icon_cable_map, false},  {"Cable length", &lpm_icon_ruler, false},
    {"Cable trace", &lpm_icon_trace, true},     {"Port flash", &lpm_icon_port_flash, true},
    {"PoE voltage", &lpm_icon_bolt, false},     {"Link status", &lpm_icon_ethernet, false},
    {"Ping monitor", &lpm_icon_latency, false}, {"IP scan", &lpm_icon_hosts, false},
    {"Settings", &lpm_icon_settings, true},
};

static const char *const link_tabs[] = {"Checklist", "DHCP"};

const lpm_item_info *lpm_item_get(lpm_item item)
{
    return item < LPM_ITEM_COUNT ? &items[item] : NULL;
}

static lpm_action action(lpm_action_kind kind, lpm_item item)
{
    lpm_action a;
    a.kind = kind;
    a.item = item;
    a.tab = 0;
    a.ip = 0;
    return a;
}

void lpm_app_init(lpm_app *app, const lpm_data *data)
{
    app->data = data;
    lpm_menu_init(&app->home, LPM_ITEM_COUNT, LPM_HOME_ROWS, data->status.menu_wrap);
    lpm_menu_init(&app->hosts, 0, LPM_SCAN_ROWS, false);
    app->screen = LPM_HOME;
    app->link_tab = 0;
    app->ping_from_scan = false;
    app->dirty = LPM_DIRTY_ALL;
    app->change.moved = false;
    app->change.scrolled = false;
}

static void show(lpm_app *app, lpm_item screen)
{
    app->screen = screen;
    app->dirty = LPM_DIRTY_ALL;
}

/* Wrapping (if enabled) only happens on a fresh press; held keys stop at the
 * ends so accelerated repeat cannot overshoot round the list. */
static void step(lpm_app *app, lpm_menu *menu, int delta, const lpm_key_event *event)
{
    bool wrap = menu->wrap;
    lpm_menu_change change;
    if (event->repeat) {
        menu->wrap = false;
    }
    change = lpm_menu_step(menu, delta);
    menu->wrap = wrap;
    if (!change.moved) {
        return;
    }
    if (app->dirty & LPM_DIRTY_LIST) {
        /* Two steps before a render: the earlier "previous" row must be repainted too. */
        app->change.scrolled = true;
    } else {
        app->change = change;
    }
    app->change.current = change.current;
    app->dirty |= LPM_DIRTY_LIST;
}

static lpm_action home_key(lpm_app *app, lpm_key_event event)
{
    lpm_item item = (lpm_item)app->home.selected;
    switch (event.key) {
    case LPM_KEY_UP:
    case LPM_KEY_DOWN:
        step(app, &app->home, event.key == LPM_KEY_UP ? -1 : 1, &event);
        return action(LPM_ACTION_NONE, item);
    case LPM_KEY_ENTER:
        if (items[item].native) {
            return action(LPM_ACTION_OPEN_NATIVE, item);
        }
        app->ping_from_scan = false;
        show(app, item);
        {
            lpm_action a = action(LPM_ACTION_START, item);
            a.tab = item == LPM_ITEM_LINK ? app->link_tab : 0;
            return a;
        }
    case LPM_KEY_BACK:
        return action(LPM_ACTION_UNHANDLED, item);
    default:
        return action(LPM_ACTION_NONE, item);
    }
}

static lpm_action scan_key(lpm_app *app, lpm_key_event event)
{
    const lpm_scan *scan = &app->data->scan;
    lpm_menu_set_count(&app->hosts, scan->count);
    switch (event.key) {
    case LPM_KEY_UP:
    case LPM_KEY_DOWN:
        step(app, &app->hosts, event.key == LPM_KEY_UP ? -1 : 1, &event);
        return action(LPM_ACTION_NONE, LPM_ITEM_IP_SCAN);
    case LPM_KEY_ENTER:
        if (scan->count) {
            lpm_action a = action(LPM_ACTION_PING_HOST, LPM_ITEM_PING);
            a.ip = scan->hosts[app->hosts.selected].ip;
            app->ping_from_scan = true;
            show(app, LPM_ITEM_PING);
            return a;
        }
        if (!scan->running) {
            app->dirty |= LPM_DIRTY_CONTENT;
            return action(LPM_ACTION_START, LPM_ITEM_IP_SCAN);
        }
        return action(LPM_ACTION_NONE, LPM_ITEM_IP_SCAN);
    case LPM_KEY_BACK:
        if (scan->running) {
            return action(LPM_ACTION_CANCEL_SCAN, LPM_ITEM_IP_SCAN);
        }
        show(app, LPM_HOME);
        return action(LPM_ACTION_STOP, LPM_ITEM_IP_SCAN);
    default:
        return action(LPM_ACTION_NONE, LPM_ITEM_IP_SCAN);
    }
}

static lpm_action tool_key(lpm_app *app, lpm_key_event event)
{
    lpm_item item = app->screen;
    lpm_action a = action(LPM_ACTION_NONE, item);
    if (item == LPM_ITEM_IP_SCAN) {
        return scan_key(app, event);
    }
    switch (event.key) {
    case LPM_KEY_LEFT:
    case LPM_KEY_RIGHT:
        if (item == LPM_ITEM_LINK && !event.repeat) {
            app->link_tab = (uint8_t)(1 - app->link_tab);
            app->dirty |= LPM_DIRTY_ALL;
        }
        break;
    case LPM_KEY_ENTER:
        a.kind = LPM_ACTION_START;
        a.tab = item == LPM_ITEM_LINK ? app->link_tab : 0;
        if (item == LPM_ITEM_PING) {
            a.ip = app->data->ping.target_ip;
        }
        app->dirty |= LPM_DIRTY_CONTENT;
        break;
    case LPM_KEY_BACK:
        a.kind = LPM_ACTION_STOP;
        if (item == LPM_ITEM_PING && app->ping_from_scan) {
            app->ping_from_scan = false;
            show(app, LPM_ITEM_IP_SCAN);
        } else {
            show(app, LPM_HOME);
        }
        break;
    default:
        break;
    }
    return a;
}

lpm_action lpm_app_key(lpm_app *app, lpm_key_event event)
{
    return app->screen == LPM_HOME ? home_key(app, event) : tool_key(app, event);
}

void lpm_app_resume(lpm_app *app)
{
    show(app, LPM_HOME);
}

void lpm_app_data_changed(lpm_app *app)
{
    app->dirty |= LPM_DIRTY_CONTENT;
}

void lpm_app_status_changed(lpm_app *app)
{
    app->dirty |= LPM_DIRTY_STATUS;
}

static void paint_home_row(const lpm_display *d, void *ctx, uint8_t index, int y, bool selected)
{
    lpm_color bg = selected ? LPM_C_ACCENT : LPM_C_BG;
    lpm_color fg = selected ? LPM_C_ON_ACCENT : LPM_C_FG;
    (void)ctx;
    lpm_row_background(d, y, LPM_ROW_H, selected);
    lpm_draw_icon(d, items[index].icon, 14, y + 6, fg, bg);
    lpm_text(d, &lpm_font_body, 48, y + 10, items[index].name, fg, bg);
}

static void position_text(const lpm_app *app, lpm_str *s)
{
    lpm_str_u32(s, app->home.selected + 1u);
    lpm_str_char(s, '/');
    lpm_str_u32(s, LPM_ITEM_COUNT);
}

static void draw_title(const lpm_app *app, const lpm_display *d)
{
    char buf[16];
    lpm_str s;
    lpm_str_init(&s, buf, sizeof buf);
    if (app->screen == LPM_HOME) {
        position_text(app, &s);
        lpm_draw_title(d, "Tools", buf);
        return;
    }
    if (app->screen == LPM_ITEM_IP_SCAN) {
        lpm_str_u32(&s, app->data->scan.count);
        lpm_str_add(&s, " found");
    }
    lpm_draw_title(d, items[app->screen].name, buf);
}

static void draw_footer(const lpm_app *app, const lpm_display *d)
{
    const lpm_scan *scan = &app->data->scan;
    switch (app->screen) {
    case LPM_HOME:
        lpm_draw_footer(d, "M open", "UP/DOWN select");
        break;
    case LPM_ITEM_IP_SCAN:
        lpm_draw_footer(d,
                        scan->count     ? "M ping host"
                        : scan->running ? ""
                                        : "M scan again",
                        scan->running ? "BACK cancel" : "BACK home");
        break;
    case LPM_ITEM_LINK:
        lpm_draw_footer(d, "M run", "L/R tab  BACK home");
        break;
    case LPM_ITEM_PING:
    case LPM_ITEM_POE:
        lpm_draw_footer(d, "M restart", app->ping_from_scan ? "BACK hosts" : "BACK stop");
        break;
    default:
        lpm_draw_footer(d, "M test again", "BACK home");
        break;
    }
}

static void draw_content(lpm_app *app, const lpm_display *d)
{
    const lpm_data *data = app->data;
    switch (app->screen) {
    case LPM_HOME:
        lpm_list_draw(d, &app->home, LPM_HOME_LIST_Y, LPM_ROW_H, paint_home_row, NULL);
        break;
    case LPM_ITEM_CABLE_MAP:
        lpm_screen_wiremap(d, &data->wiremap);
        break;
    case LPM_ITEM_LENGTH:
        lpm_screen_length(d, &data->length, data->status.feet);
        break;
    case LPM_ITEM_POE:
        lpm_screen_poe(d, &data->poe);
        break;
    case LPM_ITEM_LINK:
        lpm_draw_tabs(d, link_tabs, 2, app->link_tab);
        if (app->link_tab == 0) {
            lpm_screen_health(d, &data->health);
        } else {
            lpm_screen_dhcp(d, &data->dhcp);
        }
        break;
    case LPM_ITEM_PING:
        lpm_screen_ping(d, &data->ping);
        break;
    case LPM_ITEM_IP_SCAN:
        lpm_menu_set_count(&app->hosts, data->scan.count);
        lpm_screen_scan(d, &data->scan, &app->hosts);
        break;
    default:
        break;
    }
}

void lpm_app_render(lpm_app *app, const lpm_display *d)
{
    uint8_t dirty = app->dirty;
    app->dirty = 0;
    if (dirty & LPM_DIRTY_ALL) {
        lpm_fill(d, 0, 0, LPM_SCREEN_W, LPM_SCREEN_H, LPM_C_BG);
        lpm_draw_status(d, &app->data->status);
        draw_title(app, d);
        draw_content(app, d);
        draw_footer(app, d);
        app->change.moved = false;
        app->change.scrolled = false;
        return;
    }
    if (dirty & LPM_DIRTY_STATUS) {
        lpm_draw_status(d, &app->data->status);
    }
    if (dirty & LPM_DIRTY_CONTENT) {
        draw_content(app, d);
        if (app->screen == LPM_ITEM_IP_SCAN) {
            draw_title(app, d);
            draw_footer(app, d);
        }
    } else if (dirty & LPM_DIRTY_LIST) {
        if (app->screen == LPM_HOME) {
            char buf[8];
            lpm_str s;
            lpm_str_init(&s, buf, sizeof buf);
            position_text(app, &s);
            lpm_list_redraw(d, &app->home, app->change, LPM_HOME_LIST_Y, LPM_ROW_H, paint_home_row, NULL);
            lpm_draw_title_right(d, buf);
        } else if (app->screen == LPM_ITEM_IP_SCAN) {
            lpm_list_redraw(d, &app->hosts, app->change, LPM_SCAN_LIST_Y, LPM_ROW_H, lpm_scan_paint_row,
                            (void *)&app->data->scan);
        }
    }
    app->change.moved = false;
    app->change.scrolled = false;
}
