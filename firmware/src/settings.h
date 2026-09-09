/*
 * Persistent settings store (UI-shell P4.4).
 *
 * A key/value store backed by flash (NVS), all under the "nocsif" namespace, with typed
 * get/set helpers for strings and 32-bit ints; a possible future /sd mirror is not yet
 * implemented. Current consumers:
 *   - the FN side-button launch target (a persisted app id, defaults to "wifi")
 *   - the PWR double-press shortcut (a persisted app id, defaults to none)
 *   - (P4.4b) a hashed passcode slot.
 *
 * THREADING: the generic get/set calls touch flash and must run from a normal task, never
 * an ISR. The two button-binding getters below instead read a RAM cache (populated at init
 * and refreshed on every set), so they never touch flash and are cheap to call from the
 * LVGL task inside a button-action callback. The cache is written once at init (on the main
 * task, before the UI comes up) and afterwards only from the LVGL task (the P4.4b
 * Settings>Buttons screen), so no extra locking is required.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up NVS — init, then erase-and-retry once if the partition is new or has an
 * incompatible version — and open the "nocsif" namespace, then load the button-binding
 * cache. Safe to call more than once. Call early in app_main, before UI/button setup. */
esp_err_t nocsif_settings_init(void);

/* Typed flash-backed accessors. get_str copies up to `len` bytes (NUL-terminated) into
 * `out`, falling back to `dflt` if the key is missing or on error. Every set_* call commits
 * immediately. NVS limits keys to 15 characters or fewer. */
esp_err_t nocsif_settings_get_str(const char *key, char *out, size_t len, const char *dflt);
esp_err_t nocsif_settings_set_str(const char *key, const char *val);
int32_t   nocsif_settings_get_i32(const char *key, int32_t dflt);
esp_err_t nocsif_settings_set_i32(const char *key, int32_t val);

/* ---- button bindings: cached in RAM, safe to read on the LVGL task -------- */

/* The app id the FN button launches (passed to nocsif_app_launch). Defaults to
 * "wifi". Never returns NULL. */
const char *nocsif_settings_fn_target(void);

/* The app id bound to a PWR double-press, or "" if nothing is bound. Never NULL. */
const char *nocsif_settings_pwr_double(void);

/* Persist a new binding and update the RAM cache to match. Passing NULL or an
 * empty string clears the binding. The id is copied, so a stack buffer is fine. */
void nocsif_settings_set_fn_target(const char *id);
void nocsif_settings_set_pwr_double(const char *id);

/* ---- device name: cached in RAM, safe to read on the LVGL task ------------ *
 * The friendly name shown on the network — WiFi uses a sanitized form of it as the
 * DHCP/mDNS hostname — and eventually to a paired companion app. Defaults to
 * "NocSif watch" and never returns NULL; passing NULL or "" resets to that default. */
const char *nocsif_settings_device_name(void);
void nocsif_settings_set_device_name(const char *name);

/* ---- hashed passcode (UI-shell P4.4b) --------------------------------------- *
 * Only a random salt plus SHA-256(salt || pin) is ever written to flash — never the PIN
 * itself. This hash is intentionally lightweight (a short numeric PIN could still be
 * brute-forced from extracted flash; a proper stretch-KDF is left for later hardening),
 * but it satisfies the "store hashed" requirement and blocks casual flash inspection.
 * Actually gating unlock on it belongs to the peek/lock screen (P4.6); this module only
 * sets and persists the value. */

/* Set a new passcode: generate a random salt, hash it with the pin, write both
 * blobs, and commit. Passing NULL or an empty pin instead clears the passcode.
 * Returns ESP_OK on success. */
esp_err_t nocsif_settings_set_pin(const char *pin);

/* Erase the stored passcode. */
esp_err_t nocsif_settings_clear_pin(void);

/* True when a passcode is currently stored. RAM-cached, so safe on the LVGL task. */
bool nocsif_settings_has_pin(void);

/* Check a candidate pin against the stored passcode using a constant-time
 * comparison. Returns false when no passcode is stored. */
bool nocsif_settings_verify_pin(const char *pin);

/* Returns "set" or "none" depending on whether a passcode is stored — cheap and
 * safe to call on the LVGL task since it needs no flash access. */
const char *nocsif_settings_pin_str(void);

#ifdef __cplusplus
}
#endif
