/* Measurement state supplied by the existing firmware test routines.
 *
 * The UI only renders what these structures report: it never infers cable
 * category, per-pair lengths, load power or Internet reachability. All
 * storage is fixed-size; nothing here allocates. */
#ifndef LPM_DATA_H
#define LPM_DATA_H

#include <stdbool.h>
#include <stdint.h>

#define LPM_PINS 8
#define LPM_PAIRS 4
#define LPM_PING_HISTORY 120
#define LPM_POE_HISTORY 60
#define LPM_SCAN_MAX_HOSTS 48

/* ---- Cable map ---------------------------------------------------------- */

typedef enum { LPM_PIN_OK, LPM_PIN_OPEN, LPM_PIN_SHORT, LPM_PIN_CROSSED } lpm_pin_state;

typedef enum {
    LPM_WIRING_NO_RESULT,
    LPM_WIRING_STRAIGHT,
    LPM_WIRING_CROSSOVER, /* 1-3/2-6 swapped, optionally 4-7/5-8 too */
    LPM_WIRING_FAULT
} lpm_wiring;

typedef struct {
    bool valid;                   /* remote unit answered */
    uint8_t remote[LPM_PINS];     /* remote pin (1..8) seen for local pin i+1; 0 = open */
    uint8_t shorted_to[LPM_PINS]; /* local pin (1..8) shorted to pin i+1; 0 = none */
} lpm_wiremap;

typedef struct {
    lpm_wiring wiring;
    uint8_t open, shorted, crossed;
} lpm_wiremap_summary;

lpm_pin_state lpm_wiremap_pin(const lpm_wiremap *map, uint8_t pin_index);
lpm_wiremap_summary lpm_wiremap_summarize(const lpm_wiremap *map);

/* ---- Length ------------------------------------------------------------- */

typedef struct {
    bool valid;
    bool per_pair;               /* only true if the firmware measured pairs independently */
    uint32_t total_cm;           /* calibrated estimate */
    uint32_t pair_cm[LPM_PAIRS]; /* 1-2, 3-6, 4-5, 7-8 */
} lpm_length;

/* ---- PoE voltage -------------------------------------------------------- */

typedef struct {
    uint16_t mv[LPM_POE_HISTORY];
    uint8_t head; /* next write slot */
    uint8_t count;
    uint16_t min_mv, max_mv;
    const char *mode; /* e.g. "Pairs 1-2/3-6" if the measurement exposes it, else NULL */
} lpm_poe_log;

void lpm_poe_reset(lpm_poe_log *log);
void lpm_poe_add(lpm_poe_log *log, uint16_t millivolts);
/* index 0 is the oldest retained sample */
uint16_t lpm_poe_at(const lpm_poe_log *log, uint8_t index);

/* ---- Ping --------------------------------------------------------------- */

typedef enum { LPM_PING_REPLY = 1, LPM_PING_TIMEOUT = 2 } lpm_ping_status;

typedef struct {
    uint32_t rtt_us;
    uint16_t seq;
    uint8_t status;
    uint8_t reserved;
} lpm_ping_sample; /* 8 bytes: 120 samples cost 960 bytes */

typedef struct {
    lpm_ping_sample samples[LPM_PING_HISTORY];
    uint8_t head; /* next write slot */
    uint8_t count;
    uint32_t target_ip;
    /* Whole-session statistics, not just the retained window. */
    uint32_t sent, received;
    uint32_t min_us, max_us;
    uint64_t sum_us;
    uint64_t variation_sum_us; /* sum of |rtt[n] - rtt[n-1]| over consecutive replies */
    uint32_t variation_count;
    uint32_t last_rtt_us;
    uint16_t last_seq;
    bool chain; /* previous probe was a reply, so variation can be updated */
} lpm_ping_log;

void lpm_ping_reset(lpm_ping_log *log, uint32_t target_ip);
/* Both return false (and record nothing) for a repeat of the last sequence
 * number, e.g. a duplicate or late reply the driver did not filter. */
bool lpm_ping_add_reply(lpm_ping_log *log, uint16_t seq, uint32_t rtt_us);
bool lpm_ping_add_timeout(lpm_ping_log *log, uint16_t seq);
const lpm_ping_sample *lpm_ping_at(const lpm_ping_log *log, uint8_t index);
uint32_t lpm_ping_mean_us(const lpm_ping_log *log);
/* Mean absolute difference between consecutive replies. Shown as "RTT
 * variation", not as voice-quality jitter. */
uint32_t lpm_ping_variation_us(const lpm_ping_log *log);
uint32_t lpm_ping_lost(const lpm_ping_log *log);
/* Loss in tenths of a percent (0..1000). */
uint32_t lpm_ping_loss_permille(const lpm_ping_log *log);

/* ---- IP scan (ARP) ------------------------------------------------------ */

#define LPM_HOST_GATEWAY 0x01u
#define LPM_HOST_DUPLICATE 0x02u /* same IP answered from more than one MAC */

typedef struct {
    uint32_t ip;
    uint8_t mac[6];
    uint8_t flags;
    uint8_t reserved;
} lpm_host;

typedef struct {
    lpm_host hosts[LPM_SCAN_MAX_HOSTS];
    uint8_t count;
    bool running;
    bool cancelled;
    bool overflow; /* more replies than storage */
    uint16_t probed, total;
    uint32_t first_ip, last_ip;
    uint32_t gateway_ip; /* 0 if unknown */
} lpm_scan;

void lpm_scan_reset(lpm_scan *scan, uint32_t first_ip, uint32_t last_ip, uint32_t gateway_ip);
/* Records an ARP reply. Hosts are kept in IP order; a repeat from the same MAC
 * is ignored and a different MAC for a known IP marks both entries duplicate. */
void lpm_scan_add(lpm_scan *scan, uint32_t ip, const uint8_t mac[6]);

/* ---- DHCP --------------------------------------------------------------- */

typedef enum {
    LPM_DHCP_IDLE,
    LPM_DHCP_DISCOVERING, /* DISCOVER sent, waiting for OFFER */
    LPM_DHCP_REQUESTING,  /* OFFER received, REQUEST sent */
    LPM_DHCP_ACKED,       /* lease confirmed */
    LPM_DHCP_NAKED,
    LPM_DHCP_NO_OFFER,
    LPM_DHCP_OFFER_ONLY /* timed out after OFFER; address is NOT a confirmed lease */
} lpm_dhcp_state;

typedef struct {
    lpm_dhcp_state state;
    uint16_t offer_ms; /* DISCOVER -> OFFER */
    uint16_t ack_ms;   /* REQUEST -> ACK/NAK */
    uint32_t server, offered_ip, mask, router;
    bool lease_known; /* option 51 parsed */
    uint32_t lease_s;
} lpm_dhcp;

/* ---- Link health checklist ---------------------------------------------- */

typedef enum {
    LPM_STEP_PENDING,
    LPM_STEP_RUNNING,
    LPM_STEP_PASS,
    LPM_STEP_WARN,
    LPM_STEP_FAIL,
    LPM_STEP_SKIPPED
} lpm_step;

typedef enum { LPM_ADDR_NONE, LPM_ADDR_DHCP, LPM_ADDR_OFFER_ONLY, LPM_ADDR_STATIC } lpm_addr_source;

typedef struct {
    lpm_step link, address, gateway_arp, gateway_ping;
    uint8_t link_mbps; /* 10 or 100 from the DM9051, 0 unknown */
    bool full_duplex;
    lpm_addr_source source;
    uint32_t ip, gateway;
    uint8_t gateway_mac[6];
    uint32_t gateway_rtt_us;
} lpm_health;

/* ---- Everything the UI reads -------------------------------------------- */

typedef struct {
    int8_t battery_pct; /* -1 unknown */
    bool charging;
    bool feet;      /* length unit setting */
    bool menu_wrap; /* list boundary behaviour; default stop */
} lpm_status;

typedef struct {
    lpm_status status;
    lpm_wiremap wiremap;
    lpm_length length;
    lpm_poe_log poe;
    lpm_ping_log ping;
    lpm_scan scan;
    lpm_dhcp dhcp;
    lpm_health health;
} lpm_data;

#endif
