/*
 * NocSif — shared radio-state accessor (RAM Phase 2 / PLAN §4.13). See radio_state.h.
 *
 * Pure composition of cached getters. Every field mirrors the ONE authoritative source:
 *   - BLE on/off  = s_bt_master   (nocsif_ble_bt_enabled)      — the logical master, resident controller
 *   - BLE link    = ANCS state    (nocsif_ble_ancs_state)      — a phone actually connected/subscribed
 *   - WiFi on/off = STA power      (nocsif_wifi_enabled)        — the radio powered, regardless of link
 *   - WiFi link   = has IP         (nocsif_wifi_connected)      — associated with an AP
 */
#include "radio_state.h"

#include "ble.h"
#include "wifi.h"
#include "reliability.h"
#include "governor.h"      /* §4.6 P1: the Governor's parked flag (intent on, STA stopped) */

void nocsif_radio_state(nocsif_radio_state_t *out)
{
    if (out == NULL) {
        return;
    }
    out->ble_logical_on          = nocsif_ble_bt_enabled();
    /* Controller resident = the boot reserve claimed the block AND we are not in safe mode (which skips
     * the reserve). This is what lets a BLE tile tap be a pure logical toggle, never a re-claim. */
    out->ble_controller_resident = nocsif_ble_boot_reserve_ran() && !nocsif_reliability_safe_mode();
    nocsif_ble_ancs_state_t as   = nocsif_ble_ancs_state();
    out->ble_link_live           = (as == NOCSIF_ANCS_CONNECTED || as == NOCSIF_ANCS_READY);
    out->wifi_sta_on             = nocsif_wifi_enabled();
    out->wifi_link_live          = nocsif_wifi_connected();
    out->wifi_parked             = nocsif_gov_wifi_parked();
}
