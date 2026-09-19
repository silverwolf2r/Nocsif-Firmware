/*
 * NocSif — LoRa (Semtech SX1262) worker + UI glue (M9).
 *
 * Driven via RadioLib (vendored in components/RadioLib) through a custom
 * ESP32-S3 HAL (nocsif_esp_hal.h) on the shared SPI3 bus. This module brings
 * the radio up @915 MHz (US ISM) and does a TX test.
 *
 * Board specifics: SX1262 on SPI3 (SCK35/MISO33/MOSI34), CS36 / DIO1(IRQ)14
 * / RST47 / BUSY48, rail ALDO3, TCXO 1.6 V on DIO3, DIO2 = internal TX/RX
 * switch, built-in-antenna selector on XL9555 IO11 (HIGH). Config: SF9 /
 * BW125 / CR4:7 / sync 0x12 / 10 dBm.
 *
 * Architecture mirrors nfc.cpp / mic.c: a dedicated worker task owns the
 * radio + blocking SPI; the SX1262 shares SPI3 with the microSD (+ NFC), so
 * the worker holds nocsif_sdcard_lock() around each RadioLib operation
 * (shared-bus discipline). The status/readout getters return cached,
 * module-owned strings (no SPI) so they are LVGL-callback-safe (for a
 * future Sub-GHz status row).
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

/* Creates the idle worker task. Idempotent; cheap (a task; no radio yet). In
 * reliability safe mode this is a no-op and nocsif_lora_available() stays
 * false. Safe to call from the LVGL task as the lazy trigger, or from
 * app_main for the boot self-test. */
esp_err_t nocsif_lora_init(void);

/* Tears the LoRa worker fully down — sleeps the SX1262, drops the ALDO3
 * rail, deletes the 8 KB worker task + queue — and blocks until the stack is
 * reclaimed. Used by the Signal-Hunt radio hand-off so WiFi can re-init into
 * the freed internal RAM when leaving LoRa mode. Idempotent; never call it
 * from the worker. */
void nocsif_lora_deinit(void);

/* Requests the SX1262 bring-up + TX test: non-blocking and
 * LVGL-callback-safe (only signals the worker). The first request performs
 * the lazy bring-up (ALDO3 rail -> built-in antenna -> RadioLib begin
 * @915 MHz) then transmits a few test packets, confirming TX_DONE, and logs
 * a structured block plus a plain-English verdict over serial. This emits
 * briefly on 915 MHz ISM. Trigger it at boot with
 * -DNOCSIF_LORA_BOOT_SELFTEST=1 (see main.c). */
void nocsif_lora_request_selftest(void);

/* True once the SX1262 has been brought up (RadioLib begin() succeeded —
 * chip alive + configured); false in safe mode or before then. No hardware access. */
bool nocsif_lora_available(void);

/* ---- M9 P2P messaging (slice 2b) -------------------------------------------------- *
 * A NocSif P2P text frame (broadcast) over LoRa. Send is verifiable on the
 * watch alone (TX_DONE); receive/inbox needs a second LoRa node to verify
 * end-to-end. All calls are non-blocking + LVGL-safe (they post to the
 * worker or read cached state). The first send/listen triggers the lazy
 * bring-up. */

/* Queues a text message to broadcast (UTF-8, clamped to the frame's text capacity). */
void nocsif_lora_send_text(const char *text);

/* Enters/leaves continuous RX listen mode. Received NocSif frames land in
 * the inbox; while listening the shared bus stays usable (the worker polls
 * DIO1 and only takes the SD lock to read a packet). */
void nocsif_lora_set_listen(bool on);

/* True while the radio is in continuous RX. No hardware access. */
bool nocsif_lora_listening(void);

/* Returns this device's node id (low 4 bytes of the factory MAC), stamped into each sent frame. */
uint32_t nocsif_lora_node_id(void);

/* Number of messages in the receive inbox (0..capacity). No hardware access — LVGL-safe. */
int nocsif_lora_inbox_count(void);

/* Copies out inbox message i (0 = newest): sender id, text, RSSI (dBm), age
 * (ms). Returns false if i is out of range. Spinlock-guarded snapshot — no
 * hardware access, LVGL-safe. */
bool nocsif_lora_inbox_get(int i, uint32_t *src, char *text, size_t text_sz, int *rssi, uint32_t *age_ms);

/* Compact live status for a future Sub-GHz status row ("idle" / "test" /
 * "alive" / "dead" / "suspect" / "busy" / "err" / "off"). Module-owned
 * buffer, stable between updates. No hardware access — safe on the LVGL
 * task (this is the getter for the live-label hook). */
const char *nocsif_lora_status_str(void);

/* Full live readout line (verdict summary: status byte + syncword + R/W
 * result, or the error state). Module-owned buffer. No hardware access —
 * safe on the LVGL task. */
const char *nocsif_lora_readout_str(void);

/* ---- M9 Channel Activity (solo-verifiable RX / RSSI / CAD band scan) --------------- *
 * A band monitor: sweeps a set of sample frequencies across the US
 * 902-928 MHz ISM band, reading the instantaneous RSSI at each (a coarse
 * energy spectrum), plus a LoRa CAD pass on the 915 MHz home channel that
 * counts detected preambles. Fully verifiable on the watch alone (no peer):
 * a quiet band reads a noise floor and CAD stays free; real traffic lifts a
 * bar and trips CAD. All calls are non-blocking + LVGL-safe (they post to
 * the worker / read a cached snapshot). Mutually exclusive with listen
 * mode; the first call triggers the lazy bring-up. */
#define NOCSIF_LORA_ACT_CHANS    15   /* sample points across the band (odd -> 915 MHz is the centre) */
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

/* Enters/leaves the channel-activity band scan. */
void nocsif_lora_set_activity(bool on);

/* True while the band scan is running. No hardware access. */
bool nocsif_lora_activity_scanning(void);

/* Copies out the latest band-scan snapshot. Returns false until the first
 * sweep completes. Spinlock-guarded snapshot — no hardware access, LVGL-safe. */
bool nocsif_lora_activity_snapshot(nocsif_lora_activity_t *out);

/* Returns the centre frequency (MHz) of sample channel i
 * (0..NOCSIF_LORA_ACT_CHANS-1); 0 if out of range. */
float nocsif_lora_activity_freq_mhz(int i);

/* Boot/dev self-test: runs a few band sweeps synchronously and logs the
 * spectrum + a CAD verdict over serial (build
 * -DNOCSIF_LORA_ACTIVITY_SELFTEST=1; see main.c). Passive RX / CAD only —
 * unlike the messaging self-test this does not transmit. Non-blocking +
 * LVGL-safe (signals the worker). */
void nocsif_lora_request_activity_selftest(void);

/* ---- M9 Band Survey (fine RSSI sweep + signal detection) --------------------------- *
 * A finer, gap-free survey of the whole 902-928 MHz band: 52 contiguous
 * 500 kHz bins (the radio is widened to BW500 so adjacent samples touch)
 * with per-bin max-hold + a hit counter, a running noise floor (median of
 * the sweep), and detection = contiguous bins whose max-hold sticks
 * >=10 dB above the floor, aggregated into a list of signals (peak
 * frequency + strength + width + hit count). Sees any transmitter's energy
 * regardless of modulation (LoRa, FSK, even the OOK devices the SX1262
 * can't decode) — it just can't read their content. Fully solo-verifiable;
 * mutually exclusive with listen / channel-activity. All calls
 * non-blocking + LVGL-safe (post to the worker / read a cached snapshot). */
#define NOCSIF_LORA_SURVEY_BINS  52   /* contiguous 500 kHz bins spanning 902-928 MHz */
#define NOCSIF_LORA_SURVEY_SIGS  8    /* max detected signals reported */

typedef struct {
    int      bin;         /* peak bin index of this signal (freq via nocsif_lora_survey_freq_mhz) */
    int      peak_rssi;   /* max-hold RSSI at the peak bin (dBm) */
    int      span_bins;   /* contiguous active bins (each ~0.5 MHz wide) */
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

/* Enters/leaves the band survey (widens to BW500; restores BW125 on exit). */
void nocsif_lora_set_survey(bool on);

/* True while the survey is running. No hardware access. */
bool nocsif_lora_surveying(void);

/* Clears the max-hold + detections + hit counts and keeps surveying (re-arm the survey). */
void nocsif_lora_survey_reset(void);

/* Copies out the latest survey snapshot. Returns false until the first
 * sweep completes. Spinlock-guarded — no hardware access, LVGL-safe. */
bool nocsif_lora_survey_snapshot(nocsif_lora_survey_t *out);

/* Returns the centre frequency (MHz) of survey bin i
 * (0..NOCSIF_LORA_SURVEY_BINS-1); 0 if out of range. */
float nocsif_lora_survey_freq_mhz(int bin);

/* Boot/dev self-test: runs a few survey sweeps synchronously and logs the
 * floor + detected signals (-DNOCSIF_LORA_SURVEY_SELFTEST=1; see main.c).
 * Passive RX only — no emission. */
void nocsif_lora_request_survey_selftest(void);

/* ---- Survey range (§LoRa signal-hunt expansion) ----------------------------------- *
 * The default survey covers 902–928 MHz (US ISM). The SX1262 tunes the whole 150–960 MHz range, so the
 * 52 bins can be re-spanned across ANY window in that range to explore beyond 915. lo/hi are the CENTRE
 * frequencies of bin 0 and bin 51; the per-bin spacing is (hi-lo)/51. The RX bandwidth is auto-set to the
 * widest supported value that stays <= the bin spacing (capped at 500 kHz), so a window <= ~26 MHz is
 * sampled gap-free, and a wider one (e.g. the full 150–960 birds-eye) is COARSE — undersampled between
 * bins (the natural flow is then to zoom into a hot region for a gap-free look). Passive RX only. ⚠ the
 * board's front-end is matched for 915 MHz, so signals far from the ISM band read attenuated. */
#define NOCSIF_LORA_RANGE_MIN_MHZ 150.0f
#define NOCSIF_LORA_RANGE_MAX_MHZ 960.0f
void  nocsif_lora_survey_set_range(float lo_mhz, float hi_mhz);   /* clamped to [150,960]; re-arms the survey */
float nocsif_lora_survey_lo_mhz(void);      /* bin 0 centre (default 902.25) */
float nocsif_lora_survey_hi_mhz(void);      /* bin 51 centre (default 927.75) */
float nocsif_lora_survey_bin_khz(void);     /* current per-bin spacing, kHz (>500 → coarse/undersampled) */

/* ---- Survey omit list (§LoRa signal-hunt expansion) ------------------------------- *
 * The MAC-less analogue of the BLE/WiFi omit lists: a detected signal whose peak frequency falls within an
 * omit entry (mhz ± tol_mhz) is dropped from the survey's detected-signal list, so a known local carrier
 * stops cluttering the hunt pick list. Applied in the survey detection aggregation (the one place LoRa
 * signals surface). Persisted across boots. Mutated from the LVGL task; read on the worker under a lock. */
#define NOCSIF_LORA_OMIT_MAX 16
typedef struct { float mhz; float tol_mhz; char note[24]; } nocsif_lora_omit_t;
bool nocsif_lora_omit_contains(float mhz);
bool nocsif_lora_omit_add(float mhz, float tol_mhz, const char *note);
bool nocsif_lora_omit_remove(float mhz);    /* removes the entry whose window contains mhz */
void nocsif_lora_omit_clear(void);
int  nocsif_lora_omit_count(void);
bool nocsif_lora_omit_get(int i, nocsif_lora_omit_t *out);

/* ---- M9 Signal Hunt (energy direction-finding) ------------------------------------ *
 * Parks the radio on one frequency in RX and streams its instantaneous RSSI
 * as a fast-attack / slow-decay envelope — the live "how strong / getting
 * warmer" value the Signal Hunt bearing dial + audio cue consume
 * (radio-agnostic `huntview_t`, alongside BLE and WiFi). Tracks any energy
 * at that frequency regardless of modulation (so a Band-Survey-detected
 * signal can be direction-found even if the SX1262 can't decode it). Best
 * on a continuous/frequent transmitter; a bursty one gives a pulsing
 * envelope (the dial's per-heading max-hold still maps it). Mutually
 * exclusive with listen / scan / survey. */
typedef struct {
    int      smoothed;    /* fast-attack / slow-decay RSSI envelope, dBm */
    int      peak;        /* strongest envelope this session, dBm */
    uint32_t age_ms;      /* since the last RSSI read (stays small while hunting) */
    uint32_t frames;      /* RSSI reads taken this session */
    bool     heard;       /* true once at least one reading has been taken */
    float    freq_mhz;    /* the parked hunt frequency */
} nocsif_lora_hunt_t;

/* Starts energy-hunting the given frequency (MHz). The first call triggers the lazy bring-up. */
void nocsif_lora_set_hunt(float mhz);

/* Stops hunting (radio back to standby). */
void nocsif_lora_hunt_stop(void);

/* True while the energy hunt is running. No hardware access. */
bool nocsif_lora_hunting(void);

/* Copies out the latest hunt snapshot. Returns false until the first RSSI read. Spinlock-guarded. */
bool nocsif_lora_hunt_snapshot(nocsif_lora_hunt_t *out);

/* Boot/dev self-test: parks on 915 MHz and reads the RSSI a few times,
 * logging the envelope (-DNOCSIF_LORA_HUNT_SELFTEST=1; see main.c). Passive
 * RX only — no emission. */
void nocsif_lora_request_hunt_selftest(void);

/* ---- Sub-GHz Carrier Test (continuous-wave output) -------------------------------- *
 * A controlled, unmodulated continuous carrier (SX1262 SET_TX_CONTINUOUS_WAVE) for antenna / matching
 * characterization (VSWR / tuning) and receiver interference-resilience testing of your OWN equipment,
 * ideally in a shielded / controlled setup. The SX1262 has no VSWR readout, so this is the stable SOURCE
 * to use WITH a bench meter / analyzer / receiver-under-test — the watch emits, your gear measures.
 *
 * Safety/discipline baked in: the run is BOUNDED — the worker enforces a dead-man timer and auto-stops at
 * max_ms (hard-capped at NOCSIF_LORA_CW_MS_MAX); it is never a free-running emitter. Mutually exclusive
 * with every RX mode (listen/scan/survey/hunt). ⚠ EMITS RF. Transmit only where you are licensed or in a
 * shielded environment; the board's PA/antenna are matched for 915 MHz, so out-of-band CW is inefficient
 * (higher VSWR) and can interfere with licensed services — the UI defaults to ISM and gates the rest. All
 * calls are non-blocking + LVGL-safe (post to the worker / read a cached snapshot). */
#define NOCSIF_LORA_CW_DBM_MIN  (-9)      /* SX1262 PA range */
#define NOCSIF_LORA_CW_DBM_MAX  22
#define NOCSIF_LORA_CW_MS_MAX   120000    /* hard ceiling on a single carrier run (2 min) */

typedef struct {
    float    mhz;           /* the carrier frequency (the live swept freq while sweeping) */
    int      dbm;           /* output power */
    uint32_t elapsed_ms;    /* since the carrier started */
    uint32_t remaining_ms;  /* until the dead-man auto-stop */
    bool     sweeping;      /* true = swept across [lo,hi]; false = parked on mhz */
    float    lo;            /* sweep low edge (== hi when parked) */
    float    hi;            /* sweep high edge */
} nocsif_lora_carrier_t;

/* Start a bounded CW carrier: park on mhz at dbm and emit until stopped or max_ms elapses. Clamped
 * (dbm to [-9,22], max_ms to (0,120000], mhz to [150,960]). The first call triggers the lazy bring-up. */
void nocsif_lora_carrier_start(float mhz, int dbm, uint32_t max_ms);

/* Start a bounded SWEPT CW carrier: emit CW while stepping the frequency by step_mhz across [lo,hi] every
 * dwell_ms, wrapping at hi (or, with pingpong, reversing at each edge), until stopped or max_ms elapses.
 * For antenna/VSWR-vs-frequency characterization + broadband receiver-resilience testing of your own gear.
 * Same clamps as the single carrier; lo/hi are sorted; step floored to a sane minimum. ⚠ EMITS across the
 * whole range — transmit only where licensed or in a shielded setup. */
void nocsif_lora_carrier_sweep_start(float lo, float hi, float step_mhz, uint32_t dwell_ms,
                                     bool pingpong, int dbm, uint32_t max_ms);

/* Stop the carrier immediately (radio back to standby, default power restored). */
void nocsif_lora_carrier_stop(void);

/* True while the carrier is emitting. No hardware access — LVGL-safe. */
bool nocsif_lora_carrier_active(void);

/* Copy out the live carrier snapshot (freq / power / elapsed / remaining). Returns false when idle. */
bool nocsif_lora_carrier_snapshot(nocsif_lora_carrier_t *out);

#ifdef __cplusplus
}
#endif
