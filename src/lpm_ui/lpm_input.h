/* Logical keys and accelerated auto-repeat.
 *
 * Only Up/Down/Left/Right repeat. M (enter) and Back produce one event per
 * press so the firmware's existing long-press calibration and power actions
 * keep their meaning; the firmware's key driver must map its physical
 * buttons to these values. */
#ifndef LPM_INPUT_H
#define LPM_INPUT_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    LPM_KEY_NONE,
    LPM_KEY_UP,
    LPM_KEY_DOWN,
    LPM_KEY_LEFT,
    LPM_KEY_RIGHT,
    LPM_KEY_ENTER, /* M */
    LPM_KEY_BACK
} lpm_key;

typedef struct {
    lpm_key key;
    uint8_t repeat; /* 0 for the initial press, then 1, 2, ... while held */
} lpm_key_event;

#define LPM_REPEAT_DELAY_MS 450u
#define LPM_REPEAT_SLOW_MS 160u
#define LPM_REPEAT_FAST_MS 70u
#define LPM_REPEAT_FASTEST_MS 40u

typedef struct {
    lpm_key held;
    uint32_t next_ms;
    uint8_t count;
} lpm_repeater;

void lpm_repeater_init(lpm_repeater *r);
lpm_key_event lpm_repeater_press(lpm_repeater *r, lpm_key key, uint32_t now_ms);
void lpm_repeater_release(lpm_repeater *r);
/* Returns true and fills *event when a repeat is due. Tolerates the
 * millisecond counter wrapping. */
bool lpm_repeater_poll(lpm_repeater *r, uint32_t now_ms, lpm_key_event *event);

#endif
