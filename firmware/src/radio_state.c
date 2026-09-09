/*
 * NocSif — radio-state snapshot implementation. See radio_state.h.
 *
 * Each field is pulled straight from its one authoritative source: BLE's master-enable
 * flag and ANCS connection state, and WiFi's power and link-connected flags.
 */
#include "radio_state.h"

#include "ble.h"
#include "wifi.h"
#include "reliability.h"
#include "governor.h"      /* WiFi's parked flag (logically on, radio stopped) */

void nocsif_radio_state(nocsif_radio_state_t *out)
{
    if (out == NULL) {
        return;
    }
    out->ble_logical_on          = nocsif_ble_bt_enabled();
    /* The controller is "resident" only if the boot-time reserve actually ran and we're
     * not in safe mode (which skips it) — that's what makes a BLE tile tap a pure toggle
     * rather than needing to re-claim controller memory. */
    out->ble_controller_resident = nocsif_ble_boot_reserve_ran() && !nocsif_reliability_safe_mode();
    nocsif_ble_ancs_state_t as   = nocsif_ble_ancs_state();
    out->ble_link_live           = (as == NOCSIF_ANCS_CONNECTED || as == NOCSIF_ANCS_READY);
    out->wifi_sta_on             = nocsif_wifi_enabled();
    out->wifi_link_live          = nocsif_wifi_connected();
    out->wifi_parked             = nocsif_gov_wifi_parked();
}
