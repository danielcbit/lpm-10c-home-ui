#include "lpm_input.h"

static bool repeats(lpm_key key)
{
    return key == LPM_KEY_UP || key == LPM_KEY_DOWN || key == LPM_KEY_LEFT || key == LPM_KEY_RIGHT;
}

static uint32_t interval_after(uint8_t count)
{
    if (count < 6) {
        return LPM_REPEAT_SLOW_MS;
    }
    return count < 16 ? LPM_REPEAT_FAST_MS : LPM_REPEAT_FASTEST_MS;
}

void lpm_repeater_init(lpm_repeater *r)
{
    r->held = LPM_KEY_NONE;
    r->next_ms = 0;
    r->count = 0;
}

lpm_key_event lpm_repeater_press(lpm_repeater *r, lpm_key key, uint32_t now_ms)
{
    lpm_key_event event = {key, 0};
    r->held = repeats(key) ? key : LPM_KEY_NONE;
    r->next_ms = now_ms + LPM_REPEAT_DELAY_MS;
    r->count = 0;
    return event;
}

void lpm_repeater_release(lpm_repeater *r)
{
    r->held = LPM_KEY_NONE;
}

bool lpm_repeater_poll(lpm_repeater *r, uint32_t now_ms, lpm_key_event *event)
{
    if (r->held == LPM_KEY_NONE || (int32_t)(now_ms - r->next_ms) < 0) {
        return false;
    }
    if (r->count < UINT8_MAX) {
        r->count++;
    }
    r->next_ms = now_ms + interval_after(r->count);
    event->key = r->held;
    event->repeat = r->count;
    return true;
}
