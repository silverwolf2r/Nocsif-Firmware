/*
 * NocSif — §4.15 desktop bridge (see bridge.h for the protocol).
 */
#include "bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"     /* xTaskCreateWithCaps — PSRAM stack */
#include "esp_lvgl_port.h"              /* lvgl_port_lock — hand flash-touching work to the LVGL task */
#include "lvgl.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "driver/i2c_master.h"          /* i2c_master_probe — the health check's targeted census */
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include "cJSON.h"

#include "sdfs.h"
#include "sdcard.h"
#include "usb_gadget.h"
#include "power.h"
#include "display.h"
#include "display_io.h"
#include "i2c_scan.h"
#include "imu.h"
#include "rtc.h"
#include "audio.h"
#include "mic.h"
#include "gnss.h"
#include "lora.h"
#include "nfc.h"
#include "wifi.h"
#include "ble.h"
#include "radio_state.h"
#include "coex.h"
#include "reliability.h"
#include "logbook.h"
#include "ota.h"
#include "settings.h"
#include "ui.h"

static const char *TAG = "bridge";

#define BR_PREFIX      "NB>"
#define BR_LINE_MAX    16384        /* one request line (an 8 KB base64 chunk + JSON)              */
#define BR_OUT_MAX     960          /* one reply line — ONE write(), under the stdio buffer size    */
#define BR_FRAG_RAW    600          /* raw bytes per fragment line → 800 base64 chars (~830 B line)  */
#define BR_CHUNK_MAX   8192         /* fs.get / fs.put payload per request                          */
/* Console rings (internal RAM, claimed first in app_main). RX: the driver's ISR DROPS bytes when the
 * ring is full, so a request line must fit with margin even if this task is held off for a few ms —
 * the host keeps upload requests under ~1 KB (720 raw bytes per chunk). TX: reply lines go straight
 * into this ring as ONE item (see out_line), so it must hold a whole line (BR_OUT_MAX). */
#define BR_RX_RING     4096
#define BR_TX_RING     2048

static bool   s_console;            /* the driver is installed (usb_serial_jtag_read_bytes is valid) */
static char  *s_line;               /* PSRAM: the request line being assembled                      */
static size_t s_len;
static bool   s_overflow;
static void   xfer_close_all(void); /* drop any open transfer session (defined with the fs commands) */

bool nocsif_bridge_console_init(void)
{
    usb_serial_jtag_driver_config_t cfg = { .rx_buffer_size = BR_RX_RING, .tx_buffer_size = BR_TX_RING };
    esp_err_t e = usb_serial_jtag_driver_install(&cfg);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "console driver install -> %s; bridge disabled", esp_err_to_name(e));
        return false;
    }
    usb_serial_jtag_vfs_use_driver();
    s_console = true;
    return true;
}

/* ---- output: one line = ONE ring item, blocking until it fits ----------------------------------- *
 * Not stdio: the console VFS writes char by char and, when the ring is full, retries once for 50 ms
 * and then DROPS the rest (fail-fast, so a missing host never stalls logging) — the first on-device
 * runs lost whole reply lines that way. usb_serial_jtag_write_bytes posts the line as one ring item
 * and waits (up to 2 s) for room, so a reply is either delivered whole or not at all; log chars from
 * other tasks can only land between items. The line must be smaller than the ring (BR_TX_RING). */
static void out_line(const char *json)
{
    char buf[BR_OUT_MAX + 8];
    int n = snprintf(buf, sizeof buf, BR_PREFIX "%s\n", json);
    if (n < 0) return;
    if ((size_t)n >= sizeof buf) n = (int)sizeof buf - 1;
    usb_serial_jtag_write_bytes(buf, (size_t)n, pdMS_TO_TICKS(2000));
}

static void json_escape(const char *in, char *out, size_t len)
{
    size_t o = 0;
    for (; in && *in && o + 2 < len; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20)         { out[o++] = ' '; }
        else                       { out[o++] = (char)c; }
    }
    out[o] = '\0';
}

/* Final line: {"id":N,"ok":true,"end":true,<extra>} — extra is a ready JSON field list or NULL. */
static void reply_end(int id, const char *extra)
{
    char js[BR_OUT_MAX];
    snprintf(js, sizeof js, "{\"id\":%d,\"ok\":true,\"end\":true%s%s}", id, extra ? "," : "", extra ? extra : "");
    out_line(js);
}

static void reply_err(int id, const char *err)
{
    char esc[200], js[BR_OUT_MAX];
    json_escape(err, esc, sizeof esc);
    snprintf(js, sizeof js, "{\"id\":%d,\"ok\":false,\"end\":true,\"err\":\"%s\"}", id, esc);
    out_line(js);
}

/* One data fragment line: {"id":N,"d":"<base64>"} for up to BR_FRAG_RAW bytes. */
static void reply_frag(int id, const uint8_t *data, size_t n)
{
    char b64[BR_FRAG_RAW * 4 / 3 + 8];
    size_t olen = 0;
    if (mbedtls_base64_encode((unsigned char *)b64, sizeof b64, &olen, data, n) != 0) return;
    b64[olen] = '\0';
    char js[BR_OUT_MAX];
    snprintf(js, sizeof js, "{\"id\":%d,\"d\":\"%s\"}", id, b64);
    out_line(js);
}

static void reply_blob(int id, const uint8_t *data, size_t n)
{
    for (size_t off = 0; off < n; off += BR_FRAG_RAW) {
        size_t k = n - off;
        if (k > BR_FRAG_RAW) k = BR_FRAG_RAW;
        reply_frag(id, data + off, k);
    }
}

/* ---- small JSON helpers ------------------------------------------------------------------------- */
static const char *jstr(cJSON *root, const char *key, const char *dflt)
{
    cJSON *it = cJSON_GetObjectItem(root, key);
    return (cJSON_IsString(it) && it->valuestring) ? it->valuestring : dflt;
}
static int jint(cJSON *root, const char *key, int dflt)
{
    cJSON *it = cJSON_GetObjectItem(root, key);
    if (cJSON_IsNumber(it)) return (int)it->valuedouble;
    if (cJSON_IsBool(it))   return cJSON_IsTrue(it) ? 1 : 0;
    if (cJSON_IsString(it) && it->valuestring) return atoi(it->valuestring);
    return dflt;
}

/* ---- version / status / health ------------------------------------------------------------------ */
/* ⚠ The OTA posture is read ONCE at init on the main task (internal stack) and cached: reading it goes
 * through esp_ota_get_state_partition, which memory-maps otadata (esp_partition_mmap → a cache freeze)
 * and asserts on a task whose stack lives in PSRAM — the first on-device run of `version` panicked
 * exactly there. Same rule as the flash writers: no flash write / erase / mmap from this task. */
static bool s_ota_pending;
static char s_ota_slot[16] = "?";

static const char *ota_state_str(void)
{
    return s_ota_pending ? "pending-verify" : "valid";
}

static void cmd_version(int id)
{
    const esp_app_desc_t *a = esp_app_get_description();
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char crash[200], name[80], ver[40], proj[40], date[40], idf[40];
    json_escape(nocsif_reliability_last_crash_str(), crash, sizeof crash);
    json_escape(nocsif_settings_device_name(), name, sizeof name);
    json_escape(a ? a->version : "", ver, sizeof ver);
    json_escape(a ? a->project_name : "", proj, sizeof proj);
    snprintf(date, sizeof date, "%s %s", a ? a->date : "", a ? a->time : "");
    json_escape(a ? a->idf_ver : "", idf, sizeof idf);
    char extra[BR_OUT_MAX - 40];
    snprintf(extra, sizeof extra,
             "\"version\":\"%s\",\"project\":\"%s\",\"build\":\"%s\",\"idf\":\"%s\","
             "\"elf\":\"%02x%02x%02x%02x\",\"slot\":\"%s\",\"ota_state\":\"%s\",\"boot\":\"%s\","
             "\"safe\":%s,\"uptime_s\":%lld,\"name\":\"%s\","
             "\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"crash\":\"%s\",\"proto\":1",
             ver, proj, date, idf,
             a ? a->app_elf_sha256[0] : 0, a ? a->app_elf_sha256[1] : 0,
             a ? a->app_elf_sha256[2] : 0, a ? a->app_elf_sha256[3] : 0,
             s_ota_slot, ota_state_str(), nocsif_reliability_reset_reason_str(),
             nocsif_reliability_safe_mode() ? "true" : "false",
             (long long)(esp_timer_get_time() / 1000000), name,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], crash);
    reply_end(id, extra);
}

static const char *usb_mode_str(nocsif_usb_mode_t m)
{
    switch (m) {
        case NOCSIF_USB_MODE_CDC: return "cdc";
        case NOCSIF_USB_MODE_HID: return "hid";
        case NOCSIF_USB_MODE_MSC: return "msc";
        default:                  return "detached";
    }
}

static void cmd_status(int id)
{
    nocsif_radio_state_t rs;
    nocsif_radio_state(&rs);
    bool vbus = nocsif_power_vbus_present();
    bool sd_present; uint64_t sd_total, sd_free;
    nocsif_sdfs_info(&sd_present, &sd_total, &sd_free);
    nocsif_gnss_fix_t fx;
    bool have_fix = nocsif_gnss_fix_snapshot(&fx);
    char ssid[80];
    json_escape(nocsif_wifi_saved_ssid(), ssid, sizeof ssid);
    char extra[BR_OUT_MAX - 40];
    snprintf(extra, sizeof extra,
             "\"batt\":%d,\"vbus\":%s,\"asleep\":%s,"
             "\"heap_int\":%u,\"heap_psram\":%u,\"dma_free\":%u,\"dma_largest\":%u,"
             "\"wifi\":{\"on\":%s,\"link\":%s,\"parked\":%s,\"ssid\":\"%s\"},"
             "\"ble\":{\"on\":%s,\"link\":%s},\"usb\":\"%s\",\"companion\":%s,"
             "\"sd\":{\"present\":%s,\"total\":%llu,\"free\":%llu},"
             "\"gnss\":{\"live\":%s,\"fix\":%d,\"sats\":%d},\"lora\":%s,\"uptime_s\":%lld",
             nocsif_power_batt_pct(), vbus ? "true" : "false", nocsif_display_is_asleep() ? "true" : "false",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)nocsif_int_dma_free(), (unsigned)nocsif_int_dma_largest(),
             rs.wifi_sta_on ? "true" : "false", rs.wifi_link_live ? "true" : "false", rs.wifi_parked ? "true" : "false", ssid,
             rs.ble_logical_on ? "true" : "false", rs.ble_link_live ? "true" : "false",
             usb_mode_str(nocsif_usb_gadget_mode()), nocsif_wifi_companion_active() ? "true" : "false",
             sd_present ? "true" : "false", (unsigned long long)sd_total, (unsigned long long)sd_free,
             nocsif_gnss_live() ? "true" : "false", have_fix ? (int)fx.fix_type : 0, have_fix ? (int)fx.sats_used : 0,
             nocsif_lora_available() ? "true" : "false",
             (long long)(esp_timer_get_time() / 1000000));
    reply_end(id, extra);
}

/* One health line: {"id":N,"chk":{"n":"…","ok":true|false|null,"d":"…"}}. ok=null = not testable
 * from here (a lazy worker that isn't up, or hardware with no standalone probe). */
static void chk(int id, const char *name, int ok /* 1 / 0 / -1 */, const char *detail)
{
    char esc[400], js[BR_OUT_MAX];
    json_escape(detail, esc, sizeof esc);
    snprintf(js, sizeof js, "{\"id\":%d,\"chk\":{\"n\":\"%s\",\"ok\":%s,\"d\":\"%s\"}}",
             id, name, ok > 0 ? "true" : (ok == 0 ? "false" : "null"), esc);
    out_line(js);
}

static void cmd_health(int id)
{
    char d[240];
    int pass = 0, fail = 0, skip = 0;
#define TALLY(ok) do { if ((ok) > 0) pass++; else if ((ok) == 0) fail++; else skip++; } while (0)

    /* I²C: probe the five on-board parts by address (a full 0x08-0x77 sweep at runtime reports ghost
     * ACKs while the IMU / touch traffic is live — the first run counted 17 "devices"; the boot-time
     * sweep, on a quiet bus, is the honest census). i2c_master_probe takes the bus lock per probe. */
    static const struct { uint8_t addr; const char *name; } k_i2c[] = {
        { 0x1A, "touch" }, { 0x20, "expander" }, { 0x28, "IMU" }, { 0x34, "PMU" }, { 0x51, "RTC" },
    };
    int ok = -1, n = 0;
    i2c_master_bus_handle_t bus = nocsif_i2c_bus();
    if (bus) {
        size_t o = 0;
        for (size_t i = 0; i < sizeof k_i2c / sizeof k_i2c[0]; i++) {
            bool hit = (i2c_master_probe(bus, k_i2c[i].addr, 50) == ESP_OK);
            if (hit) n++;
            o += (size_t)snprintf(d + o, sizeof d - o, "%s%s 0x%02X %s", i ? " · " : "", k_i2c[i].name,
                                  k_i2c[i].addr, hit ? "ok" : "MISSING");
            if (o >= sizeof d) break;
        }
        ok = (n == 5) ? 1 : 0;
    } else {
        snprintf(d, sizeof d, "bus not up");
    }
    chk(id, "i2c", ok, d); TALLY(ok);

    bool vb = false;
    esp_err_t pe = nocsif_power_vbus_read(&vb);
    ok = (pe == ESP_OK) ? 1 : 0;
    snprintf(d, sizeof d, "battery %d%% · %s", nocsif_power_batt_pct(), pe == ESP_OK ? (vb ? "on USB power" : "on battery") : "PMU read failed");
    chk(id, "pmu", ok, d); TALLY(ok);

    uint32_t tf = nocsif_display_io_tx_fail();
    ok = (tf == 0) ? 1 : 0;
    snprintf(d, sizeof d, "%u DMA underruns over %u chunks · panel %s", (unsigned)tf,
             (unsigned)nocsif_display_io_color_chunks(), nocsif_display_is_asleep() ? "asleep" : "on");
    chk(id, "display", ok, d); TALLY(ok);

    ok = nocsif_imu_online() ? 1 : 0;
    snprintf(d, sizeof d, ok ? "BHI260 online · wrist-wake %s · still %u s" : "sensor hub not online",
             nocsif_imu_wrist_wake_available() ? "available" : "off", (unsigned)(nocsif_imu_still_ms() / 1000));
    chk(id, "imu", ok, d); TALLY(ok);

    struct tm t; bool have_t = nocsif_rtc_get(&t);
    ok = nocsif_rtc_time_valid() ? 1 : 0;
    if (have_t) snprintf(d, sizeof d, "%04d-%02d-%02d %02d:%02d:%02d%s", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                         t.tm_hour, t.tm_min, t.tm_sec, ok ? "" : " (not set)");
    else        snprintf(d, sizeof d, "RTC not readable");
    chk(id, "rtc", have_t ? ok : 0, d); TALLY(have_t ? ok : 0);

    bool sdp; uint64_t sdt, sdf;
    nocsif_sdfs_info(&sdp, &sdt, &sdf);
    ok = sdp ? 1 : 0;
    if (sdp) {
        sdmmc_card_t *card = nocsif_sdcard_card();
        double gb = card ? (double)card->csd.capacity * card->csd.sector_size / 1e9 : 0.0;
        snprintf(d, sizeof d, "%.1f GB card · FAT %.2f GB free of %.2f GB", gb, sdf / 1e9, sdt / 1e9);
    } else snprintf(d, sizeof d, "no card (Files, captures, carts, Update need one)");
    chk(id, "sd", ok, d); TALLY(ok);

    ok = nocsif_audio_available() ? 1 : 0;
    snprintf(d, sizeof d, ok ? "speaker worker up · I2S %s · volume %u" : "speaker worker not up",
             nocsif_audio_tx_ready() ? "ready" : "idle", (unsigned)nocsif_audio_volume());
    chk(id, "audio", ok, d); TALLY(ok);

    ok = nocsif_mic_available() ? 1 : -1;
    snprintf(d, sizeof d, ok > 0 ? "PDM mic worker up · level %u" : "worker starts with Microphone / Voice Memos (not probed)",
             (unsigned)nocsif_mic_level());
    chk(id, "mic", ok, d); TALLY(ok);

    nocsif_gnss_fix_t fx; bool hf = nocsif_gnss_fix_snapshot(&fx);
    if (nocsif_gnss_available()) {
        ok = 1;
        snprintf(d, sizeof d, "receiver answers · %s · fix %d · %d sats", nocsif_gnss_live() ? "streaming" : "idle",
                 hf ? (int)fx.fix_type : 0, hf ? (int)fx.sats_used : 0);
    } else { ok = -1; snprintf(d, sizeof d, "not brought up yet (opens with Location / the Governor cycle)"); }
    chk(id, "gnss", ok, d); TALLY(ok);

    if (nocsif_lora_available()) { ok = 1; snprintf(d, sizeof d, "SX1262 answers · node %08lx", (unsigned long)nocsif_lora_node_id()); }
    else { ok = -1; snprintf(d, sizeof d, "not brought up yet (opens with a LoRa screen; test lora = passive RSSI probe)"); }
    chk(id, "lora", ok, d); TALLY(ok);

    if (nocsif_nfc_available()) { ok = 1; snprintf(d, sizeof d, "ST25R3916 answers · run test nfc for the RF front-end check"); }
    else { ok = -1; snprintf(d, sizeof d, "not brought up yet (test nfc runs the RF front-end self-test)"); }
    chk(id, "nfc", ok, d); TALLY(ok);

    nocsif_radio_state_t rs; nocsif_radio_state(&rs);
    ok = nocsif_wifi_available() ? 1 : 0;
    snprintf(d, sizeof d, ok ? "driver up · STA %s · %s" : "driver not up",
             rs.wifi_sta_on ? (rs.wifi_parked ? "parked" : "on") : "off", rs.wifi_link_live ? "linked" : "no link");
    chk(id, "wifi", ok, d); TALLY(ok);

    ok = (nocsif_ble_available() && nocsif_ble_boot_reserve_ran()) ? 1 : 0;
    snprintf(d, sizeof d, ok ? "controller resident · Bluetooth %s · phone %s" : "controller not up",
             rs.ble_logical_on ? "on" : "off", rs.ble_link_live ? "linked" : "not linked");
    chk(id, "ble", ok, d); TALLY(ok);

    nocsif_usb_gadget_state_t us = nocsif_usb_gadget_state();
    ok = (us == NOCSIF_USB_GADGET_FAILED) ? 0 : 1;
    snprintf(d, sizeof d, "mode %s · %s%s", usb_mode_str(nocsif_usb_gadget_mode()),
             us == NOCSIF_USB_GADGET_FAILED ? "last switch failed: " : "gadget ok",
             us == NOCSIF_USB_GADGET_FAILED ? nocsif_usb_gadget_fail_reason() : "");
    chk(id, "usb", ok, d); TALLY(ok);

    size_t lg = nocsif_int_dma_largest();
    ok = (lg >= 8192) ? 1 : 0;
    snprintf(d, sizeof d, "internal free %u · largest DMA block %u · PSRAM free %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)lg,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    chk(id, "memory", ok, d); TALLY(ok);

    ok = nocsif_reliability_safe_mode() ? 0 : 1;
    const char *cr = nocsif_reliability_last_crash_str();
    snprintf(d, sizeof d, "boot: %s · %s%s", nocsif_reliability_reset_reason_str(),
             cr[0] ? "last crash: " : "no crash recorded", cr[0] ? cr : "");
    chk(id, "reliability", ok, d); TALLY(ok);

    chk(id, "haptic", -1, "no driver (DRV2605 known dead on the reference unit — not probed)"); skip++;
#undef TALLY
    char extra[96];
    snprintf(extra, sizeof extra, "\"pass\":%d,\"fail\":%d,\"skip\":%d", pass, fail, skip);
    reply_end(id, extra);
}

static void cmd_test(int id, cJSON *root)
{
    const char *t = jstr(root, "t", "");
    if (!strcmp(t, "tone"))      { nocsif_audio_tone(1000, 250, 90);          reply_end(id, "\"msg\":\"1 kHz tone played\""); }
    else if (!strcmp(t, "nfc"))  { nocsif_nfc_request_selftest();             reply_end(id, "\"msg\":\"NFC RF front-end self-test started — verdict in the log\""); }
    else if (!strcmp(t, "lora")) { nocsif_lora_request_hunt_selftest();       reply_end(id, "\"msg\":\"LoRa passive RSSI probe started — verdict in the log\""); }
    else if (!strcmp(t, "gnss")) { nocsif_gnss_request_selftest();            reply_end(id, "\"msg\":\"GNSS self-test started — verdict in the log\""); }
    else reply_err(id, "unknown test (tone | nfc | lora | gnss)");
}

/* ---- microSD --------------------------------------------------------------------------------- */
static void cmd_sd_info(int id)
{
    bool p; uint64_t t, f;
    nocsif_sdfs_info(&p, &t, &f);
    char extra[160];
    snprintf(extra, sizeof extra, "\"present\":%s,\"total\":%llu,\"free\":%llu", p ? "true" : "false",
             (unsigned long long)t, (unsigned long long)f);
    reply_end(id, extra);
}

static void cmd_sd_provision(int id)
{
    int made = 0;
    const char *err = nocsif_sdfs_provision(&made);
    if (err) { reply_err(id, err); return; }
    char extra[48];
    snprintf(extra, sizeof extra, "\"made\":%d", made);
    reply_end(id, extra);
}

static void cmd_sd_format(int id, cJSON *root)
{
    if (!jint(root, "confirm", 0)) { reply_err(id, "send confirm:1 — this erases the whole card"); return; }
    xfer_close_all();
    const char *err = nocsif_sdfs_format();
    if (err) reply_err(id, err); else reply_end(id, "\"msg\":\"card formatted\"");
}

/* ---- files ----------------------------------------------------------------------------------- */
/* Transfer sessions: the FILE stays open across chunk requests. A fopen / fseek / fclose per chunk made
 * uploads crawl at 3 KB/s (FatFs walks the cluster chain on every append-open and syncs the directory
 * entry on every close). One put + one get session at a time; closed on completion, on a different
 * path / offset, on any error, and after BR_SESSION_IDLE_MS without a request (the task loop checks).
 * The FAT lock is still taken only per chunk — the handle merely persists between them (the §4.10
 * download keeps its handle the same way). */
#define BR_SESSION_IDLE_MS 30000    /* longer than the host's retry window (6 s × 3) so a retried chunk
                                       still finds its session */
typedef struct {
    FILE   *f;
    char    path[NOCSIF_SDFS_PATH_MAX + 8];   /* put: the .part path · get: the file */
    long    off;                              /* next offset the handle is positioned at */
    long    size;                             /* get: file size */
    int64_t last_us;
} xfer_t;
static xfer_t s_put, s_get;

/* Close under the lock; discard = remove the (partial) file too. */
static void xfer_close(xfer_t *x, bool discard)
{
    if (!x->f) return;
    if (nocsif_sdcard_lock(3000)) {
        fclose(x->f);
        if (discard) remove(x->path);
        nocsif_sdcard_unlock();
    } else {
        fclose(x->f);
    }
    x->f = NULL;
    x->path[0] = '\0';
    x->off = x->size = 0;
}

static void xfer_close_all(void)
{
    xfer_close(&s_put, true);
    xfer_close(&s_get, false);
}

static void xfer_idle_check(void)
{
    int64_t now = esp_timer_get_time();
    if (s_put.f && now - s_put.last_us > (int64_t)BR_SESSION_IDLE_MS * 1000) {
        ESP_LOGW(TAG, "fs: upload %s abandoned (idle) — discarded", s_put.path);
        xfer_close(&s_put, true);
    }
    if (s_get.f && now - s_get.last_us > (int64_t)BR_SESSION_IDLE_MS * 1000) xfer_close(&s_get, false);
}

static void cmd_fs_ls(int id, cJSON *root)
{
    const char *p = jstr(root, "p", NOCSIF_SDFS_ROOT);
    if (!nocsif_sdfs_path_ok(p, true)) { reply_err(id, "bad path"); return; }
    nocsif_sdfs_ent_t *ents = heap_caps_calloc(NOCSIF_SDFS_LIST_MAX, sizeof *ents, MALLOC_CAP_SPIRAM);
    if (!ents) { reply_err(id, "out of memory"); return; }
    int n = 0; bool trunc = false;
    const char *err = nocsif_sdfs_list(p, ents, NOCSIF_SDFS_LIST_MAX, &n, &trunc);
    if (err) { free(ents); reply_err(id, err); return; }
    char esc[NOCSIF_SDFS_NAME_MAX * 2], js[BR_OUT_MAX];
    for (int i = 0; i < n; i++) {
        json_escape(ents[i].name, esc, sizeof esc);
        snprintf(js, sizeof js, "{\"id\":%d,\"e\":{\"n\":\"%s\",\"d\":%d,\"s\":%u}}", id, esc,
                 ents[i].is_dir ? 1 : 0, (unsigned)ents[i].size);
        out_line(js);
    }
    free(ents);
    char pesc[NOCSIF_SDFS_PATH_MAX * 2], extra[NOCSIF_SDFS_PATH_MAX * 2 + 64];
    json_escape(p, pesc, sizeof pesc);
    snprintf(extra, sizeof extra, "\"path\":\"%s\",\"n\":%d,\"trunc\":%s", pesc, n, trunc ? "true" : "false");
    reply_end(id, extra);
}

static void cmd_fs_get(int id, cJSON *root)
{
    const char *p = jstr(root, "p", "");
    int off = jint(root, "off", 0), len = jint(root, "len", BR_CHUNK_MAX);
    if (!nocsif_sdfs_path_ok(p, false) || off < 0 || len <= 0) { reply_err(id, "bad path / range"); return; }
    if (len > BR_CHUNK_MAX) len = BR_CHUNK_MAX;
    if (s_get.f && (strcmp(s_get.path, p) != 0 || s_get.off != (long)off)) xfer_close(&s_get, false);
    const char *why = nocsif_sdfs_claim();
    if (why) { xfer_close(&s_get, false); reply_err(id, why); return; }
    uint8_t *buf = heap_caps_malloc(BR_CHUNK_MAX, MALLOC_CAP_SPIRAM);
    const char *err = buf ? NULL : "out of memory";
    size_t n = 0;
    if (!err) {
        if (nocsif_sdcard_lock(3000)) {
            if (!s_get.f) {                                      /* (re)open + seek once per session */
                struct stat st;
                s_get.f = (stat(p, &st) == 0 && S_ISREG(st.st_mode)) ? fopen(p, "rb") : NULL;
                if (s_get.f && fseek(s_get.f, off, SEEK_SET) != 0) { fclose(s_get.f); s_get.f = NULL; }
                if (s_get.f) { snprintf(s_get.path, sizeof s_get.path, "%s", p); s_get.off = off; s_get.size = (long)st.st_size; }
                else err = "no such file";
            }
            if (!err) {
                n = fread(buf, 1, (size_t)len, s_get.f);
                s_get.off += (long)n;
                s_get.last_us = esp_timer_get_time();
            }
            nocsif_sdcard_unlock();
        } else err = "card busy";
    }
    nocsif_sdfs_release();
    if (err) { if (buf) heap_caps_free(buf); xfer_close(&s_get, false); reply_err(id, err); return; }
    long size = s_get.size;
    if (n < (size_t)len || s_get.off >= s_get.size) xfer_close(&s_get, false);   /* EOF: done with it */
    reply_blob(id, buf, n);
    heap_caps_free(buf);
    char extra[96];
    snprintf(extra, sizeof extra, "\"size\":%ld,\"off\":%d,\"n\":%u", size, off, (unsigned)n);
    reply_end(id, extra);
}

static void cmd_fs_put(int id, cJSON *root)
{
    const char *p = jstr(root, "p", "");
    const char *d = jstr(root, "d", "");
    int off = jint(root, "off", 0), final = jint(root, "final", 0);
    if (!nocsif_sdfs_path_ok(p, false) || off < 0) { reply_err(id, "bad path"); return; }
    const char *slash = strrchr(p, '/');
    if (!slash || !nocsif_sdfs_name_ok(slash + 1)) { reply_err(id, "bad file name"); return; }
    char part[NOCSIF_SDFS_PATH_MAX + 8];
    snprintf(part, sizeof part, "%s.part", p);
    uint8_t *buf = heap_caps_malloc(BR_CHUNK_MAX, MALLOC_CAP_SPIRAM);
    if (!buf) { reply_err(id, "out of memory"); return; }
    size_t n = 0;
    if (d[0] && mbedtls_base64_decode(buf, BR_CHUNK_MAX, &n, (const unsigned char *)d, strlen(d)) != 0) {
        heap_caps_free(buf); reply_err(id, "bad base64 (chunk too big?)"); return;
    }
    /* A fresh upload (off 0) or a different target replaces any stale session. */
    if (s_put.f && (off == 0 || strcmp(s_put.path, part) != 0)) xfer_close(&s_put, true);
    const char *why = nocsif_sdfs_claim();
    if (why) { heap_caps_free(buf); xfer_close(&s_put, true); reply_err(id, why); return; }
    const char *err = NULL;
    bool done = false;
    if (nocsif_sdcard_lock(3000)) {
        if (!s_put.f) {
            if (off != 0) err = "offset mismatch — restart the upload";
            else {
                s_put.f = fopen(part, "wb");
                if (!s_put.f) err = "cannot write to the card";
                else { snprintf(s_put.path, sizeof s_put.path, "%s", part); s_put.off = 0; }
            }
        }
        if (!err) {
            if (n && (long)(off + n) == s_put.off) { /* a retried chunk whose reply was lost: already written */ }
            else if ((long)off != s_put.off)        err = "offset mismatch — restart the upload";
            else if (n && fwrite(buf, 1, n, s_put.f) != n) err = "card write failed (full?)";
            else s_put.off += (long)n;
            s_put.last_us = esp_timer_get_time();
        }
        if (!err && final) {
            fclose(s_put.f);
            s_put.f = NULL;
            remove(p);
            if (rename(part, p) != 0) { err = "cannot replace the file"; remove(part); }
            done = true;
        }
        if (err && s_put.f) { fclose(s_put.f); s_put.f = NULL; remove(part); }
        if (err || done) { s_put.path[0] = '\0'; s_put.off = 0; }
        nocsif_sdcard_unlock();
    } else err = "card busy";
    nocsif_sdfs_release();
    heap_caps_free(buf);
    if (err) { reply_err(id, err); return; }
    char extra[64];
    snprintf(extra, sizeof extra, "\"off\":%ld,\"final\":%s", s_put.f ? s_put.off : (long)(off + n), final ? "true" : "false");
    if (final) ESP_LOGI(TAG, "fs: wrote %s (%u bytes)", p, (unsigned)(off + n));
    reply_end(id, extra);
}

static void cmd_fs_rm(int id, cJSON *root)
{
    const char *p = jstr(root, "p", "");
    if (!nocsif_sdfs_path_ok(p, false)) { reply_err(id, "bad path"); return; }
    const char *why = nocsif_sdfs_claim();
    if (why) { reply_err(id, why); return; }
    const char *err = NULL;
    if (nocsif_sdcard_lock(1500)) {
        struct stat st;
        if (stat(p, &st) != 0)          err = "no such file";
        else if (S_ISDIR(st.st_mode))   { if (rmdir(p) != 0) err = "folder not empty"; }
        else if (remove(p) != 0)        err = "delete failed";
        nocsif_sdcard_unlock();
    } else err = "card busy";
    nocsif_sdfs_release();
    ESP_LOGI(TAG, "fs: rm %s%s%s", p, err ? " FAILED: " : "", err ? err : "");
    if (err) reply_err(id, err); else reply_end(id, NULL);
}

static void cmd_fs_mkdir(int id, cJSON *root)
{
    const char *p = jstr(root, "p", "");
    const char *slash = strrchr(p, '/');
    if (!nocsif_sdfs_path_ok(p, false) || !slash || !nocsif_sdfs_name_ok(slash + 1)) { reply_err(id, "bad path"); return; }
    const char *why = nocsif_sdfs_claim();
    if (why) { reply_err(id, why); return; }
    const char *err = NULL;
    if (nocsif_sdcard_lock(1500)) {
        struct stat st;
        if (stat(p, &st) == 0)           err = S_ISDIR(st.st_mode) ? NULL : "a file has that name";
        else if (mkdir(p, 0777) != 0)    err = "cannot create the folder";
        nocsif_sdcard_unlock();
    } else err = "card busy";
    nocsif_sdfs_release();
    if (err) reply_err(id, err); else reply_end(id, NULL);
}

/* ---- control (rides the companion dispatch → LVGL task) ---------------------------------------- */
static void cmd_ctl(int id, cJSON *root)
{
    const char *a = jstr(root, "a", "");
    nocsif_companion_cmd_t c = { 0 };
    if (!strcmp(a, "touch")) {
        if (!nocsif_wifi_companion_touch(jint(root, "x", 0), jint(root, "y", 0), jint(root, "s", 0))) { reply_err(id, "UI not ready"); return; }
        reply_end(id, NULL);
        return;
    }
    if      (!strcmp(a, "launch")) { c.type = NOCSIF_COMPANION_CMD_LAUNCH; snprintf(c.arg, sizeof c.arg, "%s", jstr(root, "app", "")); }   /* "app": "id" is the request id */
    else if (!strcmp(a, "back"))   { c.type = NOCSIF_COMPANION_CMD_BACK; }
    else if (!strcmp(a, "home"))   { c.type = NOCSIF_COMPANION_CMD_HOME; }
    else if (!strcmp(a, "type"))   { c.type = NOCSIF_COMPANION_CMD_TYPE; snprintf(c.arg, sizeof c.arg, "%s", jstr(root, "text", "")); }
    else if (!strcmp(a, "key"))    { c.type = NOCSIF_COMPANION_CMD_KEY;  snprintf(c.arg, sizeof c.arg, "%s", jstr(root, "key", "")); }
    else if (!strcmp(a, "bright")) { c.type = NOCSIF_COMPANION_CMD_BRIGHTNESS; snprintf(c.arg, sizeof c.arg, "%d", jint(root, "v", 128)); }
    else if (!strcmp(a, "vol"))    { c.type = NOCSIF_COMPANION_CMD_VOLUME;     snprintf(c.arg, sizeof c.arg, "%d", jint(root, "v", 128)); }
    else if (!strcmp(a, "button")) { c.type = NOCSIF_COMPANION_CMD_BUTTON;
                                     snprintf(c.arg, sizeof c.arg, "%s.%s", jstr(root, "k", "fn")[0] == 'p' ? "pwr" : "fn",
                                              jint(root, "l", 0) ? "long" : "short"); }
    else if (!strcmp(a, "cast"))   { c.type = NOCSIF_COMPANION_CMD_CAST; snprintf(c.arg, sizeof c.arg, "%d", jint(root, "on", 0) ? 1 : 0); }
    else { reply_err(id, "unknown action"); return; }
    if (c.type == NOCSIF_COMPANION_CMD_LAUNCH && !c.arg[0]) { reply_err(id, "launch needs \"app\""); return; }
    if (!nocsif_wifi_companion_dispatch(&c)) { reply_err(id, "UI not ready"); return; }
    reply_end(id, NULL);
}

static void cmd_menu(int id)
{
    enum { MENU_BUF = 8192 };
    char *js = heap_caps_malloc(MENU_BUF, MALLOC_CAP_SPIRAM);
    if (!js) { reply_err(id, "out of memory"); return; }
    if (!nocsif_wifi_companion_menu_json(js, MENU_BUF)) { heap_caps_free(js); reply_err(id, "UI not ready"); return; }
    reply_blob(id, (const uint8_t *)js, strlen(js));
    heap_caps_free(js);
    reply_end(id, "\"enc\":\"json\"");
}

static void cmd_state(int id)
{
    char js[512];
    if (!nocsif_wifi_companion_state_json(js, sizeof js)) { reply_err(id, "UI not ready"); return; }
    char extra[600];
    snprintf(extra, sizeof extra, "\"state\":%s", js);
    reply_end(id, extra);
}

/* ---- live view: `mirror` (see bridge.h) ------------------------------------------------------- *
 * Pull-based so the protocol stays request/reply: the host sends the sequence it last received (and
 * full:1 to resync), the watch answers with the changed rectangle since then as PackBits RLE over
 * 16-bit pixels (the dark UI packs ~5-10×; worst case +0.4 %), or a tiny {"none":true}. One touch event
 * [x, y, pressed] may ride the same poll, so one round trip carries input and output. A sequence the
 * host echoes that isn't the last one sent means it missed a frame → full frame. */
static uint32_t s_mir_sent_seq;

static size_t rle565_encode(const uint8_t *src, size_t npx, uint8_t *dst, size_t cap)
{
    const uint16_t *p = (const uint16_t *)src;           /* src is a malloc'd (aligned) buffer */
    size_t i = 0, o = 0;
    while (i < npx) {
        size_t run = 1;
        while (i + run < npx && run < 129 && p[i + run] == p[i]) run++;
        if (run >= 2) {                                   /* 0x80..0xFF: repeat (n & 0x7F) + 2 pixels */
            if (o + 3 > cap) break;
            dst[o++] = (uint8_t)(0x80 | (run - 2));
            memcpy(dst + o, &p[i], 2);
            o += 2;
            i += run;
            continue;
        }
        size_t lit = 1;                                   /* 0x00..0x7F: n + 1 literal pixels */
        while (i + lit < npx && lit < 128 && !(i + lit + 1 < npx && p[i + lit] == p[i + lit + 1])) lit++;
        if (o + 1 + lit * 2 > cap) break;
        dst[o++] = (uint8_t)(lit - 1);
        memcpy(dst + o, &p[i], lit * 2);
        o += lit * 2;
        i += lit;
    }
    return o;
}

static void cmd_mirror(int id, cJSON *root)
{
    enum { RAW_MAX = 205 * 251 * 2, RLE_MAX = RAW_MAX + RAW_MAX / 128 + 64 };
    uint32_t hseq = (uint32_t)jint(root, "seq", 0);
    bool full = jint(root, "full", 0) != 0 || hseq != s_mir_sent_seq;
    cJSON *t = cJSON_GetObjectItem(root, "t");
    if (cJSON_IsArray(t) && cJSON_GetArraySize(t) >= 3) {
        nocsif_wifi_companion_touch((int)cJSON_GetArrayItem(t, 0)->valuedouble,
                                    (int)cJSON_GetArrayItem(t, 1)->valuedouble,
                                    (int)cJSON_GetArrayItem(t, 2)->valuedouble);
    }
    uint8_t *raw = heap_caps_malloc(RAW_MAX, MALLOC_CAP_SPIRAM);
    uint8_t *rle = heap_caps_malloc(RLE_MAX, MALLOC_CAP_SPIRAM);
    if (!raw || !rle) { if (raw) heap_caps_free(raw); if (rle) heap_caps_free(rle); reply_err(id, "out of memory"); return; }
    int x = 0, y = 0, w = 0, h = 0;
    uint32_t seq = s_mir_sent_seq;
    char extra[160];
    if (!nocsif_ui_mirror_poll(full, raw, RAW_MAX, &x, &y, &w, &h, &seq)) {
        heap_caps_free(raw); heap_caps_free(rle);
        snprintf(extra, sizeof extra, "\"none\":true,\"seq\":%u", (unsigned)s_mir_sent_seq);
        reply_end(id, extra);
        return;
    }
    size_t n = rle565_encode(raw, (size_t)w * h, rle, RLE_MAX);
    s_mir_sent_seq = seq;
    reply_blob(id, rle, n);
    heap_caps_free(raw); heap_caps_free(rle);
    snprintf(extra, sizeof extra, "\"seq\":%u,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"enc\":\"rle565\",\"raw\":%u,\"full\":%s",
             (unsigned)seq, x, y, w, h, (unsigned)((size_t)w * h * 2), full ? "true" : "false");
    reply_end(id, extra);
}

static void cmd_screenshot(int id)
{
    enum { SHOT_MAX = 205 * 251 * 2 };
    uint8_t *buf = heap_caps_malloc(SHOT_MAX, MALLOC_CAP_SPIRAM);
    if (!buf) { reply_err(id, "out of memory"); return; }
    int w = 0, h = 0;
    if (!nocsif_ui_screenshot(buf, SHOT_MAX, &w, &h)) { heap_caps_free(buf); reply_err(id, "UI busy"); return; }
    reply_blob(id, buf, (size_t)w * h * 2);
    heap_caps_free(buf);
    char extra[64];
    snprintf(extra, sizeof extra, "\"w\":%d,\"h\":%d,\"fmt\":\"rgb565le\"", w, h);
    reply_end(id, extra);
}

/* ⚠ Flash may not be touched from this PSRAM-stacked task (reads included: every SPI-flash API call
 * disables the cache and asserts esp_task_stack_is_sane_cache_disabled). The logbook lives in a flash
 * partition, so the read runs on the LVGL task (internal stack — the Diagnostics screen does the same)
 * via lv_async_call, and this task waits on a semaphore. Same route for reboot (shutdown handlers may
 * flush to flash). */
typedef struct { char *buf; size_t n; SemaphoreHandle_t done; } logtail_req_t;
static void logtail_async(void *p)
{
    logtail_req_t *r = p;
    nocsif_logbook_read_tail(r->buf, r->n);
    xSemaphoreGive(r->done);
}
static void reboot_async(void *p) { (void)p; esp_restart(); }

/* Run fn(arg) on the LVGL task; true if it was queued. */
static bool on_lvgl(lv_async_cb_t fn, void *arg)
{
    if (!lvgl_port_lock(500)) return false;
    lv_async_call(fn, arg);
    lvgl_port_unlock();
    return true;
}

static void cmd_log_tail(int id, cJSON *root)
{
    int n = jint(root, "n", 2048);
    if (n < 64) n = 64;
    if (n > 8192) n = 8192;
    /* The request lives on the heap: if the wait times out while the async is still queued, it is
     * simply leaked (never freed under a pending async, never a stack object it could touch later). */
    logtail_req_t *r = heap_caps_calloc(1, sizeof *r, MALLOC_CAP_SPIRAM);
    if (r) { r->buf = heap_caps_malloc((size_t)n, MALLOC_CAP_SPIRAM); r->n = (size_t)n; r->done = xSemaphoreCreateBinary(); }
    if (!r || !r->buf || !r->done) {
        if (r) { if (r->buf) heap_caps_free(r->buf); if (r->done) vSemaphoreDelete(r->done); heap_caps_free(r); }
        reply_err(id, "out of memory");
        return;
    }
    r->buf[0] = '\0';
    if (!on_lvgl(logtail_async, r)) {
        heap_caps_free(r->buf); vSemaphoreDelete(r->done); heap_caps_free(r);   /* never queued: safe to free */
        reply_err(id, "UI busy");
        return;
    }
    if (!xSemaphoreTake(r->done, pdMS_TO_TICKS(4000))) {
        ESP_LOGW(TAG, "log.tail: UI did not answer in 4 s (request leaked on purpose)");
        reply_err(id, "UI busy");
        return;
    }
    reply_blob(id, (const uint8_t *)r->buf, strlen(r->buf));
    heap_caps_free(r->buf); vSemaphoreDelete(r->done); heap_caps_free(r);
    reply_end(id, "\"enc\":\"text\"");
}

static void cmd_usb(int id, cJSON *root)
{
    const char *m = jstr(root, "mode", "");
    nocsif_usb_mode_t mode;
    if      (!strcmp(m, "detached")) mode = NOCSIF_USB_MODE_DETACHED;
    else if (!strcmp(m, "cdc"))      mode = NOCSIF_USB_MODE_CDC;
    else if (!strcmp(m, "hid"))      mode = NOCSIF_USB_MODE_HID;
    else if (!strcmp(m, "msc"))      mode = NOCSIF_USB_MODE_MSC;
    else { reply_err(id, "unknown mode (detached | cdc | hid | msc)"); return; }
    xfer_close_all();
    reply_end(id, "\"msg\":\"switching — this console goes away while a gadget mode is active\"");
    nocsif_usb_gadget_request_mode(mode);
}

/* ---- dispatch --------------------------------------------------------------------------------- */
static void handle_line(const char *line)
{
    if (line[0] != '{') return;                     /* not for us (a stray terminal keystroke) */
    cJSON *root = cJSON_Parse(line);
    if (!root) { reply_err(0, "bad JSON"); return; }
    int id = jint(root, "id", 0);
    const char *c = jstr(root, "c", "");
    if      (!strcmp(c, "ping"))         reply_end(id, "\"proto\":1,\"name\":\"NocSif\"");
    else if (!strcmp(c, "version"))      cmd_version(id);
    else if (!strcmp(c, "status"))       cmd_status(id);
    else if (!strcmp(c, "health"))       cmd_health(id);
    else if (!strcmp(c, "test"))         cmd_test(id, root);
    else if (!strcmp(c, "sd.info"))      cmd_sd_info(id);
    else if (!strcmp(c, "sd.provision")) cmd_sd_provision(id);
    else if (!strcmp(c, "sd.format"))    cmd_sd_format(id, root);
    else if (!strcmp(c, "fs.ls"))        cmd_fs_ls(id, root);
    else if (!strcmp(c, "fs.get"))       cmd_fs_get(id, root);
    else if (!strcmp(c, "fs.put"))       cmd_fs_put(id, root);
    else if (!strcmp(c, "fs.rm"))        cmd_fs_rm(id, root);
    else if (!strcmp(c, "fs.mkdir"))     cmd_fs_mkdir(id, root);
    else if (!strcmp(c, "ctl"))          cmd_ctl(id, root);
    else if (!strcmp(c, "menu"))         cmd_menu(id);
    else if (!strcmp(c, "state"))        cmd_state(id);
    else if (!strcmp(c, "screenshot"))   cmd_screenshot(id);
    else if (!strcmp(c, "mirror"))       cmd_mirror(id, root);
    else if (!strcmp(c, "log.tail"))     cmd_log_tail(id, root);
    else if (!strcmp(c, "usb"))          cmd_usb(id, root);
    else if (!strcmp(c, "reboot"))       { xfer_close_all(); reply_end(id, "\"msg\":\"rebooting\""); vTaskDelay(pdMS_TO_TICKS(300));
                                           if (!on_lvgl(reboot_async, NULL)) reply_err(id, "UI busy"); }
    else reply_err(id, "unknown command");
    cJSON_Delete(root);
}

static void bridge_task(void *arg)
{
    (void)arg;
    uint8_t tmp[256];
    for (;;) {
        int n = usb_serial_jtag_read_bytes(tmp, sizeof tmp, pdMS_TO_TICKS(250));
        if (n <= 0) { xfer_idle_check(); continue; }
        for (int i = 0; i < n; i++) {
            char c = (char)tmp[i];
            if (c == '\n' || c == '\r') {
                if (s_len && !s_overflow) { s_line[s_len] = '\0'; handle_line(s_line); }
                else if (s_overflow)      { reply_err(0, "line too long"); }
                s_len = 0;
                s_overflow = false;
                continue;
            }
            if (s_len + 1 >= BR_LINE_MAX) { s_overflow = true; continue; }
            s_line[s_len++] = c;
        }
    }
}

void nocsif_bridge_init(void)
{
    if (!s_console) return;
    /* On the caller's (main task, internal) stack — see ota_state_str. Called after nocsif_ota_confirm
     * so a freshly-OTA'd image already reads "valid". */
    s_ota_pending = nocsif_ota_pending_verify();
    snprintf(s_ota_slot, sizeof s_ota_slot, "%s", nocsif_ota_running_label());
    s_line = heap_caps_malloc(BR_LINE_MAX, MALLOC_CAP_SPIRAM);
    if (!s_line) { ESP_LOGW(TAG, "no memory for the line buffer; bridge disabled"); return; }
    /* 32 KB (PSRAM, so it costs nothing scarce): a directory listing (FatFs LFN buffers + stat per
     * entry + the reply buffers) overflowed 8 KB on the first run, and the screenshot renders a full
     * LVGL frame on THIS task. */
    /* Priority 10: above the UI, below WiFi/BT — the RX ring holds ~3 ms of host data at USB speed and
     * the driver's ISR drops what doesn't fit, so this task must drain it promptly. */
    if (xTaskCreateWithCaps(bridge_task, "bridge", 32768, NULL, 10, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGW(TAG, "task create failed; bridge disabled");
        return;
    }
    ESP_LOGI(TAG, "desktop bridge listening on the USB console (JSON lines, replies prefixed " BR_PREFIX ")");
}
