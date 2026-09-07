/*
 * NocSif — LoRa (Semtech SX1262) worker + UI glue (M9).
 *
 * Driven via RadioLib (vendored in components/RadioLib) through a custom ESP32-S3 HAL
 * (nocsif_esp_hal.h) on the shared SPI3 bus. Slice 1 (a raw-SPI proof-of-life) confirmed the chip
 * is ALIVE; this module now brings the radio up @915 MHz (US ISM) and does a TX test.
 *
 * Board specifics: SX1262 on SPI3 (SCK35/MISO33/MOSI34), CS36 / DIO1(IRQ)14 / RST47 / BUSY48,
 * rail ALDO3, TCXO 1.6 V on DIO3, DIO2 = internal TX/RX switch, built-in-antenna selector on
 * XL9555 IO11 (HIGH). Config: SF9 / BW125 / CR4:7 / sync 0x12 / 10 dBm.
 *
 * Architecture mirrors nfc.cpp / mic.c: a dedicated worker task owns the radio + blocking SPI; the
 * SX1262 shares SPI3 with the microSD (+ NFC), so the worker holds nocsif_sdcard_lock() around each
 * RadioLib operation (shared-bus discipline). The status/readout getters return cached, module-owned
 * strings (no SPI) so they are LVGL-callback-safe (for a future Sub-GHz status row).
 *
 * RX / round-trip messaging is a later slice (needs a second LoRa node).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Create the idle worker task. Idempotent; cheap (a task; no radio yet). In reliability safe mode
 * this is a no-op and nocsif_lora_available() stays false. Safe to call from the LVGL task (e.g. a
 * future build_lora()) as the lazy trigger, or from app_main for the boot self-test. */
esp_err_t nocsif_lora_init(void);

/* Tear the LoRa worker fully down — sleep the SX1262, drop the ALDO3 rail, delete the 8 KB worker task +
 * queue — and BLOCK until the stack is reclaimed. Used by the Signal-Hunt radio hand-off so WiFi can
 * re-init into the freed internal RAM when leaving LoRa mode. Idempotent; never call it from the worker. */
void nocsif_lora_deinit(void);

/* Request the SX1262 bring-up + TX test: non-blocking and LVGL-callback-safe (only signals the
 * worker). The first request performs the lazy bring-up (ALDO3 rail -> built-in antenna -> RadioLib
 * begin @915 MHz) then TRANSMITS a few test packets, confirming TX_DONE, and logs a structured block
 * + a plain-English VERDICT over serial. ⚠ This EMITS briefly on 915 MHz ISM. Trigger it at boot with
 * -DNOCSIF_LORA_BOOT_SELFTEST=1 (see main.c). See docs/RESUME.md for the flash + capture recipe. */
void nocsif_lora_request_selftest(void);

/* False in safe mode or until the SX1262 has been brought up; true once RadioLib begin() succeeded
 * (chip alive + configured). No hardware access. */
bool nocsif_lora_available(void);

/* ---- M9 P2P messaging (slice 2b) -------------------------------------------------- *
 * A NocSif P2P text frame (broadcast) over LoRa. Send is verifiable on the watch alone (TX_DONE);
 * receive/inbox needs a second LoRa node to verify end-to-end. All calls are non-blocking + LVGL-safe
 * (they post to the worker or read cached state). The first send/listen triggers the lazy bring-up. */

/* Queue a text message to broadcast (UTF-8, clamped to the frame's text capacity). */
void nocsif_lora_send_text(const char *text);

/* Enter/leave continuous RX listen mode. Received NocSif frames land in the inbox; while listening
 * the shared bus stays usable (the worker polls DIO1 and only takes the SD lock to read a packet). */
void nocsif_lora_set_listen(bool on);

/* True while the radio is in continuous RX. No hardware access. */
bool nocsif_lora_listening(void);

/* This device's node id (low 4 bytes of the factory MAC), stamped into each sent frame. */
uint32_t nocsif_lora_node_id(void);

/* Number of messages in the receive inbox (0..capacity). No hardware access — LVGL-safe. */
int nocsif_lora_inbox_count(void);

/* Copy out inbox message i (0 = newest): sender id, text, RSSI (dBm), age (ms). Returns false if i is
 * out of range. Spinlock-guarded snapshot — no hardware access, LVGL-safe. */
bool nocsif_lora_inbox_get(int i, uint32_t *src, char *text, size_t text_sz, int *rssi, uint32_t *age_ms);

/* Compact live status for a future Sub-GHz status row ("idle" / "test" / "alive" / "dead" / "suspect"
 * / "busy" / "err" / "off"). Module-owned buffer, stable between updates. No hardware access — safe on
 * the LVGL task (this is the getter for the live-label hook). */
const char *nocsif_lora_status_str(void);

/* Full live readout line (verdict summary: status byte + syncword + R/W result, or the error state).
 * Module-owned buffer. No hardware access — safe on the LVGL task. */
const char *nocsif_lora_readout_str(void);

/* ---- M9 Channel Activity (solo-verifiable RX / RSSI / CAD band scan) --------------- *
 * A band monitor: sweeps a set of sample frequencies across the US 902–928 MHz ISM band, reading the
 * instantaneous RSSI at each (a coarse energy spectrum), plus a LoRa CAD pass on the 915 MHz home
 * channel that counts detected preambles. Fully verifiable on the watch alone (no peer): a quiet band
 * reads a noise floor and CAD stays free; real traffic lifts a bar and trips CAD. All calls are
 * non-blocking + LVGL-safe (they post to the worker / read a cached snapshot). Mutually exclusive with
 * listen mode; the first call triggers the lazy bring-up. */
#define NOCSIF_LORA_ACT_CHANS    15   /* sample points across the band (odd → 915 MHz is the centre) */
#define NOCSIF_LORA_ACT_HOME_IDX 7    /* the centre bar == the 915 MHz home channel */

typedef struct {
    int      n;                            /* channels sampled (0 until the first sweep completes) */
    int      rssi[NOCSIF_LORA_ACT_CHANS];  /* instantaneous RSSI per channel, dBm */
    int      home_rssi;                    /* instantaneous RSSI on the 915 MHz home channel, dBm */
    int      busiest;                      /* index of the strongest (busiest) channel */
    int      quietest;                     /* index of the weakest (quietest / best) channel */
    uint32_t cad_scans;                    /* CAD passes run on the home channel */
    uint32_t cad_hits;                     /* CAD passes that detected a LoRa preamble */
    bool     last_cad;                     /* the most recent CAD detected a preamble */
    uint32_t sweeps;                       /* completed band sweeps this session */
} nocsif_lora_activity_t;

/* Enter/leave the channel-activity band scan. */
void nocsif_lora_set_activity(bool on);

/* True while the band scan is running. No hardware access. */
bool nocsif_lora_activity_scanning(void);

/* Copy out the latest band-scan snapshot. Returns false until the first sweep completes. Spinlock-
 * guarded snapshot — no hardware access, LVGL-safe. */
bool nocsif_lora_activity_snapshot(nocsif_lora_activity_t *out);

/* Centre frequency (MHz) of sample channel i (0..NOCSIF_LORA_ACT_CHANS-1); 0 if out of range. */
float nocsif_lora_activity_freq_mhz(int i);

/* Boot/dev self-test: run a few band sweeps synchronously and log the spectrum + a CAD verdict over
 * serial (build -DNOCSIF_LORA_ACTIVITY_SELFTEST=1; see main.c). Passive RX / CAD only — unlike the
 * messaging self-test this does NOT transmit. Non-blocking + LVGL-safe (signals the worker). */
void nocsif_lora_request_activity_selftest(void);

/* ---- M9 Band Survey (fine RSSI sweep + signal detection) --------------------------- *
 * A finer, gap-free survey of the whole 902–928 MHz band: 52 contiguous 500 kHz bins (the radio is
 * widened to BW500 so adjacent samples touch) with per-bin MAX-HOLD + a hit counter, a running noise
 * floor (median of the sweep), and detection = contiguous bins whose max-hold sticks ≥10 dB above the
 * floor, aggregated into a list of signals (peak frequency + strength + width + hit count). Sees ANY
 * transmitter's energy regardless of modulation (LoRa, FSK, even the OOK devices the SX1262 can't
 * decode) — it just can't read their content. Fully solo-verifiable; mutually exclusive with listen /
 * channel-activity. All calls non-blocking + LVGL-safe (post to the worker / read a cached snapshot). */
#define NOCSIF_LORA_SURVEY_BINS  52   /* contiguous 500 kHz bins spanning 902–928 MHz */
#define NOCSIF_LORA_SURVEY_SIGS  8    /* max detected signals reported */

typedef struct {
    int      bin;         /* peak bin index of this signal (freq via nocsif_lora_survey_freq_mhz) */
    int      peak_rssi;   /* max-hold RSSI at the peak bin (dBm) */
    int      span_bins;   /* contiguous active bins (each ≈ 0.5 MHz wide) */
    uint32_t hits;        /* sweeps this signal's peak bin read above the floor */
} nocsif_lora_signal_t;

typedef struct {
    int      n;                                  /* bins sampled (0 until the first sweep completes) */
    int      cur[NOCSIF_LORA_SURVEY_BINS];       /* current-sweep RSSI, dBm */
    int      peak[NOCSIF_LORA_SURVEY_BINS];      /* max-hold RSSI, dBm */
    int      floor;                              /* noise floor estimate (median of the sweep), dBm */
    int      nsig;                               /* detected signals (0..NOCSIF_LORA_SURVEY_SIGS) */
    nocsif_lora_signal_t sig[NOCSIF_LORA_SURVEY_SIGS];  /* strongest first */
    uint32_t sweeps;                             /* completed survey sweeps this session */
} nocsif_lora_survey_t;

/* Enter/leave the band survey (widens to BW500; restores BW125 on exit). */
void nocsif_lora_set_survey(bool on);

/* True while the survey is running. No hardware access. */
bool nocsif_lora_surveying(void);

/* Clear the max-hold + detections + hit counts and keep surveying (re-arm the survey). */
void nocsif_lora_survey_reset(void);

/* Copy out the latest survey snapshot. Returns false until the first sweep completes. Spinlock-guarded
 * — no hardware access, LVGL-safe. */
bool nocsif_lora_survey_snapshot(nocsif_lora_survey_t *out);

/* Centre frequency (MHz) of survey bin i (0..NOCSIF_LORA_SURVEY_BINS-1); 0 if out of range. */
float nocsif_lora_survey_freq_mhz(int bin);

/* Boot/dev self-test: run a few survey sweeps synchronously and log the floor + detected signals
 * (-DNOCSIF_LORA_SURVEY_SELFTEST=1; see main.c). Passive RX only — no emission. */
void nocsif_lora_request_survey_selftest(void);

/* ---- M9 Signal Hunt (energy direction-finding) ------------------------------------ *
 * Park the radio on ONE frequency in RX and stream its instantaneous RSSI as a fast-attack / slow-decay
 * envelope — the live "how strong / getting warmer" value the Signal Hunt bearing dial + audio cue
 * consume (radio-agnostic `huntview_t`, alongside BLE and WiFi). Tracks ANY energy at that frequency
 * regardless of modulation (so you can direction-find a Band-Survey-detected signal even if the SX1262
 * can't decode it). Best on a continuous/frequent transmitter; a bursty one gives a pulsing envelope
 * (the dial's per-heading max-hold still maps it). Mutually exclusive with listen / scan / survey. */
typedef struct {
    int      smoothed;    /* fast-attack / slow-decay RSSI envelope, dBm */
    int      peak;        /* strongest envelope this session, dBm */
    uint32_t age_ms;      /* since the last RSSI read (stays small while hunting) */
    uint32_t frames;      /* RSSI reads taken this session */
    bool     heard;       /* true once at least one reading has been taken */
    float    freq_mhz;    /* the parked hunt frequency */
} nocsif_lora_hunt_t;

/* Start energy-hunting the given frequency (MHz). The first call triggers the lazy bring-up. */
void nocsif_lora_set_hunt(float mhz);

/* Stop hunting (radio back to standby). */
void nocsif_lora_hunt_stop(void);

/* True while the energy hunt is running. No hardware access. */
bool nocsif_lora_hunting(void);

/* Copy out the latest hunt snapshot. Returns false until the first RSSI read. Spinlock-guarded. */
bool nocsif_lora_hunt_snapshot(nocsif_lora_hunt_t *out);

/* Boot/dev self-test: park on 915 MHz and read the RSSI a few times, logging the envelope
 * (-DNOCSIF_LORA_HUNT_SELFTEST=1; see main.c). Passive RX only — no emission. */
void nocsif_lora_request_hunt_selftest(void);

#ifdef __cplusplus
}
#endif
