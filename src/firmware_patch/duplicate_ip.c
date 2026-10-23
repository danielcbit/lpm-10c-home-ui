/*
 * Duplicate IP, a home item of the QC slot (tools.h): is the address DHCP hands
 * out already used by another host, and does one host answer for the gateway?
 *
 *  1. Physical link            } the first two Link status checks
 *  2. Address (DHCP)           }
 *  3. Address free: three ARP probes for the address (RFC 5227: sender address
 *     0.0.0.0, so no host learns it), a second apart; any host that answers,
 *     or claims or probes the address itself, is listed
 *  4. Gateway: ARP requests for the gateway; every MAC that answers is listed,
 *     and more than one is a conflict
 *
 * Nothing is announced and nothing is saved. Keys work while it runs: the
 * waits poll the key queue (lpm_list_poll), so BACK leaves and M stops it.
 */
#include <stdint.h>

#include "fmt.h"
#include "lpm_data.h"
#include "net.h"
#include "tools.h"

#define tick ((uint32_t (*)(void))0x08023D39u)
#define delay ((void (*)(uint32_t))0x080244E5u)

#define PROBES 3
#define PROBE_GAP_MS 1000 /* RFC 5227: PROBE_MIN 1 s, PROBE_MAX 2 s */
#define GATEWAY_REQUESTS 2
#define GATEWAY_WAIT_MS 1500
#define MACS 4 /* listed per row; more are counted */
#define ETHERTYPE_ARP 0x0806

enum { DUP_LINK, DUP_ADDRESS, DUP_FREE, DUP_GATEWAY, DUP_ROWS };

typedef struct {
    uint8_t step[DUP_ROWS]; /* rows 0 and 1 come from Link status; lpm_step */
    uint8_t top;
    uint8_t claimants;     /* hosts using our address */
    uint8_t gateways;      /* MACs answering for the gateway */
    uint8_t ran;
    uint8_t claimant_mac[MACS][6];
    uint8_t gateway_mac[MACS][6];
    uint8_t ip[4], gateway[4];
} duplicate_state;

/* After tools_state (0x20020580), in the RAM the vendor never uses. */
#define DUP ((duplicate_state *)0x20020600u)

_Static_assert(sizeof(duplicate_state) == 64, "the layout the emulator reads");

static const char *const names[DUP_ROWS] = {"Physical link", "Address", "Address free", "Gateway"};
static const uint8_t lines[DUP_ROWS] = {1, 1, 2, 2};

static int count(void)
{
    return DUP_ROWS;
}

static int row_lines(int row)
{
    return lines[row];
}

static uint8_t *top(void)
{
    return &DUP->top;
}

static char *put_macs(char *p, uint8_t (*macs)[6], int n)
{
    for (int i = 0; i < n && i < MACS; i++) {
        if (i)
            p = put(p, i == 2 ? "\n" : ", ");
        p = put_mac(p, macs[i]);
    }
    if (n > MACS)
        p = put_u(put(p, " +"), (unsigned)(n - MACS));
    return p;
}

static const char *row(int r, const char **verdict, uint16_t *colour, char *p)
{
    duplicate_state *d = DUP;
    if (r <= DUP_ADDRESS && d->ran)
        return lpm_link_row(r, verdict, colour, p);
    *verdict = lpm_verdict(d->step[r], colour);
    if (r == DUP_FREE) {
        if (d->step[r] == LPM_STEP_PASS)
            put(put(put_ip(p, d->ip), " "), "answers to no other host");
        else if (d->step[r] == LPM_STEP_FAIL)
            put_macs(put(put_u(p, d->claimants), d->claimants == 1 ? " other host: " : " other hosts: "),
                     d->claimant_mac, d->claimants);
    } else if (r == DUP_GATEWAY) {
        if (d->step[r] == LPM_STEP_PASS)
            put_mac(put(put_ip(p, d->gateway), " at "), d->gateway_mac[0]);
        else if (d->step[r] == LPM_STEP_FAIL && d->gateways)
            put_macs(put(put_u(put(put_ip(p, d->gateway), " from "), d->gateways), " MACs: "), d->gateway_mac,
                     d->gateways);
        else if (d->step[r] == LPM_STEP_FAIL)
            put(put_ip(p, d->gateway), " does not answer ARP");
    }
    return names[r];
}

const row_list lpm_duplicate_list = {count, row_lines, row, top};

const char *lpm_duplicate_heading(char *p)
{
    put(p, DUP->ran ? "Probed with ARP, nothing announced" : "Is the DHCP address free? M starts");
    return p;
}

void lpm_duplicate_open(void)
{
    uint8_t *b = (uint8_t *)DUP;
    for (unsigned i = 0; i < sizeof(duplicate_state); i++)
        b[i] = 0;
}

/* A MAC not yet in the list is added (only the first MACS are kept). */
static void note(uint8_t (*macs)[6], uint8_t *n, const uint8_t *mac)
{
    for (int i = 0; i < *n && i < MACS; i++)
        if (same(macs[i], mac, 6))
            return;
    if (*n < MACS)
        copy(macs[*n], mac, 6);
    if (*n < 255)
        (*n)++;
}

/* The ARP frames that arrive for ms: who claims our address, and who answers
 * for the gateway. 1 when a key ends the run. */
static int listen(duplicate_state *d, uint32_t ms)
{
    uint32_t start = tick();
    while (tick() - start < ms) {
        if (lpm_list_poll())
            return 1;
        int n = lpm_net_receive();
        const uint8_t *f = lpm_net_frame(), *mac = f + 22, *sender = f + 28, *target = f + 38;
        if (n < 42 || get16(f + 12) != ETHERTYPE_ARP || same(mac, lpm_net_own_mac(), 6)) {
            if (n <= 0)
                delay(1);
            continue;
        }
        /* A reply or announcement from our address, or another host's probe for it. */
        if (same(sender, d->ip, 4) || (get16(f + 20) == 1 && !(sender[0] | sender[1] | sender[2] | sender[3]) &&
                                       same(target, d->ip, 4)))
            note(d->claimant_mac, &d->claimants, mac);
        else if (same(sender, d->gateway, 4) && get16(f + 20) == 2)
            note(d->gateway_mac, &d->gateways, mac);
    }
    return 0;
}

static void begin(duplicate_state *d, int r)
{
    d->step[r] = LPM_STEP_RUNNING;
    lpm_list_follow(r);
}

void lpm_duplicate_run(void)
{
    duplicate_state *d = DUP;
    static const uint8_t none[4] = {0, 0, 0, 0};
    lpm_duplicate_open();
    d->ran = 1;
    lpm_list_show();
    int ready = lpm_net_dhcp();
    copy(d->ip, lpm_net_ip(), 4);
    copy(d->gateway, lpm_net_gateway(), 4);
    int stopped = 0;
    if (ready) {
        begin(d, DUP_FREE);
        for (int i = 0; i < PROBES && !stopped; i++) {
            lpm_net_arp_send(d->ip, none);
            stopped = listen(d, PROBE_GAP_MS);
        }
        if (!stopped)
            d->step[DUP_FREE] = d->claimants ? LPM_STEP_FAIL : LPM_STEP_PASS;
        lpm_list_show();
    }
    if (ready && !stopped && (d->gateway[0] | d->gateway[1] | d->gateway[2] | d->gateway[3])) {
        begin(d, DUP_GATEWAY);
        for (int i = 0; i < GATEWAY_REQUESTS && !stopped; i++) {
            lpm_net_arp_send(d->gateway, d->ip);
            stopped = listen(d, GATEWAY_WAIT_MS);
        }
        if (!stopped)
            d->step[DUP_GATEWAY] = d->gateways == 1 ? LPM_STEP_PASS : LPM_STEP_FAIL;
    }
    for (int r = DUP_FREE; r < DUP_ROWS; r++)
        if (d->step[r] == LPM_STEP_PENDING || d->step[r] == LPM_STEP_RUNNING)
            d->step[r] = LPM_STEP_SKIPPED;
    lpm_list_follow(DUP_ROWS - 1);
}
