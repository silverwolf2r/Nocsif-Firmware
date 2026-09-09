/*
 * NocSif physical-button input service (UI-shell P4.1 decode; P4.4 actions).
 *
 * Decodes the watch's two side buttons and forwards them into the UI action API (ui.c):
 *   - PWR: the AXP2101 PWRKEY, classified by the PMU and polled over I2C via power.c (no IRQ GPIO
 *     needed; the button is taken away from the PMU's hardware auto-power-off so firmware owns it).
 *     short = screen off/on, pop to Home, dismiss the power menu; long = power menu; double = the
 *     bound shortcut (only decoded once one is bound).
 *   - FN: the ESP32-S3's native GPIO0 (also the boot download-mode strap — read only as a runtime
 *     input, never driven), debounced in software. short = launches the FN target app.
 *
 * Runs on its own worker task; every action is marshalled onto the LVGL task by ui.c (this service
 * never touches widgets directly). Requires nocsif_power_init() first, and nocsif_ui_init() before
 * the actions have any effect — they're no-ops until the UI exists.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Configures FN=GPIO0 as an input, configures the AXP2101 PWRKEY via power.c, and starts the
 * button worker task. Idempotent. */
esp_err_t nocsif_buttons_init(void);

/* Arms or disarms PWR double-press detection. Disarmed (the default) makes a PWR short fire right
 * away with no double-press wait; arm it only when a PWR double-press shortcut is actually bound,
 * so the common single PWR tap stays instant. The UI calls this at startup and again whenever the
 * binding changes. */
void nocsif_buttons_set_pwr_double_enabled(bool enabled);

#ifdef __cplusplus
}
#endif
