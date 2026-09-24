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

/* True while a download is in flight. */
bool nocsif_webdl_busy(void);

/* Coarse state (plain read; single-writer worker / single-reader UI). LVGL-safe. */
nocsif_webdl_state_t nocsif_webdl_state(void);

/* 0..100 percent, or -1 when the server did not send a length (progress by bytes only). */
int nocsif_webdl_progress(void);

/* A short status line for the screen ("connecting…", "downloading", a failure reason, …). Static
 * buffer, stable between calls; LVGL-safe. */
const char *nocsif_webdl_status(void);

/* The basename the last successful download was saved as (in /sd/storage), or "" if none. */
const char *nocsif_webdl_saved_name(void);

#ifdef __cplusplus
}
#endif
