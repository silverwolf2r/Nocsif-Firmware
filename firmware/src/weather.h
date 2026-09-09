/*
 * Weather worker and UI glue, fetching from Open-Meteo over WiFi (section 4.1,
 * finish-the-shell).
 *
 * This is the firmware's first HTTP client. A worker task fetches current
 * conditions plus a short forecast from Open-Meteo (open-meteo.com — free, no API
 * key needed) for a location that tracks the most recent GNSS fix, and publishes a
 * spinlock-guarded snapshot that the LVGL task reads (used by both the Weather
 * screen and the watchface's peek chip).
 *
 * The design mirrors gnss.c and mic.c: a dedicated task owns all network I/O, and
 * the getter functions just return cached, module-owned data — no radio or socket
 * access — so they're safe to call from the LVGL task.
 *   - Location (lat/lon) and the Celsius/Fahrenheit preference are persisted in the
 *     NVS settings store.
 *   - A fetch needs a station link: the worker checks nocsif_wifi_connected(), and
 *     on an explicit refresh may bring the radio up itself and wait for a lease.
 *     Auto-refresh is purely opportunistic — it only fetches when already
 *     connected, so it never forces WiFi on behind a live BLE link, respecting the
 *     single-radio coexistence constraint.
 *   - Gated by safe mode: no networking happens after a boot loop.
 *
 * The transport is plain HTTP on port 80, not TLS: Open-Meteo serves its API over
 * HTTP, weather data is public anyway, and on this board the mbedTLS handshake
 * (with WiFi already up) either can't allocate its ~20 KB of record buffers or
 * stalls mid-handshake — plain HTTP sidesteps that entirely. The build adds
 * esp_http_client and json (cJSON) to PRIV_REQUIRES.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A coarse worker state, driving the screen's footer text and the peek chip's freshness indicator. */
typedef enum {
    NOCSIF_WX_IDLE = 0,   /* the worker is up but nothing has been fetched yet */
    NOCSIF_WX_NOLOC,      /* no location has been captured yet */
    NOCSIF_WX_NOWIFI,     /* no station link, so a fetch wasn't possible */
    NOCSIF_WX_FETCHING,   /* a fetch is currently in flight */
    NOCSIF_WX_OK,         /* the record below is fresh */
    NOCSIF_WX_ERR,        /* the last fetch failed; the record may be stale */
} nocsif_weather_state_t;

/* The published weather record — a snapshot copy handed to the UI. Numeric
 * fields are expressed in whichever units `metric` names (true = Celsius/km-h,
 * false = Fahrenheit/mph). */
typedef struct {
    nocsif_weather_state_t state;
    bool     valid;          /* true once at least one successful fetch has populated these fields */
    bool     metric;         /* which units the numeric fields below are expressed in */
    float    temp;           /* the current temperature */
    float    feels;          /* the apparent, "feels like" temperature */
    int      humidity;       /* relative humidity, as a percentage */
    float    wind;           /* wind speed, in mph or km/h depending on `metric` */
    int      code;           /* the WMO weather-interpretation code */
    bool     is_day;         /* whether it's currently daytime at this location */
    int      d_code[3];      /* the 3-day forecast's WMO codes */
    float    d_hi[3];        /* each day's forecast high */
    float    d_lo[3];        /* each day's forecast low */
    char     d_day[3][4];    /* a short weekday label ("Mon" through "Sun"); empty string if unknown */
    uint32_t age_s;          /* seconds elapsed since the last successful fetch (0 if there hasn't been one) */
} nocsif_weather_t;

/* Creates the idle worker task. Safe to call more than once; does no
 * networking yet. This is a no-op, and the worker stays unavailable, in
 * reliability safe mode. Call once from app_main, after nocsif_wifi_init(). */
esp_err_t nocsif_weather_init(void);

/* False in safe mode or before init has run; true once the worker task exists. */
bool nocsif_weather_available(void);

/* ---- location, captured from a GNSS fix and persisted in NVS as micro-degrees ---- */
bool nocsif_weather_has_location(void);
bool nocsif_weather_get_location(double *lat, double *lon);   /* returns false if no location has been set */
void nocsif_weather_set_location(double lat, double lon);     /* stores the location and triggers a refresh */

/* Auto-follow: the GNSS worker calls this on every valid fix, so the fetch
 * location automatically tracks wherever you last had a GPS lock, with no manual
 * step required. This is throttled — it only re-stores (and re-persists to NVS)
 * on a meaningful move, and rate-limits how often it writes. Safe to call from
 * the GNSS worker task. */
void nocsif_weather_note_fix(double lat, double lon);

/* ---- units: false means Fahrenheit/mph (the default), true means Celsius/km-h.
 * Persisted. ---- * Toggling this converts the already-cached record in place,
 * with no re-fetch needed, and reformats the peek chip. */
bool nocsif_weather_metric(void);
void nocsif_weather_set_metric(bool metric);

/* Requests a refresh; non-blocking, just posts to the worker. Passing
 * force_wifi=true lets the worker turn the radio on and wait for a link — meant
 * for an explicit user-initiated tap; false means opportunistic, only fetching
 * if a station link already exists. */
void nocsif_weather_request_refresh(bool force_wifi);

/* Snapshots the cached record, guarded by a spinlock, with age_s recomputed
 * live. Safe to call from the LVGL task. Returns false, leaving `out` untouched,
 * if nothing has ever been published yet. */
bool nocsif_weather_snapshot(nocsif_weather_t *out);

/* The coarse state, for a cheap check that doesn't require a full snapshot. */
nocsif_weather_state_t nocsif_weather_state(void);

/* The peek chip's temperature string — module-owned and stable between
 * updates (e.g. "72°" or "--°" if unknown). The degree sign (U+00B0) is included
 * in the mono font's character set. Safe to call from the LVGL task, since it
 * touches no hardware. */
const char *nocsif_weather_temp_str(void);

/* Maps a WMO weather code to a short human-readable label (a static string; returns an em dash for an unrecognized code). */
const char *nocsif_weather_code_text(int code);

/* ---- geofence, an early seed of the section 4.6 Connectivity Governor ---- *
 * A single auto-learned "last-connected" anchor point: every successful fetch —
 * proof that WiFi was connected there — updates it. Later, when GPS crosses back
 * into that anchor's radius, Weather wakes WiFi for one forced refresh (the idea
 * being "you're back where you last connected, so get fresh weather"); away from
 * that anchor, it only ever fetches opportunistically. This state stays purely
 * internal — the screen itself only shows live WiFi connectivity, not anything
 * about the geofence. */

#ifdef __cplusplus
}
#endif
