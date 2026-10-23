/*
 * Link status (output/screens/11-link-checklist.png) for the vendor LPM-10C TX
 * firmware V2.0.7, in the slot of the vendor QC test (menu index 4, state 8).
 *
 * Checks in order, each PASS or FAIL, on the DM9051 10/100 port that PING and
 * IP SCAN use:
 *   1. Physical link, with speed and duplex
 *   2. An address by DHCP: PASS with a lease (ACK), WARN with an OFFER only
 *   3. The gateway answers ARP
 *   4. The gateway answers ping (an ICMP echo built here)
 *   5. 1.1.1.1, or else 8.8.8.8, answers ping through the gateway
 *   6. A DNS lookup of example.com: PASS from the DNS server DHCP gave, WARN
 *      when only 1.1.1.1 answers, FAIL when neither does; the answer's address
 *      and the server are shown
 * then IPv6, which needs only the link:
 *   7. A router advertisement, asked for with a router solicitation
 *   8. A SLAAC address from its prefix, NONE when it offers none
 *   9. A DHCPv6 address (SOLICIT, ADVERTISE, REQUEST, REPLY): PASS with a
 *      lease, WARN with an ADVERTISE only, NONE without a server when the
 *      router does not ask for one (M flag clear), FAIL when it does
 *  10. The router answers ping (ICMPv6 echo)
 *  11. 2606:4700:4700::1111, or else 2001:4860:4860::8888, answers ping
 * A failed check skips the ones after it in its family, except that the last
 * three IPv4 checks all run once the gateway is found: many routers ignore ping
 * but still route and resolve, so one failing says little about the next.
 * Nothing is saved: the vendor's own Auto Config IP writes its result to the
 * settings, this never touches them.
 *
 * It opens waiting: M runs the checks, and M again runs them afresh.
 *
 * The rows are a list taller than the screen: UP and DOWN scroll it a row at a
 * time, and while the checks run it follows the one running. LEFT and RIGHT
 * switch to the second tab, Switch port (switch_port.c), where M listens for
 * what the switch announces about the port instead.
 *
 * Linked with home_ui.c by scripts/build_patched_image.py. The vendor calls
 * redirected here (all inside the vendor dispatchers, so tasks are unchanged):
 *   0x0800D830  key task, event 6 (M on home), QC open 0x0800BAE4 -> lpm_link_open
 *   0x0800D8CC  key task, event 0x0B, QC count init 0x0800BD98    -> lpm_link_run
 *   0x0800FABE  GUI task, message 0x0C, QC draw 0x0800BB0C        -> lpm_link_draw
 *   0x0800FC20  GUI task, message 0x36, tool frame 0x0800EE8C     -> lpm_tool_frame
 *   0x0800D802  key task, events 2 to 5 (UP, DOWN, LEFT, RIGHT), via lpm_home_key -> lpm_link_key
 * plus data patches: the key table entry for state 8 becomes M (key 4, short)
 * -> event 0x0B, the UP, DOWN, LEFT and RIGHT entries of the home screen also match state 8,
 * and the 1 s tick no longer starts a QC count in state 8.
 *
 * The checks block the key task, as the vendor Auto Config IP does; the
 * watchdog is fed from a timer interrupt (0x08019DEC), not from a task. The
 * GUI task draws: each step posts message 0x0C.
 *
 * Vendor interface (docs/lpm-10c-bootloader-analysis.md, "Link status"):
 *   0x20000150[0]   system state; 0x0800FCC0 set_sysState(n) posts message 0x36
 *   0x200000A9      network busy: while set, the 1 s tick queues no network work
 *   0x20000214      own MAC; 0x2000F0AC frame buffer; 0x20000224 frame length
 *   0x08016A70      dm9051_read(reg): reg 1 bit 0 frame waiting, bit 6 link,
 *                   bit 7 10 Mb/s; reg 0 bit 3 full duplex; reg 5 receive control
 *   0x08016AE0      dm9051_write(reg, value)
 *   0x08022088      wait_link(ms); 0x08022810 receive(); 0x080228B8 transmit()
 *   0x0801FE38      dhcp_send(type, xid, ip, server)
 *   0x0801FFD4      dhcp_wait(xid, ms, type, ip, mask, gateway, server); returns
 *                   at the matching reply, which stays in the frame buffer with
 *                   the options it does not read (option 6, DNS servers)
 *   0x0802144C      arp_request(target, sender); 0x08021FEC arp_wait(ip, ms)
 *   0x08023D38      tick count (1 ms); 0x080244E4 delay(ms)
 *   0x0800EBE8      battery icon
 *   0x0800E408      note_mark(index, on): the vendor's green arrow at the port a
 *                   tool uses; index 2 is the top-left port of PING and IP SCAN
 */
#include <stdint.h>

#include "draw.h"
#include "fmt.h"
#include "lpm_data.h"
#include "lpm_theme.h"
#include "net.h"
#include "switch_port.h"
#include "tools.h"

/* The vendor image ends its RAM at 0x20011298 (the initial stack top); nothing
 * above it is ever addressed, and SRAM dumps show its power-up pattern intact.
 * The state is never assumed zero: lpm_link_open clears it first. */
#define LINK ((link_state *)0x20020000u)

#define SYS_STATE (*(volatile uint8_t *)0x20000150u)
#define NET_BUSY (*(volatile uint8_t *)0x200000A9u)
#define OWN_MAC ((const uint8_t *)0x20000214u)
#define FRAME ((uint8_t *)0x2000F0ACu)
#define FRAME_LEN (*(volatile uint16_t *)0x20000224u)
#define DEMCR (*(volatile uint32_t *)0xE000EDFCu)
#define DWT_CTRL (*(volatile uint32_t *)0xE0001000u)
#define DWT_CYCCNT (*(volatile uint32_t *)0xE0001004u)
#define CYCLES_PER_US 144 /* SysTick reloads at 143,999 for a 1 ms tick */

#define set_sys_state ((void (*)(int))0x0800FCC1u)
#define post_message ((void (*)(int, int, int))0x0800E961u)
#define vendor_tool_frame ((void (*)(void))0x0800EE8Du)
#define vendor_battery ((void (*)(void))0x0800EBE9u)
#define note_mark ((void (*)(int, int))0x0800E409u)
#define dm9051_read ((uint8_t (*)(int))0x08016A71u)
#define dm9051_write ((void (*)(int, int))0x08016AE1u)
#define wait_link ((int (*)(uint32_t))0x08022089u)
#define vendor_receive ((int (*)(void))0x08022811u)
#define vendor_transmit ((void (*)(void))0x080228B9u)
#define receive lpm_vlan_receive /* tagged while a VLAN test runs */
#define transmit lpm_vlan_transmit
#define KEY_QUEUE (*(void *volatile *)0x20000054u)
#define queue_receive ((int (*)(void *, void *, uint32_t))0x08024941u)
#define key_match ((void (*)(void *))0x08016181u)
#define dhcp_send ((void (*)(int, uint32_t, const uint8_t *, const uint8_t *))0x0801FE39u)
#define dhcp_wait ((int (*)(uint32_t, uint32_t, int, uint8_t *, uint8_t *, uint8_t *, uint8_t *))0x0801FFD5u)
#define arp_request ((void (*)(const uint8_t *, const uint8_t *))0x0802144Du)
#define arp_wait ((int (*)(const uint8_t *, uint32_t))0x08021FEDu)
#define tick ((uint32_t (*)(void))0x08023D39u)
#define delay ((void (*)(uint32_t))0x080244E5u)

#define DRAW_MESSAGE 0x0C
#define KEY_UP 2 /* events of the home screen's UP and DOWN entries */
#define KEY_DOWN 3
#define KEY_LEFT 4
#define KEY_RIGHT 5
#define TPID_VLAN 0x8100
#define VLAN_MAX 4094
#define VLAN_DIGITS 4
#define VLAN_FIELD_X 96
#define NSR 1
#define NCR 0
#define RCR 5
#define RCR_ALL_MULTICAST 0x08 /* the vendor sets 0x31 and a hash that passes broadcast only */
#define DHCP_DISCOVER 1
#define DHCP_OFFER 2
#define DHCP_REQUEST 3
#define DHCP_ACK 5
#define DHCP_WAIT_MS 1500 /* the vendor's own timing */
#define LINK_WAIT_MS 3000 /* auto-negotiation after plugging in */
#define TRIES 2
#define PINGS 3
#define REPLY_WAIT_MS 1000
#define BUSY_POLL_MS 20 /* poll without sleeping this long, so a fast reply is timed in microseconds */
#define ECHO_ID 0x4C50
#define INTERNET_PINGS 2 /* to each address */
#define DNS_TRIES 2      /* to each server */
#define SOLICITATIONS 4  /* 1 s apart: a router may hold a multicast answer up to 3 s (RFC 4861) */
#define ECHO_BYTES 32
#define PROTO_ICMP 1
#define PROTO_UDP 17
#define PROTO_ICMP6 58
#define DNS_PORT 53
#define LOCAL_PORT 50000
#define DHCP6_CLIENT_PORT 546
#define DHCP6_SERVER_PORT 547
#define DHCP_OPTIONS (8 + 236 + 4) /* after the UDP header: BOOTP fields and the magic cookie */
#define DHCP_DNS 6
#define MAX_FRAME 1514
#define SERVER_ID_MAX 32 /* a DUID longer than this is not kept, and no REQUEST is sent */

/* ICMPv6 and DHCPv6 message types */
#define ROUTER_SOLICIT 133
#define ROUTER_ADVERT 134
#define NEIGHBOUR_SOLICIT 135
#define NEIGHBOUR_ADVERT 136
#define ECHO6_REQUEST 128
#define ECHO6_REPLY 129
#define DHCP6_SOLICIT 1
#define DHCP6_ADVERTISE 2
#define DHCP6_REQUEST 3
#define DHCP6_REPLY 7
#define RA_MANAGED 0x80 /* M: addresses from DHCPv6 */
#define PREFIX_AUTONOMOUS 0x40

/* Rows: the six IPv4 checks, a heading, then the five IPv6 checks. */
#define ROWS 12
#define ROW_IPV6 6
#define ROW_ROUTER 7
#define ROW_SLAAC 8
#define ROW_DHCP6 9
#define ROW_PING6 10
#define ROW_INTERNET6 11

/* Layout: a check is its name and verdict, then its detail lines. */
#define VIEW_Y 79 /* below the tab rule */
#define ROW_Y0 84
#define DETAIL_DY 17
#define LINE_PITCH 14
#define HEADING_H 22
#define DETAIL_W 224
#define DETAIL_CHARS 37 /* of the 6-pixel small font in DETAIL_W */
#define BAR_X 235
#define TAB_W 112 /* one of the design's two tabs */
/* The arrow at the top-left port, the DM9051 port PING and IP SCAN also use
 * (x 3-19, y 4-12): the status text moves right of it. */
#define PORT_MARK 2
#define STATUS_X 24

typedef struct {
    uint32_t rtt_us, internet_rtt_us, dns_rtt_us, rtt6_us, internet6_rtt_us;
    uint16_t router_lifetime; /* seconds; 0 = not a default router */
    uint8_t step[ROWS];       /* lpm_step, or STEP_NONE */
    uint8_t source;           /* lpm_addr_source */
    uint8_t ten_mbps;
    uint8_t full_duplex;
    uint8_t internet;   /* 1 + the internet_hosts entry that answered, or 0 */
    uint8_t dns_answer; /* DNS_BY_DHCP_SERVER, DNS_BY_FALLBACK or 0 */
    uint8_t ra_flags;   /* of the router advertisement: RA_MANAGED, ... */
    uint8_t prefix_len; /* of the prefix shown, 0 = none advertised */
    uint8_t prefix_flags;
    uint8_t dhcp6;     /* dhcp6_result */
    uint8_t internet6; /* 1 + the internet6_hosts entry that answered, or 0 */
    uint8_t in_use;    /* IN_USE_*: another host defends this address (duplicate address detection) */
    uint8_t server_id_len;
    uint8_t top;       /* the first row shown; the key task scrolls it */
    uint8_t drawn_top; /* the first row on screen; the GUI task's own */
    uint8_t rcr;       /* the receive control to restore, while all multicast passes */
    uint8_t ip[4], mask[4], gateway[4], server[4];
    uint8_t dns[4];    /* the first DNS server in the DHCP reply, or 0.0.0.0 */
    uint8_t dns_ip[4]; /* the address in the answer */
    uint8_t gateway_mac[6];
    uint8_t router_mac[6];
    uint8_t link_local[16], router[16], prefix[16], slaac[16], leased[16];
    uint8_t xid6[3];
    uint8_t server_id[SERVER_ID_MAX];
} link_state;

/* scripts/preview_link_status.py reads this layout. */
_Static_assert(sizeof(link_state) == 200 && __builtin_offsetof(link_state, top) == 46 &&
                   __builtin_offsetof(link_state, server_id) == 168,
               "the layout the emulator reads");

enum { DNS_BY_DHCP_SERVER = 1, DNS_BY_FALLBACK };
enum { DHCP6_NO_REPLY, DHCP6_NO_ADDRESS, DHCP6_ADVERTISED, DHCP6_LEASED };
enum { IN_USE_SLAAC = 1, IN_USE_LEASED = 2 };
/* What exchange sends and waits for. */
enum { ECHO, DNS_QUERY, SOLICIT_ROUTER, ECHO6, SOLICIT_DHCP6, REQUEST_DHCP6 };

static const char *const names[ROWS] = {"Physical link", "Address", "Gateway ARP", "Gateway ping", "Internet ping",
                                        "DNS lookup",    "IPv6",    "Router",      "SLAAC",        "DHCPv6",
                                        "Gateway ping",  "Internet ping"};
static const uint8_t detail_lines[ROWS] = {1, 1, 1, 1, 1, 2, 0, 2, 2, 3, 1, 1};
static const uint8_t internet_hosts[][4] = {{1, 1, 1, 1}, {8, 8, 8, 8}}; /* Cloudflare, then Google */
static const uint8_t internet6_hosts[][16] = {
    {0x26, 0x06, 0x47, 0x00, 0x47, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0x11, 0x11},  /* Cloudflare */
    {0x20, 0x01, 0x48, 0x60, 0x48, 0x60, 0, 0, 0, 0, 0, 0, 0, 0, 0x88, 0x88}}; /* Google */
static const uint8_t all_routers[16] = {0xFF, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2};
static const uint8_t dhcp6_servers[16] = {0xFF, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 2};
static const uint8_t unspecified[16];
static const char domain[] = "example.com"; /* RFC 2606, reserved for examples */
/* domain, type A, class IN */
static const uint8_t question[] = {7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'c', 'o', 'm', 0, 0, 1, 0, 1};
static const char *const verdicts[] = {"--", "TESTING", "PASS", "WARN", "FAIL", "SKIPPED", "NONE"};
static const uint16_t verdict_colours[] = {LPM_C_DIM, LPM_C_DIM,   LPM_C_PASS, LPM_C_WARN,
                                           LPM_C_FAULT, LPM_C_DIM, LPM_C_DIM};

/* ---- text --------------------------------------------------------------- */

/* RFC 5952: lower case, no leading zeros, the longest run of two or more zero
 * groups (the first of equals) as "::". */
static char *put_ip6(char *p, const uint8_t *ip)
{
    int run = 0, at = -1;
    for (int i = 0, n = 0; i < 8; i++) {
        n = ip[2 * i] | ip[2 * i + 1] ? 0 : n + 1;
        if (n > run && n >= 2) {
            run = n;
            at = i - n + 1;
        }
    }
    for (int i = 0; i < 8; i++) {
        if (i == at) {
            p = put(p, "::");
            i += run - 1;
            continue;
        }
        if (i && i != at + run)
            *p++ = ':';
        unsigned group = (unsigned)(ip[2 * i] << 8 | ip[2 * i + 1]);
        int digits = group >> 12 ? 4 : group >> 8 ? 3 : group >> 4 ? 2 : 1;
        p = put_hex(p, group, digits, "0123456789abcdef");
    }
    *p = 0;
    return p;
}

/* As lpm_str_ms: one decimal below 100 ms. */
static char *put_ms(char *p, uint32_t us)
{
    if (us < 99950u) {
        unsigned tenths = (us + 50u) / 100u;
        p = put_u(p, tenths / 10);
        *p++ = '.';
        p = put_u(p, tenths % 10);
    } else {
        p = put_u(p, (us + 500u) / 1000u);
    }
    return put(p, " ms");
}

static int known(const uint8_t *ip)
{
    return ip[0] | ip[1] | ip[2] | ip[3];
}

static int known6(const uint8_t *ip)
{
    return !same(ip, unspecified, 16);
}

static int link_local(const uint8_t *ip)
{
    return ip[0] == 0xFE && (ip[1] & 0xC0) == 0x80;
}

/* The global address IPv6 pings go from: SLAAC, else the DHCPv6 lease. */
static const uint8_t *global6(const link_state *s)
{
    if (s->step[ROW_SLAAC] == LPM_STEP_PASS)
        return s->slaac;
    if (s->step[ROW_DHCP6] == LPM_STEP_PASS || s->step[ROW_DHCP6] == LPM_STEP_WARN)
        return s->leased;
    return 0;
}

/* The detail under a check, in the words of lpm_screen_health; '\n' starts a line. */
static void detail(const link_state *s, int row, char *p)
{
    int step = s->step[row];
    *p = 0;
    if (row == 0) {
        if (step == LPM_STEP_PASS) {
            p = put(p, s->ten_mbps ? "10 Mb/s " : "100 Mb/s ");
            put(p, s->full_duplex ? "full duplex" : "half duplex");
        } else if (step == LPM_STEP_FAIL) {
            put(p, "No link detected");
        } else if (step == LPM_STEP_PENDING) {
            put(p, "Press M to start the checks"); /* before the first run */
        }
    } else if (row == 1) {
        if (s->source != LPM_ADDR_NONE) {
            p = put_ip(p, s->ip);
            put(p, s->source == LPM_ADDR_DHCP ? " DHCP lease" : " OFFER only, no lease");
        } else if (step == LPM_STEP_FAIL) {
            put(p, "No address obtained");
        }
    } else if (row == 2) {
        if (step == LPM_STEP_FAIL && !known(s->gateway)) {
            put(p, "No gateway known");
        } else if (step == LPM_STEP_PASS || step == LPM_STEP_FAIL) {
            p = put_ip(p, s->gateway);
            *p++ = ' ';
            if (step == LPM_STEP_PASS)
                put_mac(p, s->gateway_mac);
            else
                put(p, "no ARP reply");
        }
    } else if (row == 3 || row == ROW_PING6) {
        if (step == LPM_STEP_PASS)
            put_ms(put(p, "RTT "), row == 3 ? s->rtt_us : s->rtt6_us);
        else if (step == LPM_STEP_FAIL)
            put(p, "No echo reply (may be filtered)");
    } else if (row == 4) {
        if (step == LPM_STEP_PASS)
            put_ms(put(put_ip(p, internet_hosts[s->internet - 1]), " RTT "), s->internet_rtt_us);
        else if (step == LPM_STEP_FAIL)
            put(p, "No reply from 1.1.1.1 or 8.8.8.8");
    } else if (row == 5) {
        if (s->dns_answer) {
            p = put_ip(put(put(p, domain), " = "), s->dns_ip);
            p = put_ip(put(p, "\nvia "), s->dns_answer == DNS_BY_FALLBACK ? internet_hosts[0] : s->dns);
            if (s->dns_answer == DNS_BY_DHCP_SERVER)
                put_ms(put(p, " (DHCP), "), s->dns_rtt_us);
            else
                put(put_ms(put(p, ", "), s->dns_rtt_us), known(s->dns) ? ", DHCP DNS failed" : ", no DHCP DNS");
        } else if (step == LPM_STEP_FAIL) {
            p = put(put(p, domain), ": no answer\n");
            if (!known(s->dns)) {
                put(p, "No DHCP DNS, asked 1.1.1.1");
            } else {
                p = put_ip(put(p, "Asked "), s->dns);
                if (!same(s->dns, internet_hosts[0], 4))
                    put(p, ", 1.1.1.1");
            }
        }
    } else if (row == ROW_ROUTER) {
        if (known6(s->router)) {
            p = put_ip6(p, s->router);
            if (!s->router_lifetime)
                put(p, "\nLifetime 0: not a default router");
            else if (s->prefix_len)
                put_u(put(put_ip6(put(p, "\nPrefix "), s->prefix), "/"), s->prefix_len);
            else
                put(p, "\nNo prefix advertised");
        } else if (step == LPM_STEP_FAIL) {
            put(p, "No router advertisement");
        }
    } else if (row == ROW_SLAAC) {
        if (step == LPM_STEP_PASS || step == LPM_STEP_FAIL) {
            p = put_ip6(p, s->slaac);
            if (step == LPM_STEP_FAIL)
                put(p, "\nIn use by another host (DAD)");
        } else if (step == STEP_NONE) {
            put(p, "The router offers no SLAAC prefix");
        }
    } else if (row == ROW_DHCP6) {
        if (s->dhcp6 >= DHCP6_ADVERTISED) {
            p = put(put_ip6(p, s->leased), "\n");
            put(p, s->in_use & IN_USE_LEASED      ? "In use by another host (DAD)"
                   : s->dhcp6 == DHCP6_LEASED ? "DHCPv6 lease"
                                              : "ADVERTISE only, no lease");
        } else if (s->dhcp6 == DHCP6_NO_ADDRESS) {
            put(p, "The server offers no address");
        } else if (step == LPM_STEP_FAIL) {
            put(p, "No reply, though the router asks for DHCPv6 (M flag)");
        } else if (step == STEP_NONE) {
            put(p, "No server; the router does not ask for one");
        }
    } else if (row == ROW_INTERNET6) {
        if (step == LPM_STEP_PASS)
            put_ms(put(put_ip6(p, internet6_hosts[s->internet6 - 1]), " RTT "), s->internet6_rtt_us);
        else if (step == LPM_STEP_FAIL)
            put(p, "No reply from Cloudflare or Google");
        else if (step == LPM_STEP_SKIPPED && s->router_lifetime && !global6(s))
            put(p, "No global address");
    }
}

/* ---- screen (GUI task) -------------------------------------------------- */

/* ---- the lists --------------------------------------------------------------- */

static void clear(link_state *s);

static int switch_tab(void)
{
    return SWITCH->tab != 0;
}

const char *lpm_verdict(unsigned step, uint16_t *colour)
{
    if (step > STEP_NONE)
        step = LPM_STEP_PENDING;
    *colour = verdict_colours[step];
    return verdicts[step];
}

const char *lpm_link_row(int row, const char **verdict, uint16_t *colour, char *p)
{
    *verdict = lpm_verdict(LINK->step[row], colour);
    detail(LINK, row, p);
    return names[row];
}

static int link_count(void)
{
    return ROWS;
}

static int vlan_count(void)
{
    return ROW_IPV6; /* the IPv4 checks */
}

static int link_lines(int row)
{
    return detail_lines[row];
}

static uint8_t *link_top(void)
{
    return &LINK->top;
}

static int switch_count(void)
{
    return SWITCH_ROWS;
}

static int switch_lines(int row)
{
    return lpm_switch_lines[row];
}

static const char *switch_row(int row, const char **verdict, uint16_t *colour, char *p)
{
    *verdict = lpm_verdict(SWITCH->step[row], colour);
    lpm_switch_detail(row, p);
    return lpm_switch_names[row];
}

static uint8_t *switch_top(void)
{
    return &SWITCH->top;
}

static const row_list checklist = {link_count, link_lines, lpm_link_row, link_top};
static const row_list vlan_list = {vlan_count, link_lines, lpm_link_row, link_top};
static const row_list switch_list = {switch_count, switch_lines, switch_row, switch_top};

/* The rows shown: the tool's, or in Link status the tab's. */
static const row_list *list(void)
{
    switch (TOOLS->tool) {
    case TOOL_VLAN:
        return &vlan_list;
    case TOOL_DUPLICATE:
        return &lpm_duplicate_list;
    }
    return switch_tab() ? &switch_list : &checklist;
}

static int rows(void)
{
    return list()->count();
}

static uint8_t *top_row(void)
{
    return list()->top();
}

/* ---- screen (GUI task) -------------------------------------------------- */

static int row_height(int row)
{
    int lines = list()->lines(row);
    return lines ? DETAIL_DY + 2 + LINE_PITCH * lines : HEADING_H;
}
/* 1 when row, drawn below the rows from top, ends above the footer. */
static int fits(int top, int row)
{
    int y = ROW_Y0;
    for (int r = top; r < row; r++)
        y += row_height(r);
    return y + row_height(row) - 6 <= LPM_FOOTER_Y;
}

static int last_top(void)
{
    int top = 0;
    while (rows() && !fits(top, rows() - 1))
        top++;
    return top;
}

/* The next line of s into out: up to the end, a '\n', or as many characters as
 * fit, broken after the last ' ', ':' or ','. Returns the rest. */
static const char *next_line(const char *s, char *out)
{
    int n = 0, cut = 0;
    while (s[n] && s[n] != '\n' && n < DETAIL_CHARS) {
        if (s[n] == ' ' || s[n] == ':' || s[n] == ',')
            cut = n + 1;
        n++;
    }
    if (s[n] && s[n] != '\n' && cut)
        n = cut;
    copy((uint8_t *)out, (const uint8_t *)s, n);
    out[n] = 0;
    s += n;
    if (*s == '\n' || *s == ' ')
        s++;
    return s;
}

static void draw_row(int row, int y)
{
    char buffer[128], line[DETAIL_CHARS + 1];
    const char *verdict = "";
    uint16_t colour = LPM_C_DIM;
    const row_list *l = list();
    buffer[0] = 0;
    const char *name = l->row(row, &verdict, &colour, buffer);
    int lines = l->lines(row);
    if (!lines) { /* a heading */
        text(&font_body, 10, y + 3, name, LPM_C_ACCENT, LPM_C_BG);
        fill(52, y + 6, DETAIL_W - 42, 1, LPM_C_RULE);
        return;
    }
    text_field(&font_body, 10, y, 140, 0, name, LPM_C_FG, LPM_C_BG);
    text_field(&font_body, 150, y, 82, 1, verdict, colour, LPM_C_BG);
    const char *rest = buffer;
    for (int i = 0; i < lines; i++) {
        rest = next_line(rest, line);
        text_field(&font_small, 10, y + DETAIL_DY + LINE_PITCH * i, DETAIL_W, 0, line, LPM_C_DIM, LPM_C_BG);
    }
}

/* The design's two tabs, the one shown underlined in the accent, over a rule. */
static void draw_tabs(void)
{
    static const char *const tabs[2] = {"Checklist", "Switch port"};
    for (int i = 0; i < 2; i++) {
        int x = LPM_MARGIN + i * TAB_W, on = i == SWITCH->tab;
        fill(x, LPM_CONTENT_Y, TAB_W, 19, LPM_C_BG);
        text(&font_body, x + (TAB_W - text_width(&font_body, tabs[i])) / 2, LPM_CONTENT_Y + 4, tabs[i],
             on ? LPM_C_FG : LPM_C_DIM, LPM_C_BG);
        fill(x, LPM_CONTENT_Y + 19, TAB_W, 3, on ? LPM_C_ACCENT : LPM_C_BG);
    }
    fill(LPM_MARGIN, LPM_CONTENT_Y + 22, LPM_SCREEN_W - 2 * LPM_MARGIN, 1, LPM_C_RULE);
    SWITCH->drawn_tab = SWITCH->tab;
}

/* The line under the title in the added tools, where Link status has its tabs:
 * the VLAN to test, with the digit edited on the accent, or the tool's summary. */
static void draw_heading(void)
{
    char buffer[48];
    tools_state *t = TOOLS;
    fill(0, LPM_CONTENT_Y, LPM_SCREEN_W, VIEW_Y - LPM_CONTENT_Y - 1, LPM_C_BG);
    if (t->tool == TOOL_VLAN) {
        text(&font_body, 10, LPM_CONTENT_Y + 4, "VLAN ID", LPM_C_DIM, LPM_C_BG);
        for (int i = 0, place = 1000; i < VLAN_DIGITS; i++, place /= 10) {
            char digit[2] = {(char)('0' + t->vlan / place % 10), 0};
            int on = i == t->digit, x = VLAN_FIELD_X + i * 12;
            fill(x, LPM_CONTENT_Y + 1, 12, 20, on ? LPM_C_ACCENT : LPM_C_FIELD);
            text(&font_body, x + 2, LPM_CONTENT_Y + 4, digit, on ? LPM_C_ON_ACCENT : LPM_C_FG,
                 on ? LPM_C_ACCENT : LPM_C_FIELD);
        }
        text(&font_small, VLAN_FIELD_X + 56, LPM_CONTENT_Y + 6, "tagged", LPM_C_DIM, LPM_C_BG);
    } else {
        const char *line = lpm_duplicate_heading(buffer);
        text_field(&font_small, 10, LPM_CONTENT_Y + 6, DETAIL_W, 0, line, LPM_C_DIM, LPM_C_BG);
    }
    fill(LPM_MARGIN, VIEW_Y - 1, LPM_SCREEN_W - 2 * LPM_MARGIN, 1, LPM_C_RULE);
}

/* Replaces the QC draw for message 0x0C: the rows that fit from the top one,
 * redrawn after every step, every scroll and every change of tab. */
void lpm_link_draw(void)
{
    link_state *s = LINK;
    if (SYS_STATE != LINK_SYS_STATE) /* Ping monitor uses the checks below, which post this message */
        return;
    if (TOOLS->tool != TOOL_LINK) {
        draw_heading();
    } else if (SWITCH->drawn_tab != SWITCH->tab) {
        draw_tabs();
        s->drawn_top = 0xFF; /* no row on screen belongs to this tab */
    }
    int top = *top_row(), end = last_top();
    if (top > end)
        top = end;
    /* Rows differ in height, so after a scroll the gaps between lines are cleared too. */
    if (top != s->drawn_top) {
        fill(0, VIEW_Y, BAR_X, LPM_FOOTER_Y - VIEW_Y, LPM_C_BG);
        s->drawn_top = (uint8_t)top;
    }
    for (int row = top, y = ROW_Y0; row < rows() && fits(top, row); y += row_height(row++))
        draw_row(row, y);
    if (!rows()) {
        fill(BAR_X, VIEW_Y, 3, LPM_FOOTER_Y - 4 - VIEW_Y, LPM_C_BG);
        return;
    }
    int track = LPM_FOOTER_Y - 4 - VIEW_Y, thumb = track * (rows() - end) / rows();
    fill(BAR_X, VIEW_Y, 3, track, LPM_C_RULE);
    fill(BAR_X, VIEW_Y + (end ? (track - thumb) * top / end : 0), 3, thumb, LPM_C_DIM);
}

/* Replaces the vendor tool frame for message 0x36, which set_sysState posts on
 * entry. In Link status it draws the whole screen in the design, so the vendor
 * frame never shows, then the arrow at the port to use; the battery icon is
 * redrawn last, as the vendor frame does. */
void lpm_tool_frame(void)
{
    if (SYS_STATE == PING_SYS_STATE && lpm_ping_frame())
        return;
    if (SYS_STATE != LINK_SYS_STATE) {
        vendor_tool_frame();
        return;
    }
    static const char *const titles[] = {"Link status", "VLAN test", "Duplicate IP"};
    static const char *const footers[] = {"M run  UP/DN scroll  <> tab", "M run  UP/DN <> VLAN ID",
                                          "M run  UP/DN scroll"};
    int tool = TOOLS->tool;
    fill(0, LPM_CONTENT_Y, LPM_SCREEN_W, LPM_FOOTER_Y - LPM_CONTENT_Y, LPM_C_BG);
    draw_header(titles[tool], STATUS_X);
    if (tool == TOOL_LINK)
        draw_tabs();
    draw_footer(footers[tool], "BACK");
    LINK->drawn_top = *top_row(); /* the whole view was just cleared */
    lpm_link_draw();
    /* The vendor's own arrow, as its tools draw it on message 0x37, but only this one:
     * its state-8 case (0x0801B21C, for the QC test) would mark another port and paint
     * the two side arrows "off" over the scroll bar. */
    note_mark(PORT_MARK, 1);
    vendor_battery();
}

/* ---- checks (key task) -------------------------------------------------- */

static void show(void)
{
    post_message(DRAW_MESSAGE, 0, 0);
}

void lpm_list_show(void)
{
    show();
}

void lpm_list_follow(int row)
{
    uint8_t *top = top_row();
    if (row < *top)
        *top = (uint8_t)row;
    while (!fits(*top, row))
        (*top)++;
    show();
}

/* Each key waiting, through the vendor key table: BACK leaves state 8, M asks
 * the run to stop (lpm_link_run), UP and DOWN scroll. */
int lpm_list_poll(void)
{
    uint32_t item[2] = {0, 0};
    void *queue = KEY_QUEUE;
    while (queue && queue_receive(queue, item, 0) == 1)
        key_match(item);
    return SYS_STATE != LINK_SYS_STATE || TOOLS->stop;
}

/* UP or DOWN on the VLAN digit under the cursor, kept within 1-4094. */
static void step_vlan(tools_state *t, int up)
{
    static const uint16_t places[VLAN_DIGITS] = {1000, 100, 10, 1};
    unsigned place = places[t->digit], digit = t->vlan / place % 10, top = place == 1000 ? 4 : 9;
    digit = up ? (digit == top ? 0 : digit + 1) : (digit == 0 ? top : digit - 1);
    unsigned vlan = t->vlan - t->vlan / place % 10 * place + digit * place;
    t->vlan = (uint16_t)(vlan > VLAN_MAX ? VLAN_MAX : vlan ? vlan : 1);
}

/* UP, DOWN, LEFT or RIGHT in state 8 (events 2 to 5, through lpm_home_key): in
 * VLAN test they edit the VLAN, which clears the last results; elsewhere UP and
 * DOWN scroll, and LEFT and RIGHT change the Link status tab. */
void lpm_link_key(int action)
{
    tools_state *t = TOOLS;
    uint8_t *top = top_row();
    if (t->tool == TOOL_VLAN && !t->running) {
        if (action == KEY_LEFT || action == KEY_RIGHT) {
            t->digit = (uint8_t)((t->digit + (action == KEY_RIGHT ? 1 : VLAN_DIGITS - 1)) % VLAN_DIGITS);
        } else {
            step_vlan(t, action == KEY_UP);
            uint8_t drawn = LINK->drawn_top;
            clear(LINK);
            LINK->drawn_top = drawn;
        }
    } else if (action == KEY_UP && *top > 0) {
        (*top)--;
    } else if (action == KEY_DOWN && *top < last_top()) {
        (*top)++;
    } else if (t->tool == TOOL_LINK && (action == KEY_LEFT || action == KEY_RIGHT)) {
        SWITCH->tab = action == KEY_RIGHT;
    }
    show();
}

static void begin(link_state *s, int row)
{
    s->step[row] = LPM_STEP_RUNNING;
    if (TOOLS->tool <= TOOL_VLAN) { /* the checklist is shown: follow the check running */
        if (row < s->top)
            s->top = (uint8_t)row;
        while (!fits(s->top, row))
            s->top++;
    }
    show();
}

static void finish(link_state *s, int row, int step)
{
    s->step[row] = (uint8_t)step;
    show();
}

/* The Internet checksum (RFC 1071): sum16 adds n bytes, an odd last one padded with zero. */
static uint32_t sum16(uint32_t sum, const uint8_t *p, int n)
{
    for (int i = 0; i + 1 < n; i += 2)
        sum += (uint32_t)(p[i] << 8 | p[i + 1]);
    if (n & 1)
        sum += (uint32_t)p[n - 1] << 8;
    return sum;
}

static unsigned fold(uint32_t sum)
{
    while (sum >> 16)
        sum = (sum & 0xFFFFu) + (sum >> 16);
    return ~sum & 0xFFFFu;
}

static unsigned checksum(const uint8_t *p, int n)
{
    return fold(sum16(0, p, n));
}

static int check_link(link_state *s)
{
    begin(s, 0);
    int up = (dm9051_read(NSR) & 0x40) || wait_link(LINK_WAIT_MS);
    if (up) {
        s->ten_mbps = dm9051_read(NSR) >> 7 & 1;
        s->full_duplex = dm9051_read(NCR) >> 3 & 1;
    }
    finish(s, 0, up ? LPM_STEP_PASS : LPM_STEP_FAIL);
    return up;
}

/* The first DNS server (option 6) in the DHCP reply dhcp_wait left in the frame
 * buffer. The vendor reads only options 1, 3, 53 and 54. A reply without the
 * option leaves the one found before, from the OFFER. */
static void dhcp_dns(link_state *s)
{
    const uint8_t *f = FRAME;
    const uint8_t *end = f + (FRAME_LEN < MAX_FRAME ? FRAME_LEN : MAX_FRAME);
    const uint8_t *o = f + 14 + (f[14] & 15) * 4 + DHCP_OPTIONS;
    while (o + 2 <= end && *o != 255) {
        if (*o == 0) { /* pad */
            o++;
            continue;
        }
        if (o + 2 + o[1] > end)
            return;
        if (o[0] == DHCP_DNS && o[1] >= 4) {
            copy(s->dns, o + 2, 4);
            return;
        }
        o += 2 + o[1];
    }
}

/* As the vendor Auto Config IP (0x08021A60), but an OFFER without an ACK is reported, not used silently. */
static int check_address(link_state *s)
{
    uint32_t xid = tick() ^ ((uint32_t)OWN_MAC[3] << 24 | (uint32_t)OWN_MAC[4] << 16 | (uint32_t)OWN_MAC[5] << 8);
    int offered = 0;
    begin(s, 1);
    for (int i = 0; i < TRIES && !offered; i++) {
        dhcp_send(DHCP_DISCOVER, xid, 0, 0);
        delay(50);
        offered = dhcp_wait(xid, DHCP_WAIT_MS, DHCP_OFFER, s->ip, s->mask, s->gateway, s->server);
    }
    if (offered) {
        s->source = LPM_ADDR_OFFER_ONLY;
        dhcp_dns(s);
        dhcp_send(DHCP_REQUEST, xid, s->ip, s->server);
        delay(50);
        if (dhcp_wait(xid, DHCP_WAIT_MS, DHCP_ACK, s->ip, s->mask, s->gateway, 0)) {
            s->source = LPM_ADDR_DHCP;
            dhcp_dns(s);
        }
    }
    finish(s, 1, s->source == LPM_ADDR_DHCP ? LPM_STEP_PASS : offered ? LPM_STEP_WARN : LPM_STEP_FAIL);
    return offered;
}

static int check_arp(link_state *s)
{
    int found = 0;
    begin(s, 2);
    for (int i = 0; i < TRIES && !found && known(s->gateway); i++) {
        arp_request(s->gateway, s->ip);
        found = arp_wait(s->gateway, REPLY_WAIT_MS);
    }
    if (found)
        copy(s->gateway_mac, FRAME + 22, 6); /* the ARP sender, left in the frame buffer */
    finish(s, 2, found ? LPM_STEP_PASS : LPM_STEP_FAIL);
    return found;
}

/* The Ethernet and IPv4 headers of a datagram to target through the next hop
 * mac, with the payload zeroed; returns the payload. */
static uint8_t *ipv4(const link_state *s, const uint8_t *mac, const uint8_t *target, int proto, int payload,
                     unsigned id)
{
    uint8_t *f = FRAME, *ip = f + 14;
    for (int i = 0; i < 20 + payload; i++)
        ip[i] = 0;
    copy(f, mac, 6);
    copy(f + 6, OWN_MAC, 6);
    put16(f + 12, 0x0800);
    ip[0] = 0x45;
    put16(ip + 2, 20 + payload);
    put16(ip + 4, id);
    ip[8] = 64;
    ip[9] = (uint8_t)proto;
    copy(ip + 12, s->ip, 4);
    copy(ip + 16, target, 4);
    put16(ip + 10, checksum(ip, 20));
    FRAME_LEN = (uint16_t)(14 + 20 + payload);
    return ip + 20;
}

/* The Ethernet multicast address of an IPv6 group (RFC 2464). */
static void group_mac(uint8_t *mac, const uint8_t *group)
{
    mac[0] = mac[1] = 0x33;
    copy(mac + 2, group + 12, 4);
}

/* The Ethernet and IPv6 headers of a packet from source to target, through mac
 * or, with mac 0, to target's multicast group; returns the zeroed payload. */
static uint8_t *ipv6(const uint8_t *mac, const uint8_t *source, const uint8_t *target, int next, int payload,
                     int hops)
{
    uint8_t *f = FRAME, *ip = f + 14;
    for (int i = 0; i < 40 + payload; i++)
        ip[i] = 0;
    if (mac)
        copy(f, mac, 6);
    else
        group_mac(f, target);
    copy(f + 6, OWN_MAC, 6);
    put16(f + 12, 0x86DD);
    ip[0] = 0x60;
    put16(ip + 4, payload);
    ip[6] = (uint8_t)next;
    ip[7] = (uint8_t)hops;
    copy(ip + 8, source, 16);
    copy(ip + 24, target, 16);
    FRAME_LEN = (uint16_t)(14 + 40 + payload);
    return ip + 40;
}

/* The ICMPv6 or UDP checksum of the packet in the frame buffer, over the
 * pseudo-header (RFC 8200), then send it. */
static void send6(void)
{
    uint8_t *ip = FRAME + 14;
    unsigned length = get16(ip + 4);
    unsigned sum = fold(sum16(sum16(length + ip[6], ip + 8, 32), ip + 40, (int)length));
    put16(ip + 40 + (ip[6] == PROTO_UDP ? 6 : 2), sum ? sum : 0xFFFFu);
    transmit();
}

static void solicited_node(uint8_t *group, const uint8_t *ip)
{
    copy(group, dhcp6_servers, 11); /* ff02::1:ffXX:XXXX */
    group[11] = 1;
    group[12] = 0xFF;
    copy(group + 13, ip + 13, 3);
}

/* Duplicate address detection (RFC 4862): a neighbour solicitation for our
 * new address, from the unspecified address. Another host that uses it
 * answers, which the receive loop notes in in_use. */
static void probe_address(const uint8_t *ip)
{
    uint8_t group[16];
    solicited_node(group, ip);
    uint8_t *icmp = ipv6(0, unspecified, group, PROTO_ICMP6, 24, 255);
    icmp[0] = NEIGHBOUR_SOLICIT;
    copy(icmp + 8, ip, 16);
    send6();
}

static uint8_t *dhcp6_option(uint8_t *o, unsigned code, unsigned length)
{
    put16(o, code);
    put16(o + 2, length);
    return o + 4;
}

/* A DHCPv6 SOLICIT (with rapid commit) or REQUEST for one address (IA_NA). */
static void send_dhcp6(const link_state *s, int type)
{
    int request = type == DHCP6_REQUEST;
    int length = 8 + 4 + (4 + 10) + (4 + 2) + (4 + 12) + (request ? 4 + s->server_id_len + 28 : 4);
    uint8_t *udp = ipv6(0, s->link_local, dhcp6_servers, PROTO_UDP, length, 1), *o;
    put16(udp, DHCP6_CLIENT_PORT);
    put16(udp + 2, DHCP6_SERVER_PORT);
    put16(udp + 4, (unsigned)length);
    udp[8] = (uint8_t)type;
    copy(udp + 9, s->xid6, 3);
    o = dhcp6_option(udp + 12, 1, 10); /* client: DUID-LL, Ethernet */
    o[1] = 3;
    o[3] = 1;
    copy(o + 4, OWN_MAC, 6);
    o = dhcp6_option(o + 10, 8, 2) + 2; /* elapsed time 0 */
    if (request) {
        o = dhcp6_option(o, 2, s->server_id_len);
        copy(o, s->server_id, s->server_id_len);
        o += s->server_id_len;
    }
    o = dhcp6_option(o, 3, request ? 12 + 28 : 12); /* IA_NA: IAID 1, T1 and T2 left to the server */
    o[3] = 1;
    if (request)
        copy(dhcp6_option(o + 12, 5, 24), s->leased, 16); /* the address advertised */
    else
        dhcp6_option(o + 12, 14, 0); /* rapid commit */
    send6();
}

/* A request of kind, numbered seq, to target through mac. */
static void send_request(const link_state *s, const uint8_t *mac, const uint8_t *target, int kind, unsigned seq)
{
    if (kind == ECHO) {
        uint8_t *icmp = ipv4(s, mac, target, PROTO_ICMP, 8 + ECHO_BYTES, seq);
        icmp[0] = 8;
        put16(icmp + 4, ECHO_ID);
        put16(icmp + 6, seq);
        put16(icmp + 2, checksum(icmp, 8 + ECHO_BYTES));
        transmit();
    } else if (kind == DNS_QUERY) {
        int length = 8 + 12 + (int)sizeof question;
        uint8_t *udp = ipv4(s, mac, target, PROTO_UDP, length, seq), *dns = udp + 8;
        put16(udp, LOCAL_PORT);
        put16(udp + 2, DNS_PORT);
        put16(udp + 4, (unsigned)length); /* the UDP checksum is optional over IPv4, and left 0 */
        put16(dns, seq);
        dns[2] = 1; /* recursion desired */
        dns[5] = 1; /* one question */
        copy(dns + 12, question, sizeof question);
        transmit();
    } else if (kind == SOLICIT_ROUTER) {
        uint8_t *icmp = ipv6(0, s->link_local, all_routers, PROTO_ICMP6, 16, 255);
        icmp[0] = ROUTER_SOLICIT;
        icmp[8] = 1; /* our link-layer address, so the router can answer us directly */
        icmp[9] = 1;
        copy(icmp + 10, OWN_MAC, 6);
        send6();
    } else if (kind == ECHO6) {
        uint8_t *icmp = ipv6(mac, link_local(target) ? s->link_local : global6(s), target, PROTO_ICMP6,
                             8 + ECHO_BYTES, 64);
        icmp[0] = ECHO6_REQUEST;
        put16(icmp + 4, ECHO_ID);
        put16(icmp + 6, seq);
        send6();
    } else {
        send_dhcp6(s, kind == SOLICIT_DHCP6 ? DHCP6_SOLICIT : DHCP6_REQUEST);
    }
}

/* Which of our IPv6 addresses ip is: IN_USE_* for one of the new ones, -1 for
 * the link-local one, or 0. */
static int ours6(const link_state *s, const uint8_t *ip)
{
    if (same(ip, s->link_local, 16))
        return -1;
    if (s->step[ROW_SLAAC] == LPM_STEP_PASS && same(ip, s->slaac, 16))
        return IN_USE_SLAAC;
    if (s->dhcp6 >= DHCP6_ADVERTISED && same(ip, s->leased, 16))
        return IN_USE_LEASED;
    return 0;
}

/* Neighbour traffic for us, handled while waiting for a reply: 1 when the
 * frame was one. An ARP request for our IPv4 address or a neighbour
 * solicitation for an IPv6 one is answered, in case the gateway has not cached
 * us; a host that claims one of our new IPv6 addresses is noted in in_use. */
static int neighbours(link_state *s, int n)
{
    uint8_t *f = FRAME, *ip = f + 14, *icmp = ip + 40;
    if (f[12] == 0x08 && f[13] == 0x06) {
        if (n < 42 || f[21] != 1 || !known(s->ip) || !same(f + 38, s->ip, 4))
            return 1;
        copy(f, f + 22, 6);       /* to the asker */
        copy(f + 6, OWN_MAC, 6);
        f[21] = 2;                /* reply */
        copy(f + 32, f + 22, 10); /* the target becomes the asker */
        copy(f + 22, OWN_MAC, 6);
        copy(f + 28, s->ip, 4);
        FRAME_LEN = 42;
        transmit();
        return 1;
    }
    if (f[12] != 0x86 || f[13] != 0xDD || n < 14 + 40 + 24 || ip[6] != PROTO_ICMP6 || ip[7] != 255 ||
        (icmp[0] != NEIGHBOUR_SOLICIT && icmp[0] != NEIGHBOUR_ADVERT))
        return 0;
    int which = ours6(s, icmp + 8);
    if (!which || same(f + 6, OWN_MAC, 6))
        return 1;
    if (icmp[0] == NEIGHBOUR_ADVERT || !known6(ip + 8)) { /* an owner's answer, or its own probe */
        if (which > 0)
            s->in_use |= (uint8_t)which;
        return 1;
    }
    uint8_t mac[6], asker[16], target[16];
    copy(mac, f + 6, 6);
    copy(asker, ip + 8, 16);
    copy(target, icmp + 8, 16);
    icmp = ipv6(mac, target, asker, PROTO_ICMP6, 32, 255);
    icmp[0] = NEIGHBOUR_ADVERT;
    icmp[4] = 0x60; /* solicited, override */
    copy(icmp + 8, target, 16);
    icmp[24] = 2; /* our link-layer address */
    icmp[25] = 1;
    copy(icmp + 26, OWN_MAC, 6);
    send6();
    return 1;
}

/* The end of the DNS name at p, or 0 past end. */
static const uint8_t *skip_name(const uint8_t *p, const uint8_t *end)
{
    while (p < end) {
        if (*p == 0)
            return p + 1;
        if ((*p & 0xC0) == 0xC0)
            return p + 2;
        if (*p & 0xC0)
            return 0;
        p += 1 + *p;
    }
    return 0;
}

/* 1 for a DNS response to seq with no error and an A record, kept in dns_ip. */
static int dns_address(link_state *s, const uint8_t *dns, const uint8_t *end, unsigned seq)
{
    if (dns + 12 > end || get16(dns) != seq || !(dns[2] & 0x80) || (dns[3] & 15) != 0)
        return 0;
    const uint8_t *p = dns + 12;
    for (unsigned i = get16(dns + 4); i && p; i--) { /* the questions */
        p = skip_name(p, end);
        if (p)
            p += 4;
    }
    for (unsigned i = get16(dns + 6); i && p; i--) {
        p = skip_name(p, end);
        if (!p || p + 10 > end || p + 10 + get16(p + 8) > end)
            return 0;
        if (get16(p) == 1 && get16(p + 2) == 1 && get16(p + 8) == 4) {
            copy(s->dns_ip, p + 10, 4);
            return 1;
        }
        p += 10 + get16(p + 8);
    }
    return 0;
}

/* Reads a router advertisement: the router, its link-layer address, flags and
 * lifetime, and its first prefix (the first autonomous /64 if there is one).
 * One that is a default router (lifetime > 0) ends the wait; another is kept
 * only until one is. */
static int router_advert(link_state *s, const uint8_t *ip, const uint8_t *end)
{
    const uint8_t *icmp = ip + 40;
    if (icmp + 16 > end || icmp[0] != ROUTER_ADVERT || icmp[1] != 0 || !link_local(ip + 8) ||
        (known6(s->router) && s->router_lifetime))
        return 0;
    copy(s->router, ip + 8, 16);
    copy(s->router_mac, FRAME + 6, 6);
    s->ra_flags = icmp[5];
    s->router_lifetime = (uint16_t)get16(icmp + 6);
    s->prefix_len = 0;
    for (const uint8_t *o = icmp + 16; o + 8 <= end && o[1] && o + 8 * o[1] <= end; o += 8 * o[1]) {
        if (o[0] == 1) { /* source link-layer address */
            copy(s->router_mac, o + 2, 6);
        } else if (o[0] == 3 && o[1] == 4 && !link_local(o + 16) &&
                   (!s->prefix_len || (o[3] & PREFIX_AUTONOMOUS && o[2] == 64 &&
                                       !(s->prefix_flags & PREFIX_AUTONOMOUS && s->prefix_len == 64)))) {
            copy(s->prefix, o + 16, 16);
            s->prefix_len = o[2];
            s->prefix_flags = o[3];
        }
    }
    return s->router_lifetime != 0;
}

/* Reads a DHCPv6 ADVERTISE or REPLY: the server's DUID and the address in our
 * IA_NA, unless a status code refuses it. */
static int dhcp6_reply(link_state *s, const uint8_t *m, const uint8_t *end, int kind)
{
    int address = 0, refused = 0;
    if (m + 4 > end || !same(m + 1, s->xid6, 3) ||
        !(m[0] == DHCP6_REPLY || (kind == SOLICIT_DHCP6 && m[0] == DHCP6_ADVERTISE)))
        return 0;
    for (const uint8_t *o = m + 4; o + 4 <= end && o + 4 + get16(o + 2) <= end; o += 4 + get16(o + 2)) {
        unsigned code = get16(o), length = get16(o + 2);
        if (code == 2 && length <= SERVER_ID_MAX) {
            copy(s->server_id, o + 4, (int)length);
            s->server_id_len = (uint8_t)length;
        } else if (code == 13 && length >= 2 && get16(o + 4)) {
            refused = 1; /* a status other than success */
        } else if (code == 3 && length >= 12) {
            const uint8_t *ia_end = o + 4 + length;
            for (const uint8_t *a = o + 16; a + 4 <= ia_end && a + 4 + get16(a + 2) <= ia_end;
                 a += 4 + get16(a + 2)) {
                if (get16(a) == 13 && get16(a + 2) >= 2 && get16(a + 4))
                    refused = 1;
                if (get16(a) == 5 && get16(a + 2) >= 24 && !address) {
                    copy(s->leased, a + 4, 16);
                    address = 1;
                }
            }
        }
    }
    if (address && !refused)
        s->dhcp6 = m[0] == DHCP6_REPLY ? DHCP6_LEASED : DHCP6_ADVERTISED;
    else if (s->dhcp6 == DHCP6_NO_REPLY)
        s->dhcp6 = DHCP6_NO_ADDRESS;
    return 1;
}

/* 1 for target's reply of kind to our request seq: an echo reply, a DNS
 * response with an A record, a router advertisement or a DHCPv6 answer. */
static int is_reply(link_state *s, const uint8_t *target, int kind, unsigned seq, int n)
{
    uint8_t *f = FRAME, *end = f + (n < MAX_FRAME ? n : MAX_FRAME);
    if (kind <= DNS_QUERY) {
        uint8_t *ip = f + 14, *p = ip + (ip[0] & 15) * 4;
        if (n < 42 || f[12] != 0x08 || f[13] != 0x00 || ip[9] != (kind == ECHO ? PROTO_ICMP : PROTO_UDP) ||
            !same(ip + 12, target, 4))
            return 0;
        if (kind == ECHO)
            return p + 8 <= end && p[0] == 0 && get16(p + 4) == ECHO_ID && get16(p + 6) == seq;
        return p + 8 <= end && get16(p) == DNS_PORT && get16(p + 2) == LOCAL_PORT && dns_address(s, p + 8, end, seq);
    }
    uint8_t *ip = f + 14, *p = ip + 40;
    if (n < 14 + 40 + 8 || f[12] != 0x86 || f[13] != 0xDD)
        return 0;
    if (p + get16(ip + 4) < end)
        end = p + get16(ip + 4);
    if (kind == SOLICIT_ROUTER)
        return ip[6] == PROTO_ICMP6 && ip[7] == 255 && router_advert(s, ip, end);
    if (kind == ECHO6)
        return ip[6] == PROTO_ICMP6 && same(ip + 8, target, 16) && p[0] == ECHO6_REPLY &&
               get16(p + 4) == ECHO_ID && get16(p + 6) == seq;
    return ip[6] == PROTO_UDP && get16(p) == DHCP6_SERVER_PORT && get16(p + 2) == DHCP6_CLIENT_PORT &&
           same(ip + 24, s->link_local, 16) && dhcp6_reply(s, p + 8, end, kind);
}

/* Up to count requests to target through mac, numbered from seq; 1 with *rtt_us set at the first reply.
 * poll, when given, is called while waiting, and non-zero from it ends the wait with 0 (lpm_net_echo). */
static int exchange(link_state *s, const uint8_t *mac, const uint8_t *target, int kind, unsigned seq, int count,
                    uint32_t *rtt_us, int (*poll)(void))
{
    int answered = 0;
    DEMCR |= 1u << 24; /* TRCENA, so the cycle counter runs */
    DWT_CTRL |= 1u;
    for (unsigned last = seq + count; seq < last && !answered; seq++) {
        uint32_t sent = DWT_CYCCNT, start = tick();
        send_request(s, mac, target, kind, seq);
        while (!answered && tick() - start < REPLY_WAIT_MS) {
            if (poll && poll())
                return 0;
            if (dm9051_read(NSR) & 1) {
                int n = receive();
                if (n <= 0 && s->rcr) /* the vendor receive resets the receive control after an error */
                    dm9051_write(RCR, s->rcr | RCR_ALL_MULTICAST);
                if (n > 0 && !neighbours(s, n) && is_reply(s, target, kind, seq, n)) {
                    *rtt_us = (DWT_CYCCNT - sent) / CYCLES_PER_US;
                    answered = 1;
                }
            } else if (tick() - start >= BUSY_POLL_MS) {
                delay(1);
            }
        }
    }
    return answered;
}

static void check_ping(link_state *s)
{
    begin(s, 3);
    finish(s, 3, exchange(s, s->gateway_mac, s->gateway, ECHO, 1, PINGS, &s->rtt_us, 0) ? LPM_STEP_PASS
                                                                                     : LPM_STEP_FAIL);
}

static void check_internet(link_state *s)
{
    begin(s, 4);
    for (int i = 0; i < 2 && !s->internet; i++)
        if (exchange(s, s->gateway_mac, internet_hosts[i], ECHO, 1 + PINGS + i * INTERNET_PINGS, INTERNET_PINGS,
                     &s->internet_rtt_us, 0))
            s->internet = (uint8_t)(i + 1);
    finish(s, 4, s->internet ? LPM_STEP_PASS : LPM_STEP_FAIL);
}

/* A query of server for example.com. A server on our subnet other than the
 * gateway (a local resolver) is found by ARP; any other is reached through the gateway. */
static int lookup(link_state *s, const uint8_t *server, unsigned seq)
{
    uint8_t mac[6];
    int local = !same(server, s->gateway, 4);
    for (int i = 0; i < 4; i++)
        if ((server[i] ^ s->ip[i]) & s->mask[i])
            local = 0;
    if (!local) {
        copy(mac, s->gateway_mac, 6);
    } else {
        arp_request(server, s->ip);
        if (!arp_wait(server, REPLY_WAIT_MS))
            return 0;
        copy(mac, FRAME + 22, 6);
    }
    return exchange(s, mac, server, DNS_QUERY, seq, DNS_TRIES, &s->dns_rtt_us, 0);
}

static void check_dns(link_state *s)
{
    unsigned seq = 1 + PINGS + 2 * INTERNET_PINGS;
    begin(s, 5);
    if (known(s->dns) && lookup(s, s->dns, seq))
        s->dns_answer = DNS_BY_DHCP_SERVER;
    else if (!same(s->dns, internet_hosts[0], 4) && lookup(s, internet_hosts[0], seq + DNS_TRIES))
        s->dns_answer = DNS_BY_FALLBACK;
    finish(s, 5, s->dns_answer == DNS_BY_DHCP_SERVER ? LPM_STEP_PASS
                 : s->dns_answer                     ? LPM_STEP_WARN
                                                     : LPM_STEP_FAIL);
}

/* A router advertisement, after a solicitation from our link-local address
 * (modified EUI-64, RFC 4291), which is probed for duplicates first. */
static void check_router(link_state *s)
{
    uint32_t unused;
    begin(s, ROW_ROUTER);
    s->link_local[0] = 0xFE;
    s->link_local[1] = 0x80;
    copy(s->link_local + 8, OWN_MAC, 3);
    s->link_local[8] ^= 2; /* universal/local bit */
    s->link_local[11] = 0xFF;
    s->link_local[12] = 0xFE;
    copy(s->link_local + 13, OWN_MAC + 3, 3);
    probe_address(s->link_local);
    exchange(s, 0, 0, SOLICIT_ROUTER, 0, SOLICITATIONS, &unused, 0);
    finish(s, ROW_ROUTER, s->router_lifetime ? LPM_STEP_PASS : known6(s->router) ? LPM_STEP_WARN : LPM_STEP_FAIL);
}

/* SLAAC (RFC 4862): an autonomous /64 prefix and our interface identifier. */
static void check_slaac(link_state *s)
{
    if (!known6(s->router))
        return;
    begin(s, ROW_SLAAC);
    if (s->prefix_len != 64 || !(s->prefix_flags & PREFIX_AUTONOMOUS)) {
        finish(s, ROW_SLAAC, STEP_NONE);
        return;
    }
    copy(s->slaac, s->prefix, 8);
    copy(s->slaac + 8, s->link_local + 8, 8);
    probe_address(s->slaac);
    finish(s, ROW_SLAAC, LPM_STEP_PASS);
}

static void check_dhcp6(link_state *s)
{
    uint32_t unused, xid = tick() ^ ((uint32_t)OWN_MAC[4] << 8 | OWN_MAC[5]);
    begin(s, ROW_DHCP6);
    s->xid6[0] = (uint8_t)(xid >> 16);
    s->xid6[1] = (uint8_t)(xid >> 8);
    s->xid6[2] = (uint8_t)xid;
    exchange(s, 0, 0, SOLICIT_DHCP6, 0, TRIES, &unused, 0);
    if (s->dhcp6 == DHCP6_ADVERTISED && s->server_id_len) {
        s->xid6[0] ^= 0x80; /* a new transaction */
        exchange(s, 0, 0, REQUEST_DHCP6, 0, TRIES, &unused, 0);
    }
    if (s->dhcp6 >= DHCP6_ADVERTISED)
        probe_address(s->leased);
    finish(s, ROW_DHCP6, s->dhcp6 == DHCP6_LEASED       ? LPM_STEP_PASS
                         : s->dhcp6 == DHCP6_ADVERTISED ? LPM_STEP_WARN
                         : s->ra_flags & RA_MANAGED     ? (s->dhcp6 ? LPM_STEP_WARN : LPM_STEP_FAIL)
                                                        : STEP_NONE);
}

static void check_ping6(link_state *s)
{
    begin(s, ROW_PING6);
    finish(s, ROW_PING6, exchange(s, s->router_mac, s->router, ECHO6, 1, PINGS, &s->rtt6_us, 0) ? LPM_STEP_PASS
                                                                                             : LPM_STEP_FAIL);
}

static void check_internet6(link_state *s)
{
    begin(s, ROW_INTERNET6);
    for (int i = 0; i < 2 && !s->internet6; i++)
        if (exchange(s, s->router_mac, internet6_hosts[i], ECHO6, 1 + PINGS + i * INTERNET_PINGS, INTERNET_PINGS,
                     &s->internet6_rtt_us, 0))
            s->internet6 = (uint8_t)(i + 1);
    finish(s, ROW_INTERNET6, s->internet6 ? LPM_STEP_PASS : LPM_STEP_FAIL);
}

/* The IPv6 checks, with all multicast passing the DM9051 filter meanwhile:
 * router advertisements go to ff02::1 and neighbour solicitations to our
 * solicited-node group, which the vendor's broadcast-only hash drops. */
static void check_ipv6(link_state *s)
{
    uint8_t rcr = dm9051_read(RCR);
    s->rcr = rcr;
    dm9051_write(RCR, rcr | RCR_ALL_MULTICAST);
    check_router(s);
    check_slaac(s);
    check_dhcp6(s);
    /* A duplicate found while DHCPv6 ran is not used. */
    if (s->in_use & IN_USE_SLAAC)
        finish(s, ROW_SLAAC, LPM_STEP_FAIL);
    if (s->in_use & IN_USE_LEASED)
        finish(s, ROW_DHCP6, LPM_STEP_FAIL);
    if (known6(s->router))
        check_ping6(s);
    if (s->router_lifetime && global6(s))
        check_internet6(s);
    dm9051_write(RCR, rcr);
    s->rcr = 0;
}

static void clear(link_state *s)
{
    uint8_t *b = (uint8_t *)s;
    for (unsigned i = 0; i < sizeof *s; i++)
        b[i] = 0;
}

/* The checks; in VLAN test only the IPv4 ones, every frame tagged. */
static void run_checks(int vlan)
{
    link_state *s = LINK;
    tools_state *t = TOOLS;
    uint8_t drawn = s->drawn_top; /* still on screen */
    clear(s);
    s->drawn_top = drawn;
    show();
    if (vlan) {
        t->tag = t->vlan;
        t->tagging = TAGGING_MAGIC;
    }
    NET_BUSY = 1;
    if (check_link(s)) {
        if (check_address(s) && check_arp(s)) {
            check_ping(s);
            check_internet(s);
            check_dns(s);
        }
        if (!vlan)
            check_ipv6(s);
        while (!fits(s->top, rows() - 1)) /* the end of the list, whichever checks ran */
            s->top++;
    }
    NET_BUSY = 0;
    t->tagging = 0;
    for (int row = 0; row < ROWS; row++)
        if (s->step[row] == LPM_STEP_PENDING)
            s->step[row] = LPM_STEP_SKIPPED;
    show();
}

/* M in state 8 (event 0x0B): the open tool's run. A run that polls keys
 * (Duplicate IP) gets here from its own polling: M then stops it. */
void lpm_link_run(void)
{
    tools_state *t = TOOLS;
    if (t->running) {
        t->stop = 1;
        return;
    }
    if (t->tool == TOOL_DUPLICATE) {
        t->running = 1;
        t->stop = 0;
        lpm_duplicate_run();
        t->running = 0;
        show();
    } else if (t->tool == TOOL_LINK && switch_tab()) {
        lpm_switch_run();
    } else {
        run_checks(t->tool == TOOL_VLAN);
    }
}

/* M on a home item of the QC slot (event 6): enter state 8 with the tool the
 * home list shows, waiting for M. */
void lpm_link_open(void)
{
    tools_state *t = TOOLS;
    int tool = lpm_home_tool();
    clear(LINK); /* before set_sysState, whose frame message draws the state */
    lpm_switch_reset();
    t->tool = (uint8_t)tool;
    t->running = t->stop = 0;
    t->tagging = 0;
    if (t->vlan < 1 || t->vlan > VLAN_MAX) /* the power-up pattern */
        t->vlan = 1;
    if (t->digit >= VLAN_DIGITS)
        t->digit = VLAN_DIGITS - 1;
    if (tool == TOOL_DUPLICATE)
        lpm_duplicate_open();
    set_sys_state(LINK_SYS_STATE);
}

/* ---- 802.1Q tagging --------------------------------------------------------- */

/* The vendor transmit and receive, as everything here and the vendor DHCP and
 * ARP routines reach them (scripts/build_patched_image.py, VLAN_CALLS): while a
 * VLAN test runs each frame sent carries the tag, and only frames received with
 * the VLAN's tag are passed on, untagged. Otherwise both pass straight through. */
static int tagging(void)
{
    return TOOLS->tagging == TAGGING_MAGIC;
}

void lpm_vlan_transmit(void)
{
    uint8_t *f = FRAME;
    int n = FRAME_LEN;
    if (!tagging() || n < 14 || n > MAX_FRAME) {
        vendor_transmit();
        return;
    }
    for (int i = n - 1; i >= 12; i--)
        f[i + 4] = f[i];
    put16(f + 12, TPID_VLAN);
    put16(f + 14, TOOLS->tag);
    FRAME_LEN = (uint16_t)(n + 4);
    vendor_transmit();
    for (int i = 12; i < n; i++) /* as it was: a caller may send it again */
        f[i] = f[i + 4];
    FRAME_LEN = (uint16_t)n;
}

int lpm_vlan_receive(void)
{
    uint8_t *f = FRAME;
    int n = vendor_receive();
    if (!tagging() || n <= 0)
        return n;
    if (n < 18 || get16(f + 12) != TPID_VLAN || (get16(f + 14) & 0x0FFF) != TOOLS->tag)
        return 0; /* untagged or another VLAN's: not an answer on this one */
    for (int i = 12; i < n - 4; i++)
        f[i] = f[i + 4];
    return n - 4;
}

/* ---- for Ping monitor (ping_monitor.c, key task) ---------------------------- */

int lpm_net_prepare(void)
{
    link_state *s = LINK;
    clear(s);
    if (!check_link(s))
        return NET_NO_LINK;
    if (!check_address(s))
        return NET_NO_ADDRESS;
    if (!check_arp(s))
        return NET_NO_GATEWAY;
    return NET_READY;
}

int lpm_net_link_up(void)
{
    return (dm9051_read(NSR) & 0x40) != 0;
}

const uint8_t *lpm_net_ip(void)
{
    return LINK->ip;
}

const uint8_t *lpm_net_mask(void)
{
    return LINK->mask;
}

const uint8_t *lpm_net_gateway(void)
{
    return LINK->gateway;
}

const uint8_t *lpm_net_gateway_mac(void)
{
    return LINK->gateway_mac;
}

int lpm_net_arp(const uint8_t *ip, uint8_t *mac)
{
    arp_request(ip, LINK->ip);
    if (!arp_wait(ip, REPLY_WAIT_MS))
        return 0;
    copy(mac, FRAME + 22, 6);
    return 1;
}

int lpm_net_echo(const uint8_t *mac, const uint8_t *target, unsigned seq, uint32_t *rtt_us, int (*poll)(void))
{
    return exchange(LINK, mac, target, ECHO, seq, 1, rtt_us, poll);
}

/* ---- for Duplicate IP (key task) -------------------------------------------- */

int lpm_net_dhcp(void)
{
    link_state *s = LINK;
    uint8_t drawn = s->drawn_top;
    clear(s);
    s->drawn_top = drawn;
    NET_BUSY = 1; /* the vendor DHCP blocks; no key is read meanwhile */
    int ready = check_link(s) && check_address(s);
    NET_BUSY = 0;
    if (s->step[1] == LPM_STEP_PENDING)
        s->step[1] = LPM_STEP_SKIPPED;
    return ready;
}

void lpm_net_clear(void)
{
    uint8_t drawn = LINK->drawn_top;
    clear(LINK);
    LINK->drawn_top = drawn;
}

uint8_t *lpm_net_frame(void)
{
    return FRAME;
}

int lpm_net_receive(void)
{
    if (!(dm9051_read(NSR) & 1))
        return 0;
    return receive();
}

void lpm_net_arp_send(const uint8_t *target, const uint8_t *sender)
{
    arp_request(target, sender);
}

const uint8_t *lpm_net_own_mac(void)
{
    return OWN_MAC;
}
