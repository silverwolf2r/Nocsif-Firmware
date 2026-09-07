/*
 * NocSif — persistent settings store (UI-shell P4.4)
 *
 * A small NVS-backed key/value store (flash), namespace "nocsif". Typed get/set for
 * strings and int32; an optional /sd mirror is deferred. First users (P4.4):
 *   - the FN side-button launch target (a persisted app id, default "wifi")
 *   - the PWR double-press shortcut (a persisted app id, default none)
 *   - (P4.4b) a hashed passcode slot.
 *
 * THREADING: the generic get/set touch flash and must run on a normal task (NOT an ISR).
 * The two BUTTON-BINDING getters below return a RAM-cached string (loaded at init, updated
 * on set) and touch no flash, so they are cheap to read from the LVGL task in a button-action
 * callback. The cache is written at init (main task, before the UI) and thereafter only from
 * the LVGL task (the P4.4b Settings>Buttons screen), so it needs no extra locking.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up NVS (init + erase/retry on a version/page fault) and open the "nocsif"
 * namespace, then prime the button-binding RAM cache. Idempotent. Safe to call once
 * early in app_main, before the UI + button service. */
esp_err_t nocsif_settings_init(void);

/* Generic typed store (flash-backed). get_str copies up to `len` bytes (NUL-terminated)
 * into `out`, falling back to `dflt` when the key is absent or on any error. set_* persists
 * immediately (nvs_set + commit). Keys are <= 15 chars (NVS limit). */
esp_err_t nocsif_settings_get_str(const char *key, char *out, size_t len, const char *dflt);
esp_err_t nocsif_settings_set_str(const char *key, const char *val);
int32_t   nocsif_settings_get_i32(const char *key, int32_t dflt);
esp_err_t nocsif_settings_set_i32(const char *key, int32_t val);

/* ---- button bindings (RAM-cached; safe on the LVGL task) -------------------- */

/* The FN button's launch target — a stable app id passed to nocsif_app_launch (default
 * "wifi", an enabled screen). Never returns NULL. */
const char *nocsif_settings_fn_target(void);

/* The PWR double-press shortcut — a stable app id, or "" when unbound (no-op). Never NULL. */
const char *nocsif_settings_pwr_double(void);

/* Persist a new binding + refresh the cache. A NULL/empty id clears the binding. The id
 * string is copied into the cache, so a caller-owned buffer is fine. */
void nocsif_settings_set_fn_target(const char *id);
void nocsif_settings_set_pwr_double(const char *id);

/* ---- device name (RAM-cached; safe on the LVGL task) ----------------------- *
 * The friendly watch name shown on the network (WiFi sets it as the DHCP/mDNS hostname, after
 * sanitizing) and to a future companion. Never returns NULL; default "NocSif watch". A
 * NULL/empty name resets to the default. */
const char *nocsif_settings_device_name(void);
void nocsif_settings_set_device_name(const char *name);

/* ---- passcode (hashed) — UI-shell P4.4b ------------------------------------ *
 * The passcode is persisted ONLY as a random salt + SHA-256(salt || pin) — never the PIN
 * itself. This is a modest hash (a short numeric PIN is brute-forceable if flash is extracted;
 * a stretch-KDF is a future hardening); it meets the "store the passcode hashed" requirement
 * and defends against casual flash inspection. The unlock GATE that consumes it lands with the
 * peek/lock screen (P4.6); P4.4b just sets + persists it. */

/* Store a new passcode: generate a random salt, hash salt||pin, persist both blobs, commit.
 * A NULL/empty pin clears the passcode. Returns ESP_OK on success. */
esp_err_t nocsif_settings_set_pin(const char *pin);

/* Remove the stored passcode. */
esp_err_t nocsif_settings_clear_pin(void);

/* True if a passcode is stored. RAM-cached (no flash) — safe on the LVGL task. */
bool nocsif_settings_has_pin(void);

/* Verify a candidate against the stored passcode (constant-time compare). False if none stored. */
bool nocsif_settings_verify_pin(const char *pin);

/* Live-tag getter: "set" when a passcode is stored, else "none". No flash — LVGL-task-safe. */
const char *nocsif_settings_pin_str(void);

#ifdef __cplusplus
}
#endif
