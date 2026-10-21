#include "lpm_screens.h"

#include "lpm_assets.h"
#include "lpm_theme.h"

#define SMALL (&lpm_font_small)
#define BODY (&lpm_font_body)
#define DIGITS (&lpm_font_digits)
#define CONTENT_W (LPM_SCREEN_W - 2 * LPM_MARGIN)

/* ---- Chrome ------------------------------------------------------------- */

void lpm_draw_status(const lpm_display *d, const lpm_status *status)
{
    char buf[16];
    lpm_str s;
    lpm_str_init(&s, buf, sizeof buf);
    lpm_str_add(&s, status->charging ? "CHG " : "BAT ");
    if (status->battery_pct >= 0) {
        lpm_str_u32(&s, (uint32_t)status->battery_pct);
        lpm_str_char(&s, '%');
    } else {
        lpm_str_add(&s, "--");
    }
    lpm_fill(d, 0, LPM_STATUS_Y, LPM_SCREEN_W, LPM_STATUS_H - 1, LPM_C_BG);
    lpm_text(d, SMALL, 10, 4, "LPM-10C", LPM_C_DIM, LPM_C_BG);
    lpm_text(d, SMALL, LPM_SCREEN_W - 10 - lpm_text_width(SMALL, buf), 4, buf, LPM_C_DIM, LPM_C_BG);
    lpm_hline(d, 0, LPM_STATUS_H - 1, LPM_SCREEN_W, LPM_C_RULE);
}

void lpm_draw_title(const lpm_display *d, const char *title, const char *right)
{
    lpm_fill(d, 0, LPM_TITLE_Y, LPM_SCREEN_W, LPM_TITLE_H - 1, LPM_C_BG);
    lpm_text_box(d, BODY, 10, LPM_TITLE_Y + 10, 150, LPM_ALIGN_LEFT, title, LPM_C_FG, LPM_C_BG);
    if (right) {
        lpm_draw_title_right(d, right);
    }
    lpm_hline(d, 0, LPM_CONTENT_Y - 1, LPM_SCREEN_W, LPM_C_RULE);
}

void lpm_draw_title_right(const lpm_display *d, const char *right)
{
    lpm_text_box(d, SMALL, 160, LPM_TITLE_Y + 12, 70, LPM_ALIGN_RIGHT, right, LPM_C_DIM, LPM_C_BG);
}

void lpm_draw_footer(const lpm_display *d, const char *left, const char *right)
{
    lpm_hline(d, 0, LPM_FOOTER_Y, LPM_SCREEN_W, LPM_C_RULE);
    lpm_fill(d, 0, LPM_FOOTER_Y + 1, LPM_SCREEN_W, LPM_FOOTER_H - 1, LPM_C_BG);
    lpm_text(d, SMALL, 10, LPM_FOOTER_Y + 8, left, LPM_C_DIM, LPM_C_BG);
    lpm_text(d, SMALL, LPM_SCREEN_W - 10 - lpm_text_width(SMALL, right), LPM_FOOTER_Y + 8, right, LPM_C_DIM, LPM_C_BG);
}

void lpm_draw_tabs(const lpm_display *d, const char *const *labels, uint8_t count, uint8_t active)
{
    int tab_w = CONTENT_W / count;
    uint8_t i;
    for (i = 0; i < count; i++) {
        int x = LPM_MARGIN + i * tab_w;
        bool on = i == active;
        lpm_text_box(d, BODY, x, LPM_CONTENT_Y + 2, tab_w, LPM_ALIGN_CENTER, labels[i], on ? LPM_C_FG : LPM_C_DIM,
                     LPM_C_BG);
        lpm_fill(d, x, LPM_CONTENT_Y + 19, tab_w, 3, on ? LPM_C_ACCENT : LPM_C_BG);
        lpm_hline(d, x, LPM_CONTENT_Y + 22, tab_w, LPM_C_RULE);
    }
}

void lpm_clear_content(const lpm_display *d)
{
    lpm_fill(d, 0, LPM_CONTENT_Y, LPM_SCREEN_W, LPM_CONTENT_H, LPM_C_BG);
}

void lpm_draw_reading(const lpm_display *d, int x, int y, int number_w, const char *number, const char *unit,
                      lpm_color color)
{
    /* Digits: 28 px glyphs centred in a 32 px line, baseline at y + 30.
     * Body text: cap height 9 from y + 2, so its baseline aligns at y + 19. */
    lpm_text_box(d, DIGITS, x, y, number_w, LPM_ALIGN_RIGHT, number, color, LPM_C_BG);
    lpm_fill(d, x + number_w, y, 40, 19, LPM_C_BG);
    lpm_text_box(d, BODY, x + number_w + 4, y + 19, 36, LPM_ALIGN_LEFT, unit, LPM_C_DIM, LPM_C_BG);
    lpm_fill(d, x + number_w, y + 19, 4, 16, LPM_C_BG);
}

static void label(const lpm_display *d, int x, int y, int w, const char *text)
{
    lpm_text_box(d, SMALL, x, y, w, LPM_ALIGN_LEFT, text, LPM_C_DIM, LPM_C_BG);
}

/* Thicker plot stroke: doubles the line across its minor axis. */
static void thick_line(const lpm_display *d, int x0, int y0, int x1, int y1, lpm_color color)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    lpm_line(d, x0, y0, x1, y1, color);
    if (dy > dx || 2 * dy > dx) {
        lpm_line(d, x0 + 1, y0, x1 + 1, y1, color);
    }
    if (dy <= dx || 2 * dx > dy) {
        lpm_line(d, x0, y0 + 1, x1, y1 + 1, color);
    }
}

/* Plot frame with a labelled scale in a 30 px column to the left. */
#define PLOT_X 40
#define PLOT_W (LPM_SCREEN_W - PLOT_X - LPM_MARGIN)

static void plot_frame(const lpm_display *d, int y, int h, const char *top, const char *middle, const char *bottom)
{
    lpm_fill(d, 0, y - 6, PLOT_X, h + 12, LPM_C_BG);
    lpm_fill(d, PLOT_X, y, PLOT_W, h, LPM_C_BG);
    lpm_hline(d, PLOT_X, y, PLOT_W, LPM_C_RULE);
    lpm_hline(d, PLOT_X, y + h / 2, PLOT_W, LPM_C_RULE);
    lpm_hline(d, PLOT_X, y + h - 1, PLOT_W, LPM_C_DIM);
    lpm_text_box(d, SMALL, 2, y - 6, PLOT_X - 6, LPM_ALIGN_RIGHT, top, LPM_C_DIM, LPM_C_BG);
    lpm_text_box(d, SMALL, 2, y + h / 2 - 6, PLOT_X - 6, LPM_ALIGN_RIGHT, middle, LPM_C_DIM, LPM_C_BG);
    lpm_text_box(d, SMALL, 2, y + h - 7, PLOT_X - 6, LPM_ALIGN_RIGHT, bottom, LPM_C_DIM, LPM_C_BG);
}

static void centred_message(const lpm_display *d, int y, const char *line1, const char *line2)
{
    lpm_text_box(d, BODY, LPM_MARGIN, y, CONTENT_W, LPM_ALIGN_CENTER, line1, LPM_C_FG, LPM_C_BG);
    lpm_text_box(d, SMALL, LPM_MARGIN, y + 20, CONTENT_W, LPM_ALIGN_CENTER, line2, LPM_C_DIM, LPM_C_BG);
}

/* ---- Ping --------------------------------------------------------------- */

#define PING_PLOT_Y 114
#define PING_PLOT_H 96

static uint32_t ping_scale_us(const lpm_ping_log *log)
{
    static const uint32_t steps[] = {1000u,   2000u,   5000u,   10000u,   20000u,   50000u,
                                     100000u, 200000u, 500000u, 1000000u, 2000000u, 5000000u};
    uint32_t peak = 0;
    uint8_t i;
    for (i = 0; i < log->count; i++) {
        const lpm_ping_sample *s = lpm_ping_at(log, i);
        if (s->status == LPM_PING_REPLY && s->rtt_us > peak) {
            peak = s->rtt_us;
        }
    }
    for (i = 0; i < sizeof steps / sizeof steps[0] - 1 && steps[i] < peak; i++) {
    }
    return steps[i];
}

static void scale_label(lpm_str *s, uint32_t us, bool with_unit)
{
    if (us >= 1000000u && us % 1000000u == 0) {
        lpm_str_u32(s, us / 1000000u);
        lpm_str_add(s, with_unit ? "s" : "");
    } else if (us % 1000u == 0) {
        lpm_str_u32(s, us / 1000u);
        lpm_str_add(s, with_unit ? "ms" : "");
    } else {
        lpm_str_fixed(s, (int32_t)(us / 100u), 1);
    }
}

static void ping_plot(const lpm_display *d, const lpm_ping_log *log)
{
    char top[12], middle[12];
    lpm_str s;
    uint32_t scale = ping_scale_us(log);
    int base = PING_PLOT_Y + PING_PLOT_H - 2;
    int span = PING_PLOT_H - 3;
    int prev_x = -1, prev_y = 0;
    uint8_t i;

    lpm_str_init(&s, top, sizeof top);
    scale_label(&s, scale, true);
    lpm_str_init(&s, middle, sizeof middle);
    scale_label(&s, scale / 2, false);
    plot_frame(d, PING_PLOT_Y, PING_PLOT_H, top, middle, "0");

    for (i = 0; i < log->count; i++) {
        const lpm_ping_sample *sample = lpm_ping_at(log, i);
        int slot = LPM_PING_HISTORY - log->count + i;
        int x = PLOT_X + 1 + slot * (PLOT_W - 3) / (LPM_PING_HISTORY - 1);
        if (sample->status != LPM_PING_REPLY) {
            /* Explicit gap plus a marker; the line does not bridge timeouts. */
            lpm_fill(d, x, PING_PLOT_Y + 1, 2, 7, LPM_C_FAULT);
            prev_x = -1;
            continue;
        }
        {
            uint32_t rtt = sample->rtt_us < scale ? sample->rtt_us : scale;
            int y = base - (int)((uint64_t)rtt * (uint32_t)span / scale);
            if (prev_x >= 0) {
                thick_line(d, prev_x, prev_y, x, y, LPM_C_ACCENT);
            } else {
                lpm_fill(d, x, y, 2, 2, LPM_C_ACCENT);
            }
            prev_x = x;
            prev_y = y;
        }
    }
}

static void stat_cell(const lpm_display *d, int x, const char *name, uint32_t us, bool known)
{
    char buf[12];
    lpm_str s;
    lpm_str_init(&s, buf, sizeof buf);
    if (known) {
        lpm_str_ms(&s, us);
    } else {
        lpm_str_add(&s, "--");
    }
    label(d, x, 220, 56, name);
    lpm_text_box(d, BODY, x, 234, 56, LPM_ALIGN_LEFT, buf, LPM_C_FG, LPM_C_BG);
}

void lpm_screen_ping(const lpm_display *d, const lpm_ping_log *log)
{
    char buf[40];
    lpm_str s;
    const lpm_ping_sample *last = log->count ? lpm_ping_at(log, (uint8_t)(log->count - 1)) : NULL;
    bool has_reply = log->received > 0;

    label(d, LPM_MARGIN + 2, 60, 130, "LATEST RTT");
    if (last && last->status != LPM_PING_REPLY) {
        lpm_fill(d, LPM_MARGIN, 72, 132, 6, LPM_C_BG);
        lpm_text_box(d, BODY, LPM_MARGIN, 78, 132, LPM_ALIGN_LEFT, "  TIMEOUT", LPM_C_FAULT, LPM_C_BG);
        lpm_fill(d, LPM_MARGIN, 94, 132, 10, LPM_C_BG);
    } else {
        lpm_str_init(&s, buf, sizeof buf);
        if (last) {
            lpm_str_ms(&s, last->rtt_us);
        } else {
            lpm_str_add(&s, "--");
        }
        lpm_draw_reading(d, LPM_MARGIN, 72, 92, buf, "ms", LPM_C_FG);
    }

    label(d, 152, 60, 80, "LOSS");
    lpm_str_init(&s, buf, sizeof buf);
    lpm_str_u32(&s, lpm_ping_lost(log));
    lpm_str_add(&s, " of ");
    lpm_str_u32(&s, log->sent);
    lpm_text_box(d, BODY, 152, 74, 80, LPM_ALIGN_LEFT, buf, LPM_C_FG, LPM_C_BG);
    lpm_str_init(&s, buf, sizeof buf);
    lpm_str_fixed(&s, (int32_t)lpm_ping_loss_permille(log), 1);
    lpm_str_add(&s, "% lost");
    lpm_text_box(d, SMALL, 152, 92, 80, LPM_ALIGN_LEFT, buf, lpm_ping_lost(log) ? LPM_C_FAULT : LPM_C_DIM, LPM_C_BG);

    ping_plot(d, log);

    stat_cell(d, 10, "MIN ms", log->min_us, has_reply);
    stat_cell(d, 66, "AVG ms", lpm_ping_mean_us(log), has_reply);
    stat_cell(d, 122, "MAX ms", log->max_us, has_reply);
    stat_cell(d, 178, "VAR ms", lpm_ping_variation_us(log), log->variation_count > 0);

    lpm_str_init(&s, buf, sizeof buf);
    lpm_str_add(&s, "To ");
    lpm_str_ipv4(&s, log->target_ip);
    if (log->sent) {
        lpm_str_add(&s, "  seq ");
        lpm_str_u32(&s, log->last_seq);
    }
    label(d, 10, 258, CONTENT_W, buf);
    label(d, 10, 272, CONTENT_W, "VAR: mean change between replies");
}

/* ---- Cable map ---------------------------------------------------------- */

#define MAP_ROW0 84
#define MAP_PITCH 22
#define MAP_LOCAL_X 10
#define MAP_REMOTE_X 112
#define MAP_BOX_W 22
#define MAP_LABEL_X 154

static void pin_box(const lpm_display *d, int x, int cy, uint8_t pin, lpm_color edge)
{
    char text[2] = {(char)('0' + pin), '\0'};
    lpm_frame(d, x, cy - 9, MAP_BOX_W, 19, edge);
    lpm_text_box(d, BODY, x + 1, cy - 8, MAP_BOX_W - 2, LPM_ALIGN_CENTER, text, LPM_C_FG, LPM_C_BG);
    lpm_fill(d, x + 1, cy + 8, MAP_BOX_W - 2, 1, LPM_C_BG);
}

void lpm_screen_wiremap(const lpm_display *d, const lpm_wiremap *map)
{
    lpm_wiremap_summary summary = lpm_wiremap_summarize(map);
    uint8_t i;

    lpm_fill(d, 0, LPM_CONTENT_Y, LPM_SCREEN_W, 196, LPM_C_BG);
    if (!map->valid) {
        centred_message(d, 130, "No remote unit", "Connect the remote, then press M");
        lpm_fill(d, 0, 252, LPM_SCREEN_W, 40, LPM_C_BG);
        return;
    }
    label(d, MAP_LOCAL_X, 60, 60, "LOCAL");
    label(d, MAP_REMOTE_X, 60, 40, "REMOTE");
    label(d, MAP_LABEL_X, 60, 80, "RESULT");

    for (i = 0; i < LPM_PINS; i++) {
        int cy = MAP_ROW0 + i * MAP_PITCH;
        int from = MAP_LOCAL_X + MAP_BOX_W;
        lpm_pin_state state = lpm_wiremap_pin(map, i);
        char buf[16];
        lpm_str s;
        lpm_color color = LPM_C_ACCENT;

        lpm_str_init(&s, buf, sizeof buf);
        pin_box(d, MAP_LOCAL_X, cy, (uint8_t)(i + 1), LPM_C_DIM);
        pin_box(d, MAP_REMOTE_X, cy, (uint8_t)(i + 1), LPM_C_DIM);
        switch (state) {
        case LPM_PIN_OK:
            lpm_str_add(&s, "OK");
            thick_line(d, from, cy, MAP_REMOTE_X - 1, cy, LPM_C_ACCENT);
            break;
        case LPM_PIN_CROSSED: {
            int to = MAP_ROW0 + (map->remote[i] - 1) * MAP_PITCH;
            bool expected = summary.wiring == LPM_WIRING_CROSSOVER;
            color = expected ? LPM_C_ACCENT : LPM_C_WARN;
            lpm_str_add(&s, expected ? "TO " : "CROSS ");
            lpm_str_u32(&s, map->remote[i]);
            thick_line(d, from, cy, MAP_REMOTE_X - 1, to, color);
            break;
        }
        case LPM_PIN_OPEN:
            color = LPM_C_FAULT;
            lpm_str_add(&s, "OPEN");
            thick_line(d, from, cy, from + 22, cy, color);
            lpm_line(d, from + 24, cy - 3, from + 30, cy + 3, color);
            lpm_line(d, from + 24, cy + 3, from + 30, cy - 3, color);
            break;
        case LPM_PIN_SHORT: {
            int partner = MAP_ROW0 + (map->shorted_to[i] - 1) * MAP_PITCH;
            color = LPM_C_FAULT;
            lpm_str_add(&s, "SHORT ");
            lpm_str_u32(&s, map->shorted_to[i]);
            thick_line(d, from, cy, from + 14, cy, color);
            thick_line(d, from + 14, cy, from + 14, partner, color);
            break;
        }
        }
        lpm_text_box(d, BODY, MAP_LABEL_X, cy - 8, LPM_SCREEN_W - MAP_LABEL_X - 4, LPM_ALIGN_LEFT, buf,
                     state == LPM_PIN_OK ? LPM_C_DIM : color, LPM_C_BG);
    }

    {
        char buf[40];
        lpm_str s;
        bool fault = summary.wiring == LPM_WIRING_FAULT;
        lpm_str_init(&s, buf, sizeof buf);
        if (fault) {
            lpm_str_u32(&s, summary.open);
            lpm_str_add(&s, " open, ");
            lpm_str_u32(&s, summary.shorted);
            lpm_str_add(&s, " shorted, ");
            lpm_str_u32(&s, summary.crossed);
            lpm_str_add(&s, " crossed");
        } else {
            lpm_str_add(&s, "Wiring continuity only");
        }
        lpm_fill(d, 0, 252, LPM_SCREEN_W, 4, LPM_C_BG);
        lpm_text_box(d, BODY, 10, 256, 56, LPM_ALIGN_LEFT, fault ? "FAULT" : "PASS", fault ? LPM_C_FAULT : LPM_C_PASS,
                     LPM_C_BG);
        lpm_text_box(d, BODY, 66, 256, LPM_SCREEN_W - 70, LPM_ALIGN_LEFT,
                     fault                                    ? "Wiring error"
                     : summary.wiring == LPM_WIRING_CROSSOVER ? "Crossover"
                                                              : "Straight-through",
                     LPM_C_FG, LPM_C_BG);
        label(d, 10, 276, CONTENT_W, buf);
    }
}

/* ---- Length ------------------------------------------------------------- */

static void add_length(lpm_str *s, uint32_t cm, bool feet)
{
    /* tenths of a foot = cm * 10 / 30.48 */
    uint64_t tenths = feet ? ((uint64_t)cm * 1000u + 1524u) / 3048u : ((uint64_t)cm + 5u) / 10u;
    lpm_str_fixed(s, (int32_t)tenths, 1);
}

void lpm_screen_length(const lpm_display *d, const lpm_length *length, bool feet)
{
    static const char *const pairs[LPM_PAIRS] = {"1-2", "3-6", "4-5", "7-8"};
    char buf[16];
    lpm_str s;
    uint8_t i;

    label(d, 10, 60, 120, "TOTAL LENGTH");
    lpm_str_init(&s, buf, sizeof buf);
    if (length->valid) {
        add_length(&s, length->total_cm, feet);
    } else {
        lpm_str_add(&s, "--");
    }
    lpm_draw_reading(d, LPM_MARGIN, 74, 110, buf, feet ? "ft" : "m", LPM_C_FG);
    label(d, 10, 110, CONTENT_W, length->valid ? "Calibrated estimate" : "Press M to measure");
    lpm_fill(d, 0, 122, LPM_SCREEN_W, 160, LPM_C_BG);

    if (!length->valid) {
        return;
    }
    if (!length->per_pair) {
        label(d, 10, 140, CONTENT_W, "Per-pair lengths are not");
        label(d, 10, 154, CONTENT_W, "reported by this measurement");
        return;
    }
    label(d, 10, 136, 120, "PER PAIR");
    {
        uint32_t longest = 1;
        for (i = 0; i < LPM_PAIRS; i++) {
            if (length->pair_cm[i] > longest) {
                longest = length->pair_cm[i];
            }
        }
        for (i = 0; i < LPM_PAIRS; i++) {
            int y = 154 + i * 28;
            int bar = (int)((uint64_t)length->pair_cm[i] * 132u / longest);
            lpm_text_box(d, BODY, 10, y, 34, LPM_ALIGN_LEFT, pairs[i], LPM_C_FG, LPM_C_BG);
            lpm_fill(d, 46, y + 4, 132, 8, LPM_C_RULE);
            lpm_fill(d, 46, y + 4, bar, 8, LPM_C_ACCENT);
            lpm_str_init(&s, buf, sizeof buf);
            add_length(&s, length->pair_cm[i], feet);
            lpm_text_box(d, BODY, 182, y, 50, LPM_ALIGN_RIGHT, buf, LPM_C_FG, LPM_C_BG);
        }
    }
}

/* ---- PoE ---------------------------------------------------------------- */

#define POE_PLOT_Y 140
#define POE_PLOT_H 96
#define POE_SCALE_MV 60000u

static void add_volts(lpm_str *s, uint16_t mv)
{
    lpm_str_fixed(s, (int32_t)((mv + 50u) / 100u), 1);
}

void lpm_screen_poe(const lpm_display *d, const lpm_poe_log *log)
{
    char buf[24];
    lpm_str s;
    uint8_t i;
    int prev_x = -1, prev_y = 0;

    label(d, 10, 60, 120, "VOLTAGE");
    lpm_str_init(&s, buf, sizeof buf);
    if (log->count) {
        add_volts(&s, lpm_poe_at(log, (uint8_t)(log->count - 1)));
    } else {
        lpm_str_add(&s, "--");
    }
    lpm_draw_reading(d, LPM_MARGIN, 74, 92, buf, "V", LPM_C_FG);

    lpm_str_init(&s, buf, sizeof buf);
    lpm_str_add(&s, "Min ");
    if (log->count) {
        add_volts(&s, log->min_mv);
        lpm_str_add(&s, " V");
    } else {
        lpm_str_add(&s, "--");
    }
    lpm_text_box(d, SMALL, 156, 78, 78, LPM_ALIGN_LEFT, buf, LPM_C_DIM, LPM_C_BG);
    lpm_str_init(&s, buf, sizeof buf);
    lpm_str_add(&s, "Max ");
    if (log->count) {
        add_volts(&s, log->max_mv);
        lpm_str_add(&s, " V");
    } else {
        lpm_str_add(&s, "--");
    }
    lpm_text_box(d, SMALL, 156, 94, 78, LPM_ALIGN_LEFT, buf, LPM_C_DIM, LPM_C_BG);

    lpm_text_box(d, SMALL, 10, 114, CONTENT_W, LPM_ALIGN_LEFT, log->mode ? log->mode : "Pair mode not reported",
                 log->mode ? LPM_C_FG : LPM_C_DIM, LPM_C_BG);

    plot_frame(d, POE_PLOT_Y, POE_PLOT_H, "60V", "30", "0");
    for (i = 0; i < log->count; i++) {
        uint16_t mv = lpm_poe_at(log, i);
        int slot = LPM_POE_HISTORY - log->count + i;
        int x = PLOT_X + 1 + slot * (PLOT_W - 3) / (LPM_POE_HISTORY - 1);
        uint32_t clamped = mv < POE_SCALE_MV ? mv : POE_SCALE_MV;
        int y = POE_PLOT_Y + POE_PLOT_H - 2 - (int)(clamped * (POE_PLOT_H - 3) / POE_SCALE_MV);
        if (prev_x >= 0) {
            thick_line(d, prev_x, prev_y, x, y, LPM_C_ACCENT);
        } else {
            lpm_fill(d, x, y, 2, 2, LPM_C_ACCENT);
        }
        prev_x = x;
        prev_y = y;
    }
    label(d, 10, 252, CONTENT_W, "Voltage only: no load,");
    label(d, 10, 266, CONTENT_W, "current or power test");
}

/* ---- IP scan ------------------------------------------------------------ */

void lpm_scan_paint_row(const lpm_display *d, void *ctx, uint8_t index, int y, bool selected)
{
    const lpm_scan *scan = ctx;
    const lpm_host *host = &scan->hosts[index];
    lpm_color bg = selected ? LPM_C_ACCENT : LPM_C_BG;
    lpm_color fg = selected ? LPM_C_ON_ACCENT : LPM_C_FG;
    lpm_color dim = selected ? LPM_C_ON_ACCENT : LPM_C_DIM;
    const char *tag = "";
    lpm_color tag_color = dim;
    char buf[24];
    lpm_str s;

    if (host->flags & LPM_HOST_DUPLICATE) {
        tag = "DUPLICATE IP";
        tag_color = selected ? LPM_C_ON_ACCENT : LPM_C_FAULT;
    } else if (host->flags & LPM_HOST_GATEWAY) {
        tag = "GATEWAY";
    }
    lpm_row_background(d, y, LPM_ROW_H, selected);
    lpm_str_init(&s, buf, sizeof buf);
    lpm_str_ipv4(&s, host->ip);
    lpm_text_box(d, BODY, 12, y + 3, 128, LPM_ALIGN_LEFT, buf, fg, bg);
    lpm_text_box(d, SMALL, 140, y + 5, 86, LPM_ALIGN_RIGHT, tag, tag_color, bg);
    lpm_str_init(&s, buf, sizeof buf);
    lpm_str_mac(&s, host->mac);
    lpm_text_box(d, SMALL, 12, y + 21, 110, LPM_ALIGN_LEFT, buf, dim, bg);
    lpm_text_box(d, SMALL, 140, y + 21, 86, LPM_ALIGN_RIGHT, "ARP reply", dim, bg);
}

void lpm_screen_scan(const lpm_display *d, const lpm_scan *scan, const lpm_menu *list)
{
    char buf[40];
    lpm_str s;
    int done = scan->total ? (int)((uint32_t)scan->probed * CONTENT_W / scan->total) : 0;

    lpm_str_init(&s, buf, sizeof buf);
    if (scan->running) {
        lpm_str_add(&s, "Scanning ");
        lpm_str_ipv4(&s, scan->first_ip);
        lpm_str_add(&s, "-");
        lpm_str_u32(&s, scan->last_ip & 0xffu);
    } else {
        lpm_str_add(&s, scan->cancelled ? "Cancelled at " : "Complete ");
        lpm_str_u32(&s, scan->probed);
        lpm_str_char(&s, '/');
        lpm_str_u32(&s, scan->total);
        lpm_str_add(&s, " probed");
    }
    label(d, 10, 60, CONTENT_W, buf);
    lpm_fill(d, 0, 72, LPM_SCREEN_W, 4, LPM_C_BG);
    lpm_fill(d, LPM_MARGIN, 76, CONTENT_W, 6, LPM_C_RULE);
    lpm_fill(d, LPM_MARGIN, 76, done > CONTENT_W ? CONTENT_W : done, 6, LPM_C_ACCENT);
    lpm_fill(d, 0, 82, LPM_SCREEN_W, LPM_SCAN_LIST_Y - 82, LPM_C_BG);

    if (scan->count == 0) {
        lpm_fill(d, 0, LPM_SCAN_LIST_Y, LPM_SCREEN_W, LPM_SCAN_ROWS * LPM_ROW_H, LPM_C_BG);
        centred_message(d, 150, scan->running ? "No replies yet" : "No hosts replied",
                        scan->running ? "Back cancels the scan" : "M scans again");
    } else {
        lpm_list_draw(d, list, LPM_SCAN_LIST_Y, LPM_ROW_H, lpm_scan_paint_row, (void *)scan);
    }
    label(d, 10, 274, CONTENT_W, scan->overflow ? "List full: some hosts not shown" : "");
}

/* ---- Link health -------------------------------------------------------- */

static void step_row(const lpm_display *d, int y, const char *name, lpm_step step, const char *detail)
{
    static const char *const words[] = {"--", "TESTING", "PASS", "WARN", "FAIL", "SKIPPED"};
    const lpm_color colors[] = {LPM_C_DIM, LPM_C_DIM, LPM_C_PASS, LPM_C_WARN, LPM_C_FAULT, LPM_C_DIM};
    lpm_text_box(d, BODY, 10, y, 140, LPM_ALIGN_LEFT, name, LPM_C_FG, LPM_C_BG);
    lpm_text_box(d, BODY, 150, y, 82, LPM_ALIGN_RIGHT, words[step], colors[step], LPM_C_BG);
    lpm_text_box(d, SMALL, 10, y + 18, CONTENT_W, LPM_ALIGN_LEFT, detail, LPM_C_DIM, LPM_C_BG);
    lpm_fill(d, 0, y + 30, LPM_SCREEN_W, 14, LPM_C_BG);
}

void lpm_screen_health(const lpm_display *d, const lpm_health *h)
{
    char buf[40];
    lpm_str s;

    lpm_str_init(&s, buf, sizeof buf);
    if (h->link == LPM_STEP_PASS && h->link_mbps) {
        lpm_str_u32(&s, h->link_mbps);
        lpm_str_add(&s, h->full_duplex ? " Mb/s full duplex" : " Mb/s half duplex");
    } else if (h->link == LPM_STEP_PASS) {
        lpm_str_add(&s, "Link up");
    } else if (h->link == LPM_STEP_FAIL) {
        lpm_str_add(&s, "No link detected");
    }
    step_row(d, 84, "Physical link", h->link, buf);

    lpm_str_init(&s, buf, sizeof buf);
    if (h->source != LPM_ADDR_NONE) {
        lpm_str_ipv4(&s, h->ip);
        lpm_str_add(&s, h->source == LPM_ADDR_DHCP         ? " DHCP lease"
                        : h->source == LPM_ADDR_OFFER_ONLY ? " OFFER only, no lease"
                                                           : " static");
    } else if (h->address == LPM_STEP_FAIL) {
        lpm_str_add(&s, "No address obtained");
    }
    step_row(d, 128, "Address", h->address, buf);

    lpm_str_init(&s, buf, sizeof buf);
    if (h->gateway) {
        lpm_str_ipv4(&s, h->gateway);
        if (h->gateway_arp == LPM_STEP_PASS) {
            lpm_str_add(&s, " ");
            lpm_str_mac(&s, h->gateway_mac);
        } else if (h->gateway_arp == LPM_STEP_FAIL) {
            lpm_str_add(&s, " no ARP reply");
        }
    } else if (h->gateway_arp != LPM_STEP_PENDING) {
        lpm_str_add(&s, "No gateway known");
    }
    step_row(d, 172, "Gateway ARP", h->gateway_arp, buf);

    lpm_str_init(&s, buf, sizeof buf);
    if (h->gateway_ping == LPM_STEP_PASS) {
        lpm_str_add(&s, "RTT ");
        lpm_str_ms(&s, h->gateway_rtt_us);
        lpm_str_add(&s, " ms");
    } else if (h->gateway_ping == LPM_STEP_FAIL) {
        lpm_str_add(&s, "No echo reply (may be filtered)");
    }
    step_row(d, 216, "Gateway ping", h->gateway_ping, buf);

    label(d, 10, 262, CONTENT_W, "Local checks only: DNS and");
    label(d, 10, 276, CONTENT_W, "Internet access are not tested");
}

/* ---- DHCP --------------------------------------------------------------- */

typedef enum { MARK_PENDING, MARK_DONE, MARK_WAIT, MARK_FAILED } mark;

static void timeline_row(const lpm_display *d, int index, const char *name, mark m, const char *detail, bool connect)
{
    int y = 86 + index * 24;
    lpm_color color = m == MARK_DONE ? LPM_C_ACCENT : m == MARK_FAILED ? LPM_C_FAULT : LPM_C_DIM;
    lpm_fill(d, 0, y, 30, 24, LPM_C_BG);
    if (connect) {
        lpm_fill(d, 15, y + 12, 2, 24, LPM_C_RULE);
    }
    if (m == MARK_DONE || m == MARK_FAILED) {
        lpm_fill(d, 11, y + 4, 10, 10, color);
    } else {
        lpm_fill(d, 11, y + 4, 10, 10, LPM_C_BG);
        lpm_frame(d, 11, y + 4, 10, 10, color);
    }
    lpm_text_box(d, BODY, 30, y + 1, 110, LPM_ALIGN_LEFT, name, m == MARK_PENDING ? LPM_C_DIM : LPM_C_FG, LPM_C_BG);
    lpm_text_box(d, BODY, 140, y + 1, 92, LPM_ALIGN_RIGHT, detail, m == MARK_FAILED ? LPM_C_FAULT : LPM_C_DIM,
                 LPM_C_BG);
}

static const char *ms_text(lpm_str *s, uint16_t ms)
{
    lpm_str_u32(s, ms);
    lpm_str_add(s, " ms");
    return s->data;
}

static void field(const lpm_display *d, int row, const char *name, const char *value)
{
    int y = 188 + row * 14;
    label(d, 10, y, 64, name);
    lpm_text_box(d, SMALL, 76, y, LPM_SCREEN_W - 84, LPM_ALIGN_LEFT, value, LPM_C_FG, LPM_C_BG);
}

static void ip_field(const lpm_display *d, int row, const char *name, uint32_t ip, bool known)
{
    char buf[16];
    lpm_str s;
    lpm_str_init(&s, buf, sizeof buf);
    if (known && ip) {
        lpm_str_ipv4(&s, ip);
    } else {
        lpm_str_add(&s, "--");
    }
    field(d, row, name, buf);
}

void lpm_screen_dhcp(const lpm_display *d, const lpm_dhcp *dhcp)
{
    char a[16], b[16];
    lpm_str sa, sb;
    lpm_dhcp_state st = dhcp->state;
    bool offered =
        st == LPM_DHCP_REQUESTING || st == LPM_DHCP_ACKED || st == LPM_DHCP_NAKED || st == LPM_DHCP_OFFER_ONLY;
    mark ack = st == LPM_DHCP_ACKED                                ? MARK_DONE
               : st == LPM_DHCP_NAKED || st == LPM_DHCP_OFFER_ONLY ? MARK_FAILED
               : st == LPM_DHCP_REQUESTING                         ? MARK_WAIT
                                                                   : MARK_PENDING;
    const char *verdict = "Press M to run DHCP";
    const char *word = "";
    lpm_color color = LPM_C_DIM;

    lpm_str_init(&sa, a, sizeof a);
    lpm_str_init(&sb, b, sizeof b);
    timeline_row(d, 0, "DISCOVER", st == LPM_DHCP_IDLE ? MARK_PENDING : MARK_DONE, st == LPM_DHCP_IDLE ? "" : "sent",
                 true);
    timeline_row(d, 1, "OFFER",
                 offered                      ? MARK_DONE
                 : st == LPM_DHCP_NO_OFFER    ? MARK_FAILED
                 : st == LPM_DHCP_DISCOVERING ? MARK_WAIT
                                              : MARK_PENDING,
                 offered                      ? ms_text(&sa, dhcp->offer_ms)
                 : st == LPM_DHCP_NO_OFFER    ? "none"
                 : st == LPM_DHCP_DISCOVERING ? "waiting"
                                              : "",
                 true);
    timeline_row(d, 2, "REQUEST", offered ? MARK_DONE : MARK_PENDING, offered ? "sent" : "", true);
    timeline_row(d, 3, "ACK", ack,
                 st == LPM_DHCP_ACKED        ? ms_text(&sb, dhcp->ack_ms)
                 : st == LPM_DHCP_NAKED      ? "NAK"
                 : st == LPM_DHCP_OFFER_ONLY ? "no reply"
                 : st == LPM_DHCP_REQUESTING ? "waiting"
                                             : "",
                 false);

    lpm_fill(d, 0, 182, LPM_SCREEN_W, 6, LPM_C_BG);
    ip_field(d, 0, "Server", dhcp->server, offered);
    ip_field(d, 1, "Address", dhcp->offered_ip, offered);
    ip_field(d, 2, "Mask", dhcp->mask, offered);
    ip_field(d, 3, "Router", dhcp->router, offered);
    {
        char buf[24];
        lpm_str s;
        lpm_str_init(&s, buf, sizeof buf);
        if (st != LPM_DHCP_ACKED) {
            lpm_str_add(&s, "--");
        } else if (!dhcp->lease_known) {
            lpm_str_add(&s, "not reported");
        } else {
            lpm_str_u32(&s, dhcp->lease_s / 3600u);
            lpm_str_add(&s, " h ");
            lpm_str_u32(&s, dhcp->lease_s % 3600u / 60u);
            lpm_str_add(&s, " min");
        }
        field(d, 4, "Lease", buf);
    }

    switch (st) {
    case LPM_DHCP_ACKED:
        word = "PASS";
        verdict = "Lease confirmed";
        color = LPM_C_PASS;
        break;
    case LPM_DHCP_OFFER_ONLY:
        word = "WARN";
        verdict = "OFFER only, no lease";
        color = LPM_C_WARN;
        break;
    case LPM_DHCP_NAKED:
        word = "FAIL";
        verdict = "Server refused (NAK)";
        color = LPM_C_FAULT;
        break;
    case LPM_DHCP_NO_OFFER:
        word = "FAIL";
        verdict = "No DHCP server replied";
        color = LPM_C_FAULT;
        break;
    case LPM_DHCP_DISCOVERING:
    case LPM_DHCP_REQUESTING:
        verdict = "Waiting for server";
        break;
    case LPM_DHCP_IDLE:
        break;
    }
    lpm_fill(d, 0, 258, LPM_SCREEN_W, 6, LPM_C_BG);
    lpm_text_box(d, BODY, 10, 264, 48, LPM_ALIGN_LEFT, word, color, LPM_C_BG);
    lpm_text_box(d, BODY, word[0] ? 58 : 10, 264, word[0] ? 174 : CONTENT_W, LPM_ALIGN_LEFT, verdict, LPM_C_FG,
                 LPM_C_BG);
}
