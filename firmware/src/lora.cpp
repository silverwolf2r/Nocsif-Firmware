/*
 * NocSif — LoRa (Semtech SX1262) worker (M9). See lora.h.
 *
 * Driven via RadioLib (components/RadioLib) through a custom ESP32-S3 HAL
 * (nocsif_esp_hal.h) on the shared SPI3 bus. A queue-driven worker owns the
 * radio + blocking SPI; LVGL callbacks only post commands (send / listen /
 * self-test) and read cached, LVGL-safe getters.
 *
 * Board specifics (verified vs LilyGoWatchUltra.cpp + RadioLib defaults):
 * SX1262 on SPI3 (SCK35/MISO33/MOSI34), CS36 / DIO1(IRQ)14 / RST47 / BUSY48,
 * rail ALDO3; TCXO 1.6 V on DIO3; DIO2 = internal TX/RX switch,
 * built-in-antenna selector on XL9555 IO11 (HIGH); DC-DC regulator. Radio:
 * 915 MHz (US ISM), SF9 / BW125 / CR4:7, private sync 0x12, 10 dBm.
 *
 * Shared bus: the SX1262 sits on SPI3 with the microSD (+ NFC). Each radio
 * operation is bracketed by nocsif_sdcard_lock() (like nfc.cpp holds it
 * across a discovery). While merely listening the lock is not held (the
 * chip is in RX, no SPI) — the worker polls DIO1 (a GPIO read, no bus) and
 * only takes the lock for the brief readData when a packet lands, so SD
 * stays usable during a listen.
 *
 * P2P frame (v1, broadcast; the LoRa PHY provides CRC): 11-byte header +
 * UTF-8 text. Little-endian native (NocSif-to-NocSif); a network-order pass
 * is for the later Meshtastic-interop slice.
 *
 * Slice 2a proved TX (TX_DONE). Slice 2b adds the message format + send/
 * receive engine. RX decode is unverified until a second LoRa node exists
 * (the send path + RX arming + RSSI floor are verifiable).
 */
#include "nocsif_esp_hal.h"   /* pulls in RadioLib.h + the S3 HAL */

#include <cstring>
#include <cstdio>             /* FILE / fopen / fputs (capture .sub writer) */
#include <cstdlib>            /* strtol (RAW_Data duration parse) */
#include <cctype>             /* isxdigit (capture .sub loader hex parse) */
#include <sys/stat.h>         /* mkdir (capture output folder) */
#include <algorithm>          /* std::sort (survey noise-floor median) */
#include <cmath>              /* lroundf (hunt RSSI envelope) */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"   /* xTaskCreateWithCaps / vTaskDeleteWithCaps — PSRAM worker stack */
#include "freertos/queue.h"
#include "esp_heap_caps.h"            /* MALLOC_CAP_SPIRAM — external-memory task stack */
#include "esp_mac.h"
#include "esp_log.h"

#include "lora.h"             /* extern "C" API we implement */
#include "power.h"            /* nocsif_power_lora_rail (ALDO3) */
#include "sdcard.h"           /* nocsif_sdcard_lock / _unlock (shared SPI3) */
#include "reliability.h"      /* nocsif_reliability_safe_mode */
#include "xl9555.h"           /* nocsif_xl9555_set_output (antenna selector IO11) */
#include "settings.h"         /* nocsif_settings_get/set_str (persisted survey omit list) */
#include "usb_gadget.h"       /* nocsif_usb_gadget_claim_sd/release_sd (own /sd for the .sub writer) */

static const char *TAG = "lora";

/* ---- hardware (docs/HARDWARE.md §LoRa; SPI3 shared with SD + NFC) ------------------ */
#define LORA_SPI_HOST      SPI3_HOST
#define LORA_PIN_SCK       35
#define LORA_PIN_MISO      33
#define LORA_PIN_MOSI      34
#define LORA_PIN_CS        36
#define LORA_PIN_DIO1      14     /* IRQ (RxDone/TxDone) */
#define LORA_PIN_RST       47
#define LORA_PIN_BUSY      48
#define LORA_XL9555_ANT_SW 11     /* EXPANDS_LORA_RF_SW: HIGH = built-in antenna */
#define LORA_SPI_HZ        2000000

/* Radio config (US 915 ISM). */
#define LORA_FREQ_MHZ      915.0f
#define LORA_BW_KHZ        125.0f
#define LORA_SF            9
#define LORA_CR            7        /* 4/7 */
#define LORA_SYNC_WORD     0x12     /* RadioLib private-network default */
#define LORA_POWER_DBM     10       /* modest for bring-up (SX1262 PA can reach +22) */
#define LORA_PREAMBLE      8
#define LORA_TCXO_V        1.6f
#define LORA_USE_LDO       false    /* DC-DC regulator (board default) */

/* Channel-activity scan: sample points centred on the 915 MHz home channel,
 * 1.8 MHz apart, so the 15 bars span 902.4-927.6 MHz — the full US
 * 902-928 ISM band. */
#define LORA_ACT_STEP_MHZ  1.8f

/* Band survey: 52 contiguous 500 kHz bins from 902.25 MHz (bin 0 centre) so
 * the band 902-928 MHz is covered with no gaps — the radio is widened to
 * BW500 for the survey so each RSSI sample spans a full bin. A bin is a
 * "signal" when its max-hold sits >= LORA_SURVEY_DETECT_DB above the noise floor. */
#define LORA_SURVEY_BASE_MHZ  902.25f
#define LORA_SURVEY_STEP_MHZ  0.5f
#define LORA_SURVEY_BW_KHZ    500.0f
#define LORA_SURVEY_DETECT_DB  10

/* Signal hunt: park on one frequency at a sensitive narrow BW and read the
 * RSSI fast for a live "getting warmer" envelope (fast attack, slow decay so
 * a bursty signal doesn't collapse instantly). */
#define LORA_HUNT_BW_KHZ      125.0f
#define LORA_HUNT_POLL_MS     60
#define LORA_HUNT_DECAY       0.25f    /* envelope EMA toward a lower reading (per poll) */

/* Crude OOK transmit (F2/F3): the output power used when gating the CW carrier to bit-bang OOK. Modest so
 * out-of-band leakage stays limited but enough to reach a nearby fixed-code receiver on the bench. */
#define OOK_TX_DBM            12

/* ---- P2P frame -------------------------------------------------------------------- */
#define NOCSIF_LORA_MAGIC0   'N'
#define NOCSIF_LORA_MAGIC1   '9'
#define NOCSIF_LORA_VERSION  1
#define NOCSIF_LORA_TYPE_TEXT 1
#define LORA_TEXT_MAX        200          /* <= 255 - hdr; keep well under the LoRa 255-byte payload */
#define LORA_INBOX_MAX       16

typedef struct __attribute__((packed)) {
    uint8_t  magic0;   /* 'N' */
    uint8_t  magic1;   /* '9' */
    uint8_t  version;  /* 1   */
    uint8_t  type;     /* 1 = text */
    uint32_t src;      /* sender node id */
    uint16_t seq;      /* sequence */
    uint8_t  len;      /* text byte count */
} lora_hdr_t;   /* 11 bytes */

/* ---- worker command queue --------------------------------------------------------- */
typedef enum { CMD_SELFTEST, CMD_SEND, CMD_LISTEN_ON, CMD_LISTEN_OFF,
               CMD_ACTIVITY_ON, CMD_ACTIVITY_OFF, CMD_ACTIVITY_SELFTEST,
               CMD_SURVEY_ON, CMD_SURVEY_OFF, CMD_SURVEY_RESET, CMD_SURVEY_SELFTEST,
               CMD_SURVEY_RANGE,
               CMD_HUNT_ON, CMD_HUNT_OFF, CMD_HUNT_SELFTEST,
               CMD_CARRIER_ON, CMD_CARRIER_SWEEP_ON, CMD_CARRIER_OFF,
               CMD_CAP_ON, CMD_CAP_OFF, CMD_CAP_RESUME, CMD_CAP_END,
               CMD_CAP_SAVE, CMD_CAP_LOAD, CMD_CAP_REPLAY,
               CMD_TX_FILE, CMD_DEBRUIJN, CMD_TX_STOP, CMD_OOK_TX,
               CMD_DEINIT } lora_cmd_type_t;
typedef struct {
    lora_cmd_type_t type;
    char            text[LORA_TEXT_MAX + 1];
    float           fval;   /* HUNT_ON/CARRIER_ON: freq; SURVEY_RANGE/CARRIER_SWEEP_ON: lo MHz */
    float           fval2;  /* SURVEY_RANGE/CARRIER_SWEEP_ON: hi MHz */
    float           fval3;  /* CARRIER_SWEEP_ON: step MHz */
    int32_t         ival;   /* CARRIER_ON/CARRIER_SWEEP_ON: output power (dBm) */
    uint32_t        uval;   /* CARRIER_ON: max ms; CARRIER_SWEEP_ON: dwell ms */
    uint32_t        uval2;  /* CARRIER_SWEEP_ON: dead-man max duration (ms) */
    uint8_t         bval;   /* CARRIER_SWEEP_ON: ping-pong (1) vs wrap (0) */
} lora_cmd_t;
static QueueHandle_t s_cmd_q;

/* ---- inbox (received messages; spinlock-guarded, LVGL-safe copy-out) --------------- */
typedef struct {
    uint32_t src;
    int      rssi;
    int64_t  rx_us;
    char     text[LORA_TEXT_MAX + 1];
} lora_msg_t;
static lora_msg_t         s_inbox[LORA_INBOX_MAX];
static int                s_inbox_head;      /* next write slot */
static int                s_inbox_count;
static portMUX_TYPE       s_inbox_mux = portMUX_INITIALIZER_UNLOCKED;

/* ---- channel-activity snapshot (spinlock-guarded; worker writes, LVGL getter copies out) --------- */
static nocsif_lora_activity_t s_act;
static portMUX_TYPE           s_act_mux = portMUX_INITIALIZER_UNLOCKED;

/* ---- band-survey state (max-hold + hit counts live in the worker; snapshot is copied out) -------- */
static int                  s_survey_peak[NOCSIF_LORA_SURVEY_BINS];   /* max-hold RSSI per bin */
static uint32_t             s_survey_hits[NOCSIF_LORA_SURVEY_BINS];   /* sweeps each bin was active */
static nocsif_lora_survey_t s_survey;
static portMUX_TYPE         s_survey_mux = portMUX_INITIALIZER_UNLOCKED;

/* ---- survey range (re-spans the 52 bins across any window in the SX1262's 150–960 MHz) ----------- */
static float                s_range_lo = LORA_SURVEY_BASE_MHZ;   /* bin 0 centre (default 902.25) */
static float                s_range_hi = LORA_SURVEY_BASE_MHZ +
                                (float)(NOCSIF_LORA_SURVEY_BINS - 1) * LORA_SURVEY_STEP_MHZ;  /* 927.75 */
static float                s_survey_bw_khz = LORA_SURVEY_BW_KHZ; /* RX BW auto-set from the bin spacing */

/* ---- survey omit list (frequency ± tolerance dropped from the detected-signal list) -------------- *
 * MAC-less analogue of the BLE/WiFi omit lists. Written from the LVGL task (add/remove/clear), read on
 * the worker (survey_sweep) under the spinlock. Tiny (16 entries) so it stays in internal RAM like the
 * BLE list; only the persistence scratch goes to PSRAM. */
static nocsif_lora_omit_t   s_lora_omit[NOCSIF_LORA_OMIT_MAX];
static volatile int         s_lora_omit_n;
static bool                 s_lora_omit_loaded;
static portMUX_TYPE         s_lora_omit_mux = portMUX_INITIALIZER_UNLOCKED;
static void                 lora_omit_load(void);   /* fwd: called once in nocsif_lora_init */
static void                 lora_omit_save(void);   /* fwd: LVGL task (sole writer) */

/* ---- carrier-test state (worker owns the radio; bounded by a dead-man timer) --------------------- */
static volatile bool      s_carrier;         /* CW carrier is emitting */
static float              s_cw_mhz;          /* parked freq, or the live swept freq while sweeping */
static int                s_cw_dbm;
static uint32_t           s_cw_max_ms;
static int64_t            s_cw_start_us;
/* sweep sub-state (s_cw_sweep = true): step across [lo,hi] every dwell, wrap or ping-pong */
static bool               s_cw_sweep;
static float              s_cw_lo, s_cw_hi, s_cw_step;
static uint32_t           s_cw_dwell_ms;
static bool               s_cw_pingpong;
static int                s_cw_dir;          /* +1 / -1 sweep direction (ping-pong) */
static int64_t            s_cw_step_us;      /* time of the last frequency step */

/* ---- signal-hunt state (worker owns the envelope; snapshot copied out) --------------------------- */
static float              s_hunt_mhz;        /* parked hunt frequency */
static float              s_hunt_env;        /* RSSI envelope, dBm */
static int                s_hunt_peak;       /* max envelope this session, dBm */
static bool               s_hunt_have;       /* at least one reading taken */
static uint32_t           s_hunt_frames;
static int64_t            s_hunt_last_us;    /* time of the last RSSI read */
static nocsif_lora_hunt_t s_hunt;
static portMUX_TYPE       s_hunt_mux = portMUX_INITIALIZER_UNLOCKED;

/* ---- Sub-GHz packet capture (F1): a bounded PSRAM ring of captured frames + the active preset ---- *
 * The worker owns the radio + writes the ring in cap_poll; the LVGL getters copy out small metadata
 * under the spinlock (full payloads never leave the worker — save/replay read the ring worker-side).
 * The ring is allocated in PSRAM on the first capture (touched only from tasks under a spinlock, never
 * an ISR / cache-disabled) and freed on teardown. */
typedef struct {
    uint32_t idx;
    int64_t  rx_us;
    int      rssi;
    int      snr;
    uint16_t len;
    uint8_t  data[NOCSIF_SUBGHZ_PAYLOAD_MAX];
} subghz_frame_t;
static subghz_frame_t        *s_cap;               /* PSRAM ring [NOCSIF_SUBGHZ_CAP_MAX], lazily allocated */
static int                    s_cap_head;          /* next write slot */
static int                    s_cap_count;
static uint32_t               s_cap_seq;           /* 1-based frame index within the session */
static portMUX_TYPE           s_cap_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool          s_capturing;
static volatile int           s_cap_live_rssi = -128;   /* instantaneous RSSI while capturing (bar graph) */

/* Energy trace: one RSSI sample per capture tick. Always recorded (even when the SX1262 can't DECODE
 * the signal — an OOK burst still lifts the RSSI), so a session ALWAYS has content to save. It is a
 * coarse ~SUBGHZ_TRACE_MS envelope (way too slow to reconstruct OOK bit timing — that wall stands), a
 * reviewable record of when/how strong a transmitter fired at the tuned frequency. Not replayable. */
#define SUBGHZ_TRACE_MAX  480                       /* ~29 s at SUBGHZ_TRACE_MS; then it stops appending */
#define SUBGHZ_TRACE_MS   60                        /* == the capturing worker-loop poll interval */
static int8_t                *s_cap_trace;          /* PSRAM [SUBGHZ_TRACE_MAX], allocated with s_cap */
static volatile int           s_cap_trace_n;
/* The preset the UI is editing (seeds the next capture) + the snapshot the current ring was captured
 * with (drives save + replay, so a later UI edit doesn't corrupt an existing capture). */
static nocsif_subghz_preset_t s_preset = {
    NOCSIF_SUBGHZ_LORA, LORA_FREQ_MHZ,
    LORA_BW_KHZ, LORA_SF, LORA_CR, LORA_SYNC_WORD,
    4.8f, 5.0f, 156.2f, {0,0,0,0,0,0,0,0}, 0, LORA_PREAMBLE };
static nocsif_subghz_preset_t s_cap_preset = s_preset;
static char                   s_capst[2][40];
static volatile int           s_capst_i;
static void publish_capst(const char *s)
{
    int n = s_capst_i ^ 1;
    snprintf(s_capst[n], sizeof s_capst[n], "%s", s);
    s_capst_i = n;
}

/* ---- published state (lock-free double-buffer; worker writes, LVGL getters read) --- */
static char           s_status[2][16];
static char           s_readout[2][96];
static volatile int   s_status_i;
static volatile int   s_readout_i;

static void publish_status(const char *s)
{
    int n = s_status_i ^ 1;
    snprintf(s_status[n], sizeof s_status[n], "%s", s);
    s_status_i = n;
}
static void publish_readout(const char *s)
{
    int n = s_readout_i ^ 1;
    snprintf(s_readout[n], sizeof s_readout[n], "%s", s);
    s_readout_i = n;
}

/* ---- module state ----------------------------------------------------------------- */
static NocsifEspHal    *s_hal;
static Module          *s_mod;
static SX1262          *s_radio;
static TaskHandle_t     s_task;
static bool             s_brought_up;      /* rail + antenna + radio.begin() OK */
static volatile bool    s_available;       /* chip answered (begin succeeded) */
static volatile bool    s_listening;       /* radio is in continuous RX */
static volatile bool    s_scanning;        /* radio is in the channel-activity band scan */
static volatile bool    s_surveying;       /* radio is in the fine band survey (BW500) */
static volatile bool    s_hunting;         /* radio is parked on one freq streaming RSSI (hunt) */
static volatile bool    s_want_deinit;     /* worker asked to tear itself down (free the 8 KB stack) */
static uint32_t         s_node_id;
static uint16_t         s_seq;
static volatile uint32_t s_tx_count;

extern "C" bool nocsif_lora_available(void) { return s_available; }
extern "C" bool nocsif_lora_listening(void) { return s_listening; }
extern "C" uint32_t nocsif_lora_node_id(void) { return s_node_id; }
extern "C" const char *nocsif_lora_status_str(void)  { return s_status[s_status_i]; }
extern "C" const char *nocsif_lora_readout_str(void) { return s_readout[s_readout_i]; }
extern "C" bool nocsif_lora_activity_scanning(void)  { return s_scanning; }

/* Returns the centre frequency of sample channel i: 915 MHz +/- n*step, centred on the home index. */
static float act_freq(int i)
{
    return LORA_FREQ_MHZ + (float)(i - NOCSIF_LORA_ACT_HOME_IDX) * LORA_ACT_STEP_MHZ;
}
extern "C" float nocsif_lora_activity_freq_mhz(int i)
{
    if (i < 0 || i >= NOCSIF_LORA_ACT_CHANS) return 0.0f;
    return act_freq(i);
}
extern "C" bool nocsif_lora_activity_snapshot(nocsif_lora_activity_t *out)
{
    if (!out) return false;
    portENTER_CRITICAL(&s_act_mux);
    *out = s_act;
    portEXIT_CRITICAL(&s_act_mux);
    return out->n > 0;
}

extern "C" bool nocsif_lora_surveying(void) { return s_surveying; }

/* Centre frequency of survey bin i: s_range_lo + i·((hi-lo)/51). Reads the configured window. */
static float survey_freq(int bin)
{
    float step = (s_range_hi - s_range_lo) / (float)(NOCSIF_LORA_SURVEY_BINS - 1);
    return s_range_lo + (float)bin * step;
}
extern "C" float nocsif_lora_survey_freq_mhz(int bin)
{
    if (bin < 0 || bin >= NOCSIF_LORA_SURVEY_BINS) return 0.0f;
    return survey_freq(bin);
}
extern "C" float nocsif_lora_survey_lo_mhz(void)  { return s_range_lo; }
extern "C" float nocsif_lora_survey_hi_mhz(void)  { return s_range_hi; }
extern "C" float nocsif_lora_survey_bin_khz(void)
{
    return (s_range_hi - s_range_lo) / (float)(NOCSIF_LORA_SURVEY_BINS - 1) * 1000.0f;
}

/* Widest SX126x-supported LoRa bandwidth <= the bin spacing (so bins stay gap-free; capped at 500 kHz).
 * A narrow window therefore gets a narrow, MORE sensitive BW automatically; a >26 MHz window saturates at
 * 500 kHz and is undersampled (coarse). */
static float lora_pick_bw_khz(float step_khz)
{
    static const float bws[] = { 7.8f, 10.4f, 15.6f, 20.8f, 31.25f, 41.7f, 62.5f, 125.0f, 250.0f, 500.0f };
    float best = bws[0];
    for (unsigned k = 0; k < sizeof bws / sizeof bws[0]; k++)
        if (bws[k] <= step_khz + 0.01f) best = bws[k];   /* largest supported <= spacing */
    return best;
}

extern "C" void nocsif_lora_survey_set_range(float lo, float hi)
{
    if (s_cmd_q == nullptr) return;
    lora_cmd_t c = {};
    c.type = CMD_SURVEY_RANGE;
    c.fval = lo;
    c.fval2 = hi;
    xQueueSend(s_cmd_q, &c, 0);
}

/* ---- survey omit list -------------------------------------------------------------- */
#define LORA_OMIT_BUF_SZ (NOCSIF_LORA_OMIT_MAX * 40)   /* text scratch, allocated on demand from PSRAM */

/* Rebuild the persisted string from s_lora_omit and commit it. Runs on the LVGL task (the sole writer),
 * so it reads the array without the spinlock. Record: "<mhz> <tol> <note>\n" (note last → may contain
 * spaces). */
static void lora_omit_save(void)
{
    char *buf = (char *)heap_caps_malloc(LORA_OMIT_BUF_SZ, MALLOC_CAP_SPIRAM);
    if (buf == nullptr) { ESP_LOGW(TAG, "lora omit save: no scratch — list not persisted this time"); return; }
    int off = 0, n = s_lora_omit_n;
    for (int i = 0; i < n && off < LORA_OMIT_BUF_SZ - 40; i++)
        off += snprintf(buf + off, LORA_OMIT_BUF_SZ - off, "%.3f %.3f %s\n",
                        (double)s_lora_omit[i].mhz, (double)s_lora_omit[i].tol_mhz, s_lora_omit[i].note);
    nocsif_settings_set_str("lora_omit", buf);
    heap_caps_free(buf);
}

/* Load the persisted omit list once (before the first survey sweep can consult it). */
static void lora_omit_load(void)
{
    if (s_lora_omit_loaded) return;
    s_lora_omit_loaded = true;                       /* set first: a missing key just leaves it empty */
    char *buf = (char *)heap_caps_malloc(LORA_OMIT_BUF_SZ, MALLOC_CAP_SPIRAM);
    if (buf == nullptr) return;
    if (nocsif_settings_get_str("lora_omit", buf, LORA_OMIT_BUF_SZ, "") != ESP_OK || !buf[0]) {
        heap_caps_free(buf);
        return;
    }
    int cnt = 0;
    char *line = buf;
    while (line && *line && cnt < NOCSIF_LORA_OMIT_MAX) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        float mhz = 0.0f, tol = 0.0f; int pos = 0;
        if (sscanf(line, "%f %f %n", &mhz, &tol, &pos) >= 2 && mhz > 0.0f) {
            nocsif_lora_omit_t *e = &s_lora_omit[cnt++];
            e->mhz = mhz;
            e->tol_mhz = (tol > 0.0f ? tol : 0.3f);
            const char *note = (pos > 0 ? line + pos : "");
            snprintf(e->note, sizeof e->note, "%s", note);
        }
        line = nl ? nl + 1 : nullptr;
    }
    s_lora_omit_n = cnt;
    heap_caps_free(buf);
}

extern "C" bool nocsif_lora_omit_contains(float mhz)
{
    bool hit = false;
    portENTER_CRITICAL(&s_lora_omit_mux);
    for (int i = 0; i < s_lora_omit_n; i++)
        if (fabsf(mhz - s_lora_omit[i].mhz) <= s_lora_omit[i].tol_mhz) { hit = true; break; }
    portEXIT_CRITICAL(&s_lora_omit_mux);
    return hit;
}

extern "C" bool nocsif_lora_omit_add(float mhz, float tol, const char *note)
{
    if (mhz <= 0.0f) return false;
    if (tol <= 0.0f) tol = 0.3f;
    if (nocsif_lora_omit_contains(mhz)) return true;   /* already covered */
    nocsif_lora_omit_t e;                              /* build off-lock (no snprintf under the spinlock) */
    e.mhz = mhz; e.tol_mhz = tol;
    snprintf(e.note, sizeof e.note, "%s", note ? note : "");
    bool ok = false;
    portENTER_CRITICAL(&s_lora_omit_mux);
    if (s_lora_omit_n < NOCSIF_LORA_OMIT_MAX) { s_lora_omit[s_lora_omit_n] = e; s_lora_omit_n = s_lora_omit_n + 1; ok = true; }
    portEXIT_CRITICAL(&s_lora_omit_mux);
    if (ok) lora_omit_save();
    return ok;
}

extern "C" bool nocsif_lora_omit_remove(float mhz)
{
    bool removed = false;
    portENTER_CRITICAL(&s_lora_omit_mux);
    for (int i = 0; i < s_lora_omit_n; i++) {
        if (fabsf(mhz - s_lora_omit[i].mhz) <= s_lora_omit[i].tol_mhz) {
            for (int j = i; j < s_lora_omit_n - 1; j++) s_lora_omit[j] = s_lora_omit[j + 1];
            s_lora_omit_n = s_lora_omit_n - 1;
            removed = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_lora_omit_mux);
    if (removed) lora_omit_save();
    return removed;
}

extern "C" void nocsif_lora_omit_clear(void)
{
    portENTER_CRITICAL(&s_lora_omit_mux);
    s_lora_omit_n = 0;
    portEXIT_CRITICAL(&s_lora_omit_mux);
    lora_omit_save();
}

extern "C" int nocsif_lora_omit_count(void) { return s_lora_omit_n; }

extern "C" bool nocsif_lora_omit_get(int i, nocsif_lora_omit_t *out)
{
    if (!out) return false;
    bool ok = false;
    portENTER_CRITICAL(&s_lora_omit_mux);
    if (i >= 0 && i < s_lora_omit_n) { *out = s_lora_omit[i]; ok = true; }
    portEXIT_CRITICAL(&s_lora_omit_mux);
    return ok;
}
extern "C" bool nocsif_lora_survey_snapshot(nocsif_lora_survey_t *out)
{
    if (!out) return false;
    portENTER_CRITICAL(&s_survey_mux);
    *out = s_survey;
    portEXIT_CRITICAL(&s_survey_mux);
    return out->n > 0;
}

extern "C" bool nocsif_lora_hunting(void) { return s_hunting; }

extern "C" bool nocsif_lora_hunt_snapshot(nocsif_lora_hunt_t *out)
{
    if (!out) return false;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_hunt_mux);
    *out = s_hunt;
    portEXIT_CRITICAL(&s_hunt_mux);
    out->age_ms = s_hunt_have ? (uint32_t)((now - s_hunt_last_us) / 1000) : 0xFFFFFFFFu;
    return s_hunt_have;
}

extern "C" bool nocsif_lora_carrier_active(void) { return s_carrier; }

extern "C" bool nocsif_lora_carrier_snapshot(nocsif_lora_carrier_t *out)
{
    if (!out || !s_carrier) return false;
    uint32_t elapsed = (uint32_t)((esp_timer_get_time() - s_cw_start_us) / 1000);
    out->mhz          = s_cw_mhz;
    out->dbm          = s_cw_dbm;
    out->elapsed_ms   = elapsed;
    out->remaining_ms = (elapsed < s_cw_max_ms) ? (s_cw_max_ms - elapsed) : 0;
    out->sweeping     = s_cw_sweep;
    out->lo           = s_cw_lo;
    out->hi           = s_cw_hi;
    return true;
}

extern "C" int nocsif_lora_inbox_count(void)
{
    portENTER_CRITICAL(&s_inbox_mux);
    int n = s_inbox_count;
    portEXIT_CRITICAL(&s_inbox_mux);
    return n;
}

/* ---- Sub-GHz capture getters (LVGL-safe: spinlock snapshot, no hardware access) ------------------ */
extern "C" const char *nocsif_subghz_status_str(void) { return s_capst[s_capst_i]; }
extern "C" bool nocsif_subghz_capturing(void)         { return s_capturing; }
extern "C" int  nocsif_subghz_live_rssi(void)         { return s_cap_live_rssi; }
extern "C" int  nocsif_subghz_trace_count(void)       { return s_cap_trace_n; }
extern "C" float nocsif_subghz_cap_freq(void)         { return s_cap_preset.freq_mhz; }
extern "C" void nocsif_subghz_set_preset(const nocsif_subghz_preset_t *p) { if (p) s_preset = *p; }
extern "C" void nocsif_subghz_get_preset(nocsif_subghz_preset_t *out)     { if (out) *out = s_preset; }

extern "C" int nocsif_subghz_cap_count(void)
{
    portENTER_CRITICAL(&s_cap_mux);
    int n = s_cap_count;
    portEXIT_CRITICAL(&s_cap_mux);
    return n;
}

extern "C" bool nocsif_subghz_cap_get(int i, nocsif_subghz_cap_t *out)
{
    if (!out) return false;
    bool ok = false;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_cap_mux);
    if (s_cap && i >= 0 && i < s_cap_count) {
        int idx = (s_cap_head - 1 - i + 2 * NOCSIF_SUBGHZ_CAP_MAX) % NOCSIF_SUBGHZ_CAP_MAX;
        const subghz_frame_t *f = &s_cap[idx];
        out->idx    = f->idx;
        out->rssi   = f->rssi;
        out->snr    = f->snr;
        out->len    = f->len;
        out->age_ms = (uint32_t)((now - f->rx_us) / 1000);
        int hn = f->len < NOCSIF_SUBGHZ_HEAD ? f->len : NOCSIF_SUBGHZ_HEAD;
        memcpy(out->head, f->data, hn);
        if (hn < NOCSIF_SUBGHZ_HEAD) memset(out->head + hn, 0, NOCSIF_SUBGHZ_HEAD - hn);
        ok = true;
    }
    portEXIT_CRITICAL(&s_cap_mux);
    return ok;
}

extern "C" void nocsif_subghz_cap_clear(void)
{
    portENTER_CRITICAL(&s_cap_mux);
    s_cap_count = 0;
    s_cap_head  = 0;
    portEXIT_CRITICAL(&s_cap_mux);
    publish_capst("cleared");
}

/* Copy out inbox message i (0 = newest). Returns false if out of range. LVGL-safe. */
extern "C" bool nocsif_lora_inbox_get(int i, uint32_t *src, char *text, size_t text_sz,
                                      int *rssi, uint32_t *age_ms)
{
    bool ok = false;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_inbox_mux);
    if (i >= 0 && i < s_inbox_count) {
        /* newest-first: head-1 is newest */
        int idx = (s_inbox_head - 1 - i + 2 * LORA_INBOX_MAX) % LORA_INBOX_MAX;
        const lora_msg_t *m = &s_inbox[idx];
        if (src)  *src = m->src;
        if (rssi) *rssi = m->rssi;
        if (age_ms) *age_ms = (uint32_t)((now - m->rx_us) / 1000);
        if (text && text_sz) { snprintf(text, text_sz, "%s", m->text); }
        ok = true;
    }
    portEXIT_CRITICAL(&s_inbox_mux);
    return ok;
}

static void inbox_push(uint32_t src, const char *text, int rssi)
{
    portENTER_CRITICAL(&s_inbox_mux);
    lora_msg_t *m = &s_inbox[s_inbox_head];
    m->src = src;
    m->rssi = rssi;
    m->rx_us = esp_timer_get_time();
    snprintf(m->text, sizeof m->text, "%s", text);
    s_inbox_head = (s_inbox_head + 1) % LORA_INBOX_MAX;
    if (s_inbox_count < LORA_INBOX_MAX) s_inbox_count++;
    portEXIT_CRITICAL(&s_inbox_mux);
}

/* First-use bring-up: ALDO3 rail + built-in antenna + RadioLib SX1262 begin(). Lock held by caller. */
static void bring_up_locked(void)
{
    esp_err_t perr = nocsif_power_lora_rail(true);
    if (perr != ESP_OK) {
        ESP_LOGE(TAG, "LoRa rail (ALDO3) enable failed: %s", esp_err_to_name(perr));
        publish_status("err");
        publish_readout("LoRa rail failed — PMU not ready.");
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    if (nocsif_xl9555_set_output(LORA_XL9555_ANT_SW, true) != ESP_OK) {
        ESP_LOGW(TAG, "antenna select (XL9555 IO%d) failed — continuing", LORA_XL9555_ANT_SW);
    }

    if (s_radio == nullptr) {
        s_hal   = new NocsifEspHal(LORA_SPI_HOST, LORA_PIN_SCK, LORA_PIN_MISO, LORA_PIN_MOSI, LORA_SPI_HZ);
        s_mod   = new Module(s_hal, LORA_PIN_CS, LORA_PIN_DIO1, LORA_PIN_RST, LORA_PIN_BUSY);
        s_radio = new SX1262(s_mod);
    }

    int st = s_radio->begin(LORA_FREQ_MHZ, LORA_BW_KHZ, LORA_SF, LORA_CR,
                            LORA_SYNC_WORD, LORA_POWER_DBM, LORA_PREAMBLE, LORA_TCXO_V, LORA_USE_LDO);
    if (st != RADIOLIB_ERR_NONE) {
        ESP_LOGE(TAG, "radio.begin() failed: %d", st);
        publish_status("err");
        char line[96];
        snprintf(line, sizeof line, "RadioLib begin failed (%d)", st);
        publish_readout(line);
        return;
    }
    /* Ensure DIO1 is a readable input on the ESP side — the RX poll reads
     * its level (RxDone) with a plain gpio_get_level (no bus), so it must be
     * configured as input regardless of RadioLib. */
    gpio_set_direction((gpio_num_t)LORA_PIN_DIO1, GPIO_MODE_INPUT);

    s_brought_up = true;
    s_available  = true;
    ESP_LOGI(TAG, "SX1262 up via RadioLib: %.1f MHz, BW %.0f, SF%d, CR4/%d, %d dBm, TCXO %.1fV, node %08lX",
             (double)LORA_FREQ_MHZ, (double)LORA_BW_KHZ, LORA_SF, LORA_CR, LORA_POWER_DBM,
             (double)LORA_TCXO_V, (unsigned long)s_node_id);
}

/* Builds a P2P text frame into buf (must hold LORA_TEXT_MAX + sizeof(hdr)). Returns total length. */
static size_t build_text_frame(uint8_t *buf, const char *text)
{
    lora_hdr_t hdr;
    hdr.magic0  = NOCSIF_LORA_MAGIC0;
    hdr.magic1  = NOCSIF_LORA_MAGIC1;
    hdr.version = NOCSIF_LORA_VERSION;
    hdr.type    = NOCSIF_LORA_TYPE_TEXT;
    hdr.src     = s_node_id;
    hdr.seq     = ++s_seq;
    size_t tlen = strlen(text);
    if (tlen > LORA_TEXT_MAX) tlen = LORA_TEXT_MAX;
    hdr.len = (uint8_t)tlen;
    memcpy(buf, &hdr, sizeof hdr);
    memcpy(buf + sizeof hdr, text, tlen);
    return sizeof hdr + tlen;
}

/* Parses a received frame into the inbox. Returns true if it was a valid NocSif text frame. */
static bool parse_frame(const uint8_t *buf, size_t n, int rssi)
{
    if (n < sizeof(lora_hdr_t)) return false;
    lora_hdr_t hdr;
    memcpy(&hdr, buf, sizeof hdr);
    if (hdr.magic0 != NOCSIF_LORA_MAGIC0 || hdr.magic1 != NOCSIF_LORA_MAGIC1) return false;
    if (hdr.version != NOCSIF_LORA_VERSION || hdr.type != NOCSIF_LORA_TYPE_TEXT) return false;
    size_t tlen = hdr.len;
    if (tlen > LORA_TEXT_MAX || sizeof(lora_hdr_t) + tlen > n) return false;
    char text[LORA_TEXT_MAX + 1];
    memcpy(text, buf + sizeof hdr, tlen);
    text[tlen] = '\0';
    inbox_push(hdr.src, text, rssi);
    ESP_LOGI(TAG, "rx msg from %08lX (%d dBm): \"%s\"", (unsigned long)hdr.src, rssi, text);
    return true;
}

/* Non-blocking RX poll (called from the worker loop while listening; lock taken only on a packet). */
static void rx_poll(void)
{
    if (!s_brought_up || !s_listening) return;
    if (!gpio_get_level((gpio_num_t)LORA_PIN_DIO1)) return;   /* no RxDone yet — cheap GPIO read, no bus */

    uint8_t buf[256];
    size_t n = 0;
    int rssi = 0;
    int st = RADIOLIB_ERR_RX_TIMEOUT;
    if (nocsif_sdcard_lock(1000)) {
        n = s_radio->getPacketLength();
        if (n > sizeof buf) n = sizeof buf;
        st = s_radio->readData(buf, n);
        rssi = (int)s_radio->getRSSI();
        s_radio->startReceive();   /* re-arm continuous RX */
        nocsif_sdcard_unlock();
    }
    if (st == RADIOLIB_ERR_NONE && n > 0) {
        if (!parse_frame(buf, n, rssi)) {
            ESP_LOGD(TAG, "rx: %u bytes, not a NocSif frame (rssi %d)", (unsigned)n, rssi);
        }
    }
}

/* One channel-activity sweep: for each channel retune (standby -> setFrequency
 * -> startReceive), let the receiver + AGC settle, then read the
 * instantaneous RSSI (GetRssiInst); finish with a LoRa CAD pass on the home
 * channel. The SD lock is taken only for the brief SPI bursts and released
 * across each settle delay, so SD stays usable while scanning. GetRssiInst
 * returns the floor sentinel (~-127 dBm) if read too soon after entering
 * RX — the settle delay is what makes the per-channel reading real. */
#define LORA_ACT_SETTLE_MS 8      /* RX + AGC settle before GetRssiInst is valid (empirical) */
#define LORA_ACT_NO_READ   (-128) /* sentinel: this channel could not be read this sweep */

static void activity_sweep(void)
{
    if (!s_brought_up || !s_scanning) return;

    int rssi[NOCSIF_LORA_ACT_CHANS];
    for (int i = 0; i < NOCSIF_LORA_ACT_CHANS; i++) {
        rssi[i] = LORA_ACT_NO_READ;
        if (!nocsif_sdcard_lock(1000)) continue;     /* SD busy — skip this channel this sweep */
        s_radio->standby();                          /* SetRfFrequency needs STDBY/FS, not RX */
        s_radio->setFrequency(act_freq(i));
        s_radio->startReceive();                     /* continuous RX on this channel */
        nocsif_sdcard_unlock();
        vTaskDelay(pdMS_TO_TICKS(LORA_ACT_SETTLE_MS));   /* bus free during the settle */
        if (nocsif_sdcard_lock(1000)) {
            rssi[i] = (int)s_radio->getRSSI(false);  /* instantaneous channel RSSI (dBm) */
            nocsif_sdcard_unlock();
        }
    }

    /* Home channel: instantaneous RSSI + a LoRa CAD pass (detects real preambles at SF9/BW125). */
    int home_rssi = LORA_ACT_NO_READ;
    int cad = RADIOLIB_ERR_UNKNOWN;
    if (nocsif_sdcard_lock(1000)) {
        s_radio->standby();
        s_radio->setFrequency(LORA_FREQ_MHZ);
        s_radio->startReceive();
        nocsif_sdcard_unlock();
    }
    vTaskDelay(pdMS_TO_TICKS(LORA_ACT_SETTLE_MS));
    if (nocsif_sdcard_lock(2000)) {
        home_rssi = (int)s_radio->getRSSI(false);
        cad = s_radio->scanChannel();
        s_radio->standby();
        nocsif_sdcard_unlock();
    }

    bool cad_detected = (cad == RADIOLIB_LORA_DETECTED);
    bool cad_valid    = (cad == RADIOLIB_LORA_DETECTED || cad == RADIOLIB_CHANNEL_FREE);

    /* busiest = strongest, quietest = weakest; skip un-read channels (the NO_READ sentinel). */
    int bi = -1, qi = -1;
    for (int i = 0; i < NOCSIF_LORA_ACT_CHANS; i++) {
        if (rssi[i] <= LORA_ACT_NO_READ) continue;
        if (bi < 0 || rssi[i] > rssi[bi]) bi = i;
        if (qi < 0 || rssi[i] < rssi[qi]) qi = i;
    }
    if (bi < 0) bi = NOCSIF_LORA_ACT_HOME_IDX;
    if (qi < 0) qi = NOCSIF_LORA_ACT_HOME_IDX;

    portENTER_CRITICAL(&s_act_mux);
    for (int i = 0; i < NOCSIF_LORA_ACT_CHANS; i++) s_act.rssi[i] = rssi[i];
    s_act.n         = NOCSIF_LORA_ACT_CHANS;
    s_act.home_rssi = home_rssi;
    s_act.busiest   = bi;
    s_act.quietest  = qi;
    if (cad_valid) { s_act.cad_scans++; if (cad_detected) s_act.cad_hits++; s_act.last_cad = cad_detected; }
    s_act.sweeps++;
    portEXIT_CRITICAL(&s_act_mux);
}

static void do_send(const char *text)
{
    if (!nocsif_sdcard_lock(3000)) {
        publish_status("busy");
        publish_readout("SPI bus busy — try again.");
        return;
    }
    if (!s_brought_up) bring_up_locked();
    if (s_brought_up) {
        uint8_t frame[sizeof(lora_hdr_t) + LORA_TEXT_MAX];
        size_t flen = build_text_frame(frame, text);
        int st = s_radio->transmit(frame, flen);
        if (s_listening) s_radio->startReceive();   /* return to RX after the TX */
        if (st == RADIOLIB_ERR_NONE) {
            s_tx_count++;
            ESP_LOGI(TAG, "tx msg #%lu seq %u (%u B): \"%s\"",
                     (unsigned long)s_tx_count, (unsigned)s_seq, (unsigned)flen, text);
            publish_status("sent");
            char line[96];
            snprintf(line, sizeof line, "Sent #%lu (%u B) @%.0f MHz", (unsigned long)s_tx_count,
                     (unsigned)flen, (double)LORA_FREQ_MHZ);
            publish_readout(line);
        } else {
            ESP_LOGW(TAG, "tx failed: %d", st);
            publish_status("tx err");
            char line[96];
            snprintf(line, sizeof line, "Send failed (%d)", st);
            publish_readout(line);
        }
    }
    nocsif_sdcard_unlock();
}

static void do_listen(bool on)
{
    if (!nocsif_sdcard_lock(3000)) {
        publish_status("busy");
        return;
    }
    if (!s_brought_up) bring_up_locked();
    if (s_brought_up) {
        if (on) {
            int st = s_radio->startReceive();
            s_listening = (st == RADIOLIB_ERR_NONE);
            ESP_LOGI(TAG, "listen ON -> startReceive %d", st);
            publish_status(s_listening ? "listen" : "err");
            publish_readout(s_listening ? "Listening for messages\xE2\x80\xA6" : "startReceive failed");
        } else {
            s_listening = false;
            s_radio->standby();
            ESP_LOGI(TAG, "listen OFF");
            publish_status("idle");
            publish_readout("Idle.");
        }
    }
    nocsif_sdcard_unlock();
}

/* Enters/leaves the channel-activity band scan. Scanning and listening are
 * mutually exclusive (both own the radio's RX mode). The worker's loop
 * drives the actual sweeps once s_scanning is set. */
static void do_activity(bool on)
{
    if (on) {
        s_listening = false;   /* hand the radio to the scanner */
        if (!nocsif_sdcard_lock(3000)) {
            publish_status("busy");
            publish_readout("SPI bus busy — try again.");
            return;
        }
        if (!s_brought_up) bring_up_locked();
        if (s_brought_up) s_radio->standby();
        nocsif_sdcard_unlock();
        if (s_brought_up) {
            portENTER_CRITICAL(&s_act_mux);   /* fresh session counters */
            memset(&s_act, 0, sizeof s_act);
            portEXIT_CRITICAL(&s_act_mux);
            s_scanning = true;
            publish_status("scan");
            publish_readout("Scanning 902\xE2\x80\x93""928 MHz\xE2\x80\xA6");
            ESP_LOGI(TAG, "channel-activity scan ON (%d chans, %.1f\xE2\x80\x93%.1f MHz)",
                     NOCSIF_LORA_ACT_CHANS, (double)act_freq(0),
                     (double)act_freq(NOCSIF_LORA_ACT_CHANS - 1));
        }
    } else {
        s_scanning = false;
        if (nocsif_sdcard_lock(2000)) {
            if (s_brought_up) s_radio->standby();
            nocsif_sdcard_unlock();
        }
        publish_status("idle");
        publish_readout("Channel scan stopped.");
        ESP_LOGI(TAG, "channel-activity scan OFF");
    }
}

/* Channel-activity self-test: brings up + runs a few band sweeps
 * synchronously (on the worker) and logs the spectrum + CAD verdict.
 * Passive RX/CAD only — does not transmit. Confirms the scan reads a real
 * per-channel RSSI floor across 902-928 MHz (a solo, on-watch verification,
 * no peer needed). */
static void do_activity_selftest(void)
{
    ESP_LOGW(TAG, "==== LoRa CHANNEL-ACTIVITY SELF-TEST (node %08lX) ====", (unsigned long)s_node_id);
    do_activity(true);                         /* bring up + arm the scan */
    for (int k = 0; k < 6 && s_scanning; k++) {
        activity_sweep();                      /* synchronous sweep (worker owns the radio here) */
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    nocsif_lora_activity_t s;
    bool have = nocsif_lora_activity_snapshot(&s);
    if (have) {
        char line[196];
        int off = 0;
        for (int i = 0; i < NOCSIF_LORA_ACT_CHANS && off < (int)sizeof line - 8; i++)
            off += snprintf(line + off, sizeof line - off, "%d ", s.rssi[i]);
        ESP_LOGW(TAG, "  spectrum (%d chans, %.1f-%.1f MHz) dBm: %s",
                 NOCSIF_LORA_ACT_CHANS, (double)act_freq(0),
                 (double)act_freq(NOCSIF_LORA_ACT_CHANS - 1), line);
        ESP_LOGW(TAG, "  home(915) %d dBm, busiest %.1f MHz, quietest %.1f MHz, CAD hits %lu/%lu",
                 s.home_rssi, (double)act_freq(s.busiest), (double)act_freq(s.quietest),
                 (unsigned long)s.cad_hits, (unsigned long)s.cad_scans);
        bool sane = (s.home_rssi < 0 && s.home_rssi > -140 && s.cad_scans > 0);
        ESP_LOGW(TAG, "---- VERDICT: %lu sweeps, RSSI floor read %s, CAD engine %s ----",
                 (unsigned long)s.sweeps, sane ? "OK" : "SUSPECT",
                 s.cad_scans > 0 ? "OK" : "no result");
    } else {
        ESP_LOGW(TAG, "---- VERDICT: no sweep completed (SUSPECT — check bring-up) ----");
    }
    do_activity(false);
    ESP_LOGW(TAG, "======================================================================");
}

/* Clears the survey's max-hold + hit counts + published snapshot (re-arm the survey). */
static void do_survey_reset(void)
{
    portENTER_CRITICAL(&s_survey_mux);
    memset(&s_survey, 0, sizeof s_survey);
    portEXIT_CRITICAL(&s_survey_mux);
    for (int i = 0; i < NOCSIF_LORA_SURVEY_BINS; i++) {
        s_survey_peak[i] = LORA_ACT_NO_READ;
        s_survey_hits[i] = 0;
    }
}

/* Re-span the survey window to [lo,hi] (clamped to the SX1262's 150–960 MHz), pick the RX bandwidth from
 * the new bin spacing, and re-arm. Runs ON the worker (owns the radio); the UI posts CMD_SURVEY_RANGE. */
static void do_survey_range(float lo, float hi)
{
    if (lo < NOCSIF_LORA_RANGE_MIN_MHZ) lo = NOCSIF_LORA_RANGE_MIN_MHZ;
    if (hi > NOCSIF_LORA_RANGE_MAX_MHZ) hi = NOCSIF_LORA_RANGE_MAX_MHZ;
    if (hi - lo < 1.0f) {                 /* enforce a sane minimum window (~1 MHz) */
        hi = lo + 1.0f;
        if (hi > NOCSIF_LORA_RANGE_MAX_MHZ) { hi = NOCSIF_LORA_RANGE_MAX_MHZ; lo = hi - 1.0f; }
    }
    s_range_lo = lo;
    s_range_hi = hi;
    float step_khz = (hi - lo) / (float)(NOCSIF_LORA_SURVEY_BINS - 1) * 1000.0f;
    s_survey_bw_khz = lora_pick_bw_khz(step_khz);
    if (s_brought_up && s_surveying && nocsif_sdcard_lock(2000)) {
        s_radio->standby();
        s_radio->setBandwidth(s_survey_bw_khz);   /* re-apply for the new spacing */
        s_radio->standby();
        nocsif_sdcard_unlock();
    }
    do_survey_reset();                    /* clear max-hold/hits — the old window's data is meaningless now */
    ESP_LOGI(TAG, "survey range -> %.2f\xE2\x80\x93%.2f MHz (step %.1f kHz, BW %.1f kHz)",
             (double)lo, (double)hi, (double)step_khz, (double)s_survey_bw_khz);
}

/* One survey sweep: read the instantaneous RSSI of every 500 kHz bin (retune + settle + GetRssiInst,
 * same discipline as the channel-activity sweep), then update the per-bin max-hold + hit counts, a
 * median noise floor, and the detected-signal list (contiguous bins whose max-hold sits >= the detect
 * margin above the floor, aggregated + strongest-first). The SD lock is released across each settle. */
static void survey_sweep(void)
{
    if (!s_brought_up || !s_surveying) return;

    int cur[NOCSIF_LORA_SURVEY_BINS];
    for (int i = 0; i < NOCSIF_LORA_SURVEY_BINS; i++) {
        cur[i] = LORA_ACT_NO_READ;
        if (!nocsif_sdcard_lock(1000)) continue;
        s_radio->standby();
        s_radio->setFrequency(survey_freq(i));
        s_radio->startReceive();
        nocsif_sdcard_unlock();
        vTaskDelay(pdMS_TO_TICKS(LORA_ACT_SETTLE_MS));   /* RX/AGC settle (bus free) */
        if (nocsif_sdcard_lock(1000)) {
            cur[i] = (int)s_radio->getRSSI(false);
            nocsif_sdcard_unlock();
        }
    }
    if (nocsif_sdcard_lock(1000)) { s_radio->standby(); nocsif_sdcard_unlock(); }

    /* Noise floor = median of the readable bins. */
    int tmp[NOCSIF_LORA_SURVEY_BINS], m = 0;
    for (int i = 0; i < NOCSIF_LORA_SURVEY_BINS; i++)
        if (cur[i] > LORA_ACT_NO_READ) tmp[m++] = cur[i];
    int floor = -120;
    if (m > 0) { std::sort(tmp, tmp + m); floor = tmp[m / 2]; }
    int thresh = floor + LORA_SURVEY_DETECT_DB;

    /* Max-hold + hit counts. */
    for (int i = 0; i < NOCSIF_LORA_SURVEY_BINS; i++) {
        if (cur[i] <= LORA_ACT_NO_READ) continue;
        if (cur[i] > s_survey_peak[i]) s_survey_peak[i] = cur[i];
        if (cur[i] > thresh) s_survey_hits[i]++;
    }

    /* Detect signals by hit count, not by raw max-hold. A bin scores a "hit"
     * each sweep the scanner caught it >= LORA_SURVEY_DETECT_DB above the
     * floor; a transmitter the scanner keeps catching accumulates hits,
     * while noise almost never spikes 10 dB above the median so noise bins
     * stay near zero hits. (Grouping by max-hold would drift instead: over
     * many sweeps the worst noise sample creeps up and would eventually
     * merge the whole band into one false signal.) Adjacent signal-active
     * bins form one signal; its strength is the max-hold peak. The bar
     * rises slowly with run length so a lone noise blip never lists, but a
     * persistent or periodic transmitter is not filtered out. */
    int nsw = s_survey.sweeps + 1;                     /* this sweep's 1-based index (worker owns it) */
    int min_hits = nsw / 20;                            /* >= ~5% of sweeps long-term... */
    if (min_hits < 3) min_hits = 3;                     /* ...and at least three catches (reject blips) */

    nocsif_lora_signal_t found[NOCSIF_LORA_SURVEY_BINS];
    int nf = 0, i = 0;
    while (i < NOCSIF_LORA_SURVEY_BINS) {
        if (s_survey_hits[i] >= (uint32_t)min_hits) {
            int start = i, pbin = i, pval = s_survey_peak[i];
            uint32_t phits = s_survey_hits[i];
            while (i < NOCSIF_LORA_SURVEY_BINS && s_survey_hits[i] >= (uint32_t)min_hits) {
                if (s_survey_peak[i] > pval) { pval = s_survey_peak[i]; pbin = i; }
                if (s_survey_hits[i] > phits) phits = s_survey_hits[i];
                i++;
            }
            found[nf].bin = pbin;
            found[nf].peak_rssi = pval;
            found[nf].span_bins = i - start;
            found[nf].hits = phits;
            nf++;
        } else {
            i++;
        }
    }
    /* Drop omitted frequencies (known local carriers) so they don't clutter the hunt pick list. */
    {
        int w = 0;
        for (int k = 0; k < nf; k++)
            if (!nocsif_lora_omit_contains(survey_freq(found[k].bin))) found[w++] = found[k];
        nf = w;
    }
    /* Keep the strongest NOCSIF_LORA_SURVEY_SIGS (partial selection sort by peak). */
    int keep = nf < NOCSIF_LORA_SURVEY_SIGS ? nf : NOCSIF_LORA_SURVEY_SIGS;
    for (int a = 0; a < keep; a++) {
        int best = a;
        for (int b = a + 1; b < nf; b++)
            if (found[b].peak_rssi > found[best].peak_rssi) best = b;
        nocsif_lora_signal_t t = found[a]; found[a] = found[best]; found[best] = t;
    }

    portENTER_CRITICAL(&s_survey_mux);
    memcpy(s_survey.cur, cur, sizeof cur);
    memcpy(s_survey.peak, s_survey_peak, sizeof s_survey_peak);
    s_survey.n     = NOCSIF_LORA_SURVEY_BINS;
    s_survey.floor = floor;
    s_survey.nsig  = keep;
    memcpy(s_survey.sig, found, (size_t)keep * sizeof(nocsif_lora_signal_t));
    s_survey.sweeps++;
    portEXIT_CRITICAL(&s_survey_mux);
}

/* Enters/leaves the band survey. Widens the radio to BW500 (gap-free
 * 500 kHz bins) on entry and restores the BW125 messaging config on exit.
 * Mutually exclusive with listen + channel-activity. */
static void do_survey(bool on)
{
    if (on) {
        s_listening = false;
        s_scanning  = false;
        s_hunting   = false;   /* survey and hunt are mutually exclusive (the unpin path re-enters here) */
        s_carrier   = false;   /* and with the carrier test */
        s_capturing = false;   /* and with packet capture */
        if (!nocsif_sdcard_lock(3000)) {
            publish_status("busy");
            publish_readout("SPI bus busy — try again.");
            return;
        }
        if (!s_brought_up) bring_up_locked();
        if (s_brought_up) {
            s_radio->standby();
            s_radio->setBandwidth(s_survey_bw_khz);   /* auto-selected from the configured window */
            s_radio->standby();
        }
        nocsif_sdcard_unlock();
        if (s_brought_up) {
            do_survey_reset();
            s_surveying = true;
            publish_status("survey");
            char rl[96];
            snprintf(rl, sizeof rl, "Band survey %.1f\xE2\x80\x93%.1f MHz\xE2\x80\xA6",
                     (double)survey_freq(0), (double)survey_freq(NOCSIF_LORA_SURVEY_BINS - 1));
            publish_readout(rl);
            ESP_LOGI(TAG, "band survey ON (%d bins %.2f\xE2\x80\x93%.2f MHz, BW %.1f kHz)",
                     NOCSIF_LORA_SURVEY_BINS, (double)survey_freq(0),
                     (double)survey_freq(NOCSIF_LORA_SURVEY_BINS - 1), (double)s_survey_bw_khz);
        }
    } else {
        s_surveying = false;
        if (nocsif_sdcard_lock(2000)) {
            if (s_brought_up) {
                s_radio->standby();
                s_radio->setBandwidth(LORA_BW_KHZ);   /* restore the messaging BW */
                s_radio->standby();
            }
            nocsif_sdcard_unlock();
        }
        publish_status("idle");
        publish_readout("Band survey stopped.");
        ESP_LOGI(TAG, "band survey OFF (BW restored %.0f kHz)", (double)LORA_BW_KHZ);
    }
}

/* Band-survey self-test: brings up + runs a few survey sweeps synchronously
 * and logs the floor + the detected-signal list. Passive RX only — no
 * emission. Confirms the fine sweep + detection work. */
static void do_survey_selftest(void)
{
    ESP_LOGW(TAG, "==== LoRa BAND-SURVEY SELF-TEST (node %08lX) ====", (unsigned long)s_node_id);
    do_survey(true);
    for (int k = 0; k < 8 && s_surveying; k++) {
        survey_sweep();
        vTaskDelay(pdMS_TO_TICKS(80));
    }

    nocsif_lora_survey_t s;
    bool have = nocsif_lora_survey_snapshot(&s);
    if (have) {
        ESP_LOGW(TAG, "  %d bins %.2f-%.2f MHz (BW %.0f kHz), floor ~%d dBm, %lu sweeps, %d signal(s):",
                 s.n, (double)survey_freq(0), (double)survey_freq(s.n - 1), (double)LORA_SURVEY_BW_KHZ,
                 s.floor, (unsigned long)s.sweeps, s.nsig);
        for (int i = 0; i < s.nsig; i++)
            ESP_LOGW(TAG, "    #%d: %.2f MHz, peak %d dBm, %d bin(s), %lu hits", i + 1,
                     (double)survey_freq(s.sig[i].bin), s.sig[i].peak_rssi, s.sig[i].span_bins,
                     (unsigned long)s.sig[i].hits);
        bool sane = (s.floor < 0 && s.floor > -140 && s.sweeps > 0);
        ESP_LOGW(TAG, "---- VERDICT: survey %s, %d signal(s) >= floor+%d dB ----",
                 sane ? "OK" : "SUSPECT", s.nsig, LORA_SURVEY_DETECT_DB);
    } else {
        ESP_LOGW(TAG, "---- VERDICT: no survey sweep completed (SUSPECT — check bring-up) ----");
    }
    do_survey(false);
    ESP_LOGW(TAG, "======================================================================");
}

/* Starts an energy hunt: parks on one frequency at the hunt BW in
 * continuous RX. The worker loop then polls the RSSI fast (hunt_poll).
 * Mutually exclusive with listen / scan / survey. */
static void do_hunt(float mhz)
{
    s_listening = false;
    s_scanning  = false;
    s_surveying = false;
    s_carrier   = false;   /* mutually exclusive with the carrier test */
    s_capturing = false;   /* and with packet capture */
    if (!nocsif_sdcard_lock(3000)) {
        publish_status("busy");
        publish_readout("SPI bus busy — try again.");
        return;
    }
    if (!s_brought_up) bring_up_locked();
    if (s_brought_up) {
        s_radio->standby();
        s_radio->setFrequency(mhz);
        s_radio->setBandwidth(LORA_HUNT_BW_KHZ);
        s_radio->startReceive();
    }
    nocsif_sdcard_unlock();
    if (s_brought_up) {
        s_hunt_mhz    = mhz;
        s_hunt_have   = false;
        s_hunt_frames = 0;
        s_hunt_peak   = -128;
        s_hunting     = true;
        publish_status("hunt");
        char line[96];
        snprintf(line, sizeof line, "Hunting %.2f MHz\xE2\x80\xA6", (double)mhz);
        publish_readout(line);
        ESP_LOGI(TAG, "signal hunt ON @ %.2f MHz (BW %.0f kHz)", (double)mhz, (double)LORA_HUNT_BW_KHZ);
    }
}

/* Reads the parked frequency's RSSI and folds it into the envelope (fast
 * attack, slow decay). Called from the worker loop every
 * ~LORA_HUNT_POLL_MS while hunting; no retune, so it is cheap. */
static void hunt_poll(void)
{
    if (!s_brought_up || !s_hunting) return;
    int rssi;
    if (!nocsif_sdcard_lock(500)) return;   /* SD busy this tick — skip, try next */
    rssi = (int)s_radio->getRSSI(false);
    nocsif_sdcard_unlock();

    if (!s_hunt_have) {
        s_hunt_env  = (float)rssi;
        s_hunt_peak = rssi;
        s_hunt_have = true;
    } else {
        if ((float)rssi > s_hunt_env) s_hunt_env = (float)rssi;             /* fast attack */
        else s_hunt_env += ((float)rssi - s_hunt_env) * LORA_HUNT_DECAY;    /* slow decay */
        int e = (int)lroundf(s_hunt_env);
        if (e > s_hunt_peak) s_hunt_peak = e;
    }
    s_hunt_frames++;
    s_hunt_last_us = esp_timer_get_time();

    portENTER_CRITICAL(&s_hunt_mux);
    s_hunt.smoothed = (int)lroundf(s_hunt_env);
    s_hunt.peak     = s_hunt_peak;
    s_hunt.frames   = s_hunt_frames;
    s_hunt.heard    = true;
    s_hunt.freq_mhz = s_hunt_mhz;
    portEXIT_CRITICAL(&s_hunt_mux);
}

static void do_hunt_stop(void)
{
    s_hunting = false;
    if (nocsif_sdcard_lock(2000)) {
        if (s_brought_up) {
            s_radio->standby();
            s_radio->setBandwidth(LORA_BW_KHZ);   /* restore the messaging BW */
        }
        nocsif_sdcard_unlock();
    }
    publish_status("idle");
    publish_readout("Hunt stopped.");
    ESP_LOGI(TAG, "signal hunt OFF");
}

/* ---- Carrier Test (bounded CW output) --------------------------------------------------------- *
 * Park on one frequency and emit an unmodulated continuous wave (SET_TX_CONTINUOUS_WAVE via RadioLib
 * transmitDirect()). The worker loop enforces the dead-man timer (carrier_tick) so a run always ends.
 * Mutually exclusive with every RX mode. ⚠ this EMITS on the chosen frequency. */
static void do_carrier(float mhz, int dbm, uint32_t max_ms)
{
    if (dbm < NOCSIF_LORA_CW_DBM_MIN) dbm = NOCSIF_LORA_CW_DBM_MIN;
    if (dbm > NOCSIF_LORA_CW_DBM_MAX) dbm = NOCSIF_LORA_CW_DBM_MAX;
    if (max_ms == 0 || max_ms > NOCSIF_LORA_CW_MS_MAX) max_ms = NOCSIF_LORA_CW_MS_MAX;
    if (mhz < NOCSIF_LORA_RANGE_MIN_MHZ) mhz = NOCSIF_LORA_RANGE_MIN_MHZ;
    if (mhz > NOCSIF_LORA_RANGE_MAX_MHZ) mhz = NOCSIF_LORA_RANGE_MAX_MHZ;

    s_listening = false;
    s_scanning  = false;
    s_surveying = false;
    s_hunting   = false;
    s_capturing = false;   /* carrier test is mutually exclusive with packet capture */
    if (!nocsif_sdcard_lock(3000)) {
        publish_status("busy");
        publish_readout("SPI bus busy — try again.");
        return;
    }
    if (!s_brought_up) bring_up_locked();
    if (s_brought_up) {
        s_radio->standby();
        s_radio->setFrequency(mhz);
        s_radio->setOutputPower((int8_t)dbm);
        s_radio->transmitDirect();            /* SET_TX_CONTINUOUS_WAVE — unmodulated carrier */
    }
    nocsif_sdcard_unlock();
    if (s_brought_up) {
        s_cw_mhz      = mhz;
        s_cw_lo       = mhz;
        s_cw_hi       = mhz;
        s_cw_sweep    = false;
        s_cw_dbm      = dbm;
        s_cw_max_ms   = max_ms;
        s_cw_start_us = esp_timer_get_time();
        s_carrier     = true;
        publish_status("carrier");
        char line[96];
        snprintf(line, sizeof line, "CW %.2f MHz @ %d dBm (max %us)",
                 (double)mhz, dbm, (unsigned)(max_ms / 1000));
        publish_readout(line);
        ESP_LOGW(TAG, "CARRIER ON %.2f MHz %d dBm (dead-man %u ms)", (double)mhz, dbm, (unsigned)max_ms);
    }
}

/* Start a bounded SWEPT carrier across [lo,hi]. Parks CW on lo, then carrier_tick steps it every dwell. */
static void do_carrier_sweep(float lo, float hi, float step, uint32_t dwell_ms, bool pingpong,
                             int dbm, uint32_t max_ms)
{
    if (dbm < NOCSIF_LORA_CW_DBM_MIN) dbm = NOCSIF_LORA_CW_DBM_MIN;
    if (dbm > NOCSIF_LORA_CW_DBM_MAX) dbm = NOCSIF_LORA_CW_DBM_MAX;
    if (max_ms == 0 || max_ms > NOCSIF_LORA_CW_MS_MAX) max_ms = NOCSIF_LORA_CW_MS_MAX;
    if (lo < NOCSIF_LORA_RANGE_MIN_MHZ) lo = NOCSIF_LORA_RANGE_MIN_MHZ;
    if (hi > NOCSIF_LORA_RANGE_MAX_MHZ) hi = NOCSIF_LORA_RANGE_MAX_MHZ;
    if (lo > hi) { float t = lo; lo = hi; hi = t; }
    if (step < 0.05f) step = 0.05f;                 /* sane minimum hop */
    if (step > (hi - lo)) step = (hi - lo);
    if (dwell_ms < 20)   dwell_ms = 20;
    if (dwell_ms > 5000) dwell_ms = 5000;

    s_listening = false;
    s_scanning  = false;
    s_surveying = false;
    s_hunting   = false;
    s_capturing = false;   /* carrier test is mutually exclusive with packet capture */
    if (!nocsif_sdcard_lock(3000)) {
        publish_status("busy");
        publish_readout("SPI bus busy — try again.");
        return;
    }
    if (!s_brought_up) bring_up_locked();
    if (s_brought_up) {
        s_radio->standby();
        s_radio->setFrequency(lo);
        s_radio->setOutputPower((int8_t)dbm);
        s_radio->transmitDirect();
    }
    nocsif_sdcard_unlock();
    if (s_brought_up) {
        s_cw_lo       = lo;
        s_cw_hi       = hi;
        s_cw_step     = step;
        s_cw_dwell_ms = dwell_ms;
        s_cw_pingpong = pingpong;
        s_cw_dir      = 1;
        s_cw_mhz      = lo;
        s_cw_sweep    = true;
        s_cw_dbm      = dbm;
        s_cw_max_ms   = max_ms;
        s_cw_start_us = esp_timer_get_time();
        s_cw_step_us  = s_cw_start_us;
        s_carrier     = true;
        publish_status("carrier");
        char line[96];
        snprintf(line, sizeof line, "sweep %.1f\xE2\x80\x93%.1f MHz @ %d dBm", (double)lo, (double)hi, dbm);
        publish_readout(line);
        ESP_LOGW(TAG, "CARRIER SWEEP %.2f-%.2f MHz step %.2f dwell %ums %s %d dBm (dead-man %u ms)",
                 (double)lo, (double)hi, (double)step, (unsigned)dwell_ms, pingpong ? "ping-pong" : "wrap",
                 dbm, (unsigned)max_ms);
    }
}

static void do_carrier_stop(void)
{
    bool was = s_carrier;
    s_carrier = false;
    s_cw_sweep = false;
    if (nocsif_sdcard_lock(2000)) {
        if (s_brought_up) {
            s_radio->standby();                       /* drop the carrier */
            s_radio->setOutputPower(LORA_POWER_DBM);  /* restore the default TX power */
        }
        nocsif_sdcard_unlock();
    }
    if (was) {
        publish_status("idle");
        publish_readout("Carrier stopped.");
        ESP_LOGW(TAG, "CARRIER OFF");
    }
}

/* Advance the swept carrier one hop (retune the live TX). Retunes under the SD lock (shared SPI3). */
static void carrier_step(void)
{
    float next = s_cw_mhz + (float)s_cw_dir * s_cw_step;
    if (s_cw_pingpong) {
        if (next > s_cw_hi + 0.001f) { s_cw_dir = -1; next = s_cw_hi; }
        else if (next < s_cw_lo - 0.001f) { s_cw_dir = 1; next = s_cw_lo; }
    } else {
        if (next > s_cw_hi + 0.001f) next = s_cw_lo;   /* wrap */
    }
    if (nocsif_sdcard_lock(500)) {
        if (s_brought_up) {
            s_radio->standby();
            s_radio->setFrequency(next);
            s_radio->transmitDirect();                 /* re-assert CW on the new frequency */
        }
        nocsif_sdcard_unlock();
        s_cw_mhz = next;
    }
}

/* Dead-man + sweep stepping: called from the worker loop while emitting. */
static void carrier_tick(void)
{
    if (!s_carrier) return;
    int64_t now = esp_timer_get_time();
    uint32_t elapsed = (uint32_t)((now - s_cw_start_us) / 1000);
    if (elapsed >= s_cw_max_ms) {
        ESP_LOGW(TAG, "CARRIER dead-man expired (%u ms) — auto-stop", (unsigned)s_cw_max_ms);
        do_carrier_stop();
        return;
    }
    if (s_cw_sweep && (uint32_t)((now - s_cw_step_us) / 1000) >= s_cw_dwell_ms) {
        carrier_step();
        s_cw_step_us = now;
    }
}

/* ---- Sub-GHz packet capture / replay (F1) ----------------------------------------------------- *
 * cap_poll RXes matching packets into the PSRAM ring; do_capture arms/disarms; do_cap_save writes the
 * ring to /sd/nocsif/subghz/<name>.sub; do_cap_replay re-transmits one frame. All radio ops run on the
 * worker under nocsif_sdcard_lock (shared SPI3), mirroring the messaging/survey discipline. */

/* Allocate the PSRAM capture buffers (ring + energy trace) once. Returns false if either alloc fails. */
static bool subghz_ensure_bufs(void)
{
    if (!s_cap) {
        s_cap = (subghz_frame_t *)heap_caps_malloc(sizeof(subghz_frame_t) * NOCSIF_SUBGHZ_CAP_MAX, MALLOC_CAP_SPIRAM);
        if (!s_cap) return false;
    }
    if (!s_cap_trace) {
        s_cap_trace = (int8_t *)heap_caps_malloc(SUBGHZ_TRACE_MAX, MALLOC_CAP_SPIRAM);
        if (!s_cap_trace) return false;
    }
    return true;
}

/* Append a captured frame to the ring (worker only; small copy under the spinlock). */
static void cap_push(const uint8_t *d, uint16_t len, int rssi, int snr)
{
    if (!s_cap) return;
    if (len > NOCSIF_SUBGHZ_PAYLOAD_MAX) len = NOCSIF_SUBGHZ_PAYLOAD_MAX;
    portENTER_CRITICAL(&s_cap_mux);
    subghz_frame_t *f = &s_cap[s_cap_head];
    f->idx  = ++s_cap_seq;
    f->rx_us = esp_timer_get_time();
    f->rssi = rssi;
    f->snr  = snr;
    f->len  = len;
    memcpy(f->data, d, len);
    s_cap_head = (s_cap_head + 1) % NOCSIF_SUBGHZ_CAP_MAX;
    if (s_cap_count < NOCSIF_SUBGHZ_CAP_MAX) s_cap_count++;
    portEXIT_CRITICAL(&s_cap_mux);
}

/* Dial the radio to a capture/replay preset (LoRa vs FSK re-inits the packet type). Lock held; up. */
static void subghz_apply_preset_locked(const nocsif_subghz_preset_t *p)
{
    if (!s_brought_up) return;
    s_radio->standby();
    if (p->mod == NOCSIF_SUBGHZ_FSK) {
        s_radio->beginFSK(p->freq_mhz, p->br_kbps, p->fdev_khz, p->rxbw_khz,
                          LORA_POWER_DBM, p->preamble, LORA_TCXO_V, LORA_USE_LDO);
        if (p->fsk_sync_len > 0) {
            uint8_t sw[8];
            uint8_t sl = p->fsk_sync_len > 8 ? 8 : p->fsk_sync_len;
            memcpy(sw, p->fsk_sync, sl);
            s_radio->setSyncWord(sw, sl);
        }
    } else {
        s_radio->begin(p->freq_mhz, p->bw_khz, p->sf, p->cr, p->syncword,
                       LORA_POWER_DBM, p->preamble, LORA_TCXO_V, LORA_USE_LDO);
    }
}

/* Restore the default LoRa messaging config so the other modes (survey/hunt/messaging) keep working
 * after a capture/replay may have switched the modem to FSK or a different LoRa preset. Lock held. */
static void subghz_relora_baseline_locked(void)
{
    if (!s_brought_up) return;
    s_radio->standby();
    s_radio->begin(LORA_FREQ_MHZ, LORA_BW_KHZ, LORA_SF, LORA_CR,
                   LORA_SYNC_WORD, LORA_POWER_DBM, LORA_PREAMBLE, LORA_TCXO_V, LORA_USE_LDO);
    s_radio->standby();
}

/* Non-blocking capture poll (worker loop while capturing). Every tick reads the instantaneous RSSI for
 * the live bar graph (cheap, one register read — responds to ANY energy, even undecodable OOK); when
 * DIO1 flags a decoded packet it stores the RAW payload + rssi/snr and re-arms RX. */
static void cap_poll(void)
{
    if (!s_brought_up || !s_capturing) return;

    /* Live energy meter: instantaneous RSSI at the tuned frequency (drives the bar graph + the trace). */
    if (nocsif_sdcard_lock(200)) {
        int inst = (int)s_radio->getRSSI(false);
        nocsif_sdcard_unlock();
        s_cap_live_rssi = inst;
        if (s_cap_trace && s_cap_trace_n < SUBGHZ_TRACE_MAX) {
            int8_t v = inst < -128 ? -128 : (inst > 127 ? 127 : (int8_t)inst);
            s_cap_trace[s_cap_trace_n++] = v;
        }
    }

    if (!gpio_get_level((gpio_num_t)LORA_PIN_DIO1)) return;   /* no RxDone — cheap GPIO read, no bus */

    uint8_t buf[256];
    size_t n = 0;
    int rssi = 0, snr = 0;
    int st = RADIOLIB_ERR_RX_TIMEOUT;
    if (nocsif_sdcard_lock(1000)) {
        n = s_radio->getPacketLength();
        if (n > sizeof buf) n = sizeof buf;
        st = s_radio->readData(buf, n);
        rssi = (int)s_radio->getRSSI();
        if (s_cap_preset.mod == NOCSIF_SUBGHZ_LORA) snr = (int)s_radio->getSNR();
        s_radio->startReceive();   /* re-arm continuous RX */
        nocsif_sdcard_unlock();
    }
    if (st == RADIOLIB_ERR_NONE && n > 0) {
        cap_push(buf, (uint16_t)n, rssi, snr);
        char b[40];
        snprintf(b, sizeof b, "capturing %d frame%s", s_cap_count, s_cap_count == 1 ? "" : "s");
        publish_capst(b);
        ESP_LOGI(TAG, "subghz cap frame #%lu %u B rssi %d snr %d",
                 (unsigned long)s_cap_seq, (unsigned)n, rssi, snr);
    }
}

/* Enter/leave passive capture. Fresh session clears the ring; STOP keeps it (so save/replay still work)
 * and restores the LoRa baseline. Mutually exclusive with every other radio mode. */
static void do_capture(bool on)
{
    if (on) {
        s_listening = false; s_scanning = false; s_surveying = false; s_hunting = false; s_carrier = false;
        if (!subghz_ensure_bufs()) { publish_capst("no mem"); ESP_LOGE(TAG, "subghz: capture buffer alloc failed"); return; }
        if (!nocsif_sdcard_lock(3000)) { publish_capst("busy"); return; }
        if (!s_brought_up) bring_up_locked();
        if (s_brought_up) {
            s_cap_preset = s_preset;                 /* freeze the preset this ring is captured with */
            subghz_apply_preset_locked(&s_cap_preset);
            s_radio->startReceive();
        }
        nocsif_sdcard_unlock();
        if (s_brought_up) {
            portENTER_CRITICAL(&s_cap_mux);
            s_cap_head = 0; s_cap_count = 0;
            portEXIT_CRITICAL(&s_cap_mux);
            s_cap_seq       = 0;
            s_cap_trace_n   = 0;
            s_cap_live_rssi = -128;
            s_capturing     = true;
            char b[40];
            snprintf(b, sizeof b, "recording %.3f MHz", (double)s_cap_preset.freq_mhz);
            publish_capst(b);
            ESP_LOGI(TAG, "subghz capture ON (%s %.3f MHz)",
                     s_cap_preset.mod == NOCSIF_SUBGHZ_FSK ? "FSK" : "LoRa", (double)s_cap_preset.freq_mhz);
        }
    } else {
        /* PAUSE: standby, keep the ring + preset so Continue/Save still work. No baseline restore
         * (that happens on capture_end when the user leaves the flow). */
        s_capturing = false;
        if (nocsif_sdcard_lock(2000)) {
            if (s_brought_up) s_radio->standby();
            nocsif_sdcard_unlock();
        }
        publish_capst(s_cap_count > 0 ? "stopped" : "idle");
        ESP_LOGI(TAG, "subghz capture PAUSE (%d frame%s held)", s_cap_count, s_cap_count == 1 ? "" : "s");
    }
}

/* Resume the current session (keep the ring; "Continue recording") — just re-arm RX on the same preset. */
static void do_cap_resume(void)
{
    if (!s_cap) { do_capture(true); return; }   /* nothing to resume → start fresh */
    if (!nocsif_sdcard_lock(2000)) { publish_capst("busy"); return; }
    if (!s_brought_up) bring_up_locked();
    if (s_brought_up) { subghz_apply_preset_locked(&s_cap_preset); s_radio->startReceive(); }
    nocsif_sdcard_unlock();
    if (s_brought_up) {
        s_capturing = true;
        char b[40]; snprintf(b, sizeof b, "recording %.3f MHz", (double)s_cap_preset.freq_mhz);
        publish_capst(b);
        ESP_LOGI(TAG, "subghz capture RESUME (%d frame%s held)", s_cap_count, s_cap_count == 1 ? "" : "s");
    }
}

/* Leave the capture flow: standby + restore the default LoRa baseline so Messaging/Survey/etc. work. */
static void do_cap_end(void)
{
    s_capturing = false;
    if (nocsif_sdcard_lock(2000)) {
        if (s_brought_up) subghz_relora_baseline_locked();
        nocsif_sdcard_unlock();
    }
    publish_capst("idle");
    ESP_LOGI(TAG, "subghz capture END (baseline restored)");
}

/* Choose the next free /sd/nocsif/subghz/<base>-NNN.sub. Assumes the /sd lock is held. */
static void subghz_pick_path(char *out, size_t outlen, const char *base)
{
    for (int i = 0; i < 1000; i++) {
        snprintf(out, outlen, "/sd/nocsif/subghz/%s-%03d.sub", base, i);
        struct stat st;
        if (stat(out, &st) != 0) return;             /* first non-existent name */
    }
    snprintf(out, outlen, "/sd/nocsif/subghz/%s-999.sub", base);
}

/* Write the .sub header: a Flipper SubGhz container line-set (so the file is recognised + carries the
 * frequency) plus NocSif preset keys for on-watch replay. A captured LoRa/FSK PACKET has no OOK timing,
 * so Protocol is NocSifPacket (a Flipper reads the header, ignores the custom protocol); the frames
 * follow as F: lines below. Every number is an INTEGER — frequency/BW/BR/FDEV/RXBW are stored in Hz/bps —
 * so the LoRa worker's PSRAM stack never hits newlib's %f float-formatting path, which overflowed the old
 * JSON writer (0-byte file + task=lora panic). Float ARITHMETIC (MHz->Hz) is a cheap FPU op; only %f is banned. */
static void subghz_write_header(FILE *f)
{
    const nocsif_subghz_preset_t *p = &s_cap_preset;
    fputs("Filetype: Flipper SubGhz RAW File\n", f);
    fputs("Version: 1\n", f);
    fprintf(f, "Frequency: %lu\n", (unsigned long)(p->freq_mhz * 1e6f + 0.5f));
    fputs("Preset: FuriHalSubGhzPresetCustom\n", f);
    fputs("Protocol: NocSifPacket\n", f);
    fprintf(f, "Node: %08lX\n", (unsigned long)s_node_id);
    if (p->mod == NOCSIF_SUBGHZ_FSK) {
        char syn[20]; int o = 0;
        for (int k = 0; k < p->fsk_sync_len && o < (int)sizeof syn - 2; k++)
            o += snprintf(syn + o, sizeof syn - o, "%02X", p->fsk_sync[k]);
        syn[o] = '\0';
        fprintf(f, "Mod: fsk\nBR: %lu\nFDEV: %lu\nRXBW: %lu\nFskSync: %s\nPreamble: %u\n",
                (unsigned long)(p->br_kbps  * 1000.0f + 0.5f),
                (unsigned long)(p->fdev_khz * 1000.0f + 0.5f),
                (unsigned long)(p->rxbw_khz * 1000.0f + 0.5f),
                syn[0] ? syn : "-", (unsigned)p->preamble);
    } else {
        fprintf(f, "Mod: lora\nBW: %lu\nSF: %u\nCR: %u\nSync: %u\nPreamble: %u\n",
                (unsigned long)(p->bw_khz * 1000.0f + 0.5f),
                (unsigned)p->sf, (unsigned)p->cr, (unsigned)p->syncword, (unsigned)p->preamble);
    }
    fprintf(f, "Frames: %d\nTraceMs: %d\n", s_cap_count, SUBGHZ_TRACE_MS);
}

/* Save the ring to /sd/nocsif/subghz/<name>.sub (Flipper SubGhz container header + NocSif preset keys +
 * F: frame lines). Runs on the worker (owns the ring; single-threaded, so it reads the ring without the
 * spinlock). name NULL/"" → auto cap-NNN. Claims the card + takes the lock like the WiFi PCAP writer.
 * Integer-only formatting (no %f) keeps the worker's PSRAM stack off newlib's float path — the old JSON
 * writer's %.4f header overflowed it (0-byte file + task=lora panic). */
static void do_cap_save(const char *name)
{
    if (!s_cap || (s_cap_count == 0 && s_cap_trace_n == 0)) { publish_capst("nothing to save"); return; }

    esp_err_t ce = nocsif_usb_gadget_claim_sd(2000);
    if (ce != ESP_OK) {
        publish_capst(ce == ESP_ERR_INVALID_STATE ? "File Share has SD" : "no SD");
        return;
    }
    char path[96] = {0};
    FILE *f = NULL;
    if (nocsif_sdcard_lock(3000)) {
        mkdir("/sd/nocsif", 0777);
        mkdir("/sd/nocsif/subghz", 0777);
        if (name && name[0]) snprintf(path, sizeof path, "/sd/nocsif/subghz/%s.sub", name);
        else                 subghz_pick_path(path, sizeof path, "cap");
        f = fopen(path, "wb");
        if (f) {
            subghz_write_header(f);
            /* Frames, oldest → newest: "F: <idx> <t_ms> <rssi> <snr> <len> <hex>" (t_ms relative to the
             * first captured frame). All integers + hex — no %f on the worker stack. */
            int oldest = (s_cap_head - s_cap_count + 2 * NOCSIF_SUBGHZ_CAP_MAX) % NOCSIF_SUBGHZ_CAP_MAX;
            int64_t t0 = s_cap[oldest].rx_us;
            char line[NOCSIF_SUBGHZ_PAYLOAD_MAX * 2 + 96];
            for (int k = 0; k < s_cap_count; k++) {
                int idx = (oldest + k) % NOCSIF_SUBGHZ_CAP_MAX;
                const subghz_frame_t *fr = &s_cap[idx];
                int o = snprintf(line, sizeof line, "F: %lu %lu %d %d %u ",
                                 (unsigned long)fr->idx, (unsigned long)((fr->rx_us - t0) / 1000),
                                 fr->rssi, fr->snr, (unsigned)fr->len);
                for (int b = 0; b < fr->len && o < (int)sizeof line - 3; b++)
                    o += snprintf(line + o, sizeof line - o, "%02X", fr->data[b]);
                o += snprintf(line + o, sizeof line - o, "\n");
                fputs(line, f);
            }
            /* Energy trace: chunks of up to 32 int8 RSSI samples per line ("E: -92 -91 ..."). */
            for (int i = 0; i < s_cap_trace_n; ) {
                int o = snprintf(line, sizeof line, "E:");
                for (int c = 0; i < s_cap_trace_n && c < 32 && o < (int)sizeof line - 8; i++, c++)
                    o += snprintf(line + o, sizeof line - o, " %d", (int)s_cap_trace[i]);
                snprintf(line + o, sizeof line - o, "\n");
                fputs(line, f);
            }
            fclose(f);
        }
        nocsif_sdcard_unlock();
    }
    nocsif_usb_gadget_release_sd();

    if (f) {
        const char *bn = strrchr(path, '/');
        char b[40];
        snprintf(b, sizeof b, "saved %s", bn ? bn + 1 : path);
        publish_capst(b);
        ESP_LOGI(TAG, "subghz capture saved -> %s (%d frames)", path, s_cap_count);
    } else {
        publish_capst("save failed");
        ESP_LOGE(TAG, "subghz capture save failed (%s)", path[0] ? path : "no path");
    }
}

/* Decode a run of hex pairs from the .sub loader (the format is ours, so a targeted scan, not JSON). */
static int sg_hex_bytes(const char *hp, uint8_t *out, int max)
{
    int n = 0;
    while (n < max && isxdigit((unsigned char)hp[0]) && isxdigit((unsigned char)hp[1])) {
        unsigned bv = 0; sscanf(hp, "%2x", &bv); out[n++] = (uint8_t)bv; hp += 2;
    }
    return n;
}

/* Load a saved .sub back into the ring (+ its preset) so it can be replayed on the watch. Runs on the
 * worker; claims the card + takes the lock like the writer. Parses the Flipper/NocSif header keys, then
 * one packet per "F:" line. A Flipper RAW/OOK .sub (Protocol: RAW with RAW_Data) is transmit-only — it is
 * NOT loaded into the packet ring here (the OOK transmit path reads those directly). Integer-only parse
 * (no %f scanf), matching the writer. */
static void do_cap_load(const char *path)
{
    if (!path || !path[0]) { publish_capst("no path"); return; }
    if (!subghz_ensure_bufs()) { publish_capst("no mem"); return; }
    s_cap_trace_n = 0;   /* a loaded capture is for replaying its packets; no live trace */
    esp_err_t ce = nocsif_usb_gadget_claim_sd(2000);
    if (ce != ESP_OK) { publish_capst(ce == ESP_ERR_INVALID_STATE ? "File Share has SD" : "no SD"); return; }

    bool ok = false, is_raw = false;
    int  loaded = 0;
    nocsif_subghz_preset_t pr = s_preset;   /* start from defaults, override from the header */
    if (nocsif_sdcard_lock(3000)) {
        FILE *f = fopen(path, "rb");
        if (f) {
            portENTER_CRITICAL(&s_cap_mux); s_cap_head = 0; s_cap_count = 0; portEXIT_CRITICAL(&s_cap_mux);
            s_cap_seq = 0;
            char line[NOCSIF_SUBGHZ_PAYLOAD_MAX * 2 + 128];
            unsigned long uv; int iv;
            while (fgets(line, sizeof line, f)) {
                if      (!strncmp(line, "Protocol:", 9)) { if (strstr(line, "RAW")) is_raw = true; }
                else if (!strncmp(line, "Mod:", 4))       pr.mod = strstr(line, "fsk") ? NOCSIF_SUBGHZ_FSK : NOCSIF_SUBGHZ_LORA;
                else if (!strncmp(line, "Frequency:", 10) && sscanf(line + 10, "%lu", &uv) == 1) pr.freq_mhz = uv / 1e6f;
                else if (!strncmp(line, "BW:",   3) && sscanf(line + 3, "%lu", &uv) == 1) pr.bw_khz   = uv / 1000.0f;
                else if (!strncmp(line, "BR:",   3) && sscanf(line + 3, "%lu", &uv) == 1) pr.br_kbps  = uv / 1000.0f;
                else if (!strncmp(line, "FDEV:", 5) && sscanf(line + 5, "%lu", &uv) == 1) pr.fdev_khz = uv / 1000.0f;
                else if (!strncmp(line, "RXBW:", 5) && sscanf(line + 5, "%lu", &uv) == 1) pr.rxbw_khz = uv / 1000.0f;
                else if (!strncmp(line, "SF:",   3) && sscanf(line + 3, "%d",  &iv) == 1) pr.sf       = (uint8_t)iv;
                else if (!strncmp(line, "CR:",   3) && sscanf(line + 3, "%d",  &iv) == 1) pr.cr       = (uint8_t)iv;
                else if (!strncmp(line, "Sync:", 5) && sscanf(line + 5, "%d",  &iv) == 1) pr.syncword = (uint8_t)iv;
                else if (!strncmp(line, "Preamble:", 9) && sscanf(line + 9, "%d", &iv) == 1) pr.preamble = (uint16_t)iv;
                else if (!strncmp(line, "FskSync:", 8)) {
                    const char *hp = line + 8; while (*hp == ' ') hp++;
                    pr.fsk_sync_len = (uint8_t)sg_hex_bytes(hp, pr.fsk_sync, 8);
                }
                else if (!strncmp(line, "F:", 2) && loaded < NOCSIF_SUBGHZ_CAP_MAX) {
                    int fi, ft, frssi = 0, fsnr = 0, flen = 0, consumed = 0;
                    if (sscanf(line, "F: %d %d %d %d %d %n", &fi, &ft, &frssi, &fsnr, &flen, &consumed) >= 5
                        && consumed > 0) {
                        uint8_t data[NOCSIF_SUBGHZ_PAYLOAD_MAX];
                        int dl = sg_hex_bytes(line + consumed, data, NOCSIF_SUBGHZ_PAYLOAD_MAX);
                        if (dl > 0) { cap_push(data, (uint16_t)dl, frssi, fsnr); loaded++; }
                    }
                }
                /* "E:" energy-trace lines are ignored on load (replay needs packets, not the RSSI trace). */
            }
            fclose(f);
            ok = true;
        }
        nocsif_sdcard_unlock();
    }
    nocsif_usb_gadget_release_sd();

    if (ok && loaded > 0) {
        s_cap_preset = pr;
        const char *bn = strrchr(path, '/');
        char b[40];
        snprintf(b, sizeof b, "loaded %s (%d)", bn ? bn + 1 : path, loaded);
        publish_capst(b);
        unsigned long hz = (unsigned long)(pr.freq_mhz * 1e6f + 0.5f);
        ESP_LOGI(TAG, "subghz loaded %s: %d frame(s), %s %lu.%03lu MHz", path, loaded,
                 pr.mod == NOCSIF_SUBGHZ_FSK ? "FSK" : "LoRa", hz / 1000000UL, (hz / 1000UL) % 1000UL);
    } else {
        publish_capst(ok ? (is_raw ? "raw .sub — TX only" : "empty capture") : "load failed");
        ESP_LOGW(TAG, "subghz load %s: %s", path,
                 ok ? (is_raw ? "raw/ook — transmit-only (Phase 3)" : "no frames") : "open failed");
    }
}

/* Re-transmit captured frame i on its capture preset. EMITS (the UI arms + ISM-gates first). */
static void do_cap_replay(int i)
{
    subghz_frame_t fr;
    bool ok = false;
    portENTER_CRITICAL(&s_cap_mux);
    if (s_cap && i >= 0 && i < s_cap_count) {
        int idx = (s_cap_head - 1 - i + 2 * NOCSIF_SUBGHZ_CAP_MAX) % NOCSIF_SUBGHZ_CAP_MAX;
        fr = s_cap[idx];
        ok = true;
    }
    portEXIT_CRITICAL(&s_cap_mux);
    if (!ok) { publish_capst("bad index"); return; }

    if (!nocsif_sdcard_lock(3000)) { publish_capst("busy"); return; }
    int st = RADIOLIB_ERR_UNKNOWN;
    if (!s_brought_up) bring_up_locked();
    if (s_brought_up) {
        subghz_apply_preset_locked(&s_cap_preset);
        st = s_radio->transmit(fr.data, fr.len);
        if (s_capturing) s_radio->startReceive();   /* back to capturing */
        else             subghz_relora_baseline_locked();
    }
    nocsif_sdcard_unlock();

    char b[40];
    if (st == RADIOLIB_ERR_NONE) snprintf(b, sizeof b, "replayed #%lu (%uB)", (unsigned long)fr.idx, (unsigned)fr.len);
    else                         snprintf(b, sizeof b, "replay err %d", st);
    publish_capst(b);
    ESP_LOGW(TAG, "subghz replay #%lu %u B -> %d", (unsigned long)fr.idx, (unsigned)fr.len, st);
}

/* ==== Crude OOK transmit (F2/F3): CW-gated bit-bang + auto-routed .sub transmit + de Bruijn ======= *
 * The SX1262 has no OOK modulator and DIO2 is the internal T/R switch on this board, so there is no data
 * pin to bit-bang. OOK is instead produced by GATING the continuous-wave carrier: transmitDirect()
 * (SET_TX_CONTINUOUS_WAVE) keys it ON, standby() keys it OFF — the same two ops the Carrier Test uses,
 * toggled in time. A timestamp-accumulator busy-wait keeps cumulative timing accurate despite the
 * per-edge SPI latency (~tens of µs), so timing is CRUDE (fine for tolerant fixed-code remotes, useless
 * against rolling codes). Every run is dead-man bounded; the UI arms + ISM-gates it. Runs ON the worker
 * (owns the radio + shared SPI3); a foreground caller stops it by setting s_ook_stop directly. */
static volatile bool s_ook_active;     /* an OOK / brute-force run is emitting */
static volatile bool s_ook_stop;       /* a foreground task asked to stop early (checked in the busy-wait) */
static int32_t      *s_ook_dur;        /* PSRAM scratch [NOCSIF_OOK_DUR_MAX]; + = carrier-on µs, - = off µs */
/* Params for a CMD_OOK_TX prepared by the LVGL task (durations already copied into s_ook_dur). */
static int           s_ook_n;
static float         s_ook_freq;
static int           s_ook_reps;
static uint32_t      s_ook_max;

extern "C" bool nocsif_subghz_tx_active(void) { return s_ook_active; }

static bool ook_ensure_buf(void)
{
    if (!s_ook_dur)
        s_ook_dur = (int32_t *)heap_caps_malloc(sizeof(int32_t) * NOCSIF_OOK_DUR_MAX, MALLOC_CAP_SPIRAM);
    return s_ook_dur != nullptr;
}

/* Key the carrier ON / OFF. Assumes the SD lock is held and the radio is up + tuned + power set. */
static inline void ook_key_on(void)  { s_radio->transmitDirect(); }   /* SET_TX_CONTINUOUS_WAVE */
static inline void ook_key_off(void) { s_radio->standby(); }

/* Emit n durations (Flipper RAW convention: + = mark/carrier-on µs, - = space/off µs). *t_ref is the
 * running edge deadline (seeded by the caller); busy-waits absorb the command latency into each gap so
 * cumulative timing does not drift. Assumes the SD lock is held. Returns false if stopped mid-frame. */
static bool ook_emit(const int32_t *dur, int n, int64_t *t_ref)
{
    for (int i = 0; i < n; i++) {
        int32_t d = dur[i];
        if (d == 0) continue;
        bool on = d > 0;
        if (on) ook_key_on(); else ook_key_off();
        *t_ref += (on ? d : -d);
        /* A long OFF gap (carrier already down, no timing-critical edge held) is coarse-slept so the busy-
         * wait can't starve the idle task / WDT on a RAW file with big inter-frame gaps; the ON marks and
         * short gaps spin precisely. */
        for (;;) {
            int64_t rem = *t_ref - esp_timer_get_time();
            if (rem <= 0) break;
            if (!on && rem > 4000) { vTaskDelay(pdMS_TO_TICKS((uint32_t)(rem / 1000) - 1)); continue; }
            /* spin the remainder to the edge deadline */
        }
        if (s_ook_stop) { ook_key_off(); return false; }
    }
    ook_key_off();
    return true;
}

/* Bring the radio up, tune + set OOK power, emit dur[n] `repeats` times (gap_ms between), dead-man bounded.
 * Locks per repeat so the shared SPI3 is free between frames. Restores standby + default power + the LoRa
 * baseline on the way out so Messaging / Survey / etc. keep working. */
static void ook_run(const int32_t *dur, int n, float freq_mhz, int repeats, uint32_t gap_ms,
                    uint32_t max_ms, const char *label)
{
    if (n <= 0) { publish_capst("empty pattern"); return; }
    if (freq_mhz < NOCSIF_LORA_RANGE_MIN_MHZ) freq_mhz = NOCSIF_LORA_RANGE_MIN_MHZ;
    if (freq_mhz > NOCSIF_LORA_RANGE_MAX_MHZ) freq_mhz = NOCSIF_LORA_RANGE_MAX_MHZ;
    if (max_ms == 0 || max_ms > NOCSIF_OOK_MS_MAX) max_ms = NOCSIF_OOK_MS_MAX;

    s_listening = s_scanning = s_surveying = s_hunting = s_carrier = s_capturing = false;

    if (!nocsif_sdcard_lock(3000)) { publish_capst("busy"); return; }
    if (!s_brought_up) bring_up_locked();
    bool up = s_brought_up;
    if (up) { s_radio->standby(); s_radio->setFrequency(freq_mhz); s_radio->setOutputPower(OOK_TX_DBM); }
    nocsif_sdcard_unlock();
    if (!up) { publish_capst("radio down"); return; }

    s_ook_stop   = false;
    s_ook_active = true;
    int64_t start = esp_timer_get_time();
    { char b[40]; snprintf(b, sizeof b, "TX %s", label ? label : "ook"); publish_capst(b); }
    ESP_LOGW(TAG, "OOK TX %s: %d durations x%d @ %.3f MHz %d dBm (dead-man %u ms)",
             label ? label : "?", n, repeats, (double)freq_mhz, OOK_TX_DBM, (unsigned)max_ms);

    for (int r = 0; r < repeats && !s_ook_stop; r++) {
        if ((uint32_t)((esp_timer_get_time() - start) / 1000) >= max_ms) break;
        if (nocsif_sdcard_lock(2000)) {
            int64_t t = esp_timer_get_time();
            ook_emit(dur, n, &t);
            nocsif_sdcard_unlock();
        }
        if (r + 1 < repeats && !s_ook_stop) vTaskDelay(pdMS_TO_TICKS(gap_ms ? gap_ms : 20));
    }

    if (nocsif_sdcard_lock(2000)) {
        if (s_brought_up) { s_radio->standby(); s_radio->setOutputPower(LORA_POWER_DBM); subghz_relora_baseline_locked(); }
        nocsif_sdcard_unlock();
    }
    s_ook_active = false;
    publish_capst(s_ook_stop ? "tx stopped" : "tx done");
    ESP_LOGW(TAG, "OOK TX %s %s", label ? label : "?", s_ook_stop ? "stopped" : "complete");
}

/* ---- .sub parse helpers (RAW timings + fixed-code encoder synthesis) ---- */

/* Case-insensitive substring test (no POSIX strcasestr in newlib C++). */
static bool proto_has(const char *hay, const char *needle)
{
    if (!hay || !needle) return false;
    size_t nl = strlen(needle);
    if (nl == 0) return false;
    for (const char *p = hay; *p; p++) {
        size_t k = 0;
        while (k < nl && p[k] && tolower((unsigned char)p[k]) == tolower((unsigned char)needle[k])) k++;
        if (k == nl) return true;
    }
    return false;
}

/* Parse Flipper "Key: AA BB .." hex bytes (big-endian) into a uint64 (low `Bit` bits are the code). */
static uint64_t sub_parse_key(const char *s)
{
    uint64_t k = 0;
    while (*s) {
        while (*s == ' ' || *s == '\t') s++;
        if (!isxdigit((unsigned char)*s)) break;
        unsigned b = 0;
        if (sscanf(s, "%2x", &b) != 1) break;
        k = (k << 8) | (uint64_t)(b & 0xFF);
        s += 2;
    }
    return k;
}

/* Concatenate all "RAW_Data:" timing tokens from an open .sub into out[max]. Returns the count. */
static int sub_parse_raw(FILE *f, int32_t *out, int max)
{
    int n = 0;
    char line[512];
    rewind(f);
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "RAW_Data:", 9) != 0) continue;
        char *p = line + 9;
        while (n < max) {
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '\0' || *p == '\n' || *p == '\r') break;
            char *end = nullptr;
            long v = strtol(p, &end, 10);
            if (end == p) break;
            out[n++] = (int32_t)v;
            p = end;
        }
    }
    return n;
}

static bool sub_is_encoder(const char *proto)
{
    return proto_has(proto, "Princeton") || proto_has(proto, "CAME") || proto_has(proto, "Nice") ||
           proto_has(proto, "Holtek")    || proto_has(proto, "HS2XX");
}

static int enc_default_te(const char *proto)
{
    if (proto_has(proto, "Nice"))   return 700;   /* Nice FLO */
    if (proto_has(proto, "CAME"))   return 320;
    if (proto_has(proto, "Holtek") || proto_has(proto, "HS2XX")) return 430;
    return 400;                                    /* Princeton / PT2262 */
}

/* Synthesize a fixed-code encoder frame (approximate, on-air-plausible timings) into out[max] as OOK
 * durations. Princeton/PT2262 + Holtek/HS2XX use short/long pulse-pairs + a long guard; CAME + Nice FLO
 * use a leading low sync + PWM bits. `key`'s low `bits` bits are the code, MSB first. Returns count. */
static int enc_synth(const char *proto, uint64_t key, int bits, int te, int32_t *out, int max)
{
    int n = 0;
    if (bits < 1)  bits = 1;
    if (bits > 64) bits = 64;
    bool pt = proto_has(proto, "Princeton") || proto_has(proto, "Holtek") || proto_has(proto, "HS2XX");
    if (pt) {
        for (int i = bits - 1; i >= 0 && n + 2 <= max; i--) {
            int b = (int)((key >> i) & 1ULL);
            out[n++] =  (b ? 3 : 1) * te;
            out[n++] = -((b ? 1 : 3) * te);
        }
        if (n + 2 <= max) { out[n++] = te; out[n++] = -31 * te; }   /* guard / sync gap */
    } else {   /* CAME / Nice FLO: leading low sync, then PWM bits */
        if (n < max) out[n++] = -36 * te;
        for (int i = bits - 1; i >= 0 && n + 2 <= max; i--) {
            int b = (int)((key >> i) & 1ULL);
            out[n++] =  (b ? 2 : 1) * te;
            out[n++] = -((b ? 1 : 2) * te);
        }
    }
    return n;
}

/* ---- de Bruijn B(2,n) OOK brute-force (OpenSesame-style) ---- *
 * Generate the sequence in which every n-bit window of the OOK bitstream appears exactly once (FKM /
 * prefer-lex, O(2^n) time, O(n) stack), into a PSRAM bit buffer; emit one TE-length OOK symbol per bit.
 * Emitted in overlapping chunks (overlap n-1 so the inter-chunk yield can't split a window) so the worker
 * yields to the idle task / WDT between chunks. Dead-man bounded; stops on s_ook_stop. */
static uint8_t *s_db_buf;
static uint32_t s_db_len;
static int      s_db_n;
static uint8_t  s_db_a[NOCSIF_OOK_DEBRUIJN_BITS_MAX + 1];

static void db_gen(int t, int p)
{
    if (s_ook_stop) return;
    if (t > s_db_n) {
        if (s_db_n % p == 0) {
            for (int j = 1; j <= p; j++) {
                uint32_t idx = s_db_len++;
                if (s_db_a[j]) s_db_buf[idx >> 3] |=  (uint8_t)(1u << (idx & 7));
                else           s_db_buf[idx >> 3] &= (uint8_t)~(1u << (idx & 7));
            }
        }
        return;
    }
    s_db_a[t] = s_db_a[t - 1];
    db_gen(t + 1, p);
    for (int j = s_db_a[t - 1] + 1; j <= 1; j++) { s_db_a[t] = (uint8_t)j; db_gen(t + 1, t); }
}

static void do_debruijn(int bits, int te_us, float freq_mhz, uint32_t max_ms)
{
    if (bits < NOCSIF_OOK_DEBRUIJN_BITS_MIN) bits = NOCSIF_OOK_DEBRUIJN_BITS_MIN;
    if (bits > NOCSIF_OOK_DEBRUIJN_BITS_MAX) bits = NOCSIF_OOK_DEBRUIJN_BITS_MAX;
    if (te_us < 100)  te_us = 100;
    if (te_us > 2000) te_us = 2000;
    if (freq_mhz < NOCSIF_LORA_RANGE_MIN_MHZ) freq_mhz = NOCSIF_LORA_RANGE_MIN_MHZ;
    if (freq_mhz > NOCSIF_LORA_RANGE_MAX_MHZ) freq_mhz = NOCSIF_LORA_RANGE_MAX_MHZ;
    if (max_ms == 0 || max_ms > NOCSIF_OOK_MS_MAX) max_ms = NOCSIF_OOK_MS_MAX;
    if (!ook_ensure_buf()) { publish_capst("no mem"); return; }

    uint32_t total = 1u << bits;
    size_t   bytes = (total + 7) / 8;
    s_db_buf = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!s_db_buf) { publish_capst("no mem"); return; }

    s_listening = s_scanning = s_surveying = s_hunting = s_carrier = s_capturing = false;
    s_ook_stop = false;
    s_db_len = 0; s_db_n = bits;
    memset(s_db_a, 0, sizeof s_db_a);
    db_gen(1, 1);                       /* fill s_db_buf with B(2,bits); s_db_len == total afterwards */

    if (!nocsif_sdcard_lock(3000)) { heap_caps_free(s_db_buf); s_db_buf = nullptr; publish_capst("busy"); return; }
    if (!s_brought_up) bring_up_locked();
    bool up = s_brought_up;
    if (up) { s_radio->standby(); s_radio->setFrequency(freq_mhz); s_radio->setOutputPower(OOK_TX_DBM); }
    nocsif_sdcard_unlock();
    if (!up) { heap_caps_free(s_db_buf); s_db_buf = nullptr; publish_capst("radio down"); return; }

    s_ook_active = true;
    int64_t start = esp_timer_get_time();
    { char b[40]; snprintf(b, sizeof b, "brute %d-bit", bits); publish_capst(b); }
    ESP_LOGW(TAG, "OOK de Bruijn %d-bit TE %dus @ %.3f MHz (%lu symbols, dead-man %u ms)",
             bits, te_us, (double)freq_mhz, (unsigned long)total, (unsigned)max_ms);

    const int CHUNK  = 512;            /* <= NOCSIF_OOK_DUR_MAX; ~CHUNK*TE per locked busy-wait */
    int       overlap = bits - 1;
    for (uint32_t i = 0; i < total && !s_ook_stop; ) {
        if ((uint32_t)((esp_timer_get_time() - start) / 1000) >= max_ms) break;
        uint32_t end = i + CHUNK; if (end > total) end = total;
        int cnt = 0;
        for (uint32_t k = i; k < end; k++) {
            int bit = (s_db_buf[k >> 3] >> (k & 7)) & 1;
            s_ook_dur[cnt++] = bit ? te_us : -te_us;
        }
        if (nocsif_sdcard_lock(2000)) {
            int64_t t = esp_timer_get_time();
            ook_emit(s_ook_dur, cnt, &t);
            nocsif_sdcard_unlock();
        }
        if (end >= total) break;
        i = (end > (uint32_t)overlap) ? end - overlap : end;   /* overlap so a window isn't split by the gap */
        vTaskDelay(1);                                          /* yield to idle/WDT between chunks */
    }

    if (nocsif_sdcard_lock(2000)) {
        if (s_brought_up) { s_radio->standby(); s_radio->setOutputPower(LORA_POWER_DBM); subghz_relora_baseline_locked(); }
        nocsif_sdcard_unlock();
    }
    heap_caps_free(s_db_buf); s_db_buf = nullptr;
    s_ook_active = false;
    publish_capst(s_ook_stop ? "brute stopped" : "brute done");
    ESP_LOGW(TAG, "OOK de Bruijn %s", s_ook_stop ? "stopped" : "complete");
}

/* Auto-routed transmit of a saved .sub: read the header, pick the emit path. RAW / a known fixed-code
 * encoder / a NocSifDeBruijn generator go OOK; a captured NocSif packet file replays through the packet
 * engine. Reads the file under a claim+lock (mirrors do_cap_load), then hands off to the emit path (which
 * re-locks per frame). EMITS — the UI arms + ISM-gates before calling. */
static void do_tx_file(const char *path, uint32_t max_ms)
{
    if (!path || !path[0]) { publish_capst("no path"); return; }
    if (!ook_ensure_buf()) { publish_capst("no mem"); return; }

    esp_err_t ce = nocsif_usb_gadget_claim_sd(2000);
    if (ce != ESP_OK) { publish_capst(ce == ESP_ERR_INVALID_STATE ? "File Share has SD" : "no SD"); return; }

    char     proto[24] = {0};
    float    freq = 0.0f;
    int      te = 0, bitn = 0;
    uint64_t key = 0;
    bool     have_raw = false, have_frame = false;
    int      ndur = 0;

    if (nocsif_sdcard_lock(3000)) {
        FILE *f = fopen(path, "rb");
        if (f) {
            char line[512];
            unsigned long uv;
            while (fgets(line, sizeof line, f)) {
                if      (!strncmp(line, "Protocol:", 9))  sscanf(line + 9, " %23s", proto);
                else if (!strncmp(line, "Frequency:", 10) && sscanf(line + 10, "%lu", &uv) == 1) freq = uv / 1e6f;
                else if (!strncmp(line, "TE:", 3))         sscanf(line + 3, "%d", &te);
                else if (!strncmp(line, "Bit:", 4))        sscanf(line + 4, "%d", &bitn);
                else if (!strncmp(line, "Key:", 4))        key = sub_parse_key(line + 4);
                else if (!strncmp(line, "RAW_Data:", 9))   have_raw = true;
                else if (!strncmp(line, "F:", 2))          have_frame = true;
            }
            if (have_raw) ndur = sub_parse_raw(f, s_ook_dur, NOCSIF_OOK_DUR_MAX);   /* rewinds internally */
            fclose(f);
        }
        nocsif_sdcard_unlock();
    }
    nocsif_usb_gadget_release_sd();

    float use_freq = (freq > 0.0f) ? freq : 433.92f;

    if (have_raw && ndur > 0) {
        ook_run(s_ook_dur, ndur, use_freq, 3, 25, max_ms, "raw");
        return;
    }
    if (proto[0] && sub_is_encoder(proto)) {
        if (te   <= 0) te   = enc_default_te(proto);
        if (bitn <= 0) bitn = 24;
        int n = enc_synth(proto, key, bitn, te, s_ook_dur, NOCSIF_OOK_DUR_MAX);
        ook_run(s_ook_dur, n, use_freq, 5, 20, max_ms, proto);
        return;
    }
    if (proto[0] && proto_has(proto, "DeBruijn")) {
        do_debruijn(bitn > 0 ? bitn : 12, te > 0 ? te : 400, use_freq, max_ms);
        return;
    }
    if (have_frame) {                 /* captured NocSif LoRa/FSK packet -> packet-engine replay */
        do_cap_load(path);
        do_cap_replay(0);
        return;
    }
    publish_capst("no tx route");
    ESP_LOGW(TAG, "subghz tx: %s has no recognized transmit route (proto '%s')", path, proto);
}

/* Signal-hunt self-test: park on 915 MHz and read the RSSI a few times, logging the envelope. Passive
 * RX only — no emission. Confirms the hunt engine parks + reads + builds an envelope without crashing. */
static void do_hunt_selftest(void)
{
    ESP_LOGW(TAG, "==== LoRa SIGNAL-HUNT SELF-TEST (node %08lX) ====", (unsigned long)s_node_id);
    do_hunt(LORA_FREQ_MHZ);
    for (int k = 0; k < 20 && s_hunting; k++) {
        hunt_poll();
        vTaskDelay(pdMS_TO_TICKS(LORA_HUNT_POLL_MS));
    }
    nocsif_lora_hunt_t h;
    bool have = nocsif_lora_hunt_snapshot(&h);
    if (have) {
        ESP_LOGW(TAG, "  hunting %.2f MHz (BW %.0f kHz): envelope %d dBm, peak %d dBm, %lu frames",
                 (double)h.freq_mhz, (double)LORA_HUNT_BW_KHZ, h.smoothed, h.peak,
                 (unsigned long)h.frames);
        bool sane = (h.smoothed < 0 && h.smoothed > -140 && h.frames > 0);
        ESP_LOGW(TAG, "---- VERDICT: hunt %s (RSSI envelope %s) ----",
                 sane ? "OK" : "SUSPECT", sane ? "reads" : "bad");
    } else {
        ESP_LOGW(TAG, "---- VERDICT: no hunt reading (SUSPECT — check bring-up) ----");
    }
    do_hunt_stop();
    ESP_LOGW(TAG, "======================================================================");
}

/* Slice-2b self-test: bring-up + send a formatted text frame (verify TX) +
 * arm RX + read the RSSI floor (verify listen arming). Actual decode needs
 * a peer. */
static void do_selftest(void)
{
    publish_status("test");
    publish_readout("LoRa messaging self-test\xE2\x80\xA6");

    char hello[64];
    snprintf(hello, sizeof hello, "NocSif %08lX: M9 slice-2b hello", (unsigned long)s_node_id);
    do_send(hello);

    ESP_LOGW(TAG, "==== LoRa MESSAGING SELF-TEST (node %08lX) ====", (unsigned long)s_node_id);
    ESP_LOGW(TAG, "  sent a text frame (see 'tx msg' above). Arming RX to read the channel floor...");

    do_listen(true);
    vTaskDelay(pdMS_TO_TICKS(1500));           /* listen briefly */
    int rssi_floor = 0;
    if (s_listening && nocsif_sdcard_lock(1000)) {
        rssi_floor = (int)s_radio->getRSSI();
        nocsif_sdcard_unlock();
    }
    ESP_LOGW(TAG, "  RX armed=%d, channel RSSI floor ~%d dBm, inbox=%d (decode needs a 2nd node).",
             s_listening, rssi_floor, nocsif_lora_inbox_count());
    do_listen(false);
    ESP_LOGW(TAG, "---- VERDICT: send %s, RX arming %s ----",
             s_tx_count > 0 ? "OK" : "FAILED", s_listening ? "left ON?" : "OK");
    ESP_LOGW(TAG, "======================================================================");
}

/* Full teardown (Signal-Hunt radio hand-off): stops all activity, sleeps
 * the SX1262, drops the ALDO3 rail, and flags the worker loop to exit so
 * its ~8 KB stack + the command queue are freed — WiFi needs that RAM to
 * re-init after LoRa borrowed it. Runs on the worker (it owns the radio +
 * the shared SPI3 bus). s_radio/s_hal/s_mod are kept (small heap objects);
 * the next bring-up powers the rail back + re-begins. */
static void do_deinit(void)
{
    s_hunting = false; s_surveying = false; s_scanning = false; s_listening = false; s_carrier = false;
    s_capturing = false;
    s_ook_stop = true; s_ook_active = false;
    if (s_cap) { heap_caps_free(s_cap); s_cap = nullptr; s_cap_head = 0; s_cap_count = 0; }
    if (s_cap_trace) { heap_caps_free(s_cap_trace); s_cap_trace = nullptr; s_cap_trace_n = 0; }
    if (s_ook_dur) { heap_caps_free(s_ook_dur); s_ook_dur = nullptr; }
    if (s_brought_up) {
        if (nocsif_sdcard_lock(2000)) {   /* SPI3 is shared with the SD card */
            if (s_radio) s_radio->sleep();
            nocsif_sdcard_unlock();
        }
        nocsif_power_lora_rail(false);    /* ALDO3 off */
        s_brought_up = false;
        s_available  = false;
    }
    publish_status("off");
    publish_readout("LoRa off (radio handed back).");
    s_want_deinit = true;                 /* the loop breaks after this dispatch, then self-deletes */
    ESP_LOGI(TAG, "LoRa teardown: radio slept, rail off — freeing the worker");
}

/* Worker task: services commands from the queue, and between commands
 * drives whichever mode is active (hunt / survey / scan / listen) at its
 * own cadence, blocking indefinitely when idle. */
static void lora_task(void *arg)
{
    (void)arg;
    lora_cmd_t c;
    for (;;) {
        /* Hunt: poll RSSI fast; carrier: enforce the dead-man; survey/scan: one sweep then pause;
         * listen: poll DIO1; else block. */
        TickType_t wait;
        if (s_hunting)        { hunt_poll();      wait = pdMS_TO_TICKS(LORA_HUNT_POLL_MS); }
        else if (s_carrier)   { carrier_tick();   wait = pdMS_TO_TICKS(s_cw_sweep ? 20 : 100); }
        else if (s_surveying) { survey_sweep();   wait = pdMS_TO_TICKS(200); }
        else if (s_scanning)  { activity_sweep(); wait = pdMS_TO_TICKS(250); }
        else if (s_capturing) { cap_poll();       wait = pdMS_TO_TICKS(60);  }
        else if (s_listening) { rx_poll();        wait = pdMS_TO_TICKS(25);  }
        else                    wait = portMAX_DELAY;
        if (xQueueReceive(s_cmd_q, &c, wait) == pdTRUE) {
            switch (c.type) {
                case CMD_SELFTEST:     do_selftest();      break;
                case CMD_SEND:         do_send(c.text);    break;
                case CMD_LISTEN_ON:    do_listen(true);    break;
                case CMD_LISTEN_OFF:   do_listen(false);   break;
                case CMD_ACTIVITY_ON:  do_activity(true);  break;
                case CMD_ACTIVITY_OFF: do_activity(false); break;
                case CMD_ACTIVITY_SELFTEST: do_activity_selftest(); break;
                case CMD_SURVEY_ON:    do_survey(true);    break;
                case CMD_SURVEY_OFF:   do_survey(false);   break;
                case CMD_SURVEY_RESET: do_survey_reset();  break;
                case CMD_SURVEY_RANGE: do_survey_range(c.fval, c.fval2); break;
                case CMD_SURVEY_SELFTEST: do_survey_selftest(); break;
                case CMD_HUNT_ON:      do_hunt(c.fval);    break;
                case CMD_HUNT_OFF:     do_hunt_stop();     break;
                case CMD_HUNT_SELFTEST: do_hunt_selftest(); break;
                case CMD_CARRIER_ON:   do_carrier(c.fval, c.ival, c.uval); break;
                case CMD_CARRIER_SWEEP_ON: do_carrier_sweep(c.fval, c.fval2, c.fval3, c.uval, c.bval != 0, c.ival, c.uval2); break;
                case CMD_CARRIER_OFF:  do_carrier_stop();  break;
                case CMD_CAP_ON:       do_capture(true);   break;
                case CMD_CAP_OFF:      do_capture(false);  break;
                case CMD_CAP_RESUME:   do_cap_resume();    break;
                case CMD_CAP_END:      do_cap_end();       break;
                case CMD_CAP_SAVE:     do_cap_save(c.text[0] ? c.text : nullptr); break;
                case CMD_CAP_LOAD:     do_cap_load(c.text); break;
                case CMD_CAP_REPLAY:   do_cap_replay(c.ival); break;
                case CMD_TX_FILE:      do_tx_file(c.text, c.uval); break;
                case CMD_DEBRUIJN:     do_debruijn(c.ival, (int)c.uval2, c.fval, c.uval); break;
                case CMD_TX_STOP:      s_ook_stop = true; break;
                case CMD_OOK_TX:       ook_run(s_ook_dur, s_ook_n, s_ook_freq, s_ook_reps, 20, s_ook_max, "ook"); break;
                case CMD_DEINIT:       do_deinit();        break;
            }
        }
        if (s_want_deinit) break;   /* teardown requested -> exit + self-delete below */
    }
    /* Free the queue + null the handles, then self-delete. vTaskDeleteWithCaps
     * (paired with the xTaskCreateWithCaps above) frees the PSRAM stack +
     * TCB — a plain vTaskDelete would leak them. */
    QueueHandle_t q = s_cmd_q;
    s_cmd_q = nullptr;
    s_task  = nullptr;
    s_want_deinit = false;
    if (q) vQueueDeleteWithCaps(q);   /* paired with xQueueCreateWithCaps (PSRAM queue storage) */
    ESP_LOGI(TAG, "lora worker torn down (task + queue freed)");
    vTaskDeleteWithCaps(nullptr);
}

static void post_cmd(lora_cmd_type_t type, const char *text)
{
    if (s_cmd_q == nullptr) return;
    lora_cmd_t c = {};
    c.type = type;
    if (text) snprintf(c.text, sizeof c.text, "%s", text);
    xQueueSend(s_cmd_q, &c, 0);   /* non-blocking; drops if the (depth-4) queue is full */
}

extern "C" void nocsif_lora_send_text(const char *text)      { if (text && text[0]) post_cmd(CMD_SEND, text); }
extern "C" void nocsif_lora_set_listen(bool on)              { post_cmd(on ? CMD_LISTEN_ON : CMD_LISTEN_OFF, nullptr); }
extern "C" void nocsif_lora_set_activity(bool on)            { post_cmd(on ? CMD_ACTIVITY_ON : CMD_ACTIVITY_OFF, nullptr); }
extern "C" void nocsif_lora_set_survey(bool on)              { post_cmd(on ? CMD_SURVEY_ON : CMD_SURVEY_OFF, nullptr); }
extern "C" void nocsif_lora_survey_reset(void)               { post_cmd(CMD_SURVEY_RESET, nullptr); }
extern "C" void nocsif_lora_hunt_stop(void)                  { post_cmd(CMD_HUNT_OFF, nullptr); }
extern "C" void nocsif_lora_set_hunt(float mhz)
{
    if (s_cmd_q == nullptr) return;
    lora_cmd_t c = {};
    c.type = CMD_HUNT_ON;
    c.fval = mhz;
    xQueueSend(s_cmd_q, &c, 0);
}
extern "C" void nocsif_lora_carrier_stop(void)              { post_cmd(CMD_CARRIER_OFF, nullptr); }
extern "C" void nocsif_lora_carrier_start(float mhz, int dbm, uint32_t max_ms)
{
    if (s_cmd_q == nullptr) return;
    lora_cmd_t c = {};
    c.type = CMD_CARRIER_ON;
    c.fval = mhz;
    c.ival = dbm;
    c.uval = max_ms;
    xQueueSend(s_cmd_q, &c, 0);
}
extern "C" void nocsif_lora_carrier_sweep_start(float lo, float hi, float step_mhz, uint32_t dwell_ms,
                                                bool pingpong, int dbm, uint32_t max_ms)
{
    if (s_cmd_q == nullptr) return;
    lora_cmd_t c = {};
    c.type  = CMD_CARRIER_SWEEP_ON;
    c.fval  = lo;
    c.fval2 = hi;
    c.fval3 = step_mhz;
    c.ival  = dbm;
    c.uval  = dwell_ms;
    c.uval2 = max_ms;
    c.bval  = pingpong ? 1 : 0;
    xQueueSend(s_cmd_q, &c, 0);
}
extern "C" void nocsif_subghz_capture_start(void)            { post_cmd(CMD_CAP_ON, nullptr); }
extern "C" void nocsif_subghz_capture_stop(void)             { post_cmd(CMD_CAP_OFF, nullptr); }
extern "C" void nocsif_subghz_capture_resume(void)           { post_cmd(CMD_CAP_RESUME, nullptr); }
extern "C" void nocsif_subghz_capture_end(void)              { post_cmd(CMD_CAP_END, nullptr); }
extern "C" void nocsif_subghz_capture_save(const char *name) { post_cmd(CMD_CAP_SAVE, name); }
extern "C" void nocsif_subghz_capture_load(const char *path) { post_cmd(CMD_CAP_LOAD, path); }
extern "C" void nocsif_subghz_replay(int i)
{
    if (s_cmd_q == nullptr) return;
    lora_cmd_t c = {};
    c.type = CMD_CAP_REPLAY;
    c.ival = i;
    xQueueSend(s_cmd_q, &c, 0);
}
extern "C" void nocsif_subghz_tx_file(const char *path, uint32_t max_ms)
{
    if (s_cmd_q == nullptr || !path || !path[0]) return;
    lora_cmd_t c = {};
    c.type = CMD_TX_FILE;
    snprintf(c.text, sizeof c.text, "%s", path);
    c.uval = max_ms;
    xQueueSend(s_cmd_q, &c, 0);
}
extern "C" void nocsif_subghz_debruijn_tx(int bits, int te_us, float freq_mhz, uint32_t max_ms)
{
    if (s_cmd_q == nullptr) return;
    lora_cmd_t c = {};
    c.type  = CMD_DEBRUIJN;
    c.ival  = bits;
    c.uval2 = (uint32_t)te_us;
    c.fval  = freq_mhz;
    c.uval  = max_ms;
    xQueueSend(s_cmd_q, &c, 0);
}
/* Stop immediately: set the volatile flag DIRECTLY (the worker is busy inside the emit loop and cannot
 * process a queued command until it returns) — the busy-wait checks it each edge. The queued CMD_TX_STOP
 * is just a harmless post-emit no-op. */
extern "C" void nocsif_subghz_tx_stop(void) { s_ook_stop = true; post_cmd(CMD_TX_STOP, nullptr); }

/* LVGL-parsed transmit: the caller (LVGL task, internal stack → SD-safe) parsed the .sub and built the
 * OOK durations; copy them into the engine buffer and post a RADIO-ONLY transmit (the worker never reads
 * the card, so it can't hit the PSRAM-stack SD-bounce panic). */
extern "C" void nocsif_subghz_ook_tx(const int32_t *dur, int n, float freq_mhz, int repeats, uint32_t max_ms)
{
    if (s_cmd_q == nullptr || !dur || n <= 0 || s_ook_active) return;
    if (!ook_ensure_buf()) { publish_capst("no mem"); return; }
    if (n > NOCSIF_OOK_DUR_MAX) n = NOCSIF_OOK_DUR_MAX;
    memcpy(s_ook_dur, dur, (size_t)n * sizeof(int32_t));
    s_ook_n    = n;
    s_ook_freq = freq_mhz;
    s_ook_reps = repeats > 0 ? repeats : 1;
    s_ook_max  = max_ms;
    post_cmd(CMD_OOK_TX, nullptr);
}

extern "C" int nocsif_subghz_encoder_synth(const char *proto, uint64_t key, int bits, int te, int32_t *out, int max)
{
    if (!proto || !out || max <= 0) return 0;
    return enc_synth(proto, key, bits, te, out, max);
}

extern "C" void nocsif_subghz_ring_begin(const nocsif_subghz_preset_t *preset)
{
    if (!subghz_ensure_bufs()) { publish_capst("no mem"); return; }
    portENTER_CRITICAL(&s_cap_mux);
    s_cap_head = 0; s_cap_count = 0;
    portEXIT_CRITICAL(&s_cap_mux);
    s_cap_seq     = 0;
    s_cap_trace_n = 0;
    if (preset) s_cap_preset = *preset;
}

extern "C" void nocsif_subghz_ring_push(const uint8_t *data, int len, int rssi, int snr)
{
    if (!data || len <= 0) return;
    cap_push(data, (uint16_t)len, rssi, snr);
}

extern "C" void nocsif_subghz_get_cap_preset(nocsif_subghz_preset_t *out) { if (out) *out = s_cap_preset; }
extern "C" void nocsif_subghz_status_set(const char *s) { if (s) publish_capst(s); }

extern "C" bool nocsif_subghz_cap_get_full(int i, uint8_t *data, int max_len, uint16_t *len,
                                           int *rssi, int *snr, uint32_t *idx)
{
    bool ok = false;
    portENTER_CRITICAL(&s_cap_mux);
    if (s_cap && i >= 0 && i < s_cap_count) {
        int slot = (s_cap_head - 1 - i + 2 * NOCSIF_SUBGHZ_CAP_MAX) % NOCSIF_SUBGHZ_CAP_MAX;
        const subghz_frame_t *f = &s_cap[slot];
        uint16_t l = f->len;
        if (max_len > 0 && l > (uint16_t)max_len) l = (uint16_t)max_len;
        if (data && l) memcpy(data, f->data, l);
        if (len)  *len  = l;
        if (rssi) *rssi = f->rssi;
        if (snr)  *snr  = f->snr;
        if (idx)  *idx  = f->idx;
        ok = true;
    }
    portEXIT_CRITICAL(&s_cap_mux);
    return ok;
}
extern "C" void nocsif_lora_request_selftest(void)           { post_cmd(CMD_SELFTEST, nullptr); }
extern "C" void nocsif_lora_request_activity_selftest(void)  { post_cmd(CMD_ACTIVITY_SELFTEST, nullptr); }
extern "C" void nocsif_lora_request_survey_selftest(void)    { post_cmd(CMD_SURVEY_SELFTEST, nullptr); }
extern "C" void nocsif_lora_request_hunt_selftest(void)      { post_cmd(CMD_HUNT_SELFTEST, nullptr); }

extern "C" esp_err_t nocsif_lora_init(void)
{
    if (s_task != nullptr) {
        return ESP_OK;   /* idempotent */
    }
    /* Node id = low 4 bytes of the factory MAC (stable per device). */
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        s_node_id = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) |
                    ((uint32_t)mac[4] << 8) | mac[5];
    }

    lora_omit_load();   /* the persisted survey omit list, before any sweep can consult it */

    if (nocsif_reliability_safe_mode()) {
        publish_status("off");
        publish_readout("LoRa disabled (safe mode).");
        ESP_LOGW(TAG, "safe mode — LoRa bring-up skipped");
        return ESP_OK;
    }
    publish_status("idle");
    publish_readout("LoRa idle. Send a message or listen.");
    publish_capst("idle");

    /* Queue storage in PSRAM: the depth-4 queue of lora_cmd_t (~1.1 KB, the
     * text[] payload dominates) was the LoRa worker's single biggest
     * internal-DMA consumer. It is only ever touched from tasks (worker
     * receive; UI/other-task sends) — never an ISR, never with the flash
     * cache disabled — so it is safe in external RAM. This keeps the scarce
     * int-DMA pool for what truly needs it: during a LoRa survey with BLE
     * resident + WiFi up, largest int-DMA was collapsing to ~80 B; moving
     * the queue recovers ~1.1 KB of that. Paired with vQueueDeleteWithCaps below. */
    s_cmd_q = xQueueCreateWithCaps(4, sizeof(lora_cmd_t), MALLOC_CAP_SPIRAM);
    if (s_cmd_q == nullptr) {
        ESP_LOGE(TAG, "failed to create lora command queue");
        return ESP_ERR_NO_MEM;
    }
    /* 12 KB stack, in PSRAM (RAM-BUDGET.md remake #8, C12): the worker talks to the SX1262 over SPI3
     * with the driver's OWN DMA buffers and never DMAs from its stack, nor runs while the flash cache
     * is disabled, so its stack does not belong in the scarce internal-DMA pool. Moving it off internal
     * DELETES the LORA_TASK_MIN_DMA gate + the "turn Bluetooth off to hunt LoRa" wall + the spawn-fails-
     * under-fragmentation class — LoRa now fits alongside the resident BLE controller + WiFi, always.
     * Requires CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y; MUST be torn down with vTaskDeleteWithCaps.
     * Sizing: RadioLib call depth + on-stack frame/inbox buffers + the survey sweep's ~1.3 KB of locals
     * (found[]/cur[]/tmp[] over 52 bins) + the .sub writer's ~600 B line buffer over the FatFs f_write /
     * LFN path + (Phase 3) RAW parse / OOK transmit. Bumped 8 KB -> 12 KB for that FatFs+file headroom
     * (PSRAM only — costs nothing in the int-DMA pool). Every number formatted as an integer (no %f). */
    if (xTaskCreateWithCaps(lora_task, "lora", 12288, nullptr, 3, &s_task, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "failed to create lora worker task");
        vQueueDeleteWithCaps(s_cmd_q);   /* paired with xQueueCreateWithCaps (PSRAM queue storage) */
        s_cmd_q = nullptr;
        s_task = nullptr;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "lora worker ready (RadioLib SX1262, node %08lX; lazy bring-up on first cmd)",
             (unsigned long)s_node_id);
    return ESP_OK;
}

/* Tears the LoRa worker down: sleeps the radio, drops the ALDO3 rail,
 * deletes the worker task + its queue. Signal Hunt keeps the worker warm
 * across radio switches now (the PSRAM stack no longer competes for the
 * internal-DMA pool), so this only runs on an actual feature exit. Blocks
 * (bounded, ~<1 s) for a clean self-delete so a caller that immediately
 * re-inits doesn't race the old task. Idempotent — no-op if the worker
 * isn't running. Must not be called from the worker task itself.
 *
 * There is no busy-wait or settle delay here: the stack lives in PSRAM, so
 * there is no internal-DMA to hand back to WiFi and nothing to wait for
 * beyond the worker finishing its own radio-off teardown. */
extern "C" void nocsif_lora_deinit(void)
{
    if (s_task == nullptr) {
        return;
    }
    if (s_cmd_q) {
        lora_cmd_t c;
        c.type = CMD_DEINIT;
        c.text[0] = '\0';
        c.fval = 0.0f;
        xQueueSend(s_cmd_q, &c, pdMS_TO_TICKS(200));
    }
    for (int i = 0; i < 50 && s_task != nullptr; i++) {   /* ~<1 s for the worker to self-delete cleanly */
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
