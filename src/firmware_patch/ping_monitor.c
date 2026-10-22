/*
 * Ping monitor (output/screens/03-ping.png) in the slot of the vendor PING tool
 * (menu index 7, system state 11): one ICMP echo a second to a target, with the
 * latest round trip, loss, a graph of the last 190 round trips, and minimum,
 * average, maximum and variation (the mean change between consecutive replies).
 *
 * It opens waiting: M starts a run, which takes a DHCP address and the
 * gateway's MAC with the first three Link status checks (net.h), then pings
 * until M again, BACK, or the link going down. LEFT and RIGHT choose the target:
 * the gateway, 1.1.1.1, 8.8.8.8, and the vendor PING tool's saved target when
 * one is set. Holding M opens the vendor PING screen itself, for its static
 * address and target settings. Nothing is saved.
 *
 * The pings run in the key task, as the Link status checks do, but never block
 * it: between pings and while waiting for a reply the key queue is polled, and
 * each key goes through the vendor key table as usual (0x08016180), so BACK
 * leaves at once and M, LEFT and RIGHT arrive here as events. The network busy
 * flag (0x200000A9) is never set here: the vendor key scan (0x0801630E) drops
 * every key while it is. Instead the 1 ms tick's call that starts the vendor
 * PING's own network work is filtered (lpm_ping_tick_work). While a run goes,
 * each ping clears the Auto Off idle count (0x0800FF04), as a key would.
 *
 * The vendor calls redirected here (scripts/build_patched_image.py, PING_CALLS):
 *   0x0800D84E  key task, event 6 (M on home), PING open 0x08013794   -> lpm_ping_open
 *   0x0800D890  key task, event 7 (BACK) in state 11, 0x080137B8      -> lpm_ping_back
 *   0x0800D984  key task, events 0x25-0x2A in states 11 and 12, 0x08014778 -> lpm_ping_key
 *   0x0800FB04  GUI task, message 0x15, 0x0801378E (bx lr)            -> lpm_ping_draw
 *   0x080233E6  1 ms tick, states 11 and 12, app command 0x08015114(4) -> lpm_ping_tick_work
 * and lpm_tool_frame (link_status.c) draws the frame through lpm_ping_frame.
 *
 * Vendor interface (docs/lpm-10c-bootloader-analysis.md, "Ping monitor"):
 *   0x20000054      the key task's queue of {key, press} items
 *   0x08024940      xQueueReceive(queue, item, ticks); 0x08016180 key_match(item)
 *   0x20000306      PING and IP settings: +0x14 the PING target, +0x1E the PING
 *                   screen (0 = its main screen, from which BACK goes home)
 *   0x0800FF04      clears the Auto Off idle count 0x2000016C, as each key does
 */
#include <stdint.h>

#include "draw.h"
#include "fmt.h"
#include "lpm_theme.h"
#include "net.h"

#define SYS_STATE (*(volatile uint8_t *)0x20000150u)
#define KEY_QUEUE (*(void *volatile *)0x20000054u)
#define IP_SETTINGS ((volatile uint8_t *)0x20000306u)
#define SAVED_TARGET 0x14
#define PING_SCREEN 0x1E

#define set_sys_state ((void (*)(int))0x0800FCC1u)
#define post_message ((void (*)(int, int, int))0x0800E961u)
#define note_mark ((void (*)(int, int))0x0800E409u)
#define vendor_battery ((void (*)(void))0x0800EBE9u)
#define queue_receive ((int (*)(void *, void *, uint32_t))0x08024941u)
#define key_match ((void (*)(void *))0x08016181u)
#define vendor_ping_open ((void (*)(void))0x08013795u)
#define vendor_ping_back ((void (*)(void))0x080137B9u)
#define vendor_ping_key ((void (*)(int))0x08014779u)
#define app_command ((void (*)(int, int, int))0x08015115u)
#define idle_reset ((void (*)(void))0x0800FF05u)
#define tick ((uint32_t (*)(void))0x08023D39u)
#define delay ((void (*)(uint32_t))0x080244E5u)

#define DRAW_MESSAGE 0x15
#define EVENT_UP 0x25 /* the vendor PING and IP SCAN key events */
#define EVENT_LEFT 0x27
#define EVENT_RIGHT 0x28
#define EVENT_M 0x29
#define EVENT_M_HELD 0x2A
#define INTERVAL_MS 1000
#define KEY_POLL_MS 10
#define TIMEOUT 0xFFFFFFFFu

/* Layout, after the design. */
#define STATUS_X 24 /* clear of the port arrow, as in Link status */
#define READING_Y 72
#define READING_W 92
#define LOSS_X 152
#define PLOT_X 40
#define PLOT_Y 114
#define PLOT_H 96
#define PLOT_W (LPM_SCREEN_W - PLOT_X - LPM_MARGIN)
#define HISTORY (PLOT_W - 2) /* one round trip per pixel column */
#define STATS_Y 222
#define INFO_Y 262

enum { TARGET_GATEWAY, TARGET_CLOUDFLARE, TARGET_GOOGLE, TARGET_SAVED, TARGETS };
enum { REQUEST_STOP = 1, REQUEST_TARGET = 2, REQUEST_VENDOR = 4 };
/* phase: a NET_* result of the last start, or one of these */
enum { PHASE_STARTING = NET_READY + 1, PHASE_IDLE, PHASE_LINK_LOST };

typedef struct {
    uint32_t rtt_us[HISTORY]; /* a ring, from head; TIMEOUT for no reply */
    uint64_t sum_us;          /* over the whole run, not just the ring */
    uint64_t change_sum_us;   /* |rtt - previous rtt| over consecutive replies */
    uint32_t sent, received, min_us, max_us, changes, last_us;
    uint16_t seq;
    uint8_t head, count;
    uint8_t phase;
    uint8_t target;
    uint8_t running; /* in lpm_ping_run, so a key only asks */
    uint8_t request; /* REQUEST_*, from a key */
    uint8_t vendor;  /* the vendor PING screen is in use */
    uint8_t chain;   /* the last ping was answered */
    uint8_t mac_known;
    uint8_t plotted; /* the graph area shows the graph, not a message; the GUI task's own */
    uint8_t footer;  /* 1 + running, as the footer shows it, 0 = not drawn; the GUI task's own */
    uint8_t target_ip[4], target_mac[6];
} ping_state;

/* Above link_state and switch_state, in the RAM the vendor never uses. */
#define PING ((ping_state *)0x20020200u)

/* scripts/preview_link_status.py reads this layout. */
_Static_assert(sizeof(ping_state) == 824 && __builtin_offsetof(ping_state, seq) == 800,
               "the layout the emulator reads");

static const uint8_t cloudflare[4] = {1, 1, 1, 1}, google[4] = {8, 8, 8, 8};

/* ---- statistics ------------------------------------------------------------ */

/* n / d, rounded, without the library's 64-bit division. */
static uint32_t mean(uint64_t n, uint32_t d)
{
    uint64_t q = 0, r = 0;
    if (!d)
        return 0;
    n += d / 2;
    for (int bit = 63; bit >= 0; bit--) {
        r = r << 1 | (n >> bit & 1);
        if (r >= d) {
            r -= d;
            q |= (uint64_t)1 << bit;
        }
    }
    return (uint32_t)q;
}

static void reset(ping_state *p)
{
    p->sum_us = p->change_sum_us = 0;
    p->sent = p->received = p->min_us = p->max_us = p->changes = p->last_us = 0;
    p->seq = 0;
    p->head = p->count = 0;
    p->chain = 0;
}

static void record(ping_state *p, uint32_t rtt_us)
{
    p->rtt_us[p->head] = rtt_us;
    p->head = (uint8_t)((p->head + 1) % HISTORY);
    if (p->count < HISTORY)
        p->count++;
    p->sent++;
    if (rtt_us == TIMEOUT) {
        p->chain = 0;
        return;
    }
    if (!p->received || rtt_us < p->min_us)
        p->min_us = rtt_us;
    if (!p->received || rtt_us > p->max_us)
        p->max_us = rtt_us;
    p->received++;
    p->sum_us += rtt_us;
    if (p->chain) {
        p->change_sum_us += rtt_us > p->last_us ? rtt_us - p->last_us : p->last_us - rtt_us;
        p->changes++;
    }
    p->last_us = rtt_us;
    p->chain = 1;
}

/* The i-th round trip in the ring, oldest first. */
static uint32_t at(const ping_state *p, int i)
{
    return p->rtt_us[(p->head + HISTORY - p->count + i) % HISTORY];
}

/* ---- targets --------------------------------------------------------------- */

static const uint8_t *saved_target(void)
{
    return (const uint8_t *)&IP_SETTINGS[SAVED_TARGET];
}

static int has_saved_target(void)
{
    const uint8_t *ip = saved_target();
    return ip[0] | ip[1] | ip[2] | ip[3];
}

static const uint8_t *target_ip(int target)
{
    return target == TARGET_GATEWAY      ? lpm_net_gateway()
           : target == TARGET_CLOUDFLARE ? cloudflare
           : target == TARGET_GOOGLE     ? google
                                         : saved_target();
}

/* The next hop for the target: itself by ARP when it is on our subnet, else the gateway. */
static int resolve(ping_state *p)
{
    const uint8_t *ip = lpm_net_ip(), *mask = lpm_net_mask();
    int local = p->target == TARGET_SAVED;
    copy(p->target_ip, target_ip(p->target), 4);
    for (int i = 0; i < 4; i++)
        if ((p->target_ip[i] ^ ip[i]) & mask[i])
            local = 0;
    if (local)
        return lpm_net_arp(p->target_ip, p->target_mac);
    copy(p->target_mac, lpm_net_gateway_mac(), 6);
    return 1;
}

/* ---- the run (key task) ---------------------------------------------------- */

static void show(void)
{
    post_message(DRAW_MESSAGE, 0, 0);
}

/* Each waiting key, through the vendor key table; 1 when one asks the run to stop. */
static int poll_keys(void)
{
    uint32_t item[2] = {0, 0};
    void *queue = KEY_QUEUE;
    while (queue && queue_receive(queue, item, 0) == 1)
        key_match(item);
    return SYS_STATE != PING_SYS_STATE || PING->request;
}

/* M from the start screen, or after a stop: an address, then a ping a second
 * until M, BACK, M held, or the link going down. */
static void run(ping_state *p)
{
    p->running = 1;
    p->request = 0;
    reset(p);
    p->phase = PHASE_STARTING;
    show();
    p->phase = (uint8_t)lpm_net_prepare(); /* a fresh address each time: the cable may have moved */
    p->mac_known = 0;
    show();
    while (p->phase == NET_READY && SYS_STATE == PING_SYS_STATE && !(p->request & (REQUEST_STOP | REQUEST_VENDOR))) {
        if (p->request & REQUEST_TARGET) {
            reset(p);
            p->mac_known = 0;
            p->request &= (uint8_t)~REQUEST_TARGET;
            show();
        }
        uint32_t start = tick(), rtt_us = 0;
        if (!p->mac_known)
            p->mac_known = (uint8_t)resolve(p);
        p->seq++;
        int answered = p->mac_known && lpm_net_echo(p->target_mac, p->target_ip, p->seq, &rtt_us, poll_keys);
        if (poll_keys())
            continue; /* a key came: this ping does not count */
        if (!answered && !lpm_net_link_up()) {
            p->phase = PHASE_LINK_LOST;
            break;
        }
        record(p, answered ? rtt_us : TIMEOUT);
        idle_reset(); /* a run is activity: Auto Off waits for it to stop */
        show();
        while (tick() - start < INTERVAL_MS && !poll_keys())
            delay(KEY_POLL_MS);
    }
    p->running = 0;
    p->request &= (uint8_t)~REQUEST_STOP;
    if (p->request & REQUEST_VENDOR) {
        p->request = 0;
        p->vendor = 1;
        vendor_ping_open(); /* state 11 again, now drawn and run by the vendor */
        return;
    }
    show();
}

/* M on the Ping monitor item (event 6): enter state 11 and wait for M. */
void lpm_ping_open(void)
{
    uint8_t *b = (uint8_t *)PING;
    for (unsigned i = 0; i < sizeof(ping_state); i++)
        b[i] = 0;
    PING->phase = PHASE_IDLE;
    IP_SETTINGS[PING_SCREEN] = 0; /* as the vendor open: BACK then goes home */
    set_sys_state(PING_SYS_STATE);
}

/* BACK in state 11 (event 7): home, as the vendor PING main screen does. A run
 * sees the new state and ends. */
void lpm_ping_back(void)
{
    if (!PING->vendor)
        IP_SETTINGS[PING_SCREEN] = 0;
    vendor_ping_back();
}

/* The PING and IP SCAN key events 0x25-0x2A. While a run is going this is
 * reached from its key polling, so it only asks; otherwise it acts. */
void lpm_ping_key(int event)
{
    ping_state *p = PING;
    if (SYS_STATE != PING_SYS_STATE || p->vendor) {
        vendor_ping_key(event);
        return;
    }
    if (event == EVENT_M) {
        if (p->running)
            p->request |= REQUEST_STOP;
        else
            run(p);
    } else if (event == EVENT_LEFT || event == EVENT_RIGHT) {
        int step = event == EVENT_RIGHT ? 1 : TARGETS - 1;
        do
            p->target = (uint8_t)((p->target + step) % TARGETS);
        while (p->target == TARGET_SAVED && !has_saved_target());
        if (p->running) {
            p->request |= REQUEST_TARGET;
        } else { /* the last run's results were for the old target */
            reset(p);
            p->phase = PHASE_IDLE;
            show();
        }
    } else if (event == EVENT_M_HELD) {
        if (p->running) {
            p->request |= REQUEST_VENDOR; /* the run ends and opens it */
            return;
        }
        p->vendor = 1;
        vendor_ping_open(); /* state 11 again, now drawn and run by the vendor */
    }
}

/* Replaces the 1 ms tick's 0x08015114(4) in states 11 and 12 (0x080233E6): the
 * vendor PING and IP SCAN network work. None while Ping monitor has the screen. */
void lpm_ping_tick_work(int command, int a, int b)
{
    if (SYS_STATE == PING_SYS_STATE && !PING->vendor)
        return;
    app_command(command, a, b);
}

/* ---- screen (GUI task) ----------------------------------------------------- */

/* A round trip as the design shows it: one decimal below 100 ms, without the unit. */
static char *put_rtt(char *p, uint32_t us)
{
    if (us < 99950u) {
        unsigned tenths = (us + 50u) / 100u;
        p = put_u(p, tenths / 10);
        *p++ = '.';
        return put_u(p, tenths % 10);
    }
    return put_u(p, (us + 500u) / 1000u);
}

/* The smallest of 1, 2, 5, 10, ... ms at or above the highest round trip shown. */
static uint32_t scale_us(const ping_state *p)
{
    uint32_t peak = 0, scale = 1000;
    for (int i = 0; i < p->count; i++)
        if (at(p, i) != TIMEOUT && at(p, i) > peak)
            peak = at(p, i);
    for (int i = 0; scale < peak && scale < 5000000u; i++)
        scale = scale * (i % 3 == 1 ? 5 : 2) / (i % 3 == 1 ? 2 : 1);
    return scale;
}

static char *put_scale(char *p, uint32_t us)
{
    if (us % 1000u)
        return put_rtt(p, us);
    return put_u(p, us / 1000u);
}

/* One column of the graph from y0 to y1 (inclusive) in colour, the rest the
 * background with the grid, each pixel written once. */
static void column(int x, int y0, int y1, uint16_t colour)
{
    static const int grid[] = {PLOT_Y, PLOT_Y + PLOT_H / 2};
    int y = PLOT_Y;
    for (int g = 0; g <= 2; g++) {
        int end = g < 2 ? grid[g] : PLOT_Y + PLOT_H - 1; /* the baseline row stays */
        while (y < end) {
            int stop = y < y0 ? (y0 < end ? y0 : end) : y <= y1 ? (y1 + 1 < end ? y1 + 1 : end) : end;
            fill(x, y, 1, stop - y, y >= y0 && y <= y1 ? colour : LPM_C_BG);
            y = stop;
        }
        if (g < 2 && y == grid[g]) {
            fill(x, y, 1, 1, y >= y0 && y <= y1 ? colour : LPM_C_RULE);
            y++;
        }
    }
}

static void draw_plot(const ping_state *p)
{
    char top[12], middle[12];
    uint32_t scale = scale_us(p);
    int base = PLOT_Y + PLOT_H - 3, span = PLOT_H - 5, last_y = -1;
    put(put_scale(top, scale), "ms");
    put_scale(middle, scale / 2);
    text_field(&font_small, 0, PLOT_Y - 6, PLOT_X - 6, 1, top, LPM_C_DIM, LPM_C_BG);
    text_field(&font_small, 0, PLOT_Y + PLOT_H / 2 - 6, PLOT_X - 6, 1, middle, LPM_C_DIM, LPM_C_BG);
    text_field(&font_small, 0, PLOT_Y + PLOT_H - 7, PLOT_X - 6, 1, "0", LPM_C_DIM, LPM_C_BG);
    for (int col = 0; col < HISTORY; col++) {
        int i = col - (HISTORY - p->count), x = PLOT_X + 1 + col;
        uint32_t rtt = i >= 0 ? at(p, i) : 0;
        if (i < 0) {
            column(x, -1, -2, LPM_C_BG);
        } else if (rtt == TIMEOUT) {
            column(x, PLOT_Y + 2, PLOT_Y + 9, LPM_C_FAULT); /* a mark at the top; the line breaks */
            last_y = -1;
        } else {
            int y = base - (int)((uint64_t)(rtt < scale ? rtt : scale) * (uint32_t)span / scale);
            int y0 = last_y < 0 || y < last_y ? y : last_y, y1 = last_y > y ? last_y : y;
            column(x, y0, y1 + 1, LPM_C_ACCENT); /* two pixels thick */
            last_y = y;
        }
    }
}

static void centred(int y, const sub_font *f, const char *s, uint16_t colour)
{
    int x = (LPM_SCREEN_W - text_width(f, s)) / 2;
    text(f, x, y, s, colour, LPM_C_BG);
}

static void draw_message(ping_state *p)
{
    static const char *const lines[][2] = {
        {"No link", "Plug into the top-left port, then M"},
        {"No DHCP address", "M tries again"},
        {"The gateway does not answer ARP", "M tries again"},
        {"", ""},
        {"Getting an address", "Link, DHCP and the gateway"},
        {"Press M to start", "LEFT and RIGHT choose the target"},
    };
    fill(0, PLOT_Y - 6, LPM_SCREEN_W, PLOT_H + 12, LPM_C_BG);
    centred(PLOT_Y + 28, &font_body, lines[p->phase][0], p->phase >= PHASE_STARTING ? LPM_C_FG : LPM_C_FAULT);
    centred(PLOT_Y + 50, &font_small, lines[p->phase][1], LPM_C_DIM);
    p->plotted = 0;
}

static void stat(int x, uint32_t us, int known)
{
    char buffer[12];
    if (known)
        put_rtt(buffer, us);
    else
        put(buffer, "--");
    text_field(&font_body, x, STATS_Y + 14, 54, 0, buffer, LPM_C_FG, LPM_C_BG);
}

/* GUI message 0x15: the readings, the graph and the target, after each ping. */
void lpm_ping_draw(void)
{
    ping_state *p = PING;
    char buffer[48], *q;
    if (SYS_STATE != PING_SYS_STATE || p->vendor)
        return;
    uint32_t last = p->count ? at(p, p->count - 1) : 0;
    if (p->phase == PHASE_LINK_LOST || (p->count && last == TIMEOUT)) {
        fill(LPM_MARGIN, READING_Y, READING_W, 32, LPM_C_BG);
        text_field(&font_body, LPM_MARGIN, READING_Y + 10, READING_W + 4 + 16, 1,
                   p->phase == PHASE_LINK_LOST ? "NO LINK" : "TIMEOUT", LPM_C_FAULT, LPM_C_BG);
    } else {
        if (p->count)
            put_rtt(buffer, last);
        else
            put(buffer, "--");
        big_number(LPM_MARGIN, READING_Y, READING_W, buffer, LPM_C_FG, LPM_C_BG);
        text_field(&font_body, LPM_MARGIN + READING_W, READING_Y + 14, 4 + 16, 1, "ms", LPM_C_DIM, LPM_C_BG);
    }

    q = put(put_u(buffer, p->sent - p->received), " of ");
    put_u(q, p->sent);
    text_field(&font_body, LOSS_X, READING_Y + 2, LPM_SCREEN_W - LOSS_X - LPM_MARGIN, 0, buffer, LPM_C_FG, LPM_C_BG);
    unsigned permille = p->sent ? ((p->sent - p->received) * 1000u + p->sent / 2) / p->sent : 0;
    q = put(put_u(buffer, permille / 10), ".");
    put(put_u(q, permille % 10), "% lost");
    text_field(&font_small, LOSS_X, READING_Y + 22, LPM_SCREEN_W - LOSS_X - LPM_MARGIN, 0, buffer,
               p->sent != p->received ? LPM_C_FAULT : LPM_C_DIM, LPM_C_BG);

    /* The graph once a run has pinged, and after it stops; a message before. */
    if (p->phase != NET_READY && p->phase != PHASE_LINK_LOST && !(p->phase == PHASE_IDLE && p->count)) {
        draw_message(p);
    } else {
        if (!p->plotted) {
            fill(0, PLOT_Y - 6, LPM_SCREEN_W, PLOT_H + 12, LPM_C_BG);
            fill(PLOT_X, PLOT_Y + PLOT_H - 1, PLOT_W, 1, LPM_C_DIM);
            p->plotted = 1;
        }
        draw_plot(p);
    }

    stat(10, p->min_us, p->received != 0);
    stat(66, mean(p->sum_us, p->received), p->received != 0);
    stat(122, p->max_us, p->received != 0);
    stat(178, mean(p->change_sum_us, p->changes), p->changes != 0);

    q = put(buffer, p->target == TARGET_GATEWAY ? "To gateway " : p->target == TARGET_SAVED ? "To saved " : "To ");
    if (p->target != TARGET_GATEWAY || (p->phase == NET_READY || p->phase == PHASE_LINK_LOST))
        q = put_ip(q, target_ip(p->target)); /* the gateway is known once a run has started */
    if (p->sent)
        put_u(put(q, "  seq "), p->seq);
    text_field(&font_small, 10, INFO_Y, LPM_SCREEN_W - 20, 0, buffer, LPM_C_DIM, LPM_C_BG);

    if (p->footer != 1 + p->running) {
        draw_footer(p->running ? "M stop  <> target" : "M start  <> target", "BACK home");
        p->footer = (uint8_t)(1 + p->running);
    }
}

int lpm_ping_frame(void)
{
    if (PING->vendor)
        return 0;
    fill(0, LPM_CONTENT_Y, LPM_SCREEN_W, LPM_FOOTER_Y - LPM_CONTENT_Y, LPM_C_BG);
    draw_header("Ping monitor", STATUS_X);
    text(&font_small, 10, LPM_CONTENT_Y + 6, "LATEST RTT", LPM_C_DIM, LPM_C_BG);
    text(&font_small, LOSS_X, LPM_CONTENT_Y + 6, "LOSS", LPM_C_DIM, LPM_C_BG);
    text(&font_small, 10, STATS_Y, "MIN ms", LPM_C_DIM, LPM_C_BG);
    text(&font_small, 66, STATS_Y, "AVG ms", LPM_C_DIM, LPM_C_BG);
    text(&font_small, 122, STATS_Y, "MAX ms", LPM_C_DIM, LPM_C_BG);
    text(&font_small, 178, STATS_Y, "VAR ms", LPM_C_DIM, LPM_C_BG);
    text(&font_small, 10, INFO_Y + 14, "VAR: mean change. Hold M: setup", LPM_C_DIM, LPM_C_BG);
    PING->plotted = 0;
    PING->footer = 0;
    lpm_ping_draw();
    note_mark(2, 1); /* the vendor's arrow at the top-left port, which PING uses */
    vendor_battery();
    return 1;
}
