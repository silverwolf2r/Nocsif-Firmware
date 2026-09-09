/*
 * NocSif — GNSS (u-blox MIA-M10Q / LS550G) worker (M8, slice: proof-of-life). See gnss.h.
 *
 * Pure UART liveness probe: power BLDO1, bring up UART1 (RX=44/TX=43), sweep
 * bauds x pin orders, and listen for NMEA ('$G..'/'$P..') or UBX (0xB5 0x62)
 * framing. No fix is attempted (needs sky view) — this only answers "is the
 * module transmitting?".
 *
 * The UART is a dedicated bus (not the shared SPI3), so unlike lora.c/nfc.cpp there is NO SD lock.
 */
#include "gnss.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>      /* mkdir, stat (GPX file on /sd) */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "power.h"          /* nocsif_power_gnss_rail (BLDO1) */
#include "reliability.h"    /* nocsif_reliability_safe_mode */
#include "freertos/idf_additions.h" /* xTaskCreateWithCaps — PSRAM worker stack */
#include "esp_heap_caps.h"          /* MALLOC_CAP_SPIRAM */
#include "esp_memory_utils.h"       /* esp_ptr_external_ram — PSRAM-stack placement probe */
#include "weather.h"        /* feed each valid fix to Weather (auto-follow + geofence) */
#include "sdcard.h"         /* nocsif_sdcard_lock/unlock — shared SPI3 (GPX log) */
#include "usb_gadget.h"     /* nocsif_usb_gadget_claim_sd — own /sd away from USB-MSC while writing */
#include "wifi.h"           /* wardrive: passive monitor AP table + control (WiFi+GPS fusion) */

static const char *TAG = "gnss";

/* ---- hardware (docs/HARDWARE.md + LilyGoWatchUltra.cpp powerControl(POWER_GPS)) ------ */
#define GNSS_UART_PORT   UART_NUM_1
#define GNSS_PIN_RX      44        /* ESP RX  <- module TX */
#define GNSS_PIN_TX      43        /* ESP TX  -> module RX */
#define GNSS_PIN_PPS     13        /* pulse-per-second (module output; pulses only once locked) */
#define GNSS_RX_BUF      2048
#define GNSS_WARMUP_MS   1000      /* let the module cold-start before its UART is active */
#define GNSS_LISTEN_MS   1200      /* per baud/pin attempt */

/* Candidate line rates: 38400 = LilyGo's u-blox default; 115200 = LS550G; 9600 = classic u-blox. */
static const int k_bauds[] = { 38400, 115200, 9600 };

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
static TaskHandle_t   s_task;
static bool           s_installed;    /* UART driver installed */
static bool           s_brought_up;   /* rail on + UART up (selftest path) */
static bool           s_powered;      /* BLDO1 currently enabled */
static volatile bool  s_available;    /* module seen transmitting */

/* ---- live-fix control (LVGL-safe API flips *_want; the worker owns *_on) ----------- */
static volatile bool  s_live_want;        /* requested live-streaming state */
static volatile bool  s_hold_want;        /* Governor P2: background fix-cycle hold (no screen) */
static volatile bool  s_live_on;          /* actual live-streaming state */
static volatile bool  s_selftest_pending; /* a proof-of-life run was requested */

/* ---- parsed live fix (spinlock-guarded; worker writes, LVGL getter copies out) ----- */
static nocsif_gnss_fix_t s_fix;                                   /* published snapshot */
static nocsif_gnss_fix_t s_wf;                                    /* worker accumulator */
static portMUX_TYPE      s_fix_mux = portMUX_INITIALIZER_UNLOCKED;
static char              s_line[128];      /* NMEA line assembler (worker-owned) */
static int               s_line_len;
static int64_t           s_last_sentence_us;   /* liveness: last parsed sentence */
static int64_t           s_last_fix_us;        /* last valid position */
static volatile bool     s_have_sentence;      /* >=1 sentence parsed this session */
static volatile bool     s_have_fix;           /* >=1 valid position this session */
static struct { char t[2]; uint8_t nsv; } s_gsv[8];   /* per-talker GSV numSV (in-view sum) */
static int               s_gsv_n;

bool nocsif_gnss_available(void) { return s_available; }
bool nocsif_gnss_live(void)      { return s_live_on; }
const char *nocsif_gnss_status_str(void)  { return s_status[s_status_i]; }
const char *nocsif_gnss_readout_str(void) { return s_readout[s_readout_i]; }

bool nocsif_gnss_fix_snapshot(nocsif_gnss_fix_t *out)
{
    if (!out) return false;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_fix_mux);
    *out = s_fix;
    portEXIT_CRITICAL(&s_fix_mux);
    out->stream_age_ms = s_have_sentence ? (uint32_t)((now - s_last_sentence_us) / 1000) : UINT32_MAX;
    out->fix_age_ms    = s_have_fix      ? (uint32_t)((now - s_last_fix_us) / 1000)      : UINT32_MAX;
    return s_have_sentence;
}

/* ---- GPX track logging (worker owns the FILE*; forces live streaming while active) -- */
#define GPX_MIN_PERIOD_MS  1500     /* rate cap: never log points faster than this */
#define GPX_MAX_PERIOD_MS  15000    /* heartbeat: log even when stationary after this */
#define GPX_MIN_MOVE_M     3.0f     /* must move at least this far to log a point (unless heartbeat) */
#define GPX_FOOTER         "</trkseg></trk></gpx>\n"
#define DEG2RAD            0.017453292519943295

static volatile bool     s_gpx_want;        /* requested logging state (LVGL-safe flag) */
static volatile bool     s_gpx_on;           /* file open + logging */
static FILE             *s_gpx_f;
static long              s_gpx_body_off;      /* file offset of the footer (rewritten each point) */
static uint32_t          s_gpx_points;
static float             s_gpx_dist_m;
static int64_t           s_gpx_start_us;
static int64_t           s_gpx_last_us;       /* time of the last logged point */
static double            s_gpx_last_lat, s_gpx_last_lon;
static bool              s_gpx_have_last;
static char              s_gpx_path[48];
static nocsif_gnss_gpx_state_t s_gpx_state = NOCSIF_GPX_OFF;
static nocsif_gnss_gpx_t s_gpx;              /* published stats (spinlock-guarded) */
static portMUX_TYPE      s_gpx_mux = portMUX_INITIALIZER_UNLOCKED;

bool nocsif_gnss_gpx_logging(void) { return s_gpx_on; }

bool nocsif_gnss_gpx_snapshot(nocsif_gnss_gpx_t *out)
{
    if (!out) return false;
    portENTER_CRITICAL(&s_gpx_mux);
    *out = s_gpx;
    portEXIT_CRITICAL(&s_gpx_mux);
    if (s_gpx_on) out->dur_s = (uint32_t)((esp_timer_get_time() - s_gpx_start_us) / 1000000);
    return true;
}

/* ---- Wardrive (geotagged WiFi survey; worker owns the CSV + drives the WiFi monitor) ---- */
#define WD_PERIOD_MS  1500      /* scan-and-log cadence */
#define WD_SEEN_MAX   512       /* dedup cap: unique BSSIDs logged once each */

static volatile bool     s_wd_want;
static volatile bool     s_wd_on;
static FILE             *s_wd_f;
static uint32_t          s_wd_networks;
static int64_t           s_wd_start_us;
static int64_t           s_wd_last_us;        /* last scan-and-log run */
static char              s_wd_path[52];
static uint8_t           s_wd_seen[WD_SEEN_MAX][6];
static int               s_wd_seen_n;
static nocsif_gnss_wardrive_state_t s_wd_state = NOCSIF_WD_OFF;
static nocsif_gnss_wardrive_t s_wd;
static portMUX_TYPE      s_wd_mux = portMUX_INITIALIZER_UNLOCKED;

bool nocsif_gnss_wardrive_logging(void) { return s_wd_on; }

bool nocsif_gnss_wardrive_snapshot(nocsif_gnss_wardrive_t *out)
{
    if (!out) return false;
    portENTER_CRITICAL(&s_wd_mux);
    *out = s_wd;
    portEXIT_CRITICAL(&s_wd_mux);
    if (s_wd_on) out->dur_s = (uint32_t)((esp_timer_get_time() - s_wd_start_us) / 1000000);
    return true;
}

/* Points UART1 at (rx,tx)+baud and listens for GNSS_LISTEN_MS. Returns raw
 * bytes read; sets *nmea/*ubx when framing is seen; keeps a short printable
 * sample. Exits early once framing is confirmed. */
static int listen_once(int rx, int tx, int baud, bool *nmea, bool *ubx, char *sample, size_t sample_sz)
{
    uart_set_pin(GNSS_UART_PORT, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_set_baudrate(GNSS_UART_PORT, baud);
    uart_flush_input(GNSS_UART_PORT);
    vTaskDelay(pdMS_TO_TICKS(30));

    int total = 0;
    bool nm = false, ub = false;
    uint8_t prev = 0;
    size_t si = 0;
    uint8_t buf[256];
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(GNSS_LISTEN_MS);
    while ((int32_t)(xTaskGetTickCount() - deadline) < 0) {
        int n = uart_read_bytes(GNSS_UART_PORT, buf, sizeof buf, pdMS_TO_TICKS(50));
        if (n <= 0) {
            continue;
        }
        total += n;
        for (int i = 0; i < n; i++) {
            uint8_t c = buf[i];
            if (prev == '$' && c >= 'A' && c <= 'Z') {
                nm = true;   /* NMEA sentence start: '$' + talker letter (e.g. $GPGGA, $PQTM) */
            }
            if (prev == 0xB5 && c == 0x62) {
                ub = true;   /* UBX sync chars */
            }
            prev = c;
            if (si + 1 < sample_sz && c >= 0x20 && c < 0x7F) {
                sample[si++] = (char)c;
            }
        }
        if ((nm || ub) && si > 24) {
            break;   /* clear signal + enough of a sample — stop early */
        }
    }
    if (sample && sample_sz) {
        sample[si < sample_sz ? si : sample_sz - 1] = '\0';
    }
    if (nmea) *nmea = nm;
    if (ubx)  *ubx  = ub;
    return total;
}

/* Polls UBX-MON-VER (class 0x0A id 0x04, empty payload; checksum 0x0E 0x34) —
 * a u-blox streaming UBX-only (NMEA disabled) still answers this. Harmless
 * to send to a non-u-blox module (just a bad frame). */
static void send_ubx_mon_ver(int rx, int tx, int baud)
{
    static const uint8_t poll[] = { 0xB5, 0x62, 0x0A, 0x04, 0x00, 0x00, 0x0E, 0x34 };
    uart_set_pin(GNSS_UART_PORT, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_set_baudrate(GNSS_UART_PORT, baud);
    uart_write_bytes(GNSS_UART_PORT, (const char *)poll, sizeof poll);
    uart_wait_tx_done(GNSS_UART_PORT, pdMS_TO_TICKS(100));
}

/* ==== M8-P1 Live Fix — NMEA parser + streaming pump ================================ *
 * A live receiver auto-streams NMEA at 38400 8N1. Lines are assembled, the
 * checksum verified, and GGA/RMC/GSA/GSV/TXT folded into s_wf, publishing a
 * spinlock-guarded snapshot each pump. Sentences are dispatched on the 3-char
 * sentence id (talker-agnostic), since combined solutions use talker "GN".
 * Position stays empty until a real fix; the sentence counter + antenna
 * status prove liveness with no sky view. */

/* XOR checksum over the chars between '$' and '*'. Sentences with no "*cs"
 * are accepted (u-blox always sends one; this stays lenient). Must be called
 * BEFORE nmea_split, which overwrites the '*'. */
static bool nmea_checksum_ok(const char *s)
{
    const char *star = strrchr(s, '*');
    if (!star || !star[1] || !star[2]) return true;
    uint8_t cs = 0;
    for (const char *p = s + 1; p < star; p++) cs ^= (uint8_t)*p;
    return cs == (uint8_t)strtol(star + 1, NULL, 16);
}

/* In-place comma split. Fields point into s (commas become NUL); a '*' ends the last field. */
static int nmea_split(char *s, char **f, int maxf)
{
    int n = 0;
    f[n++] = s;
    for (char *p = s; *p && n < maxf; p++) {
        if (*p == ',') { *p = '\0'; f[n++] = p + 1; }
        else if (*p == '*') { *p = '\0'; break; }
    }
    return n;
}

/* Converts NMEA ddmm.mmmm / dddmm.mmmm + hemisphere to signed decimal degrees. */
static double nmea_coord(const char *v, const char *hemi)
{
    if (!v || !*v) return 0.0;
    double raw = atof(v);
    int deg = (int)(raw / 100.0);
    double d = deg + (raw - deg * 100.0) / 60.0;
    if (hemi && (*hemi == 'S' || *hemi == 'W')) d = -d;
    return d;
}
static void nmea_time(const char *v, nocsif_gnss_fix_t *o)   /* hhmmss(.ss) */
{
    if (!v || strlen(v) < 6) return;
    o->hh = (v[0] - '0') * 10 + (v[1] - '0');
    o->mm = (v[2] - '0') * 10 + (v[3] - '0');
    o->ss = (v[4] - '0') * 10 + (v[5] - '0');
}
static void nmea_date(const char *v, nocsif_gnss_fix_t *o)   /* ddmmyy */
{
    if (!v || strlen(v) < 6) return;
    o->day  = (v[0] - '0') * 10 + (v[1] - '0');
    o->mon  = (v[2] - '0') * 10 + (v[3] - '0');
    o->year = 2000 + (v[4] - '0') * 10 + (v[5] - '0');
}

/* Parses one NMEA line and folds its fields into the worker accumulator s_wf. */
static void process_line(char *line)
{
    if (line[0] != '$' || strlen(line) < 6) return;
    if (!nmea_checksum_ok(line)) return;   /* drop corrupt frames */

    s_have_sentence = true;
    s_available = true;                    /* streaming proves the module is alive */
    s_last_sentence_us = esp_timer_get_time();
    s_wf.sentences++;

    char  type[4] = { line[3], line[4], line[5], 0 };   /* 3-char sentence id after $tt */
    char  talker[2] = { line[1], line[2] };
    char *f[24];
    int   nf = nmea_split(line, f, 24);

    if (!strcmp(type, "GGA") && nf >= 10) {
        nmea_time(f[1], &s_wf);
        s_wf.quality   = (uint8_t)atoi(f[6]);
        s_wf.sats_used = (uint8_t)atoi(f[7]);
        s_wf.hdop      = (float)atof(f[8]);
        s_wf.alt_m     = (float)atof(f[9]);
        if (s_wf.quality > 0) {
            s_wf.lat_deg = nmea_coord(f[2], f[3]);
            s_wf.lon_deg = nmea_coord(f[4], f[5]);
            s_have_fix = true;
            s_last_fix_us = s_last_sentence_us;
        }
    } else if (!strcmp(type, "RMC") && nf >= 10) {
        nmea_time(f[1], &s_wf);
        s_wf.valid      = (f[2][0] == 'A');
        s_wf.speed_kmh  = (float)atof(f[7]) * 1.852f;   /* knots -> km/h */
        s_wf.course_deg = (float)atof(f[8]);
        nmea_date(f[9], &s_wf);
        if (s_wf.valid) {
            s_wf.lat_deg = nmea_coord(f[3], f[4]);
            s_wf.lon_deg = nmea_coord(f[5], f[6]);
            s_have_fix = true;
            s_last_fix_us = s_last_sentence_us;
        }
    } else if (!strcmp(type, "GSA") && nf >= 3) {
        s_wf.fix_type = (uint8_t)atoi(f[2]);            /* 1 none / 2 = 2D / 3 = 3D */
    } else if (!strcmp(type, "GSV") && nf >= 4) {
        int idx = -1;                                   /* sats-in-view: latest numSV per talker */
        for (int i = 0; i < s_gsv_n; i++)
            if (s_gsv[i].t[0] == talker[0] && s_gsv[i].t[1] == talker[1]) { idx = i; break; }
        if (idx < 0 && s_gsv_n < (int)(sizeof s_gsv / sizeof s_gsv[0])) {
            idx = s_gsv_n++;
            s_gsv[idx].t[0] = talker[0];
            s_gsv[idx].t[1] = talker[1];
        }
        if (idx >= 0) s_gsv[idx].nsv = (uint8_t)atoi(f[3]);
        int sum = 0;
        for (int i = 0; i < s_gsv_n; i++) sum += s_gsv[i].nsv;
        s_wf.sats_in_view = (uint8_t)(sum > 255 ? 255 : sum);
    } else if (!strcmp(type, "TXT") && nf >= 5) {
        char *a = strstr(f[4], "ANTSTATUS=");           /* e.g. $GNTXT...ANTSTATUS=OK */
        if (a) snprintf(s_wf.antenna, sizeof s_wf.antenna, "%s", a + 10);
    }
}

/* Copies the running accumulator into the published snapshot (ages recomputed in the getter). */
static void publish_fix(void)
{
    portENTER_CRITICAL(&s_fix_mux);
    s_fix = s_wf;
    portEXIT_CRITICAL(&s_fix_mux);

    /* Auto-follow: hand Weather the last real position for its home. note_fix
     * is cheap here (RAM compare + a task wake, no flash) — the weather
     * worker does the throttled NVS persist, so this GPS hot path never
     * stalls on a flash write. */
    if ((s_wf.quality > 0 || s_wf.valid) && (s_wf.lat_deg != 0.0 || s_wf.lon_deg != 0.0))
        nocsif_weather_note_fix(s_wf.lat_deg, s_wf.lon_deg);
}

/* Drains whatever NMEA is available (blocks <=120 ms — this also paces the
 * live loop), feeds the line assembler, then publishes. Called repeatedly by
 * the worker while live. */
static void gnss_pump(void)
{
    uint8_t buf[256];
    int n = uart_read_bytes(GNSS_UART_PORT, buf, sizeof buf, pdMS_TO_TICKS(120));
    for (int i = 0; i < n; i++) {
        uint8_t c = buf[i];
        if (c == '\n' || c == '\r') {
            if (s_line_len > 0) { s_line[s_line_len] = '\0'; process_line(s_line); s_line_len = 0; }
        } else if (c >= 0x20 && c < 0x7F) {
            if (s_line_len < (int)sizeof(s_line) - 1) s_line[s_line_len++] = (char)c;
            else s_line_len = 0;   /* overrun — drop the line */
        }
    }
    publish_fix();
}

/* First-use UART bring-up (driver install once). Receive-only: RX=GPIO44;
 * leave TX (GPIO43 = the console) unmapped so streaming never contends with
 * the log console. */
static bool ensure_uart(void);   /* fwd — shared with the selftest path */

/* Powers up (if cold) and starts live NMEA streaming, resetting the accumulator first. */
static void live_start(void)
{
    if (!ensure_uart()) { publish_status("err"); publish_readout("UART init failed."); return; }

    /* Fresh session: clear the accumulator + liveness BEFORE going live, so the
     * UI shows "warming up" rather than stale data from a previous session. */
    memset(&s_wf, 0, sizeof s_wf);
    s_gsv_n = 0;
    s_line_len = 0;
    s_have_sentence = s_have_fix = false;
    publish_fix();

    bool cold = !s_powered;
    if (cold) {
        esp_err_t e = nocsif_power_gnss_rail(true);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "GNSS rail (BLDO1) enable failed: %s", esp_err_to_name(e));
            publish_status("err");
            publish_readout("GNSS rail failed — PMU not ready.");
            return;
        }
        s_powered = true;
    }

    s_live_on = true;   /* live now — the tick shows WARMING UP through the settle below */
    publish_status("live");
    publish_readout("GNSS live \xE2\x80\x94 streaming NMEA.");

    /* receive-only: RX only, TX untouched (console-safe) */
    uart_set_pin(GNSS_UART_PORT, UART_PIN_NO_CHANGE, GNSS_PIN_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_set_baudrate(GNSS_UART_PORT, 38400);
    uart_flush_input(GNSS_UART_PORT);

    if (cold) vTaskDelay(pdMS_TO_TICKS(GNSS_WARMUP_MS));   /* let the module cold-start */
    ESP_LOGI(TAG, "live fix ON (rx=%d @38400, rail %s)", GNSS_PIN_RX, cold ? "on" : "warm");
}

/* Stops live streaming and drops the power rail. */
static void live_stop(void)
{
    s_live_on = false;
    if (s_powered) { nocsif_power_gnss_rail(false); s_powered = false; }
    s_brought_up = false;
    publish_status("idle");
    publish_readout("GNSS idle.");
    ESP_LOGI(TAG, "live fix OFF (rail off)");
}

/* Equirectangular distance (m) between two lat/lon — accurate enough for short track deltas. */
static float gpx_dist_m(double la1, double lo1, double la2, double lo2)
{
    double mlat = (la1 + la2) * 0.5 * DEG2RAD;
    double dlat = (la2 - la1) * 111320.0;
    double dlon = (lo2 - lo1) * 111320.0 * cos(mlat);
    return (float)sqrt(dlat * dlat + dlon * dlon);
}

/* Publishes the GPX stats snapshot (builds the struct off-lock, copies under the spinlock). */
static void gpx_pub(void)
{
    nocsif_gnss_gpx_t g;
    g.state  = s_gpx_state;
    g.points = s_gpx_points;
    g.dist_m = s_gpx_dist_m;
    g.dur_s  = s_gpx_on ? (uint32_t)((esp_timer_get_time() - s_gpx_start_us) / 1000000) : 0;
    snprintf(g.path, sizeof g.path, "%s", s_gpx_path);
    portENTER_CRITICAL(&s_gpx_mux);
    s_gpx = g;
    portEXIT_CRITICAL(&s_gpx_mux);
}

/* Opens a fresh track file: claims /sd away from USB-MSC, picks a free name,
 * writes the GPX header plus an (empty) closing footer so the file is valid
 * immediately. Worker task. Returns true on success. */
static bool gpx_open(void)
{
    s_gpx_state = NOCSIF_GPX_WAIT;
    s_gpx_path[0] = '\0';

    esp_err_t ce = nocsif_usb_gadget_claim_sd(2000);
    if (ce != ESP_OK) {                     /* INVALID_STATE = no card or File Share owns it */
        s_gpx_state = NOCSIF_GPX_NOSD;
        ESP_LOGW(TAG, "gpx: claim_sd -> %s", esp_err_to_name(ce));
        gpx_pub();
        return false;
    }
    if (!nocsif_sdcard_lock(3000)) {
        s_gpx_state = NOCSIF_GPX_ERR;
        ESP_LOGE(TAG, "gpx: /sd lock timeout");
        gpx_pub();
        return false;
    }
    mkdir("/sd/nocsif", 0777);              /* ignore EEXIST */
    mkdir("/sd/nocsif/tracks", 0777);
    for (int i = 0; i < 1000; i++) {        /* first free trk-NNN.gpx */
        snprintf(s_gpx_path, sizeof s_gpx_path, "/sd/nocsif/tracks/trk-%03d.gpx", i);
        struct stat st;
        if (stat(s_gpx_path, &st) != 0) break;
    }
    FILE *f = fopen(s_gpx_path, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "gpx: fopen(%s) failed", s_gpx_path);
        nocsif_sdcard_unlock();
        s_gpx_state = NOCSIF_GPX_ERR;
        s_gpx_path[0] = '\0';
        gpx_pub();
        return false;
    }
    char when[24] = "";                     /* ISO-8601 UTC from the current fix, if known */
    if (s_wf.year) snprintf(when, sizeof when, "%04u-%02u-%02uT%02u:%02u:%02uZ",
                            s_wf.year, s_wf.mon, s_wf.day, s_wf.hh, s_wf.mm, s_wf.ss);
    char hdr[256];
    int hn = snprintf(hdr, sizeof hdr,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<gpx version=\"1.1\" creator=\"NocSif\" xmlns=\"http://www.topografix.com/GPX/1/1\">\n"
        "%s%s%s"
        " <trk><name>NocSif track %s</name><trkseg>\n",
        when[0] ? " <metadata><time>" : "", when[0] ? when : "", when[0] ? "</time></metadata>\n" : "",
        when[0] ? when : "(no fix yet)");
    fwrite(hdr, 1, (size_t)hn, f);
    s_gpx_body_off = ftell(f);
    fputs(GPX_FOOTER, f);                    /* keep the file valid on disk from the start */
    fflush(f);
    nocsif_sdcard_unlock();

    s_gpx_f = f;
    s_gpx_points = 0;
    s_gpx_dist_m = 0.0f;
    s_gpx_have_last = false;
    s_gpx_last_us = 0;
    s_gpx_start_us = esp_timer_get_time();
    s_gpx_on = true;
    s_gpx_state = NOCSIF_GPX_WAIT;
    gpx_pub();
    ESP_LOGI(TAG, "gpx: recording -> %s", s_gpx_path);
    return true;
}

/* Appends one <trkpt> for fix f, overwriting the old footer and rewriting it
 * after (keeps the file always valid). Lock taken internally; skips the
 * point if the card is momentarily busy. Worker task. */
static void gpx_append(const nocsif_gnss_fix_t *f)
{
    if (!s_gpx_f) return;
    if (!nocsif_sdcard_lock(1000)) return;   /* card busy — try again next cadence */

    char line[144];
    int n;
    if (f->year)
        n = snprintf(line, sizeof line,
            "  <trkpt lat=\"%.6f\" lon=\"%.6f\"><ele>%.1f</ele><time>%04u-%02u-%02uT%02u:%02u:%02uZ</time></trkpt>\n",
            f->lat_deg, f->lon_deg, (double)f->alt_m, f->year, f->mon, f->day, f->hh, f->mm, f->ss);
    else
        n = snprintf(line, sizeof line,
            "  <trkpt lat=\"%.6f\" lon=\"%.6f\"><ele>%.1f</ele></trkpt>\n",
            f->lat_deg, f->lon_deg, (double)f->alt_m);

    fseek(s_gpx_f, s_gpx_body_off, SEEK_SET);
    fwrite(line, 1, (size_t)n, s_gpx_f);
    s_gpx_body_off = ftell(s_gpx_f);
    fputs(GPX_FOOTER, s_gpx_f);
    fflush(s_gpx_f);
    nocsif_sdcard_unlock();

    if (s_gpx_have_last)
        s_gpx_dist_m += gpx_dist_m(s_gpx_last_lat, s_gpx_last_lon, f->lat_deg, f->lon_deg);
    s_gpx_last_lat = f->lat_deg;
    s_gpx_last_lon = f->lon_deg;
    s_gpx_have_last = true;
    s_gpx_last_us = esp_timer_get_time();
    s_gpx_points++;
    s_gpx_state = NOCSIF_GPX_REC;
    gpx_pub();
}

/* Closes the track file (footer is already on disk). Idempotent. Worker task. */
static void gpx_close(void)
{
    if (s_gpx_f) {
        if (nocsif_sdcard_lock(2000)) { fclose(s_gpx_f); nocsif_sdcard_unlock(); }
        else                          { fclose(s_gpx_f); }   /* best-effort */
        s_gpx_f = NULL;
        ESP_LOGI(TAG, "gpx: closed (%u pts, %.0f m) -> %s",
                 (unsigned)s_gpx_points, (double)s_gpx_dist_m, s_gpx_path);
    }
    s_gpx_on = false;
    s_gpx_state = NOCSIF_GPX_OFF;
    gpx_pub();
}

/* Per-pump GPX service: lazy-opens on request, closes on de-request, else
 * appends a point on the cadence when a valid fix is present. Reads the
 * worker-owned accumulator s_wf directly (no lock). Worker task. */
static void gpx_service(void)
{
    if (s_gpx_want && !s_gpx_f) { gpx_open(); return; }
    if (!s_gpx_want && s_gpx_f) { gpx_close(); return; }
    if (!s_gpx_f) return;

    if (!(s_wf.quality > 0 || s_wf.valid)) return;   /* no valid position yet — stay WAIT */

    int64_t now = esp_timer_get_time();
    uint32_t since = s_gpx_last_us ? (uint32_t)((now - s_gpx_last_us) / 1000) : 0xFFFFFFFFu;
    if (s_gpx_last_us && since < GPX_MIN_PERIOD_MS) return;        /* rate cap */
    bool heartbeat = (since >= GPX_MAX_PERIOD_MS);
    float moved = s_gpx_have_last
                ? gpx_dist_m(s_gpx_last_lat, s_gpx_last_lon, s_wf.lat_deg, s_wf.lon_deg) : 1e9f;
    if (!s_gpx_have_last || heartbeat || moved >= GPX_MIN_MOVE_M) gpx_append(&s_wf);
}

/* Returns the WiGLE AuthMode capability string for the monitor's security class. */
static const char *wd_authmode(uint8_t sec)
{
    switch (sec) {
        case NOCSIF_WIFI_SEC_WEP:   return "[WEP][ESS]";
        case NOCSIF_WIFI_SEC_WPA:   return "[WPA-PSK-TKIP][ESS]";
        case NOCSIF_WIFI_SEC_WPA2:  return "[WPA2-PSK-CCMP][ESS]";
        case NOCSIF_WIFI_SEC_WPA3:  return "[WPA3-SAE-CCMP][ESS]";
        case NOCSIF_WIFI_SEC_WPA2E: return "[WPA2-EAP-CCMP][ESS]";
        default:                    return "[ESS]";   /* OPEN */
    }
}

/* Copies an SSID into a CSV-safe field (strips commas/quotes/control chars). */
static void wd_sanitize(const char *in, char *out, size_t n)
{
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < n; i++) {
        char c = in[i];
        out[j++] = (c == ',' || c == '"' || (unsigned char)c < 0x20) ? '_' : c;
    }
    out[j] = '\0';
}

static bool wd_seen(const uint8_t bssid[6])
{
    for (int i = 0; i < s_wd_seen_n; i++)
        if (memcmp(s_wd_seen[i], bssid, 6) == 0) return true;
    return false;
}
static void wd_mark(const uint8_t bssid[6])
{
    if (s_wd_seen_n < WD_SEEN_MAX) memcpy(s_wd_seen[s_wd_seen_n++], bssid, 6);
}

static void wd_pub(void)
{
    int vis = nocsif_wifi_mon_ap_count();
    nocsif_gnss_wardrive_t g;
    g.state       = s_wd_state;
    g.networks    = s_wd_networks;
    g.aps_visible = (uint16_t)(vis < 0 ? 0 : vis);
    g.dur_s       = s_wd_on ? (uint32_t)((esp_timer_get_time() - s_wd_start_us) / 1000000) : 0;
    snprintf(g.path, sizeof g.path, "%s", s_wd_path);
    portENTER_CRITICAL(&s_wd_mux);
    s_wd = g;
    portEXIT_CRITICAL(&s_wd_mux);
}

/* Opens the CSV (WigleWifi-1.4 header) and starts the passive WiFi monitor hopping. Worker task. */
static bool wd_open(void)
{
    s_wd_state = NOCSIF_WD_WAIT;
    s_wd_path[0] = '\0';

    esp_err_t ce = nocsif_usb_gadget_claim_sd(2000);
    if (ce != ESP_OK) { s_wd_state = NOCSIF_WD_NOSD; wd_pub(); return false; }
    if (!nocsif_sdcard_lock(3000)) { s_wd_state = NOCSIF_WD_ERR; wd_pub(); return false; }
    mkdir("/sd/nocsif", 0777);
    mkdir("/sd/nocsif/wardrive", 0777);
    for (int i = 0; i < 1000; i++) {
        snprintf(s_wd_path, sizeof s_wd_path, "/sd/nocsif/wardrive/wardrive-%03d.csv", i);
        struct stat st;
        if (stat(s_wd_path, &st) != 0) break;
    }
    FILE *f = fopen(s_wd_path, "w");
    if (f == NULL) {
        nocsif_sdcard_unlock();
        s_wd_state = NOCSIF_WD_ERR; s_wd_path[0] = '\0'; wd_pub();
        return false;
    }
    fputs("WigleWifi-1.4,appRelease=NocSif,model=T-Watch-Ultra,release=1.0,"
          "device=esp32s3,display=,board=,brand=LilyGo\n", f);
    fputs("MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,CurrentLongitude,"
          "AltitudeMeters,AccuracyMeters,Type\n", f);
    fflush(f);
    nocsif_sdcard_unlock();

    /* Bring up the passive WiFi monitor (channel-hopping capture + AP parser) for the survey. */
    nocsif_wifi_init();
    nocsif_wifi_request_parse(true);
    nocsif_wifi_request_monitor_hop(true);

    s_wd_f = f;
    s_wd_networks = 0;
    s_wd_seen_n = 0;
    s_wd_last_us = 0;
    s_wd_start_us = esp_timer_get_time();
    s_wd_on = true;
    s_wd_state = NOCSIF_WD_WAIT;
    wd_pub();
    ESP_LOGI(TAG, "wardrive: logging -> %s", s_wd_path);
    return true;
}

static void wd_close(void)
{
    if (s_wd_f) {
        if (nocsif_sdcard_lock(2000)) { fclose(s_wd_f); nocsif_sdcard_unlock(); }
        else                          { fclose(s_wd_f); }
        s_wd_f = NULL;
        ESP_LOGI(TAG, "wardrive: closed (%u nets) -> %s", (unsigned)s_wd_networks, s_wd_path);
    }
    nocsif_wifi_request_parse(false);      /* stop the survey monitor */
    nocsif_wifi_request_monitor(false);
    s_wd_on = false;
    s_wd_state = NOCSIF_WD_OFF;
    wd_pub();
}

/* Per-pump wardrive service: lazy-opens on request, closes on de-request,
 * else — on the cadence and with a valid fix — appends a CSV row for each
 * newly-seen BSSID at the current position. Worker task. */
static void wd_service(void)
{
    if (s_wd_want && !s_wd_f) { wd_open(); return; }
    if (!s_wd_want && s_wd_f) { wd_close(); return; }
    if (!s_wd_f) return;

    if (!(s_wf.quality > 0 || s_wf.valid)) { s_wd_state = NOCSIF_WD_WAIT; wd_pub(); return; }

    int64_t now = esp_timer_get_time();
    uint32_t since = s_wd_last_us ? (uint32_t)((now - s_wd_last_us) / 1000) : 0xFFFFFFFFu;
    if (s_wd_last_us && since < WD_PERIOD_MS) { wd_pub(); return; }
    s_wd_last_us = now;

    int count = nocsif_wifi_mon_ap_count();
    if (count <= 0) { s_wd_state = NOCSIF_WD_REC; wd_pub(); return; }
    if (!nocsif_sdcard_lock(1000)) { wd_pub(); return; }    /* card busy — next cadence */

    char when[24];
    snprintf(when, sizeof when, "%04u-%02u-%02u %02u:%02u:%02u",
             s_wf.year, s_wf.mon, s_wf.day, s_wf.hh, s_wf.mm, s_wf.ss);
    float acc = s_wf.hdop > 0 ? s_wf.hdop * 5.0f : 10.0f;   /* rough accuracy from HDOP */
    int wrote = 0;
    for (int i = 0; i < count; i++) {
        nocsif_wifi_mon_ap_t ap;
        if (!nocsif_wifi_mon_ap_get(i, &ap)) continue;
        if (wd_seen(ap.bssid)) continue;
        wd_mark(ap.bssid);
        char ssid[40];
        wd_sanitize(ap.ssid, ssid, sizeof ssid);
        char line[176];
        int n = snprintf(line, sizeof line,
            "%02X:%02X:%02X:%02X:%02X:%02X,%s,%s,%s,%u,%d,%.6f,%.6f,%.1f,%.1f,WIFI\n",
            ap.bssid[0], ap.bssid[1], ap.bssid[2], ap.bssid[3], ap.bssid[4], ap.bssid[5],
            ssid, wd_authmode(ap.security), when, ap.channel, ap.rssi,
            s_wf.lat_deg, s_wf.lon_deg, (double)s_wf.alt_m, (double)acc);
        if (n > 0) { fwrite(line, 1, (size_t)n, s_wd_f); s_wd_networks++; wrote++; }
    }
    if (wrote) fflush(s_wd_f);
    nocsif_sdcard_unlock();
    s_wd_state = NOCSIF_WD_REC;
    wd_pub();
}

/* ---- the M8 proof-of-life --------------------------------------------------------- */
/* Sweeps bauds x pin orders listening passively, then falls back to an
 * active UBX poll, and logs a plain verdict of whether the module is alive. */
static void hw_selftest(void)
{
    ESP_LOGW(TAG, "==== GNSS HW PROOF-OF-LIFE (u-blox/LS550G, passive NMEA/UBX + UBX poll) ====");
    ESP_LOGW(TAG, "0) setup     : BLDO1 on, UART%d warmup %dms; sweep bauds {38400,115200,9600} x pins {rx44/tx43, rx43/tx44}",
             GNSS_UART_PORT, GNSS_WARMUP_MS);

    const int pins[2][2] = { { GNSS_PIN_RX, GNSS_PIN_TX }, { GNSS_PIN_TX, GNSS_PIN_RX } };
    bool alive = false;
    int a_rx = 0, a_tx = 0, a_baud = 0, a_bytes = 0;
    bool a_nmea = false, a_ubx = false;
    char sample[64] = { 0 };

    /* 1) Passive sweep — a live module auto-streams NMEA (both module types do). */
    for (int p = 0; p < 2 && !alive; p++) {
        for (unsigned b = 0; b < sizeof k_bauds / sizeof k_bauds[0] && !alive; b++) {
            bool nmea = false, ubx = false;
            char s[64] = { 0 };
            int bytes = listen_once(pins[p][0], pins[p][1], k_bauds[b], &nmea, &ubx, s, sizeof s);
            ESP_LOGW(TAG, "1) listen    : rx=%d tx=%d @%d -> bytes=%d nmea=%d ubx=%d%s",
                     pins[p][0], pins[p][1], k_bauds[b], bytes, nmea, ubx, bytes ? "" : " (silent)");
            if (nmea || ubx) {
                alive = true;
                a_rx = pins[p][0]; a_tx = pins[p][1]; a_baud = k_bauds[b];
                a_bytes = bytes; a_nmea = nmea; a_ubx = ubx;
                snprintf(sample, sizeof sample, "%s", s);
            }
        }
    }

    /* 2) Last resort — active UBX-MON-VER poll on the two most likely u-blox configs. */
    if (!alive) {
        const int try_baud[2] = { 38400, 9600 };
        for (int i = 0; i < 2 && !alive; i++) {
            bool nmea = false, ubx = false;
            char s[64] = { 0 };
            send_ubx_mon_ver(GNSS_PIN_RX, GNSS_PIN_TX, try_baud[i]);
            int bytes = listen_once(GNSS_PIN_RX, GNSS_PIN_TX, try_baud[i], &nmea, &ubx, s, sizeof s);
            ESP_LOGW(TAG, "2) UBX poll  : rx=%d tx=%d @%d -> bytes=%d nmea=%d ubx=%d",
                     GNSS_PIN_RX, GNSS_PIN_TX, try_baud[i], bytes, nmea, ubx);
            if (nmea || ubx) {
                alive = true;
                a_rx = GNSS_PIN_RX; a_tx = GNSS_PIN_TX; a_baud = try_baud[i];
                a_bytes = bytes; a_nmea = nmea; a_ubx = ubx;
                snprintf(sample, sizeof sample, "%s", s);
            }
        }
    }

    /* 3) Verdict. */
    ESP_LOGW(TAG, "---- VERDICT (alive=%d) ----", alive);
    if (alive) {
        ESP_LOGW(TAG, "  GNSS ALIVE: module streaming @%d baud (rx=%d tx=%d), nmea=%d ubx=%d, %d bytes.",
                 a_baud, a_rx, a_tx, a_nmea, a_ubx, a_bytes);
        ESP_LOGW(TAG, "  sample: %s", sample[0] ? sample : "(binary/UBX)");
        ESP_LOGW(TAG, "  -> GPS front-end responds on UART (no fix implied — needs sky view). M8 may be viable.");
        publish_status("alive");
        char line[96];
        snprintf(line, sizeof line, "GNSS alive @%d (rx%d/tx%d) nmea=%d ubx=%d", a_baud, a_rx, a_tx, a_nmea, a_ubx);
        publish_readout(line);
        s_available = true;
    } else {
        ESP_LOGW(TAG, "  DEAD: no NMEA/UBX on any baud x pin combo -> the module is not transmitting.");
        ESP_LOGW(TAG, "  BLDO1 rail was enabled + verified; a live receiver streams within ~1 s even with no fix.");
        ESP_LOGW(TAG, "  Confirms the operator's dead-GPS finding (same class as the dead NFC/haptic). M8 stays HW-BLOCKED.");
        publish_status("dead");
        publish_readout("GNSS silent on all bauds — not transmitting (confirms dead GPS).");
    }
    ESP_LOGW(TAG, "======================================================================");
}

/* First-use UART install (once). Receive-only: RX=GPIO44; TX is left
 * unmapped so streaming never contends with the console on GPIO43 (the
 * selftest re-pins explicitly for its active UBX poll). */
static bool ensure_uart(void)
{
    if (s_installed) {
        return true;
    }
    const uart_config_t cfg = {
        .baud_rate  = 38400,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t e = uart_driver_install(GNSS_UART_PORT, GNSS_RX_BUF, 0, 0, NULL, 0);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(e));
        return false;
    }
    if ((e = uart_param_config(GNSS_UART_PORT, &cfg)) != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(e));
        return false;
    }
    if ((e = uart_set_pin(GNSS_UART_PORT, UART_PIN_NO_CHANGE, GNSS_PIN_RX,
                          UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE)) != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(e));
        return false;
    }
    /* PPS is an input to the SoC (module output; only pulses when locked) —
     * configured for completeness, unused by the proof-of-life. */
    gpio_config_t pps = {
        .pin_bit_mask = 1ULL << GNSS_PIN_PPS,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pps);

    s_installed = true;
    ESP_LOGI(TAG, "GNSS UART%d up (RX=%d PPS=%d, receive-only)", GNSS_UART_PORT, GNSS_PIN_RX, GNSS_PIN_PPS);
    return true;
}

/* Powers the rail and brings up the UART for the selftest path. */
static void bring_up(void)
{
    esp_err_t perr = nocsif_power_gnss_rail(true);
    if (perr != ESP_OK) {
        ESP_LOGE(TAG, "GNSS rail (BLDO1) enable failed: %s", esp_err_to_name(perr));
        publish_status("err");
        publish_readout("GNSS rail failed — PMU not ready.");
        return;
    }
    s_powered = true;
    vTaskDelay(pdMS_TO_TICKS(GNSS_WARMUP_MS));   /* cold-start before the UART is active */

    if (!ensure_uart()) {
        publish_status("err");
        publish_readout("UART init failed.");
        return;
    }
    s_brought_up = true;
}

/* Runs the proof-of-life selftest, bringing the receiver up first if needed. */
static void do_selftest(void)
{
    publish_status("test");
    publish_readout("GNSS proof-of-life\xE2\x80\xA6");

    if (!s_brought_up) {
        bring_up();
    }
    if (s_brought_up) {
        hw_selftest();
    }
}

/* Worker task loop: waits for a request when idle, otherwise pumps NMEA and
 * services GPX/wardrive logging continuously while live. */
static void gnss_task(void *arg)
{
    (void)arg;
    volatile uint8_t probe;                          /* is this stack in PSRAM? */
    ESP_LOGI(TAG, "worker up: stack in %s", esp_ptr_external_ram((void *)&probe) ? "PSRAM" : "INTERNAL");
    for (;;) {
        /* Idle -> block until a request. Live -> don't block; gnss_pump() paces the loop (<=120 ms). */
        ulTaskNotifyTake(pdTRUE, s_live_on ? 0 : portMAX_DELAY);

        if (s_selftest_pending) { s_selftest_pending = false; do_selftest(); }

        /* GPX / wardrive keep the receiver live in the background even after
         * their screen closes; the Governor's hold (P2) is a fourth want, so
         * no holder can end another's session. */
        bool want_live = s_live_want || s_gpx_want || s_wd_want || s_hold_want;
        if (want_live && !s_live_on)       live_start();
        else if (!want_live && s_live_on) { gpx_close(); wd_close(); live_stop(); }

        if (s_live_on) { gnss_pump(); gpx_service(); wd_service(); }
    }
}

esp_err_t nocsif_gnss_init(void)
{
    if (s_task != NULL) {
        return ESP_OK;   /* idempotent */
    }
    if (nocsif_reliability_safe_mode()) {
        publish_status("off");
        publish_readout("GNSS disabled (safe mode).");
        ESP_LOGW(TAG, "safe mode — GNSS bring-up skipped");
        return ESP_OK;
    }
    publish_status("idle");
    publish_readout("GNSS idle. Run the proof-of-life to probe the receiver.");

    /* Stack in PSRAM: the UART RX ring is driver-owned (internal-DMA,
     * ISR-fed); this task only parses NMEA and writes GPX/wardrive files over
     * SD-SPI, and persists nothing to NVS itself (Weather owns fix
     * persistence). A PSRAM stack spawns even when internal RAM is tight. Never deleted. */
    if (xTaskCreateWithCaps(gnss_task, "gnss", 6144, NULL, 3, &s_task, MALLOC_CAP_SPIRAM) != pdPASS) {   /* 6144: SD writes + float snprintf (GPX) */
        ESP_LOGE(TAG, "failed to create gnss worker task");
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "gnss worker ready (lazy bring-up on first request)");
    return ESP_OK;
}

void nocsif_gnss_request_selftest(void)
{
    if (s_task != NULL) {
        s_selftest_pending = true;
        xTaskNotifyGive(s_task);
    }
}

void nocsif_gnss_set_live(bool on)
{
    s_live_want = on;
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}

void nocsif_gnss_set_hold(bool on)
{
    s_hold_want = on;
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}

void nocsif_gnss_gpx_set(bool on)
{
    s_gpx_want = on;
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}

void nocsif_gnss_wardrive_set(bool on)
{
    s_wd_want = on;
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}
