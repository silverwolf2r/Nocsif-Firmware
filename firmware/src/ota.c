/*
 * NocSif — over-the-air firmware update (M-OTA). See ota.h for the design.
 *
 * One worker task, woken by a task-notification, does the two blocking jobs off the LVGL thread:
 *   SCAN    — claim /sd, stat + header-validate NOCSIF_OTA_SD_PATH, publish present/size/version.
 *   INSTALL — claim /sd, stream the .bin into the inactive slot (esp_ota_*) with a live progress %,
 *             mark it bootable, and reboot into it. The bootloader boots it as PENDING_VERIFY;
 *             nocsif_ota_confirm() (main heartbeat, ~30 s) cancels the rollback once it runs healthy.
 *
 * Status fields are single-writer (this worker) / single-reader (the LVGL poll) plain reads — the
 * same lock-free convention the wifi/mic screens use. s_status points at string literals only (a
 * pointer store is atomic on this target), so there is no torn-string race.
 */
#include "ota.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_app_desc.h"

#include <sys/stat.h>        /* mkdir — the nocsif/firmware folder on the card */

#include "freertos/idf_additions.h"   /* xTaskCreateWithCaps — the §4.10 web worker's PSRAM stack */
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "mbedtls/sha256.h"

#include "sdcard.h"          /* nocsif_sdcard_lock/unlock — serialize app-side FAT access */
#include "usb_gadget.h"      /* nocsif_usb_gadget_claim_sd/release_sd — own /sd vs USB-MSC */
#include "reliability.h"     /* nocsif_reliability_ui_liveness_suspend — the flash ops starve LVGL */
#include "settings.h"        /* "ota_repo" */
#include "wifi.h"            /* nocsif_wifi_connected / request_enable — the pull needs the STA */
#include "governor.h"        /* nocsif_gov_wifi_wake — a parked STA is woken for a pull */
#include "power.h"           /* nocsif_power_batt_pct / vbus_present — the install battery gate */

static const char *TAG = "nocsif_ota";

/* ESP application image: byte 0 is the image magic; the app descriptor sits at a fixed offset right
 * after the image header (24 B) + first segment header (8 B). */
#define OTA_IMG_MAGIC        0xE9u
#define OTA_APP_DESC_OFFSET  32
#define OTA_HDR_READ         (OTA_APP_DESC_OFFSET + sizeof(esp_app_desc_t))   /* bytes to sniff */

/* Streaming chunk (worker-only; keeps the task stack light — the whole image is never buffered). */
#define OTA_CHUNK            4096

typedef enum { REQ_NONE = 0, REQ_SCAN, REQ_INSTALL } ota_req_t;

static TaskHandle_t                 s_task;
static volatile ota_req_t           s_req;
static volatile nocsif_ota_state_t  s_state    = NOCSIF_OTA_IDLE;
static volatile int                 s_progress;                   /* 0..100 */
static const char * volatile        s_status   = "";              /* literals only */
static volatile bool                s_sd_present;
static volatile uint32_t            s_sd_size;
static char                         s_sd_ver[32];                 /* worker-written, UI-read */

/* 64-byte aligned + DMA-capable internal RAM so the SD read DMAs STRAIGHT into it with no bounce:
 * spi_master only allocates an internal-DMA bounce buffer when the target is not DMA-capable OR not
 * aligned, and under int-DMA fragmentation that alloc can fault. Aligning s_buf means FatFs's direct
 * multi-sector DATA read needs no bounce. NOTE: WiFi is no longer yielded for the read (the yield
 * machinery was deleted with the resident-BLE-controller model, RAM-BUDGET.md remake #5), so this
 * aligned staging buffer is the sole guarantee the streaming read stays bounce-free. Only the small FAT
 * directory reads inside fopen use FatFs's own window buffer; verify an OTA install with WiFi ASSOCIATED
 * on-device (RAM-BUDGET.md conflict C6). */
static uint8_t                      s_buf[OTA_CHUNK] __attribute__((aligned(64)));  /* worker-only */

/* ---- §4.10 GitHub pull state (web worker writes, UI reads plain) --------------------------- */
typedef enum { WEB_NONE = 0, WEB_CHECK, WEB_DOWNLOAD, WEB_SCAN } web_req_t;
static TaskHandle_t                       s_web_task;
static volatile web_req_t                 s_web_req;
static volatile nocsif_ota_web_state_t    s_web_state = NOCSIF_OTA_WEB_IDLE;
static volatile int                       s_web_progress;
static const char * volatile              s_web_status = "";      /* literals only */
static char                               s_web_ver[32];          /* published version (manifest)     */
static char                               s_web_sha[65];          /* published sha256, lower-case hex */
static volatile uint32_t                  s_web_size;             /* published image size             */
static char                               s_repo[64];             /* "owner/repo"; loaded lazily      */
static bool                               s_repo_loaded;
#define OTA_WEB_STACK      16384          /* PSRAM: TLS handshake + cJSON + sha256 on this task        */
#define OTA_WEB_CHUNK      8192           /* PSRAM read size; each read is written at once (short card
                                           * lock, socket kept drained — WiFi's RX buffers are internal) */
#define OTA_WEB_RESUMES    5              /* stalled reads reopen with an HTTP Range at the byte offset */
#define OTA_WEB_LINK_WAIT_S 20            /* wait this long for the STA after waking it               */
#define OTA_WEB_BASE       "https://raw.githubusercontent.com/"
#define OTA_WEB_FOLDER     "/main/nocsif/firmware/"
#define OTA_INSTALL_BATT_MIN 30           /* % — install refused below this unless USB power is present */

/* ---- helpers --------------------------------------------------------------------------------- */

/* Open the card image: the §4.10 folder copy first, then the pre-§4.10 root path. *used names it. */
static FILE *sd_open_image(const char **used)
{
    FILE *f = fopen(NOCSIF_OTA_SD_PATH, "rb");
    if (f) { if (used) *used = NOCSIF_OTA_SD_PATH; return f; }
    f = fopen(NOCSIF_OTA_SD_PATH_OLD, "rb");
    if (f && used) *used = NOCSIF_OTA_SD_PATH_OLD;
    return f;
}

/* Sniff an open image file: confirm it is an ESP app image and (optionally) extract its version.
 * Leaves the file position past the header — the caller rewinds before streaming. */
static bool image_sniff(FILE *f, char *ver, size_t vern)
{
    uint8_t hdr[OTA_HDR_READ];
    if (fseek(f, 0, SEEK_SET) != 0) return false;
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr) return false;
    if (hdr[0] != OTA_IMG_MAGIC) return false;
    const esp_app_desc_t *d = (const esp_app_desc_t *)(hdr + OTA_APP_DESC_OFFSET);
    if (d->magic_word != ESP_APP_DESC_MAGIC_WORD) return false;
    if (ver && vern) {
        snprintf(ver, vern, "%s", d->version);
    }
    return true;
}

/* Take /sd away from USB-MSC AND the app FAT lock. Returns true on success; on failure sets
 * s_status/s_state(fail_state) and returns false with nothing held. */
static bool sd_grab(nocsif_ota_state_t fail_state)
{
    if (nocsif_usb_gadget_claim_sd(2000) != ESP_OK) {
        s_status = "microSD unavailable";
        s_state  = fail_state;
        return false;
    }
    if (!nocsif_sdcard_lock(3000)) {
        nocsif_usb_gadget_release_sd();
        s_status = "microSD busy";
        s_state  = fail_state;
        return false;
    }
    return true;
}

static void sd_drop(void)
{
    nocsif_sdcard_unlock();
    nocsif_usb_gadget_release_sd();
}

/* ---- jobs ------------------------------------------------------------------------------------ */

static void do_scan(void)
{
    s_state = NOCSIF_OTA_SCANNING;
    s_status = "scanning card\xE2\x80\xA6";
    s_sd_present = false;
    s_sd_size = 0;
    s_sd_ver[0] = '\0';

    if (!sd_grab(NOCSIF_OTA_IDLE)) return;

    const char *result = "no firmware.bin on card";
    FILE *f = sd_open_image(NULL);
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        char ver[32] = "";
        const esp_partition_t *tgt = esp_ota_get_next_update_partition(NULL);
        if (sz <= 0 || !image_sniff(f, ver, sizeof ver)) {
            result = "firmware.bin is not a valid image";
        } else if (tgt && (uint32_t)sz > tgt->size) {
            result = "image too large for slot";
        } else {
            s_sd_size = (uint32_t)sz;
            snprintf(s_sd_ver, sizeof s_sd_ver, "%s", ver);
            s_sd_present = true;
            result = "image ready";
        }
        fclose(f);
    }
    sd_drop();
    s_status = result;
    s_state = NOCSIF_OTA_IDLE;
}

static void do_install(void)
{
    s_state = NOCSIF_OTA_RUNNING;
    s_progress = 0;
    s_status = "preparing\xE2\x80\xA6";

    const esp_partition_t *tgt = esp_ota_get_next_update_partition(NULL);
    if (!tgt) { s_status = "no OTA slot in table"; s_state = NOCSIF_OTA_FAILED; return; }

    bool             wdt_suspended = false;
    bool             sd_held       = false;
    bool             have_handle   = false;
    esp_ota_handle_t h             = 0;
    FILE            *f             = NULL;
    const char      *err           = NULL;
    long             sz            = 0;
    size_t           written       = 0;
    esp_err_t        e             = ESP_OK;

    /* The streaming read below DMAs each chunk straight into the aligned s_buf (bounce-free); WiFi is no
     * longer yielded for it (the yield machinery was deleted with the resident-BLE model). See the s_buf
     * note re: verifying an install with WiFi associated on-device.
     *
     * Pause the UI-liveness watchdog for the duration: the flash erase/write below repeatedly disables
     * the cache the LVGL task's pet-timer runs from, so it cannot pet even though nothing is wrong —
     * the watchdog would otherwise panic mid-install (the other crash root cause). Resumed on every
     * exit below (on success we reboot anyway). */
    nocsif_reliability_ui_liveness_suspend(true);
    wdt_suspended = true;

    if (!sd_grab(NOCSIF_OTA_FAILED)) goto fail_kept_status;   /* sd_grab already set s_status */
    sd_held = true;

    const char *used = NULL;
    f = sd_open_image(&used);
    if (!f) { err = "no firmware.bin on card"; goto fail; }
    ESP_LOGI(TAG, "OTA: installing %s", used);

    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || !image_sniff(f, NULL, 0)) { err = "not a firmware image"; goto fail; }
    if ((uint32_t)sz > tgt->size)            { err = "image too large for slot"; goto fail; }
    fseek(f, 0, SEEK_SET);   /* rewind past the sniff read before streaming */

    /* OTA_WITH_SEQUENTIAL_WRITES: do NOT erase the whole slot up front (that single multi-second
     * blocking erase is what froze the UI + tripped the watchdog). Each sector is erased lazily inside
     * esp_ota_write, in small chunks the loop below yields between — the progress bar keeps animating. */
    e = esp_ota_begin(tgt, OTA_WITH_SEQUENTIAL_WRITES, &h);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin(%s): %s", tgt->label, esp_err_to_name(e));
        err = "OTA begin failed"; goto fail;
    }
    have_handle = true;

    ESP_LOGW(TAG, "OTA: writing %ld bytes to %s (sequential erase-on-write)", sz, tgt->label);
    s_status = "writing\xE2\x80\xA6";
    for (;;) {
        size_t got = fread(s_buf, 1, sizeof s_buf, f);
        if (got == 0) break;                       /* EOF (or read error — caught by the size check) */
        e = esp_ota_write(h, s_buf, got);          /* erases this sector then writes it */
        if (e != ESP_OK) { ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(e));
                           err = "flash write failed"; goto fail; }
        written += got;
        s_progress = (int)((uint64_t)written * 100u / (size_t)sz);
        if ((written & 0x7FFFu) == 0) vTaskDelay(1);   /* ~every 32 KB: let the UI redraw the progress */
    }
    fclose(f); f = NULL;
    if (written != (size_t)sz) { err = "short read from card"; goto fail; }

    e = esp_ota_end(h);            /* validates the written image (checksum / signature) */
    have_handle = false;
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(e));
        err = (e == ESP_ERR_OTA_VALIDATE_FAILED) ? "image failed validation" : "finalize failed";
        goto fail;
    }
    e = esp_ota_set_boot_partition(tgt);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition(%s): %s", tgt->label, esp_err_to_name(e));
        err = "set-boot failed"; goto fail;
    }

    sd_drop();   /* release the card before the reboot */
    nocsif_reliability_ui_liveness_suspend(false);   /* flash work done — re-arm the UI watchdog */
    s_progress = 100;
    s_status = "installed \xE2\x80\x94 rebooting";
    s_state  = NOCSIF_OTA_SUCCESS;
    ESP_LOGW(TAG, "OTA installed to %s (%u bytes). Rebooting into it (PENDING_VERIFY — confirms once "
                  "the UI is up, else rolls back).", tgt->label, (unsigned)written);
    vTaskDelay(pdMS_TO_TICKS(1600));   /* let the UI paint SUCCESS before the reboot */
    esp_restart();
    return;

fail:
    s_status = err ? err : "install failed";
fail_kept_status:
    if (f)            fclose(f);
    if (have_handle)  esp_ota_abort(h);
    if (sd_held)      sd_drop();
    if (wdt_suspended) nocsif_reliability_ui_liveness_suspend(false);   /* re-arm the UI watchdog */
    s_state = NOCSIF_OTA_FAILED;
    ESP_LOGE(TAG, "OTA install aborted: %s", s_status);
}

static void ota_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        ota_req_t r = s_req;
        s_req = REQ_NONE;
        if (r == REQ_SCAN)         do_scan();
        else if (r == REQ_INSTALL) do_install();
    }
}

/* ---- §4.10 GitHub pull (web worker: PSRAM stack; network + card only, never flash) ----------- */

static const char *repo_get(void)
{
    if (!s_repo_loaded) {
        nocsif_settings_get_str("ota_repo", s_repo, sizeof s_repo, NOCSIF_OTA_REPO_DEFAULT);
        if (s_repo[0] == '\0' || strchr(s_repo, '/') == NULL) snprintf(s_repo, sizeof s_repo, "%s", NOCSIF_OTA_REPO_DEFAULT);
        s_repo_loaded = true;
    }
    return s_repo;
}

static void web_url(char *out, size_t n, const char *file)
{
    snprintf(out, n, OTA_WEB_BASE "%s" OTA_WEB_FOLDER "%s", repo_get(), file);
}

/* Bring the STA up for the pull: wake a parked radio (the Governor keeps intent on) and wait for a link. */
static bool web_wait_link(void)
{
    if (nocsif_wifi_connected()) return true;
    nocsif_wifi_request_enable(true);
    nocsif_gov_wifi_wake();
    for (int i = 0; i < OTA_WEB_LINK_WAIT_S * 2; i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (nocsif_wifi_connected()) { vTaskDelay(pdMS_TO_TICKS(1500)); return true; }   /* let DHCP settle */
    }
    return false;
}

/* Open an HTTPS GET through the CA bundle, from byte `offset` (a Range request when > 0 — raw GitHub
 * honours it with 206). Returns the client (headers fetched) or NULL; *status = HTTP code. */
static esp_http_client_handle_t web_open_at(const char *url, uint32_t offset, int *status, int64_t *content_len)
{
    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_GET,
        .timeout_ms        = 15000,
        .buffer_size       = 4096,
        .buffer_size_tx    = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return NULL;
    if (offset > 0) {
        char range[40];
        snprintf(range, sizeof range, "bytes=%u-", (unsigned)offset);
        esp_http_client_set_header(c, "Range", range);
    }
    esp_err_t e = esp_http_client_open(c, 0);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "web: open %s -> %s", url, esp_err_to_name(e));
        esp_http_client_cleanup(c);
        return NULL;
    }
    *content_len = esp_http_client_fetch_headers(c);
    *status = esp_http_client_get_status_code(c);
    return c;
}
static esp_http_client_handle_t web_open(const char *url, int *status, int64_t *content_len)
{
    return web_open_at(url, 0, status, content_len);
}

static void do_web_check(void)
{
    s_web_state  = NOCSIF_OTA_WEB_CHECKING;
    s_web_status = "checking\xE2\x80\xA6";
    s_web_ver[0] = '\0';
    s_web_sha[0] = '\0';
    s_web_size   = 0;
    if (!web_wait_link()) { s_web_status = "no WiFi link"; s_web_state = NOCSIF_OTA_WEB_FAILED; return; }

    char url[160];
    web_url(url, sizeof url, "manifest.json");
    ESP_LOGI(TAG, "web: GET %s", url);
    int status = 0; int64_t clen = 0;
    esp_http_client_handle_t c = web_open(url, &status, &clen);
    if (!c) { s_web_status = "GitHub unreachable"; s_web_state = NOCSIF_OTA_WEB_FAILED; return; }
    char *buf = heap_caps_calloc(1, 2048, MALLOC_CAP_SPIRAM);
    int total = 0, n;
    while (buf && total < 2047 && (n = esp_http_client_read(c, buf + total, 2047 - total)) > 0) total += n;
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (!buf) { s_web_status = "out of memory"; s_web_state = NOCSIF_OTA_WEB_FAILED; return; }
    if (status != 200 || total <= 0) {
        ESP_LOGW(TAG, "web: manifest status %d (%d bytes)", status, total);
        s_web_status = (status == 404) ? "no manifest in that repo" : "manifest fetch failed";
        s_web_state  = NOCSIF_OTA_WEB_FAILED;
        heap_caps_free(buf);
        return;
    }
    cJSON *j = cJSON_ParseWithLength(buf, (size_t)total);
    heap_caps_free(buf);
    const cJSON *ver  = j ? cJSON_GetObjectItemCaseSensitive(j, "version") : NULL;
    const cJSON *sha  = j ? cJSON_GetObjectItemCaseSensitive(j, "sha256")  : NULL;
    const cJSON *size = j ? cJSON_GetObjectItemCaseSensitive(j, "size")    : NULL;
    if (!cJSON_IsString(ver) || !cJSON_IsString(sha) || !cJSON_IsNumber(size) || strlen(sha->valuestring) != 64) {
        if (j) cJSON_Delete(j);
        s_web_status = "bad manifest";
        s_web_state  = NOCSIF_OTA_WEB_FAILED;
        return;
    }
    snprintf(s_web_ver, sizeof s_web_ver, "%s", ver->valuestring);
    for (int i = 0; i < 64; i++) {                      /* normalise to lower-case hex */
        char ch = sha->valuestring[i];
        s_web_sha[i] = (ch >= 'A' && ch <= 'F') ? (char)(ch - 'A' + 'a') : ch;
    }
    s_web_sha[64] = '\0';
    s_web_size = (uint32_t)size->valuedouble;
    cJSON_Delete(j);

    const char *run = nocsif_ota_running_version();
    bool same = strcmp(run, s_web_ver) == 0;
    ESP_LOGI(TAG, "web: published %s (%u bytes) vs running %s -> %s", s_web_ver, (unsigned)s_web_size, run,
             same ? "up to date" : "update available");
    s_web_status = same ? "up to date" : "update available";
    s_web_state  = same ? NOCSIF_OTA_WEB_UPTODATE : NOCSIF_OTA_WEB_AVAILABLE;
}

static void do_web_download(void)
{
    if (s_web_ver[0] == '\0' || s_web_size == 0) { s_web_status = "check first"; s_web_state = NOCSIF_OTA_WEB_FAILED; return; }
    s_web_state    = NOCSIF_OTA_WEB_DOWNLOADING;
    s_web_progress = 0;
    s_web_status   = "connecting\xE2\x80\xA6";
    if (!web_wait_link()) { s_web_status = "no WiFi link"; s_web_state = NOCSIF_OTA_WEB_FAILED; return; }

    /* Own the card for the whole transfer (away from File Share); the FAT lock is taken per chunk so the
     * rest of the watch keeps its short card accesses. */
    esp_err_t ce = nocsif_usb_gadget_claim_sd(2000);
    if (ce == ESP_ERR_INVALID_STATE) { s_web_status = "File Share has the card"; s_web_state = NOCSIF_OTA_WEB_FAILED; return; }
    if (ce != ESP_OK)                { s_web_status = "microSD unavailable";    s_web_state = NOCSIF_OTA_WEB_FAILED; return; }

    const char *part = NOCSIF_OTA_SD_DIR "/firmware.bin.part";
    FILE *f = NULL;
    if (nocsif_sdcard_lock(3000)) {
        mkdir("/sd/nocsif", 0777);
        mkdir(NOCSIF_OTA_SD_DIR, 0777);
        f = fopen(part, "wb");
        nocsif_sdcard_unlock();
    }
    if (!f) { nocsif_usb_gadget_release_sd(); s_web_status = "cannot write to the card"; s_web_state = NOCSIF_OTA_WEB_FAILED; return; }

    char url[160];
    web_url(url, sizeof url, "firmware.bin");
    ESP_LOGI(TAG, "web: GET %s (%u bytes expected)", url, (unsigned)s_web_size);
    uint8_t *buf = heap_caps_malloc(OTA_WEB_CHUNK, MALLOC_CAP_SPIRAM);
    const char *err = buf ? NULL : "out of memory";
    uint32_t total = 0;
    int      resumes = 0;
    esp_http_client_handle_t c = NULL;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    /* Stream in small reads written at once. A stalled / dropped connection is REOPENED at the byte offset
     * (HTTP Range) up to OTA_WEB_RESUMES times — WiFi's RX buffers live in the scarce internal pool and a
     * sustained TLS stream can starve them mid-transfer; the hash is fed in order, so a resume is exact. */
    while (!err) {
        if (c == NULL) {
            if (resumes > OTA_WEB_RESUMES) { err = "download kept stalling"; break; }
            if (total > 0) {
                ESP_LOGW(TAG, "web: stalled at %u/%u — resuming (try %d)", (unsigned)total, (unsigned)s_web_size, resumes);
                s_web_status = "resuming\xE2\x80\xA6";
                vTaskDelay(pdMS_TO_TICKS(1500));
                if (!web_wait_link()) { err = "no WiFi link"; break; }
            }
            int status = 0; int64_t clen = 0;
            c = web_open_at(url, total, &status, &clen);
            if (c == NULL) { resumes++; continue; }
            int want = (total == 0) ? 200 : 206;
            if (status != want) {
                err = (status == 404) ? "no firmware.bin in that repo" : (total == 0 ? "download failed" : "resume refused");
                break;
            }
            s_web_status = "downloading\xE2\x80\xA6";
        }
        int n = esp_http_client_read(c, (char *)buf, OTA_WEB_CHUNK);
        if (n < 0 || (n == 0 && total < s_web_size)) {      /* error / early EOF: drop the connection, resume */
            esp_http_client_close(c);
            esp_http_client_cleanup(c);
            c = NULL;
            resumes++;
            continue;
        }
        if (n == 0) break;                                    /* complete */
        mbedtls_sha256_update(&sha, buf, (size_t)n);
        if (!nocsif_sdcard_lock(3000)) { err = "card busy"; break; }
        size_t w = fwrite(buf, 1, (size_t)n, f);
        nocsif_sdcard_unlock();
        if (w != (size_t)n) { err = "card write failed (full?)"; break; }
        total += (uint32_t)n;
        s_web_progress = (int)((uint64_t)total * 100u / s_web_size);
        if (total > s_web_size) { err = "image larger than published"; break; }
        if (total == s_web_size) break;
    }
    if (c) { esp_http_client_close(c); esp_http_client_cleanup(c); }
    heap_caps_free(buf);
    if (nocsif_sdcard_lock(3000)) { fclose(f); nocsif_sdcard_unlock(); } else { fclose(f); }
    f = NULL;

    unsigned char digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    if (!err && total != s_web_size) err = "download incomplete";
    if (!err) {
        char hex[65];
        for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
        if (strcmp(hex, s_web_sha) != 0) { ESP_LOGE(TAG, "web: sha256 %s != published %s", hex, s_web_sha); err = "sha256 mismatch"; }
    }
    if (!err && nocsif_sdcard_lock(3000)) {                  /* replace the card copy atomically-ish */
        remove(NOCSIF_OTA_SD_PATH);
        if (rename(part, NOCSIF_OTA_SD_PATH) != 0) err = "cannot replace firmware.bin";
        nocsif_sdcard_unlock();
    } else if (!err) {
        err = "card busy";
    }
    if (err && nocsif_sdcard_lock(3000)) { remove(part); nocsif_sdcard_unlock(); }
    nocsif_usb_gadget_release_sd();

    if (err) {
        ESP_LOGE(TAG, "web: download aborted: %s (%u/%u bytes)", err, (unsigned)total, (unsigned)s_web_size);
        s_web_status = err;
        s_web_state  = NOCSIF_OTA_WEB_FAILED;
        return;
    }
    ESP_LOGW(TAG, "web: %s downloaded + verified (%u bytes, sha256 ok) -> %s", s_web_ver, (unsigned)total, NOCSIF_OTA_SD_PATH);
    s_web_progress = 100;
    s_web_status   = "downloaded " "\xC2\xB7" " verified";
    s_web_state    = NOCSIF_OTA_WEB_DOWNLOADED;
    nocsif_ota_request_sd_scan();                            /* the card line + Install pick it up */
}

static void web_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        web_req_t r = s_web_req;
        s_web_req = WEB_NONE;
        if (r == WEB_CHECK)         do_web_check();
        else if (r == WEB_DOWNLOAD) do_web_download();
        else if (r == WEB_SCAN)     do_scan();               /* card reads only — fine on a PSRAM stack */
    }
}

static bool web_task_ensure(void)
{
    if (s_web_task) return true;
    if (xTaskCreateWithCaps(web_task, "ota_web", OTA_WEB_STACK, NULL, 3, &s_web_task, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "failed to create the web worker");
        s_web_task = NULL;
        return false;
    }
    return true;
}

void nocsif_ota_request_web_check(void)
{
    if (nocsif_ota_web_busy() || s_state == NOCSIF_OTA_RUNNING || !web_task_ensure()) return;
    s_web_req = WEB_CHECK;
    xTaskNotifyGive(s_web_task);
}

void nocsif_ota_request_web_download(void)
{
    if (nocsif_ota_web_busy() || s_state == NOCSIF_OTA_RUNNING || !web_task_ensure()) return;
    if (s_web_state != NOCSIF_OTA_WEB_AVAILABLE && s_web_state != NOCSIF_OTA_WEB_DOWNLOADED) return;
    s_web_req = WEB_DOWNLOAD;
    xTaskNotifyGive(s_web_task);
}

nocsif_ota_web_state_t nocsif_ota_web_state(void) { return s_web_state; }
int         nocsif_ota_web_progress(void)         { return s_web_progress; }
const char *nocsif_ota_web_status_str(void)       { return s_web_status ? s_web_status : ""; }
const char *nocsif_ota_web_version(void)          { return s_web_ver; }
uint32_t    nocsif_ota_web_size(void)             { return s_web_size; }
bool        nocsif_ota_web_busy(void)
{
    return s_web_state == NOCSIF_OTA_WEB_CHECKING || s_web_state == NOCSIF_OTA_WEB_DOWNLOADING;
}
const char *nocsif_ota_repo(void)                 { return repo_get(); }
void nocsif_ota_set_repo(const char *repo)
{
    if (!repo || !repo[0] || strchr(repo, '/') == NULL) repo = NOCSIF_OTA_REPO_DEFAULT;
    snprintf(s_repo, sizeof s_repo, "%s", repo);
    s_repo_loaded = true;
    nocsif_settings_set_str("ota_repo", s_repo);
    s_web_state = NOCSIF_OTA_WEB_IDLE;                       /* a new source: the last answer is stale */
    s_web_status = "";
    s_web_ver[0] = '\0';
    s_web_size = 0;
}
const char *nocsif_ota_running_version(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    return app ? app->version : "?";
}

/* ---- public API ------------------------------------------------------------------------------ */

/* The INSTALL worker: its 8 KB stack must be internal (esp_ota_write runs with the cache disabled, so a
 * PSRAM stack would fault) and that 8 KB comes out of the pool WiFi's RX buffers need — so it is created
 * only when Install is tapped (§4.10: creating it at screen-open starved a GitHub download at 6 KB/s). */
static bool web_task_ensure(void);
static bool ota_task_ensure(void)
{
    if (s_task != NULL) return true;
    if (xTaskCreate(ota_task, "ota", 8192, NULL, 4, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "failed to create OTA task");
        s_task = NULL;
        return false;
    }
    return true;
}

esp_err_t nocsif_ota_init(void)
{
    return web_task_ensure() ? ESP_OK : ESP_ERR_NO_MEM;   /* the PSRAM worker (scan / check / download) */
}

void nocsif_ota_confirm(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (!run) return;
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            ESP_LOGI(TAG, "OTA image on %s confirmed valid — rollback cancelled", run->label);
        } else {
            ESP_LOGW(TAG, "esp_ota_mark_app_valid_cancel_rollback failed");
        }
    }
}

void nocsif_ota_request_sd_scan(void)
{
    if (s_state == NOCSIF_OTA_RUNNING || nocsif_ota_web_busy() || !web_task_ensure()) return;
    s_web_req = WEB_SCAN;                                    /* card reads → the PSRAM worker */
    xTaskNotifyGive(s_web_task);
}

void nocsif_ota_request_install_sd(void)
{
    if (s_state == NOCSIF_OTA_RUNNING || !s_sd_present) return;
    if (nocsif_power_batt_pct() < OTA_INSTALL_BATT_MIN && !nocsif_power_vbus_present()) {
        s_status = "battery under 30% " "\xE2\x80\x94" " plug in to install";   /* worker idle: safe to set */
        s_state  = NOCSIF_OTA_FAILED;
        return;
    }
    if (!ota_task_ensure()) {
        s_status = "no memory for the installer";
        s_state  = NOCSIF_OTA_FAILED;
        return;
    }
    s_req = REQ_INSTALL;
    xTaskNotifyGive(s_task);
}

nocsif_ota_state_t nocsif_ota_state(void)   { return s_state; }
int         nocsif_ota_progress(void)       { return s_progress; }
const char *nocsif_ota_status_str(void)     { return s_status ? s_status : ""; }
bool        nocsif_ota_sd_present(void)     { return s_sd_present; }
uint32_t    nocsif_ota_sd_size(void)        { return s_sd_size; }
const char *nocsif_ota_sd_version(void)     { return s_sd_ver; }

const char *nocsif_ota_running_label(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    return run ? run->label : "?";
}

bool nocsif_ota_pending_verify(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (!run) return false;
    esp_ota_img_states_t st;
    return esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
}
