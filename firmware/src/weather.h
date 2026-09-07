/*
 * NocSif — Weather (Open-Meteo over WiFi) worker + UI glue (§4.1, finish-the-shell).
 *
 * The FIRST HTTP *client* in the firmware. A worker task fetches current conditions + a
 * short forecast from Open-Meteo (open-meteo.com — free, no API key) for a location
 * that follows the last GNSS fix, and publishes a spinlock-guarded
 * snapshot the LVGL task reads (the Weather screen + the watchface peek chip).
 *
 * Design mirrors gnss.c / mic.c: a dedicated task owns the network I/O; the getters return
 * cached, module-owned data (no radio / no socket) so they are safe on the LVGL task.
 *   - Location (lat/lon) + the C/F unit preference persist in the NVS settings store.
 *   - A fetch needs a station link: the worker checks nocsif_wifi_connected() and, on an
 *     explicit refresh, may bring the radio up and wait for a lease. Auto-refresh is purely
 *     OPPORTUNISTIC (fetches only when already connected) so it never forces WiFi up behind
 *     a live BLE link (single-radio coexistence).
 *   - Safe-mode gated (no networking after a boot loop).
 *
 * Transport is plain HTTP (port 80), not TLS: Open-Meteo serves the API over HTTP, weather is
 * public data, and on this board the mbedTLS handshake (WiFi up) either can't allocate its ~20 KB
 * of record buffers or stalls mid-handshake — HTTP sidesteps all of that. The build adds
 * esp_http_client + json (cJSON) to PRIV_REQUIRES.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Coarse worker state — drives the screen's footer + the peek chip freshness. */
typedef enum {
    NOCSIF_WX_IDLE = 0,   /* worker up, nothing fetched yet            */
    NOCSIF_WX_NOLOC,      /* no location has been captured yet         */
    NOCSIF_WX_NOWIFI,     /* no station link (couldn't fetch)          */
    NOCSIF_WX_FETCHING,   /* a fetch is in flight                      */
    NOCSIF_WX_OK,         /* the record below is fresh                 */
    NOCSIF_WX_ERR,        /* last fetch failed (record may be stale)   */
} nocsif_weather_state_t;

/* Published weather record (a snapshot copy for the UI). Numeric fields are in the units named
 * by `metric` (true = °C / km·h, false = °F / mph). */
typedef struct {
    nocsif_weather_state_t state;
    bool     valid;          /* at least one successful fetch has populated the fields */
    bool     metric;         /* units of the numeric fields                            */
    float    temp;           /* current temperature                                    */
    float    feels;          /* apparent ("feels like") temperature                    */
    int      humidity;       /* relative humidity, %                                    */
    float    wind;           /* wind speed (mph or km/h per `metric`)                   */
    int      code;           /* WMO weather-interpretation code                         */
    bool     is_day;         /* daytime at the location                                 */
    int      d_code[3];      /* 3-day forecast: WMO code                                */
    float    d_hi[3];        /* daily high                                              */
    float    d_lo[3];        /* daily low                                               */
    char     d_day[3][4];    /* short weekday label ("Mon".."Sun"); "" if unknown       */
    uint32_t age_s;          /* seconds since the last successful fetch (0 if never)    */
} nocsif_weather_t;

/* Create the idle worker task (idempotent; no networking yet). No-op + unavailable in
 * reliability safe mode. Call once from app_main after nocsif_wifi_init(). */
esp_err_t nocsif_weather_init(void);

/* False in safe mode or before init; true once the worker exists. */
bool nocsif_weather_available(void);

/* ---- location (captured from a GNSS fix; persisted in NVS as micro-degrees) ----------- */
bool nocsif_weather_has_location(void);
bool nocsif_weather_get_location(double *lat, double *lon);   /* false if none set */
void nocsif_weather_set_location(double lat, double lon);     /* store + trigger a refresh */

/* Auto-follow: the GNSS worker calls this on each valid fix so the fetch location tracks wherever
 * you last had GPS — no manual step. Throttled: it re-stores (and re-persists to NVS) only on a
 * meaningful move, and rate-limits writes. Safe to call from the GNSS worker task. */
void nocsif_weather_note_fix(double lat, double lon);

/* ---- units: false = °F / mph (default), true = °C / km·h. Persisted. ----------------- *
 * Toggling converts the cached record in place (no re-fetch needed) and reformats the chip. */
bool nocsif_weather_metric(void);
void nocsif_weather_set_metric(bool metric);

/* Request a refresh (non-blocking; posts to the worker). force_wifi=true lets the worker
 * enable the radio and wait for a link (an explicit user tap); false = opportunistic (fetch
 * only if a station link already exists). */
void nocsif_weather_request_refresh(bool force_wifi);

/* Snapshot the cached record (spinlock-guarded; age_s recomputed live). LVGL-safe. Returns
 * false if nothing has been published yet (out untouched). */
bool nocsif_weather_snapshot(nocsif_weather_t *out);

/* The coarse state, for a quick check without a full snapshot. */
nocsif_weather_state_t nocsif_weather_state(void);

/* Peek-chip temperature string, module-owned + stable between updates ("72°" / "--°"). The
 * degree sign (U+00B0) is in the mono cut. LVGL-safe (no hardware). */
const char *nocsif_weather_temp_str(void);

/* WMO weather-code → short human label (a static string; "—" for an unknown code). */
const char *nocsif_weather_code_text(int code);

/* ---- geofence (a seed of the §4.6 Connectivity Governor) ----------------------------- *
 * A single auto-learned "last-connected" anchor: each successful fetch (proof WiFi connected there)
 * stamps it. When GPS later crosses back INTO the anchor radius, Weather wakes WiFi for one forced
 * refresh ("back where you last connected → fresh weather"); out in the open it only ever fetches
 * opportunistically. State is internal — the screen shows live WiFi connectivity, not the geofence. */

#ifdef __cplusplus
}
#endif
