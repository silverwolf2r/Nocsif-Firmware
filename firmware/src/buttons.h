/*
 * NocSif — physical-button input service (UI-shell P4.1 decode; P4.4 actions)
 *
 * Decodes the watch's two side buttons and dispatches them into the UI action API (ui.c):
 *   - PWR: the AXP2101 PWRKEY, classified by the PMU and read over I2C (via power.c —
 *     no IRQ GPIO needed; the button is taken away from the PMU's hardware auto-power-off
 *     so firmware owns it). short = screen off/on · pop-to-Home · dismiss power menu;
 *     long = power menu; double = the bound shortcut (only decoded when one is bound).
 *   - FN: the ESP32-S3 native GPIO0 (also the boot download-mode strap — read as a runtime
 *     input only, never driven), debounced. short = launch the FN target app.
 *
 * Runs on its own worker task; every action is MARSHALLED onto the LVGL task by ui.c (this
 * service never touches widgets). Requires nocsif_power_init() first (and nocsif_ui_init for
 * the actions to have any effect — they no-op until the UI is up).
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Configure FN=GPIO0 as an input, configure the AXP2101 PWRKEY (via power.c), and start
 * the button worker task. Idempotent. */
esp_err_t nocsif_buttons_init(void);

/* Arm/disarm PWR double-press detection. When disarmed (default) a PWR short fires
 * immediately (no double-wait); arm it only when a PWR double-press shortcut is bound, so the
 * common PWR tap stays instant. The UI calls this at startup and whenever the binding changes. */
void nocsif_buttons_set_pwr_double_enabled(bool enabled);

#ifdef __cplusplus
}
#endif
