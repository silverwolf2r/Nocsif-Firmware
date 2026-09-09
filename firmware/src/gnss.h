/*
 * NocSif — GNSS (u-blox MIA-M10Q / LS550G) worker + UI glue (M8, slice: proof-of-life).
 *
 * A hardware proof-of-life check for the on-board GNSS receiver: does it
 * transmit at all, not whether it can get a fix (that needs sky view).
 *
 * The receiver is on a dedicated UART (ESP RX=GPIO44, TX=GPIO43, PPS=GPIO13),
 * rail BLDO1, opened at 38400 8N1. The board ships with either a u-blox
 * (UBX + NMEA) or an LS550G ($PQTM NMEA @115200) module, so the probe sweeps
 * bauds {38400,115200,9600} x both pin orders and listens for NMEA/UBX
 * framing; a UBX-MON-VER poll is the last-resort active check.
 *
 * Architecture mirrors imu.c / lora.c: a worker task owns the UART (a
 * dedicated bus, no SD lock). Status/readout getters return cached,
 * module-owned strings (no UART access), so they are LVGL-safe.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Parsed live GNSS state (M8-P1 Live Fix). The worker parses NMEA
 * (GGA/RMC/GSA/GSV/TXT) into this struct; LVGL reads a spinlock-guarded copy
 * via nocsif_gnss_fix_snapshot(). Position fields stay 0 until a fix is
 * solved; the stream/sat/antenna fields prove liveness with no fix.
 * Coordinates are signed decimal degrees (+N/+E). */
typedef struct {
    bool     valid;         /* RMC status == 'A' (navigation-valid position) */
    uint8_t  quality;       /* GGA fix quality: 0 none, 1 GPS, 2 DGPS, ... */
    uint8_t  fix_type;      /* GSA fix: 1 none, 2 = 2D, 3 = 3D */
    uint8_t  sats_used;     /* GGA numSV — satellites in the position solution */
    uint8_t  sats_in_view;  /* sum of latest per-talker GSV numSV (all constellations) */
    double   lat_deg;       /* decimal degrees, +N / -S (0 until fixed) */
    double   lon_deg;       /* decimal degrees, +E / -W (0 until fixed) */
    float    alt_m;         /* GGA altitude above mean sea level, metres */
    float    hdop;          /* GGA horizontal dilution of precision */
    float    speed_kmh;     /* RMC speed over ground, km/h (converted from knots) */
    float    course_deg;    /* RMC course over ground, degrees true */
    uint16_t year;          /* UTC date/time (RMC date + GGA/RMC time); 0 = unknown */
    uint8_t  mon, day, hh, mm, ss;
    char     antenna[12];   /* GNTXT ANTSTATUS token ("OK"/"OPEN"/"SHORT") or "" */
    uint32_t sentences;     /* total NMEA sentences parsed this session (liveness) */
    uint32_t fix_age_ms;    /* ms since the last valid position; UINT32_MAX if never */
    uint32_t stream_age_ms; /* ms since the last parsed sentence; UINT32_MAX if never */
} nocsif_gnss_fix_t;

/* Creates the idle worker task. Idempotent; no-op in reliability safe mode.
 * Safe to call from the LVGL task or app_main. */
esp_err_t nocsif_gnss_init(void);

/* Requests the GNSS proof-of-life run: non-blocking, LVGL-callback-safe (only
 * signals the worker). The first request lazily brings up the UART (BLDO1
 * rail), then runs the baud/pin sweep + UBX poll, logging a verdict over serial. */
void nocsif_gnss_request_selftest(void);

/* True once the receiver has been seen transmitting (NMEA/UBX); false in safe mode or before then. */
bool nocsif_gnss_available(void);

/* Compact live status string ("idle"/"test"/"alive"/"dead"/"err"/"off").
 * Module-owned buffer, no hardware access — LVGL-safe. */
const char *nocsif_gnss_status_str(void);

/* Full live readout line (verdict summary: baud/pins/framing, or the error
 * state). Module-owned buffer, no hardware access — LVGL-safe. */
const char *nocsif_gnss_readout_str(void);

/* ---- M8-P1 Live Fix ---------------------------------------------------------------- *
 * Starts/stops continuous NMEA streaming + parsing. LVGL-callback-safe (only
 * flips a request flag and signals the worker). Live-on powers BLDO1, warms
 * the module, and streams; live-off stops the pump and drops the rail. */
void nocsif_gnss_set_live(bool on);

/* True while the worker is actively streaming + parsing (reflects real state, not the request). */
bool nocsif_gnss_live(void);

/* Governor P2 background hold: keeps the receiver live without a screen open,
 * OR'd with the Live Fix / GPX / Wardrive wants so neither can end the other's
 * session early. The Governor holds for one fix (or a timeout) and releases. LVGL-safe. */
void nocsif_gnss_set_hold(bool on);

/* Copies the latest parsed live state into *out (spinlock-guarded). Returns
 * false if no sentence has been parsed yet (out untouched). LVGL-safe. */
bool nocsif_gnss_fix_snapshot(nocsif_gnss_fix_t *out);

/* ---- M8-P2 GPX track logging ------------------------------------------------------- *
 * Logs the live fix to a GPX 1.1 track file on the microSD
 * (/sd/nocsif/tracks/trk-NNN.gpx). The worker owns the FILE* and appends a
 * <trkpt> on a time/distance cadence, rewriting the closing footer each
 * append so the file stays parseable after a power loss mid-track. Logging
 * forces live streaming on, independent of the Live Fix screen, and stops on
 * gpx_set(false) or when both logging and Live Fix are off. Writes go through
 * nocsif_usb_gadget_claim_sd() + nocsif_sdcard_lock(); refused while USB File
 * Share owns the card. */
typedef enum {
    NOCSIF_GPX_OFF = 0,     /* not logging */
    NOCSIF_GPX_WAIT,        /* armed — waiting for the first valid fix */
    NOCSIF_GPX_REC,         /* actively recording track points */
    NOCSIF_GPX_NOSD,        /* no card, or USB File Share owns it */
    NOCSIF_GPX_ERR,         /* file open / write error */
} nocsif_gnss_gpx_state_t;

typedef struct {
    nocsif_gnss_gpx_state_t state;
    uint32_t points;        /* track points written this session */
    float    dist_m;        /* cumulative track distance, metres */
    uint32_t dur_s;         /* seconds since logging started */
    char     path[48];      /* /sd path of the current .gpx, or "" */
} nocsif_gnss_gpx_t;

/* Starts/stops GPX track logging. LVGL-callback-safe. Starting also brings
 * live streaming up if it is not already on. */
void nocsif_gnss_gpx_set(bool on);

/* True while a track file is open and recording. */
bool nocsif_gnss_gpx_logging(void);

/* Copies the GPX logging stats into *out (spinlock-guarded); dur_s is recomputed live. LVGL-safe. */
bool nocsif_gnss_gpx_snapshot(nocsif_gnss_gpx_t *out);

/* ---- M8 Wardrive — geotagged WiFi survey -------------------------------------------- *
 * Logs nearby WiFi access points plus the live GPS position to a WiGLE-1.4
 * CSV on microSD (/sd/nocsif/wardrive/wardrive-NNN.csv, importable to
 * wigle.net). Starting forces GNSS live and the passive WiFi monitor
 * (channel-hopping capture + AP parser) on; each newly-seen BSSID is written
 * once, tagged with the current fix. Purely passive; nothing is transmitted. */
typedef enum {
    NOCSIF_WD_OFF = 0,      /* not logging */
    NOCSIF_WD_WAIT,         /* armed — waiting for a valid fix (APs may already be visible) */
    NOCSIF_WD_REC,          /* recording geotagged APs */
    NOCSIF_WD_NOSD,         /* no card, or USB File Share owns it */
    NOCSIF_WD_ERR,          /* file open / write error */
} nocsif_gnss_wardrive_state_t;

typedef struct {
    nocsif_gnss_wardrive_state_t state;
    uint32_t networks;      /* unique APs logged this session */
    uint16_t aps_visible;   /* APs currently in the monitor table */
    uint32_t dur_s;         /* seconds since logging started */
    char     path[52];      /* /sd path of the current .csv, or "" */
} nocsif_gnss_wardrive_t;

/* Starts/stops the wardrive session (engine drives GNSS live + the WiFi monitor). LVGL-safe. */
void nocsif_gnss_wardrive_set(bool on);

/* True while a wardrive CSV is open and logging. */
bool nocsif_gnss_wardrive_logging(void);

/* Copies the wardrive stats into *out (spinlock-guarded); dur_s recomputed live. LVGL-safe. */
bool nocsif_gnss_wardrive_snapshot(nocsif_gnss_wardrive_t *out);

#ifdef __cplusplus
}
#endif
