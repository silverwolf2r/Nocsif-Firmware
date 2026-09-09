/*
 * NocSif — one shared read-only snapshot of BLE + WiFi radio state.
 *
 * Combines the cached getters from ble.c and wifi.c into a single struct, so every UI
 * surface that shows radio status (Control Center tiles, the Connectivity hub, watchface
 * labels) reads the same truth instead of keeping its own local copy. Purely a composition
 * of existing cached reads — no locking, no task, no hardware touched here.
 *
 * This accessor only reports state; the UI tiles are what actually toggle the radios.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool ble_logical_on;          /* Bluetooth master switch is on (this drives the tile's on/off look) */
    bool ble_controller_resident; /* the BLE controller memory was claimed and is currently held */
    bool ble_link_live;           /* a phone is actually connected (an accent state, not "on") */
    bool wifi_sta_on;             /* WiFi radio is powered on (drives the tile's on/off look) */
    bool wifi_link_live;          /* associated with an AP and has an IP (an accent, not "on") */
    bool wifi_parked;             /* WiFi is logically on but the radio is stopped to save power;
                                   * the tile still shows on, and a tap wakes the radio back up */
} nocsif_radio_state_t;

/* Fill *out with the current radio state snapshot. Safe to call from the LVGL task. */
void nocsif_radio_state(nocsif_radio_state_t *out);

#ifdef __cplusplus
}
#endif
