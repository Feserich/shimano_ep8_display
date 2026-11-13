#pragma once
#include <stdint.h>

typedef enum {
    EP8_STATE_SCANNING,
    EP8_STATE_CONNECTING,
    EP8_STATE_PAIRING,
    EP8_STATE_CONNECTED,
} ep8_state_t;

/* level: 0..100, or -1 if unknown. Called from the NimBLE host task. */
typedef void (*ep8_update_cb_t)(ep8_state_t state, int level);

void ble_battery_start(ep8_update_cb_t cb);
