/*
 * NocSif — shared radio-state accessor (RAM Phase 2 / PLAN §4.13, docs/RAM-BUDGET.md remake #6).
 *
 * ONE read-only view of the live radio state, composed from the resident-controller BLE model
 * (ble.c) + the WiFi STA worker (wifi.c), so the Control Center tiles, the Connectivity hub, and the
 * watchface / planet status labels all read the SAME truth instead of divergent local UI flags. Pure
 * composition of existing LVGL-safe cached getters — no lock, no task, no int-DMA touch.
 *
 * NOT folded into coex.h on purpose: coex.h is a dependency-free leaf (esp_heap_caps.h / esp_log.h
 * only, safe to include from bare drivers), and this accessor pulls in ble.h + wifi.h.
 *
 * Governor note (RAM Phase 2): a BLE tile tap is a LOGICAL activity toggle over the resident
 * controller (nocsif_ble_bt_set_enabled) — never a crash/refuse/restart, because the ~31.7 KB block
 * is claimed at boot and held for the session. A WiFi tile tap toggles the STA
 * (nocsif_wifi_request_enable). This accessor only REFLECTS; the tiles DRIVE.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool ble_logical_on;          /* Bluetooth master toggled on (nocsif_ble_bt_enabled) — the tile on/off */
    bool ble_controller_resident; /* the ~31.7 KB controller block is claimed + held (boot reserve ran)    */
    bool ble_link_live;           /* a phone is actually linked (ANCS connected/ready) — an accent, not on  */
    bool wifi_sta_on;             /* STA radio powered on (nocsif_wifi_enabled) — the tile on/off           */
    bool wifi_link_live;          /* associated + has IP (nocsif_wifi_connected) — an accent, not on        */
    bool wifi_parked;             /* §4.6 Governor P1: intent ON but the STA is stopped to save power — the
                                   * tile reads ON ("idle"); a tap wakes it (nocsif_gov_wifi_wake)          */
} nocsif_radio_state_t;

/* Fill *out with the current live radio state. Safe from the LVGL task (cached getters only). */
void nocsif_radio_state(nocsif_radio_state_t *out);

#ifdef __cplusplus
}
#endif
