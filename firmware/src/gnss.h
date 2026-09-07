/*
 * NocSif — GNSS (u-blox MIA-M10Q / LS550G) worker + UI glue (M8, slice: proof-of-life).
 *
 * A HARDWARE PROOF-OF-LIFE for the on-board GNSS receiver. The GPS on THIS unit was reported
 * dead by the operator (same suspected flex-cluster fault as NFC/GPS/haptic), but that was an
 * observation, not an instrumented probe — and the M9 LoRa check overturned a similar suspicion,
 * so this gives the GPS a hard verdict with the same rigor.
 *
 * The receiver is on a dedicated UART (ESP RX=GPIO44, TX=GPIO43, PPS=GPIO13), rail BLDO1, and
 * LilyGoLib opens it at 38400 8N1 (`Serial1.begin(38400,...)`). The board ships with EITHER a
 * u-blox (UBX + NMEA) or an LS550G (`$PQTM` NMEA @115200), so the probe SWEEPS bauds
 * {38400,115200,9600} × both pin orders and listens for NMEA/UBX framing — a live module streams
 * sentences within ~1 s even with no fix. A UBX-MON-VER poll is the last-resort active check.
 * No fix is required or attempted (that needs sky view); this only answers "is it transmitting?".
 *
 * Architecture mirrors imu.c / lora.c: a worker task owns the UART (dedicated bus — no SD lock).
 * The status/readout getters return cached, module-owned strings (no UART) so they are LVGL-safe.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Parsed live GNSS state (M8-P1 Live Fix). The worker parses NMEA (GGA/RMC/GSA/GSV/TXT) into
 * this struct; LVGL reads a spinlock-guarded copy via nocsif_gnss_fix_snapshot(). Position fields
 * stay 0 until the receiver actually solves a fix (needs sky view) — the stream/sat/antenna fields
 * prove liveness indoors with no fix. Coordinates are signed decimal degrees (+N/+E). */
typedef struct {
    bool     valid;         /* RMC status == 'A' (navigation-valid position)          */
    uint8_t  quality;       /* GGA fix quality: 0 none, 1 GPS, 2 DGPS, …              */
    uint8_t  fix_type;      /* GSA fix: 1 none, 2 = 2D, 3 = 3D                        */
    uint8_t  sats_used;     /* GGA numSV — satellites in the position solution        */
    uint8_t  sats_in_view;  /* Σ latest per-talker GSV numSV (all constellations)     */
    double   lat_deg;       /* decimal degrees, +N / -S (0 until fixed)               */
    double   lon_deg;       /* decimal degrees, +E / -W (0 until fixed)               */
    float    alt_m;         /* GGA altitude above mean sea level, metres              */
    float    hdop;          /* GGA horizontal dilution of precision                   */
    float    speed_kmh;     /* RMC speed over ground, km/h (converted from knots)     */
    float    course_deg;    /* RMC course over ground, degrees true                   */
    uint16_t year;          /* UTC date/time (RMC date + GGA/RMC time); 0 = unknown   */
    uint8_t  mon, day, hh, mm, ss;
    char     antenna[12];   /* GNTXT ANTSTATUS token ("OK"/"OPEN"/"SHORT") or ""      */
    uint32_t sentences;     /* total NMEA sentences parsed this session (liveness)    */
    uint32_t fix_age_ms;    /* ms since the last valid position; UINT32_MAX if never  */
    uint32_t stream_age_ms; /* ms since the last parsed sentence;  UINT32_MAX if never*/
} nocsif_gnss_fix_t;

/* Create the idle worker task. Idempotent; cheap (a task + no UART yet). No-op in reliability
 * safe mode (nocsif_gnss_available() stays false). Safe to call from the LVGL task or app_main. */
esp_err_t nocsif_gnss_init(void);

/* Request the GNSS proof-of-life: non-blocking and LVGL-callback-safe (only signals the worker).
 * The first request performs the lazy bring-up (BLDO1 rail -> UART) then runs the baud/pin sweep +
 * UBX poll, logging a structured block + a plain-English VERDICT over serial. Trigger it at boot
 * with -DNOCSIF_GNSS_BOOT_SELFTEST=1 (see main.c). See docs/RESUME.md for the capture recipe. */
void nocsif_gnss_request_selftest(void);

/* False in safe mode or until the receiver has been seen transmitting (NMEA/UBX); true once it has. */
bool nocsif_gnss_available(void);

/* Compact live status for a future GNSS status row ("idle"/"test"/"alive"/"dead"/"err"/"off").
 * Module-owned buffer, no hardware access — LVGL-safe. */
const char *nocsif_gnss_status_str(void);

/* Full live readout line (verdict summary: baud/pins/framing, or the error state). Module-owned
 * buffer, no hardware access — LVGL-safe. */
const char *nocsif_gnss_readout_str(void);

/* ---- M8-P1 Live Fix ---------------------------------------------------------------- *
 * Start/stop continuous NMEA streaming + parsing. LVGL-callback-safe (only flips a request flag +
 * signals the worker). Live-on powers BLDO1 (receive-only: RX=GPIO44; the console keeps GPIO43),
 * warms the module, and streams; live-off stops the pump and drops the rail. The Live Fix screen
 * calls set_live(true) on enter and set_live(false) on exit. */
void nocsif_gnss_set_live(bool on);

/* True while the worker is actively streaming + parsing (reflects the real state, not the request). */
bool nocsif_gnss_live(void);

/* §4.6 Governor P2 — a BACKGROUND hold: keeps the receiver live without a screen, OR'd with the Live
 * Fix / GPX / Wardrive wants (so a governor fix cycle can never end a foreground session, nor a screen
 * exit end the cycle). The Governor holds for one fix (or a timeout) and releases. LVGL-safe. */
void nocsif_gnss_set_hold(bool on);

/* Copy the latest parsed live state into *out (spinlock-guarded). Returns false if no sentence has
 * been parsed yet (out untouched). Ages are recomputed at call time. LVGL-safe (no hardware). */
bool nocsif_gnss_fix_snapshot(nocsif_gnss_fix_t *out);

/* ---- M8-P2 GPX track logging ------------------------------------------------------- *
 * Log the live fix to a GPX 1.1 track file on the microSD (`/sd/nocsif/tracks/trk-NNN.gpx`). The
 * worker owns the FILE* and appends a <trkpt> on a time/distance cadence; the file is kept valid
 * on disk after every point (the closing footer is rewritten each append) so a power-loss mid-track
 * still yields parseable GPX. Logging FORCES live streaming on (independent of the Live Fix screen)
 * so a track keeps recording in the background after you leave the screen; it stops on gpx_set(false)
 * or when both logging and Live Fix are off. Writes go through nocsif_usb_gadget_claim_sd() +
 * nocsif_sdcard_lock() (shared SPI3); refused while USB File Share owns the card. */
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

/* Start/stop GPX track logging. LVGL-callback-safe (flips a request flag + signals the worker).
 * Starting also brings live streaming up if it is not already on. */
void nocsif_gnss_gpx_set(bool on);

/* True while a track file is open and recording (reflects the real state, not the request). */
bool nocsif_gnss_gpx_logging(void);

/* Copy the GPX logging stats into *out (spinlock-guarded). dur_s is recomputed live. LVGL-safe. */
bool nocsif_gnss_gpx_snapshot(nocsif_gnss_gpx_t *out);

/* ---- M8 Wardrive — geotagged WiFi survey -------------------------------------------- *
 * Logs nearby WiFi access points + the live GPS position to a WiGLE-1.4 CSV on microSD
 * (`/sd/nocsif/wardrive/wardrive-NNN.csv`, importable to wigle.net). The session is engine-owned:
 * starting forces GNSS live AND the passive WiFi monitor (channel-hopping capture + AP parser) on,
 * then each newly-seen BSSID is written once, tagged with the current fix. Like GPX it keeps logging
 * in the background after you leave the screen; stopping turns the monitor + streaming back off.
 * Purely passive recon of your own airspace; nothing is transmitted. */
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

/* Start/stop the wardrive session (engine drives GNSS live + the WiFi monitor). LVGL-safe. */
void nocsif_gnss_wardrive_set(bool on);

/* True while a wardrive CSV is open and logging. */
bool nocsif_gnss_wardrive_logging(void);

/* Copy the wardrive stats into *out (spinlock-guarded). dur_s recomputed live. LVGL-safe. */
bool nocsif_gnss_wardrive_snapshot(nocsif_gnss_wardrive_t *out);

#ifdef __cplusplus
}
#endif
