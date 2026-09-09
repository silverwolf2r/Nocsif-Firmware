/*
 * NocSif — OTA update worker implementation. See ota.h for the public API.
 *
 * A worker task, woken by a task notification, runs SCAN (validate the card image and
 * publish its size/version) and INSTALL (stream it into the inactive OTA partition with
 * live progress, then reboot) off the LVGL thread. A second, GitHub-facing worker below
 * handles checking for and downloading updates over WiFi.
 *
 * Status fields are written only by the worker and read only by the UI poll, with no lock
 * needed — plain pointer/int reads and writes are atomic enough for this single-writer use.
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

#include <sys/stat.h>        /* mkdir, for creating the firmware folder on the card */

#include "freertos/idf_additions.h"   /* xTaskCreateWithCaps, for the web worker's PSRAM stack */
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "mbedtls/sha256.h"

#include "sdcard.h"          /* card FAT lock */
#include "usb_gadget.h"      /* claim/release the card away from USB-MSC */
#include "reliability.h"     /* pause the UI-liveness watchdog during flash writes */
#include "settings.h"        /* persisted "ota_repo" setting */
#include "wifi.h"            /* bring the STA link up for a download */
#include "governor.h"        /* wake a parked radio for a download */
#include "power.h"           /* battery/USB-power checks before installing */

static const char *TAG = "nocsif_ota";

/* ESP app-image layout: byte 0 is a fixed magic byte, and the app descriptor sits right
 * after the image header and first segment header. */
#define OTA_IMG_MAGIC        0xE9u
#define OTA_APP_DESC_OFFSET  32
#define OTA_HDR_READ         (OTA_APP_DESC_OFFSET + sizeof(esp_app_desc_t))   /* bytes to read for a header check */

/* Streaming read/write chunk size; the whole image is never buffered at once. */
#define OTA_CHUNK            4096

typedef enum { REQ_NONE = 0, REQ_SCAN, REQ_INSTALL } ota_req_t;

static TaskHandle_t                 s_task;
static volatile ota_req_t           s_req;
static volatile nocsif_ota_state_t  s_state    = NOCSIF_OTA_IDLE;
static volatile int                 s_progress;                   /* 0..100 */
static const char * volatile        s_status   = "";              /* always a string literal */
static volatile bool                s_sd_present;
static volatile uint32_t            s_sd_size;
static char                         s_sd_ver[32];                 /* written by the worker, read by the UI */

/* Aligned, DMA-capable buffer so the card read streams straight into it without the SPI
 * driver needing to allocate a bounce buffer. Worker-task use only. */
static uint8_t                      s_buf[OTA_CHUNK] __attribute__((aligned(64)));  /* worker-only */

/* ---- GitHub download worker state (worker writes, UI reads plain) --------------------------- */
typedef enum { WEB_NONE = 0, WEB_CHECK, WEB_DOWNLOAD, WEB_SCAN } web_req_t;
static TaskHandle_t                       s_web_task;
static volatile web_req_t                 s_web_req;
static volatile nocsif_ota_web_state_t    s_web_state = NOCSIF_OTA_WEB_IDLE;
static volatile int                       s_web_progress;
static const char * volatile              s_web_status = "";      /* always a string literal */
static char                               s_web_ver[32];          /* published version, from the manifest */
static char                               s_web_sha[65];          /* published sha256, lower-case hex */
static volatile uint32_t                  s_web_size;             /* published image size in bytes */
static char                               s_repo[64];             /* "owner/repo", loaded on first use */
static bool                               s_repo_loaded;
#define OTA_WEB_STACK      16384          /* generous: TLS + JSON parsing + sha256 all run here */
#define OTA_WEB_CHUNK      8192           /* read size for the download stream */
#define OTA_WEB_RESUMES    5              /* how many times a stalled download may reconnect */
#define OTA_WEB_LINK_WAIT_S 20            /* how long to wait for the WiFi link after waking it */
#define OTA_WEB_BASE       "https://raw.githubusercontent.com/"
#define OTA_WEB_FOLDER     "/main/nocsif/firmware/"
#define OTA_INSTALL_BATT_MIN 30           /* minimum battery percent to allow an install without USB power */

/* ---- helpers --------------------------------------------------------------------------------- */

/* Open the update image on the card, trying the current path then the legacy one. */
static FILE *sd_open_image(const char **used)
{
    FILE *f = fopen(NOCSIF_OTA_SD_PATH, "rb");
    if (f) { if (used) *used = NOCSIF_OTA_SD_PATH; return f; }
    f = fopen(NOCSIF_OTA_SD_PATH_OLD, "rb");
    if (f && used) *used = NOCSIF_OTA_SD_PATH_OLD;
    return f;
}

/* Check that an open file is a valid ESP app image, and optionally read out its version.
 * Leaves the file position past the header, so callers rewind before streaming. */
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

/* Claim both the card (away from USB-MSC) and the FAT lock. On failure, sets s_status and
 * s_state to fail_state and returns false having taken neither. */
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

    /* Pause the UI-liveness watchdog for the duration of the flash write below: erasing/writing
     * flash disables the cache the LVGL pet-timer runs from, so it can't pet even though nothing
     * is actually wrong — left running, the watchdog would panic mid-install. Every exit path
     * below re-arms it (except success, which reboots anyway). */
    nocsif_reliability_ui_liveness_suspend(true);
    wdt_suspended = true;

    if (!sd_grab(NOCSIF_OTA_FAILED)) goto fail_kept_status;   /* sd_grab already set the status message */
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
    fseek(f, 0, SEEK_SET);   /* rewind past the header check before streaming */

    /* Sequential-write mode erases each sector lazily inside esp_ota_write instead of erasing
     * the whole slot up front — a single multi-second blocking erase would freeze the UI and
     * trip the watchdog, whereas small per-chunk erases let the progress bar keep animating. */
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
        if (got == 0) break;                       /* end of file (a short read is caught below) */
        e = esp_ota_write(h, s_buf, got);          /* erases then writes this sector */
        if (e != ESP_OK) { ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(e));
                           err = "flash write failed"; goto fail; }
        written += got;
        s_progress = (int)((uint64_t)written * 100u / (size_t)sz);
        if ((written & 0x7FFFu) == 0) vTaskDelay(1);   /* yield periodically so the UI can redraw */
    }
    fclose(f); f = NULL;
    if (written != (size_t)sz) { err = "short read from card"; goto fail; }

    e = esp_ota_end(h);            /* validates the image that was just written */
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
    nocsif_reliability_ui_liveness_suspend(false);   /* done with flash work — re-arm the watchdog */
    s_progress = 100;
    s_status = "installed \xE2\x80\x94 rebooting";
    s_state  = NOCSIF_OTA_SUCCESS;
    ESP_LOGW(TAG, "OTA installed to %s (%u bytes). Rebooting into it (PENDING_VERIFY — confirms once "
                  "the UI is up, else rolls back).", tgt->label, (unsigned)written);
    vTaskDelay(pdMS_TO_TICKS(1600));   /* give the UI time to show success before rebooting */
    esp_restart();
    return;

fail:
    s_status = err ? err : "install failed";
fail_kept_status:
    if (f)            fclose(f);
    if (have_handle)  esp_ota_abort(h);
    if (sd_held)      sd_drop();
    if (wdt_suspended) nocsif_reliability_ui_liveness_suspend(false);   /* always re-arm on the way out */
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

/* ---- GitHub download worker: touches network + card only, never writes flash ----------------- */

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

/* Make sure the WiFi station is connected before a download, waking it if it's parked. */
static bool web_wait_link(void)
{
    if (nocsif_wifi_connected()) return true;
    nocsif_wifi_request_enable(true);
    nocsif_gov_wifi_wake();
    for (int i = 0; i < OTA_WEB_LINK_WAIT_S * 2; i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (nocsif_wifi_connected()) { vTaskDelay(pdMS_TO_TICKS(1500)); return true; }   /* give DHCP a moment */
    }
    return false;
}

/* Open an HTTPS GET, optionally resuming from a byte offset via a Range header. Returns the
 * open client with headers already fetched, or NULL on failure; *status gets the HTTP code. */
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
    for (int i = 0; i < 64; i++) {                      /* fold to lower-case hex */
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

    /* Claim the card for the whole transfer, but only hold the FAT lock per chunk so other
     * short card accesses elsewhere in the firmware aren't blocked out for the whole download. */
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
    /* Stream small reads straight to the card. A dropped connection is reopened at the current
     * byte offset (via Range) up to OTA_WEB_RESUMES times; since the hash is fed strictly in
     * order, a resumed download still produces the correct checksum. */
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
        if (n < 0 || (n == 0 && total < s_web_size)) {      /* read error or early EOF: drop and resume */
            esp_http_client_close(c);
            esp_http_client_cleanup(c);
            c = NULL;
            resumes++;
            continue;
        }
        if (n == 0) break;                                    /* download finished */
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
    if (!err && nocsif_sdcard_lock(3000)) {                  /* swap the verified download into place */
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
    nocsif_ota_request_sd_scan();                            /* refresh the card status for Install */
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
        else if (r == WEB_SCAN)     do_scan();               /* card-only, fine to run on this worker */
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
    s_web_state = NOCSIF_OTA_WEB_IDLE;                       /* changing the source invalidates the last check */
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

/* The install worker needs an internal (non-PSRAM) stack since esp_ota_write runs with the
 * flash cache disabled; it's only created when an install is actually requested, to avoid
 * competing with WiFi's internal RAM needs while it isn't needed. */
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
    return web_task_ensure() ? ESP_OK : ESP_ERR_NO_MEM;   /* starts the scan/check/download worker */
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
    s_web_req = WEB_SCAN;                                    /* handled by the card/network worker */
    xTaskNotifyGive(s_web_task);
}

void nocsif_ota_request_install_sd(void)
{
    if (s_state == NOCSIF_OTA_RUNNING || !s_sd_present) return;
    if (nocsif_power_batt_pct() < OTA_INSTALL_BATT_MIN && !nocsif_power_vbus_present()) {
        s_status = "battery under 30% " "\xE2\x80\x94" " plug in to install";   /* worker is idle, safe to set directly */
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
