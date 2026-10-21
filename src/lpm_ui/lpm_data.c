#include "lpm_data.h"

lpm_pin_state lpm_wiremap_pin(const lpm_wiremap *map, uint8_t pin_index)
{
    if (map->shorted_to[pin_index]) {
        return LPM_PIN_SHORT;
    }
    if (map->remote[pin_index] == 0) {
        return LPM_PIN_OPEN;
    }
    return map->remote[pin_index] == pin_index + 1 ? LPM_PIN_OK : LPM_PIN_CROSSED;
}

static bool maps(const lpm_wiremap *map, uint8_t local, uint8_t remote)
{
    return map->remote[local - 1] == remote;
}

lpm_wiremap_summary lpm_wiremap_summarize(const lpm_wiremap *map)
{
    lpm_wiremap_summary summary = {LPM_WIRING_NO_RESULT, 0, 0, 0};
    uint8_t i;
    if (!map->valid) {
        return summary;
    }
    for (i = 0; i < LPM_PINS; i++) {
        switch (lpm_wiremap_pin(map, i)) {
        case LPM_PIN_OPEN:
            summary.open++;
            break;
        case LPM_PIN_SHORT:
            summary.shorted++;
            break;
        case LPM_PIN_CROSSED:
            summary.crossed++;
            break;
        case LPM_PIN_OK:
            break;
        }
    }
    if (summary.open || summary.shorted) {
        summary.wiring = LPM_WIRING_FAULT;
    } else if (!summary.crossed) {
        summary.wiring = LPM_WIRING_STRAIGHT;
    } else if (maps(map, 1, 3) && maps(map, 2, 6) && maps(map, 3, 1) && maps(map, 6, 2) &&
               ((maps(map, 4, 4) && maps(map, 5, 5) && maps(map, 7, 7) && maps(map, 8, 8)) ||
                (maps(map, 4, 7) && maps(map, 5, 8) && maps(map, 7, 4) && maps(map, 8, 5)))) {
        summary.wiring = LPM_WIRING_CROSSOVER;
    } else {
        summary.wiring = LPM_WIRING_FAULT;
    }
    return summary;
}

void lpm_poe_reset(lpm_poe_log *log)
{
    log->head = 0;
    log->count = 0;
    log->min_mv = 0;
    log->max_mv = 0;
}

void lpm_poe_add(lpm_poe_log *log, uint16_t millivolts)
{
    if (log->count == 0 || millivolts < log->min_mv) {
        log->min_mv = millivolts;
    }
    if (log->count == 0 || millivolts > log->max_mv) {
        log->max_mv = millivolts;
    }
    log->mv[log->head] = millivolts;
    log->head = (uint8_t)((log->head + 1) % LPM_POE_HISTORY);
    if (log->count < LPM_POE_HISTORY) {
        log->count++;
    }
}

uint16_t lpm_poe_at(const lpm_poe_log *log, uint8_t index)
{
    return log->mv[(log->head + LPM_POE_HISTORY - log->count + index) % LPM_POE_HISTORY];
}

void lpm_ping_reset(lpm_ping_log *log, uint32_t target_ip)
{
    log->head = 0;
    log->count = 0;
    log->target_ip = target_ip;
    log->sent = 0;
    log->received = 0;
    log->min_us = 0;
    log->max_us = 0;
    log->sum_us = 0;
    log->variation_sum_us = 0;
    log->variation_count = 0;
    log->last_rtt_us = 0;
    log->last_seq = 0;
    log->chain = false;
}

static bool record(lpm_ping_log *log, uint16_t seq, uint32_t rtt_us, lpm_ping_status status)
{
    lpm_ping_sample *sample;
    if (log->sent && seq == log->last_seq) {
        return false;
    }
    sample = &log->samples[log->head];
    sample->rtt_us = rtt_us;
    sample->seq = seq;
    sample->status = (uint8_t)status;
    sample->reserved = 0;
    log->head = (uint8_t)((log->head + 1) % LPM_PING_HISTORY);
    if (log->count < LPM_PING_HISTORY) {
        log->count++;
    }
    log->sent++;
    log->last_seq = seq;
    return true;
}

bool lpm_ping_add_reply(lpm_ping_log *log, uint16_t seq, uint32_t rtt_us)
{
    if (!record(log, seq, rtt_us, LPM_PING_REPLY)) {
        return false;
    }
    if (log->received == 0 || rtt_us < log->min_us) {
        log->min_us = rtt_us;
    }
    if (log->received == 0 || rtt_us > log->max_us) {
        log->max_us = rtt_us;
    }
    log->received++;
    log->sum_us += rtt_us;
    if (log->chain) {
        log->variation_sum_us += rtt_us > log->last_rtt_us ? rtt_us - log->last_rtt_us : log->last_rtt_us - rtt_us;
        log->variation_count++;
    }
    log->last_rtt_us = rtt_us;
    log->chain = true;
    return true;
}

bool lpm_ping_add_timeout(lpm_ping_log *log, uint16_t seq)
{
    if (!record(log, seq, 0, LPM_PING_TIMEOUT)) {
        return false;
    }
    log->chain = false;
    return true;
}

const lpm_ping_sample *lpm_ping_at(const lpm_ping_log *log, uint8_t index)
{
    return &log->samples[(log->head + LPM_PING_HISTORY - log->count + index) % LPM_PING_HISTORY];
}

uint32_t lpm_ping_mean_us(const lpm_ping_log *log)
{
    return log->received ? (uint32_t)((log->sum_us + log->received / 2) / log->received) : 0;
}

uint32_t lpm_ping_variation_us(const lpm_ping_log *log)
{
    return log->variation_count ? (uint32_t)((log->variation_sum_us + log->variation_count / 2) / log->variation_count)
                                : 0;
}

uint32_t lpm_ping_lost(const lpm_ping_log *log)
{
    return log->sent - log->received;
}

uint32_t lpm_ping_loss_permille(const lpm_ping_log *log)
{
    return log->sent ? (lpm_ping_lost(log) * 1000u + log->sent / 2) / log->sent : 0;
}

void lpm_scan_reset(lpm_scan *scan, uint32_t first_ip, uint32_t last_ip, uint32_t gateway_ip)
{
    scan->count = 0;
    scan->running = true;
    scan->cancelled = false;
    scan->overflow = false;
    scan->probed = 0;
    scan->total = last_ip >= first_ip && last_ip - first_ip < UINT16_MAX ? (uint16_t)(last_ip - first_ip + 1) : 0;
    scan->first_ip = first_ip;
    scan->last_ip = last_ip;
    scan->gateway_ip = gateway_ip;
}

static bool same_mac(const uint8_t *a, const uint8_t *b)
{
    uint8_t i;
    for (i = 0; i < 6; i++) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

void lpm_scan_add(lpm_scan *scan, uint32_t ip, const uint8_t mac[6])
{
    uint8_t at = 0;
    uint8_t i;
    bool duplicate = false;

    while (at < scan->count && scan->hosts[at].ip < ip) {
        at++;
    }
    for (i = at; i < scan->count && scan->hosts[i].ip == ip; i++) {
        if (same_mac(scan->hosts[i].mac, mac)) {
            return;
        }
        scan->hosts[i].flags |= LPM_HOST_DUPLICATE;
        duplicate = true;
    }
    if (scan->count == LPM_SCAN_MAX_HOSTS) {
        scan->overflow = true;
        return;
    }
    at = i;
    for (i = scan->count; i > at; i--) {
        scan->hosts[i] = scan->hosts[i - 1];
    }
    scan->hosts[at].ip = ip;
    for (i = 0; i < 6; i++) {
        scan->hosts[at].mac[i] = mac[i];
    }
    scan->hosts[at].flags =
        (uint8_t)((ip == scan->gateway_ip ? LPM_HOST_GATEWAY : 0u) | (duplicate ? LPM_HOST_DUPLICATE : 0u));
    scan->hosts[at].reserved = 0;
    scan->count++;
}
