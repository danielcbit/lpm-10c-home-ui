/*
 * Switch port: the second tab of Link status. Listens on the DM9051 port for
 * what the switch announces about the port the tester is plugged into:
 *   LLDP (IEEE 802.1AB, 01:80:C2:00:00:0E): switch name and model, port, port
 *     VLAN (802.1 TLV), voice VLAN (LLDP-MED network policy), PoE (802.3 and
 *     LLDP-MED power TLVs), management address
 *   CDP (Cisco, 01:00:0C:CC:CC:CC, SNAP): the same, from its own TLVs
 *   802.1Q tags on any frame received: the VLANs seen on the wire
 *   802.1X: an EAPOL-Start is sent; an EAP Request/Identity in answer means
 *     the port wants authentication
 * One LLDP frame announcing an LLDP-MED endpoint is sent first: LLDP-MED
 * switches then answer within a second or two instead of at their next
 * 30-second interval ("fast start", ANSI/TIA-1057).
 *
 * The listening blocks the key task, as the Link status checks do, for at most
 * LISTEN_S seconds, and ends 3 s after the first LLDP or CDP frame. The screen
 * is drawn by link_status.c, which asks here for each row's text.
 */
#include <stdint.h>

#include "fmt.h"
#include "lpm_data.h"
#include "switch_port.h"

#define OWN_MAC ((const uint8_t *)0x20000214u)
#define FRAME ((uint8_t *)0x2000F0ACu)
#define FRAME_LEN (*(volatile uint16_t *)0x20000224u)
#define NET_BUSY (*(volatile uint8_t *)0x200000A9u)

#define post_message ((void (*)(int, int, int))0x0800E961u)
#define dm9051_read ((uint8_t (*)(int))0x08016A71u)
#define dm9051_write ((void (*)(int, int))0x08016AE1u)
#define wait_link ((int (*)(uint32_t))0x08022089u)
#define receive ((int (*)(void))0x08022811u)
#define transmit ((void (*)(void))0x080228B9u)
#define tick ((uint32_t (*)(void))0x08023D39u)
#define delay ((void (*)(uint32_t))0x080244E5u)

#define DRAW_MESSAGE 0x0C
#define NSR 1
#define NCR 0
#define RCR 5
#define RCR_ALL_MULTICAST 0x08
#define LINK_WAIT_MS 3000
#define LISTEN_S 32       /* LLDP's default interval is 30 s; CDP's 60 s is too long to block */
#define AFTER_FIRST_MS 3000 /* after the first LLDP or CDP frame, for the other and for 802.1X */
#define EAPOL_AGAIN_MS 5000
#define MAX_FRAME 1514

#define ETH_LLDP 0x88CC
#define ETH_EAPOL 0x888E
#define ETH_VLAN 0x8100

static const uint8_t lldp_group[6] = {0x01, 0x80, 0xC2, 0x00, 0x00, 0x0E};
static const uint8_t pae_group[6] = {0x01, 0x80, 0xC2, 0x00, 0x00, 0x03};
static const uint8_t cdp_group[6] = {0x01, 0x00, 0x0C, 0xCC, 0xCC, 0xCC};
static const uint8_t cdp_snap[8] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x0C, 0x20, 0x00};
static const uint8_t oui_8021[3] = {0x00, 0x80, 0xC2};
static const uint8_t oui_8023[3] = {0x00, 0x12, 0x0F};
static const uint8_t oui_med[3] = {0x00, 0x12, 0xBB};

const char *const lpm_switch_names[SWITCH_ROWS] = {"Physical link", "Switch", "Port", "VLAN",
                                                   "Management",    "PoE",    "802.1X"};
const uint8_t lpm_switch_lines[SWITCH_ROWS] = {1, 2, 2, 2, 1, 1, 1};

/* ---- parsing ------------------------------------------------------------- */

/* A string from a TLV, printable ASCII only, cut to the field. */
static void keep_text(char *to, int size, const uint8_t *from, int n)
{
    int i = 0;
    for (; i < n && i < size - 1; i++)
        to[i] = (char)(from[i] >= 0x20 && from[i] < 0x7F ? from[i] : '?');
    to[i] = 0;
}

static int printable(const uint8_t *p, int n)
{
    for (int i = 0; i < n; i++)
        if (p[i] < 0x20 || p[i] >= 0x7F)
            return 0;
    return n > 0;
}

/* An LLDP ID (chassis or port): text when it is text, else a MAC address. */
static void keep_id(char *to, int size, const uint8_t *id, int n)
{
    if (n == 6 && !printable(id, n))
        put_mac(to, id);
    else
        keep_text(to, size, id, n);
}

static void note_vlan(switch_state *w, unsigned vlan)
{
    vlan &= 0xFFF;
    if (!vlan)
        return;
    for (int i = 0; i < w->tagged_count; i++)
        if (w->tagged[i] == vlan)
            return;
    if (w->tagged_count < SWITCH_TAGGED_MAX)
        w->tagged[w->tagged_count++] = (uint16_t)vlan;
}

static void lldp_org(switch_state *w, const uint8_t *v, int n)
{
    if (n < 4)
        return;
    int sub = v[3];
    if (same(v, oui_8021, 3) && sub == 1 && n >= 6) { /* port VLAN ID */
        w->vlan = (uint16_t)(get16(v + 4) & 0xFFF);
    } else if (same(v, oui_8023, 3) && sub == 2 && n >= 7) { /* power via MDI */
        if (v[6])
            w->poe_class = v[6]; /* 1-5 = class 0-4 */
        if (n >= 12)            /* 802.3at: type, source, priority, requested, allocated (0.1 W) */
            w->poe_dw = (uint16_t)get16(v + 10);
    } else if (same(v, oui_med, 3) && sub == 2 && n >= 8 && v[4] == 1) { /* network policy, voice */
        if (!(v[5] & 0x80))                                              /* the policy is known */
            w->voice_vlan = (uint16_t)((v[5] << 7 | v[6] >> 1) & 0xFFF);
    } else if (same(v, oui_med, 3) && sub == 4 && n >= 7) { /* extended power via MDI */
        w->poe_dw = (uint16_t)get16(v + 5);
    }
}

static void lldp(switch_state *w, const uint8_t *p, const uint8_t *end)
{
    char chassis[SWITCH_TEXT];
    chassis[0] = 0;
    while (p + 2 <= end) {
        int type = p[0] >> 1, n = (p[0] & 1) << 8 | p[1];
        const uint8_t *v = p + 2;
        if (type == 0 || v + n > end)
            break;
        if (type == 1 && n >= 2)
            keep_id(chassis, sizeof chassis, v + 1, n - 1);
        else if (type == 2 && n >= 2)
            keep_id(w->port, sizeof w->port, v + 1, n - 1);
        else if (type == 4)
            keep_text(w->port_desc, sizeof w->port_desc, v, n);
        else if (type == 5)
            keep_text(w->name, sizeof w->name, v, n);
        else if (type == 6 && !w->platform[0])
            keep_text(w->platform, sizeof w->platform, v, n);
        else if (type == 8 && n >= 6 && v[0] == 5 && v[1] == 1) /* address length 5, IPv4 */
            copy(w->address, v + 2, 4);
        else if (type == 127)
            lldp_org(w, v, n);
        p = v + n;
    }
    if (!w->name[0])
        put(w->name, chassis);
    w->heard |= HEARD_LLDP;
}

static void cdp(switch_state *w, const uint8_t *p, const uint8_t *end)
{
    for (p += 4; p + 4 <= end; ) { /* version, TTL, checksum */
        unsigned type = get16(p), n = get16(p + 2);
        const uint8_t *v = p + 4;
        if (n < 4 || p + n > end)
            break;
        int len = (int)n - 4;
        if (type == 0x0001 && !(w->heard & HEARD_LLDP))
            keep_text(w->name, sizeof w->name, v, len);
        else if (type == 0x0003 && !(w->heard & HEARD_LLDP))
            keep_text(w->port, sizeof w->port, v, len);
        else if (type == 0x0006)
            keep_text(w->platform, sizeof w->platform, v, len);
        else if (type == 0x0002 && len >= 4 + 9 && v[4] == 1 && v[6] == 0xCC && get16(v + 7) == 4)
            copy(w->address, v + 9, 4); /* the first address, NLPID IP */
        else if (type == 0x000A && len >= 2 && !w->vlan)
            w->vlan = (uint16_t)(get16(v) & 0xFFF);
        else if (type == 0x000E && len >= 3)
            w->voice_vlan = (uint16_t)(get16(v + 1) & 0xFFF);
        else if (type == 0x0010 && len >= 2 && !w->poe_dw) /* power consumption, mW */
            w->poe_dw = (uint16_t)((get16(v) + 50) / 100);
        else if (type == 0x001A && len >= 8) /* power available: request, management, mW */
            w->poe_dw = (uint16_t)((((uint32_t)get16(v + 4) << 16 | get16(v + 6)) + 50) / 100);
        p += n;
    }
    w->heard |= HEARD_CDP;
}

/* One received frame. */
static void frame(switch_state *w, int n)
{
    uint8_t *f = FRAME, *end = f + (n < MAX_FRAME ? n : MAX_FRAME);
    unsigned type = get16(f + 12);
    const uint8_t *p = f + 14;
    if (n < 14 || same(f + 6, OWN_MAC, 6))
        return;
    if (type == ETH_VLAN && n >= 18) {
        note_vlan(w, get16(f + 14));
        type = get16(f + 16);
        p = f + 18;
    }
    if (type == ETH_LLDP && same(f, lldp_group, 6)) {
        lldp(w, p, end);
    } else if (type == ETH_EAPOL && p + 9 <= end && p[1] == 0 && p[4] == 1 && p[8] == 1) {
        w->dot1x = DOT1X_REQUESTED; /* EAP packet, code Request, type Identity */
    } else if (type <= 1500 && same(f, cdp_group, 6) && p + 8 + 4 <= end && same(p, cdp_snap, 8)) {
        uint8_t *cdp_end = p + type < end ? (uint8_t *)p + type : end;
        cdp(w, p + 8, cdp_end);
    }
}

/* ---- sending ------------------------------------------------------------- */

static uint8_t *ethernet(const uint8_t *to, unsigned type)
{
    uint8_t *f = FRAME;
    copy(f, to, 6);
    copy(f + 6, OWN_MAC, 6);
    put16(f + 12, type);
    return f + 14;
}

static uint8_t *tlv(uint8_t *p, int type, int n)
{
    p[0] = (uint8_t)(type << 1 | n >> 8);
    p[1] = (uint8_t)n;
    return p + 2;
}

static void send_eapol_start(void)
{
    uint8_t *p = ethernet(pae_group, ETH_EAPOL);
    p[0] = 1; /* version */
    p[1] = 1; /* EAPOL-Start */
    p[2] = p[3] = 0;
    FRAME_LEN = 60; /* the minimum frame; the rest is padding */
    for (int i = 18; i < 60; i++)
        FRAME[i] = 0;
    transmit();
}

/* Chassis and port by MAC address, TTL 120 s, LLDP-MED capabilities (class I
 * endpoint), end. */
static void send_lldp(void)
{
    uint8_t *p = ethernet(lldp_group, ETH_LLDP), *start = p;
    p = tlv(p, 1, 7);
    p[0] = 4; /* chassis ID: MAC address */
    copy(p + 1, OWN_MAC, 6);
    p = tlv(p + 7, 2, 7);
    p[0] = 3; /* port ID: MAC address */
    copy(p + 1, OWN_MAC, 6);
    p = tlv(p + 7, 3, 2);
    put16(p, 120);
    p = tlv(p + 2, 127, 7);
    copy(p, oui_med, 3);
    p[3] = 1;    /* LLDP-MED capabilities */
    put16(p + 4, 0x0001);
    p[6] = 1;    /* class I endpoint */
    p = tlv(p + 7, 0, 0);
    int n = (int)(p - start) + 14;
    for (int i = n; i < 60; i++)
        FRAME[i] = 0;
    FRAME_LEN = (uint16_t)(n < 60 ? 60 : n);
    transmit();
}

/* ---- the run (key task) -------------------------------------------------- */

static void show(void)
{
    post_message(DRAW_MESSAGE, 0, 0);
}

static void clear(switch_state *w)
{
    uint8_t tab = w->tab, drawn_tab = w->drawn_tab;
    uint8_t *b = (uint8_t *)w;
    for (unsigned i = 0; i < sizeof *w; i++)
        b[i] = 0;
    w->tab = tab; /* still on screen */
    w->drawn_tab = drawn_tab;
}

void lpm_switch_reset(void)
{
    uint8_t *b = (uint8_t *)SWITCH;
    for (unsigned i = 0; i < sizeof(switch_state); i++)
        b[i] = 0;
}

void lpm_switch_run(void)
{
    switch_state *w = SWITCH;
    clear(w);
    w->step[SW_LINK] = LPM_STEP_RUNNING;
    show();
    NET_BUSY = 1;
    int up = (dm9051_read(NSR) & 0x40) || wait_link(LINK_WAIT_MS);
    if (up) {
        w->ten_mbps = dm9051_read(NSR) >> 7 & 1;
        w->full_duplex = dm9051_read(NCR) >> 3 & 1;
    }
    w->step[SW_LINK] = up ? LPM_STEP_PASS : LPM_STEP_FAIL;
    if (up) {
        for (int row = SW_SWITCH; row < SWITCH_ROWS; row++)
            w->step[row] = LPM_STEP_RUNNING;
        show();
        uint8_t rcr = dm9051_read(RCR);
        dm9051_write(RCR, rcr | RCR_ALL_MULTICAST);
        send_lldp();
        send_eapol_start();
        uint32_t start = tick(), first = 0, eapol = start, second = start;
        for (;;) {
            uint32_t now = tick();
            if (now - start >= LISTEN_S * 1000u || (first && now - first >= AFTER_FIRST_MS))
                break;
            if (now - second >= 1000) { /* the countdown */
                second += 1000;
                w->seconds = (uint8_t)((now - start) / 1000);
                show();
            }
            if (!w->dot1x && now - eapol >= EAPOL_AGAIN_MS) {
                eapol = now;
                send_eapol_start();
            }
            if (!(dm9051_read(NSR) & 1)) {
                delay(5);
                continue;
            }
            int n = receive();
            if (n <= 0) { /* the vendor receive resets the receive control after an error */
                dm9051_write(RCR, rcr | RCR_ALL_MULTICAST);
                continue;
            }
            frame(w, n);
            if (w->heard && !first)
                first = tick();
        }
        dm9051_write(RCR, rcr);
        int heard = w->heard != 0;
        w->step[SW_SWITCH] = heard ? LPM_STEP_PASS : STEP_NONE;
        w->step[SW_PORT] = w->port[0] ? LPM_STEP_PASS : STEP_NONE;
        w->step[SW_VLAN] = w->vlan || w->voice_vlan || w->tagged_count ? LPM_STEP_PASS : STEP_NONE;
        w->step[SW_ADDRESS] = w->address[0] ? LPM_STEP_PASS : STEP_NONE;
        w->step[SW_POE] = w->poe_class || w->poe_dw ? LPM_STEP_PASS : STEP_NONE;
        w->step[SW_DOT1X] = w->dot1x ? LPM_STEP_WARN : STEP_NONE;
    } else {
        for (int row = SW_SWITCH; row < SWITCH_ROWS; row++)
            w->step[row] = LPM_STEP_SKIPPED;
    }
    NET_BUSY = 0;
    show();
}

/* ---- text (GUI task) ----------------------------------------------------- */

static char *put_vlans(char *p, const switch_state *w)
{
    for (int i = 0; i < w->tagged_count; i++) {
        p = put(p, i ? ", " : "Tagged: VLAN ");
        p = put_u(p, w->tagged[i]);
    }
    return p;
}

void lpm_switch_detail(int row, char *p)
{
    const switch_state *w = SWITCH;
    int step = w->step[row];
    *p = 0;
    if (step == LPM_STEP_PENDING && row == SW_SWITCH) {
        put(p, "M listens for LLDP, CDP and 802.1X");
        return;
    }
    if (step == LPM_STEP_RUNNING && row == SW_SWITCH) {
        put(put(put_u(put(p, "Listening for LLDP and CDP, "), w->seconds), " of "), "32 s");
        return;
    }
    if (step == LPM_STEP_RUNNING || step == LPM_STEP_PENDING || step == LPM_STEP_SKIPPED)
        return;
    switch (row) {
    case SW_LINK:
        if (step == LPM_STEP_PASS)
            put(put(p, w->ten_mbps ? "10 Mb/s " : "100 Mb/s "), w->full_duplex ? "full duplex" : "half duplex");
        else
            put(p, "No link detected");
        break;
    case SW_SWITCH:
        if (step == STEP_NONE) {
            put(p, "No LLDP or CDP in 32 s\nThe switch may not send them");
            break;
        }
        p = put(put(p, w->name[0] ? w->name : "(no name)"), w->heard == (HEARD_LLDP | HEARD_CDP) ? " (LLDP, CDP)"
                                                             : w->heard & HEARD_LLDP           ? " (LLDP)"
                                                                                               : " (CDP)");
        put(put(p, "\n"), w->platform);
        break;
    case SW_PORT:
        if (step == STEP_NONE)
            put(p, "Not announced");
        else
            put(put(put(p, w->port), "\n"), w->port_desc);
        break;
    case SW_VLAN:
        if (step == STEP_NONE) {
            put(p, "Not announced, no tagged frames");
            break;
        }
        if (w->vlan)
            p = put_u(put(p, "Port VLAN "), w->vlan);
        if (w->voice_vlan)
            p = put_u(put(p, w->vlan ? ", voice " : "Voice VLAN "), w->voice_vlan);
        put_vlans(put(p, "\n"), w);
        break;
    case SW_ADDRESS:
        if (step == STEP_NONE)
            put(p, "Not announced");
        else
            put_ip(p, w->address);
        break;
    case SW_POE:
        if (step == STEP_NONE) {
            put(p, "Not announced");
            break;
        }
        if (w->poe_class)
            p = put_u(put(p, "Class "), w->poe_class - 1u);
        if (w->poe_dw) {
            p = put_u(put(p, w->poe_class ? ", " : ""), w->poe_dw / 10u);
            p = put(put_u(put(p, "."), w->poe_dw % 10u), " W");
        }
        break;
    case SW_DOT1X:
        put(p, w->dot1x ? "Required: the switch asked who we are" : "Not required (no EAP request)");
        break;
    }
}
