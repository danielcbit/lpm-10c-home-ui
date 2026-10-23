/*
 * Discovery, a home item of the QC slot (tools.h): the hosts on the subnet.
 *
 * M takes a DHCP address (the first two Link status checks), then sends an ARP
 * request to every address of the subnet, twice for those that stay silent,
 * and lists each host that answers: its address, MAC and manufacturer (the MAC
 * prefix, oui_table.c), and the name the DHCP DNS server gives for it (a PTR
 * query; home routers answer with their DHCP clients' names). A host that also
 * answers from a second MAC is marked as a duplicate. A subnet wider than /24
 * is scanned in the /24 of our address only.
 *
 * Keys work while it runs (lpm_list_poll): UP and DOWN scroll the hosts found
 * so far, M stops, BACK leaves. Nothing is saved.
 */
#include <stdint.h>

#include "fmt.h"
#include "lpm_theme.h"
#include "net.h"
#include "oui_table.h"
#include "tools.h"

#define tick ((uint32_t (*)(void))0x08023D39u)
#define delay ((void (*)(uint32_t))0x080244E5u)

#define HOSTS_MAX 254
#define NAME_MAX 24
#define SWEEPS 2         /* the second asks only those not heard yet */
#define SEND_GAP_MS 4    /* between ARP requests: about a second for a /24 */
#define SWEEP_WAIT_MS 1500
#define NAME_WAIT_MS 400
#define PROGRESS_EVERY 16
#define ETHERTYPE_ARP 0x0806
#define ETHERTYPE_IPV4 0x0800
#define NAME_PORT 50001
#define DNS_PORT 53
#define DNS_ID 0x4400 /* + the host's index */
#define TYPE_PTR 12
#define MAX_FRAME 1514

enum { HOST_SELF = 1, HOST_GATEWAY = 2, HOST_DUPLICATE = 4 };
enum { VENDOR_UNKNOWN = 0, VENDOR_PRIVATE = 0xFF }; /* else 1 + lpm_oui_names index */
enum { DISC_IDLE, DISC_ADDRESS, DISC_NO_LINK, DISC_NO_ADDRESS, DISC_SCANNING, DISC_NAMING, DISC_DONE, DISC_STOPPED };

typedef struct {
    uint8_t ip[4], mac[6];
    uint8_t other_mac[6]; /* the second MAC answering for ip */
    uint8_t flags;        /* HOST_* */
    uint8_t vendor;       /* VENDOR_* */
    char name[NAME_MAX];  /* from the PTR answer, cut; empty when none */
} host;

typedef struct {
    uint8_t phase; /* DISC_* */
    uint8_t top;
    uint8_t prefix;  /* of the subnet scanned */
    uint8_t wider;   /* the subnet is wider than the /24 scanned */
    uint8_t net[4];  /* its first address */
    uint16_t count;  /* hosts found */
    uint16_t done;   /* addresses asked, or hosts named */
    uint16_t total;  /* addresses to ask, or hosts to name */
    uint16_t duplicates;
} discovery_state;

/* After duplicate_state (0x20020600), in the RAM the vendor never uses: 16 bytes,
 * then 254 hosts of 42 bytes up to 0x200231AC. */
#define DISC ((discovery_state *)0x20020680u)
#define HOSTS ((host *)0x20020800u)

_Static_assert(sizeof(discovery_state) == 16 && sizeof(host) == 42, "the layout the emulator reads");

/* ---- the list (GUI task) ------------------------------------------------------ */

static int count(void)
{
    return DISC->count;
}

static int lines(int row)
{
    (void)row;
    return 2;
}

static uint8_t *top(void)
{
    return &DISC->top;
}

/* The detail goes in p[0..95]; the name, the address, is made at p + 96. */
static const char *row(int r, const char **verdict, uint16_t *colour, char *p)
{
    const host *h = &HOSTS[r];
    char *name = p + 96;
    put_ip(name, h->ip);
    *verdict = h->flags & HOST_DUPLICATE ? "DUPLICATE" : h->flags & HOST_SELF ? "THIS" : h->flags & HOST_GATEWAY ? "GATEWAY" : "";
    *colour = h->flags & HOST_DUPLICATE ? LPM_C_FAULT : h->flags & HOST_GATEWAY ? LPM_C_ACCENT : LPM_C_DIM;
    p = put_mac(p, h->mac);
    if (h->vendor == VENDOR_PRIVATE)
        p = put(p, " private MAC");
    else if (h->vendor != VENDOR_UNKNOWN)
        p = put(put(p, " "), lpm_oui_names[h->vendor - 1]);
    *p++ = '\n';
    if (h->flags & HOST_DUPLICATE)
        p = put_mac(put(p, "and "), h->other_mac);
    else if (h->name[0])
        p = put(p, h->name);
    else if (h->flags & HOST_SELF)
        p = put(p, "this tester");
    *p = 0;
    return name;
}

const row_list lpm_discovery_list = {count, lines, row, top};

static char *put_subnet(char *p, const discovery_state *d)
{
    return put_u(put(put_ip(p, d->net), "/"), d->prefix);
}

const char *lpm_discovery_heading(char *p)
{
    discovery_state *d = DISC;
    char *q = p;
    switch (d->phase) {
    case DISC_IDLE:
        put(q, "Finds the hosts on this subnet. M starts");
        break;
    case DISC_ADDRESS:
        put(q, "Getting an address");
        break;
    case DISC_NO_LINK:
        put(q, "No link: plug into the top-left port");
        break;
    case DISC_NO_ADDRESS:
        put(q, "No DHCP address");
        break;
    case DISC_SCANNING:
        put_u(put(put_u(put(put_subnet(put(q, "ARP "), d), ": "), d->done), " of "), d->total);
        break;
    case DISC_NAMING:
        put_u(put(put_u(put(q, "Names: "), d->done), " of "), d->total);
        break;
    default:
        q = put(put_u(q, d->count), d->count == 1 ? " host on " : " hosts on ");
        q = put_subnet(q, d);
        if (d->wider)
            q = put(q, " only");
        if (d->duplicates)
            q = put(put_u(put(q, ", "), d->duplicates), " dup");
        if (d->phase == DISC_STOPPED)
            put(q, ", stopped");
    }
    return p;
}

/* ---- the scan (key task) ------------------------------------------------------ */

void lpm_discovery_open(void)
{
    uint8_t *b = (uint8_t *)DISC;
    for (unsigned i = 0; i < sizeof(discovery_state); i++)
        b[i] = 0;
}

/* The manufacturer of a MAC: its prefix in the sorted chunks, by halving. */
static uint8_t vendor_of(const uint8_t *mac)
{
    if (mac[0] & 2) /* locally administered: a phone's random address, a VM */
        return VENDOR_PRIVATE;
    uint32_t key = (uint32_t)mac[0] << 16 | mac[1] << 8 | mac[2];
    for (int c = 0; c < LPM_OUI_CHUNKS; c++) {
        const uint8_t(*e)[4] = lpm_oui_chunks[c].entries;
        int low = 0, high = lpm_oui_chunks[c].count - 1;
        while (low <= high) {
            int mid = (low + high) / 2;
            uint32_t k = (uint32_t)e[mid][0] << 16 | e[mid][1] << 8 | e[mid][2];
            if (k == key)
                return (uint8_t)(e[mid][3] + 1);
            if (k < key)
                low = mid + 1;
            else
                high = mid - 1;
        }
    }
    return VENDOR_UNKNOWN;
}

static uint32_t value(const uint8_t *ip)
{
    return (uint32_t)ip[0] << 24 | (uint32_t)ip[1] << 16 | (uint32_t)ip[2] << 8 | ip[3];
}

static void bytes(uint8_t *ip, uint32_t v)
{
    ip[0] = (uint8_t)(v >> 24);
    ip[1] = (uint8_t)(v >> 16);
    ip[2] = (uint8_t)(v >> 8);
    ip[3] = (uint8_t)v;
}

static int in_subnet(const discovery_state *d, const uint8_t *ip)
{
    uint32_t base = value(d->net), v = value(ip);
    return v > base && v - base < (1u << (32 - d->prefix)) - 1;
}

static host *find(const discovery_state *d, const uint8_t *ip)
{
    for (int i = 0; i < d->count; i++)
        if (same(HOSTS[i].ip, ip, 4))
            return &HOSTS[i];
    return 0;
}

/* A host heard at ip from mac: added in address order, or marked a duplicate. */
static void heard(discovery_state *d, const uint8_t *ip, const uint8_t *mac, int flags)
{
    host *h = find(d, ip);
    if (h) {
        if (!same(h->mac, mac, 6) && !(h->flags & HOST_DUPLICATE)) {
            h->flags |= HOST_DUPLICATE;
            copy(h->other_mac, mac, 6);
            d->duplicates++;
        }
        return;
    }
    if (d->count >= HOSTS_MAX)
        return;
    int at = d->count;
    while (at > 0 && value(HOSTS[at - 1].ip) > value(ip))
        at--;
    for (int i = d->count; i > at; i--) /* no struct copy: it would call memcpy, which is not linked */
        copy((uint8_t *)&HOSTS[i], (const uint8_t *)&HOSTS[i - 1], sizeof(host));
    h = &HOSTS[at];
    uint8_t *b = (uint8_t *)h;
    for (unsigned i = 0; i < sizeof(host); i++)
        b[i] = 0;
    copy(h->ip, ip, 4);
    copy(h->mac, mac, 6);
    h->flags = (uint8_t)(flags | (same(ip, lpm_net_gateway(), 4) ? HOST_GATEWAY : 0));
    h->vendor = vendor_of(mac);
    d->count++;
}

/* Every ARP frame waiting: its sender, when on the subnet. */
static void drain(discovery_state *d)
{
    int n;
    while ((n = lpm_net_receive()) > 0) {
        const uint8_t *f = lpm_net_frame();
        if (n >= 42 && get16(f + 12) == ETHERTYPE_ARP && in_subnet(d, f + 28) && !same(f + 22, lpm_net_own_mac(), 6))
            heard(d, f + 28, f + 22, 0);
    }
}

/* Waits ms for ARP answers; 1 when a key ends the run. */
static int listen(discovery_state *d, uint32_t ms)
{
    uint32_t start = tick();
    while (tick() - start < ms) {
        if (lpm_list_poll())
            return 1;
        drain(d);
        delay(1);
    }
    return 0;
}

static int sweep(discovery_state *d, int pass)
{
    uint32_t base = value(d->net), size = 1u << (32 - d->prefix);
    uint8_t target[4];
    d->done = 0;
    for (uint32_t i = 1; i + 1 < size; i++) {
        bytes(target, base + i);
        d->done = (uint16_t)i;
        if (i % PROGRESS_EVERY == 0)
            lpm_list_show();
        if (same(target, lpm_net_ip(), 4) || (pass && find(d, target)))
            continue;
        lpm_net_arp_send(target, lpm_net_ip());
        drain(d);
        if (lpm_list_poll())
            return 1;
        delay(SEND_GAP_MS);
    }
    d->done = d->total;
    return listen(d, SWEEP_WAIT_MS);
}

/* ---- names ---------------------------------------------------------------------- */

static unsigned checksum(const uint8_t *p, int n)
{
    uint32_t sum = 0;
    for (int i = 0; i + 1 < n; i += 2)
        sum += (uint32_t)(p[i] << 8 | p[i + 1]);
    if (n & 1)
        sum += (uint32_t)p[n - 1] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return ~sum & 0xFFFF;
}

/* "d.c.b.a.in-addr.arpa" PTR, from NAME_PORT to the server through mac. */
static void ask_name(const uint8_t *mac, const uint8_t *server, const uint8_t *ip, unsigned id)
{
    static const uint8_t arpa[] = {7, 'i', 'n', '-', 'a', 'd', 'd', 'r', 4, 'a', 'r', 'p', 'a', 0, 0, TYPE_PTR, 0, 1};
    uint8_t *f = lpm_net_frame(), *ipv4 = f + 14, *udp = ipv4 + 20, *dns = udp + 8, *q = dns + 12;
    for (int i = 0; i < 12; i++)
        dns[i] = 0;
    put16(dns, id);
    dns[2] = 1; /* recursion desired */
    dns[5] = 1; /* one question */
    for (int i = 3; i >= 0; i--) {
        char digits[4];
        char *end = put_u(digits, ip[i]);
        *q++ = (uint8_t)(end - digits);
        for (char *c = digits; c < end; c++)
            *q++ = (uint8_t)*c;
    }
    copy(q, arpa, sizeof arpa);
    q += sizeof arpa;
    int dns_len = (int)(q - dns);
    put16(udp, NAME_PORT);
    put16(udp + 2, DNS_PORT);
    put16(udp + 4, 8 + dns_len);
    put16(udp + 6, 0); /* no UDP checksum, as IPv4 allows */
    for (int i = 0; i < 20; i++)
        ipv4[i] = 0;
    ipv4[0] = 0x45;
    put16(ipv4 + 2, 20 + 8 + dns_len);
    put16(ipv4 + 4, id);
    ipv4[8] = 64;
    ipv4[9] = 17;
    copy(ipv4 + 12, lpm_net_ip(), 4);
    copy(ipv4 + 16, server, 4);
    put16(ipv4 + 10, checksum(ipv4, 20));
    copy(f, mac, 6);
    copy(f + 6, lpm_net_own_mac(), 6);
    put16(f + 12, ETHERTYPE_IPV4);
    lpm_net_transmit(14 + 20 + 8 + dns_len);
}

/* The name at p in the message m..end, following compression, into out as
 * dotted labels cut to NAME_MAX - 1. Returns the length, or -1 if malformed. */
static int read_name(const uint8_t *m, const uint8_t *end, const uint8_t *p, char *out)
{
    int n = 0, hops = 0;
    while (p < end && *p) {
        if ((*p & 0xC0) == 0xC0) {
            if (p + 1 >= end || ++hops > 8)
                return -1;
            p = m + ((p[0] & 0x3F) << 8 | p[1]);
            continue;
        }
        int len = *p++;
        if (p + len > end)
            return -1;
        if (n && n < NAME_MAX - 1)
            out[n++] = '.';
        for (int i = 0; i < len && n < NAME_MAX - 1; i++)
            out[n++] = (char)p[i];
        p += len;
    }
    out[n] = 0;
    return n;
}

static const uint8_t *skip_name(const uint8_t *p, const uint8_t *end)
{
    while (p < end && *p) {
        if ((*p & 0xC0) == 0xC0)
            return p + 2;
        p += 1 + *p;
    }
    return p + 1;
}

/* A PTR answer with id for h, read into its name; 1 when it was the answer. */
static int name_reply(host *h, unsigned id, int n)
{
    const uint8_t *f = lpm_net_frame(), *end = f + (n < MAX_FRAME ? n : MAX_FRAME);
    const uint8_t *ip = f + 14, *udp = ip + (ip[0] & 15) * 4, *m = udp + 8, *p;
    if (n < 14 + 20 + 8 + 12 || get16(f + 12) != ETHERTYPE_IPV4 || ip[9] != 17 || get16(udp) != DNS_PORT ||
        get16(udp + 2) != NAME_PORT || get16(m) != id)
        return 0;
    if ((m[3] & 15) || !get16(m + 6)) /* an error, or no answer: no name, but answered */
        return 1;
    p = m + 12;
    for (unsigned i = 0; i < get16(m + 4); i++)
        p = skip_name(p, end) + 4;
    for (unsigned i = 0; i < get16(m + 6) && p < end; i++) {
        p = skip_name(p, end);
        if (p + 10 > end)
            return 1;
        unsigned type = get16(p), length = get16(p + 8);
        if (type == TYPE_PTR && p + 10 + length <= end) {
            char name[NAME_MAX];
            if (read_name(m, end, p + 10, name) > 0)
                copy((uint8_t *)h->name, (const uint8_t *)name, NAME_MAX);
            return 1;
        }
        p += 10 + length;
    }
    return 1;
}

/* Names of the hosts from the DHCP DNS server, reached through its own MAC on
 * the subnet or the gateway's. 1 when a key ends the run. */
static int names(discovery_state *d)
{
    const uint8_t *server = lpm_net_dns();
    host *via = in_subnet(d, server) ? find(d, server) : find(d, lpm_net_gateway());
    if (!(server[0] | server[1] | server[2] | server[3]) || !via)
        return 0;
    d->phase = DISC_NAMING;
    d->total = d->count;
    for (int i = 0; i < d->count; i++) {
        host *h = &HOSTS[i];
        d->done = (uint16_t)(i + 1);
        lpm_list_show();
        if (h->flags & HOST_SELF)
            continue;
        unsigned id = DNS_ID + (unsigned)i;
        ask_name(via->mac, server, h->ip, id);
        uint32_t start = tick();
        int answered = 0;
        while (!answered && tick() - start < NAME_WAIT_MS) {
            if (lpm_list_poll())
                return 1;
            int n = lpm_net_receive();
            if (n > 0)
                answered = name_reply(h, id, n);
            else
                delay(1);
        }
    }
    return 0;
}

void lpm_discovery_run(void)
{
    discovery_state *d = DISC;
    lpm_discovery_open();
    lpm_list_reset_view();
    d->phase = DISC_ADDRESS;
    lpm_list_show();
    if (!lpm_net_dhcp()) {
        d->phase = lpm_net_link_up() ? DISC_NO_ADDRESS : DISC_NO_LINK;
        return;
    }
    const uint8_t *ip = lpm_net_ip(), *mask = lpm_net_mask();
    int prefix = 0;
    for (int i = 0; i < 4; i++)
        for (int bit = 7; bit >= 0 && (mask[i] >> bit & 1); bit--)
            prefix++;
    d->wider = prefix < 24;
    d->prefix = (uint8_t)(prefix < 24 ? 24 : prefix > 30 ? 30 : prefix);
    bytes(d->net, value(ip) & ~((1u << (32 - d->prefix)) - 1));
    d->total = (uint16_t)((1u << (32 - d->prefix)) - 2);
    heard(d, ip, lpm_net_own_mac(), HOST_SELF);
    d->phase = DISC_SCANNING;
    int stopped = 0;
    for (int pass = 0; pass < SWEEPS && !stopped; pass++)
        stopped = sweep(d, pass);
    if (!stopped)
        stopped = names(d);
    d->phase = stopped ? DISC_STOPPED : DISC_DONE;
}
