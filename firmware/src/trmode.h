/*
 * trmode.h — Travel Router mode (NocSif). docs/design/travel-router-mode.md.
 *
 * One stored flag (NVS "tr_mode"). On a boot with the flag set the watch is a dedicated travel router: the
 * Bluetooth controller block and the USB entry reserve are left to Wi-Fi (gateway buffer set), sharing is on,
 * and everything outside Wi-Fi is paused — not started, never reconfigured. Entering or leaving writes ONLY
 * this flag and restarts; every other setting stays exactly as the owner left it. Safe mode ignores the flag.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* main.c, early (after settings, before any radio): read the flag for THIS boot. Internal-stack task. */
void nocsif_trmode_boot_init(bool safe_mode);

/* This boot runs in Travel Router mode. RAM-cached: safe from any task. */
bool nocsif_trmode_active(void);

/* The app id is paused in the mode (Bluetooth, USB, Sub-GHz, NFC, Location, microphone, Audio Player,
 * Signal Hunt, Controllers). Always false outside the mode. */
bool nocsif_trmode_app_paused(const char *app_id);

/* The owner-facing list of what the mode pauses (one line, ASCII). */
const char *nocsif_trmode_paused_text(void);

/* Save the flag and restart into / out of the mode. NVS write + esp_restart: call on an INTERNAL-stack
 * task (the LVGL task). Does not return. */
void nocsif_trmode_set_and_restart(bool on);

#ifdef __cplusplus
}
#endif
