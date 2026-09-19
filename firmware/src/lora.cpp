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

/* Copies out inbox message i (0 = newest). Returns false if out of range. LVGL-safe. */
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
    /* 8192 B stack, in PSRAM: the worker talks to the SX1262 over SPI3 with
     * the driver's own DMA buffers and never DMAs from its stack, nor runs
     * while the flash cache is disabled, so its stack does not belong in
     * the scarce internal-DMA pool. Moving it off internal removes the
     * spawn-fails-under-fragmentation class — LoRa now fits alongside the
     * resident BLE controller + WiFi, always. Requires
     * CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y; must be torn down with
     * vTaskDeleteWithCaps. Stack sizing unchanged: RadioLib call depth +
     * on-stack frame/inbox buffers + the survey sweep's ~1.3 KB of locals
     * (found[]/cur[]/tmp[] over 52 bins) + snprintf. */
    if (xTaskCreateWithCaps(lora_task, "lora", 8192, nullptr, 3, &s_task, MALLOC_CAP_SPIRAM) != pdPASS) {
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
