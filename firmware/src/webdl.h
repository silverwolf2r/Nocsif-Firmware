/*
 * NocSif — WiFi "Download to SD" worker (grab-bag batch). See webdl.c.
 *
 * A generic URL -> /sd/storage/<file> downloader. One PSRAM-stacked worker task does the blocking
 * HTTP(S) GET + card writes off the LVGL thread; the UI (Cyber > USB Gadget > Download to SD) posts a
 * URL and polls the state/progress/status getters. Mirrors ota.c's web worker: claim the card away from
 * File Share for the transfer, take the /sd lock per chunk, write to a ".part" then rename into place.
 *
 * Filenames come from the URL's last path segment (query stripped, sanitized); a collision appends
 * " (1)", " (2)", … before the extension. HTTPS works through the mbedTLS CA bundle (the §4.10 lesson:
 * MBEDTLS_EXTERNAL_MEM_ALLOC=y + MBEDTLS_HARDWARE_AES=n, already set for OTA); plain HTTP works too.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Where downloads land (created on first use). */
#define NOCSIF_WEBDL_DIR "/sd/nocsif/storage"

typedef enum {
    NOCSIF_WEBDL_IDLE = 0,    /* nothing running                              */
    NOCSIF_WEBDL_RUNNING,     /* a download is in flight                      */
    NOCSIF_WEBDL_DONE,        /* the last download completed (saved_name set) */
    NOCSIF_WEBDL_FAILED,      /* the last download failed (status = reason)   */
} nocsif_webdl_state_t;

/* Create the idle worker task (idempotent; no networking yet). No-op + unavailable in safe mode. */
esp_err_t nocsif_webdl_init(void);

/* True once the worker exists. */
bool nocsif_webdl_available(void);

/* Begin downloading `url` to /sd/storage (non-blocking; posts to the worker). Copies the URL.
 * No-op if a download is already running. */
void nocsif_webdl_start(const char *url);

/* Like nocsif_webdl_start, but to a caller-chosen `dir` (created if missing), optionally resuming a
 * prior "<name>.part" via an HTTP Range request — for large OS images (Bootable OS). When resuming, the
 * file keeps a stable name (no "(1)" de-duplication); an interrupted resumable transfer leaves the .part
 * in place so a re-tap continues it. */
void nocsif_webdl_start_to(const char *url, const char *dir, bool resume);

/* Stop the in-flight download. Non-blocking: the worker stops at the next chunk (or retry) and keeps the
 * ".part" so a later Download resumes it. No-op when nothing is running. LVGL-safe. */
void nocsif_webdl_cancel(void);

/* True while a download is in flight. */
bool nocsif_webdl_busy(void);

/* Coarse state (plain read; single-writer worker / single-reader UI). LVGL-safe. */
nocsif_webdl_state_t nocsif_webdl_state(void);

/* 0..100 percent, or -1 when the server did not send a length (progress by bytes only). */
int nocsif_webdl_progress(void);

/* Bytes written so far, and the total target size in bytes (0 if unknown). For a byte/MB readout — a
 * coarse percent is useless on a multi-GB image (1% can be ~18 MB). LVGL-safe plain reads. */
uint32_t nocsif_webdl_bytes(void);
uint64_t nocsif_webdl_total_bytes(void);

/* A short status line for the screen ("connecting…", "downloading", a failure reason, …). Static
 * buffer, stable between calls; LVGL-safe. */
const char *nocsif_webdl_status(void);

/* The basename the last successful download was saved as (in /sd/storage), or "" if none. */
const char *nocsif_webdl_saved_name(void);

/* Grab the captive-portal / landing page of the network the watch is CURRENTLY connected to: GET
 * http://<gateway>/ and save the returned HTML to /sd/nocsif/wifi/portals/<ssid>.html, then pair that
 * page with the SSID in the Networks folder (nocsif_net_set_portal). Non-blocking (shares the same
 * worker). No-op while a transfer is running. Requires a live STA link — the caller should check
 * nocsif_wifi_connected() and toast an instruction otherwise. Reuses the download state/status getters. */
void nocsif_webdl_grab_portal(const char *ssid);

#ifdef __cplusplus
}
#endif
