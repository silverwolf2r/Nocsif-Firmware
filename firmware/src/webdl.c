/*
 * NocSif — WiFi "Download to SD" worker. See webdl.h for the design notes.
 *
 * Modeled on ota.c's §4.10 web worker (PSRAM stack; network + card only, never flash): bring the STA
 * link up, claim the card away from File Share for the transfer, stream the body in small reads written
 * under the /sd lock to a ".part", then rename into place. Progress + status are single-writer (this
 * worker) / single-reader (the UI poll) plain reads.
 */
#include "webdl.h"

#include <string.h>
#include <stdio.h>
#include <sys/stat.h>          /* stat / mkdir */
#include <unistd.h>            /* — */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"   /* xTaskCreateWithCaps — PSRAM worker stack */

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"    /* HTTPS through the mbedTLS CA bundle (same as ota.c) */

#include "wifi.h"
#include "sdcard.h"
#include "usb_gadget.h"
#include "reliability.h"

static const char *TAG = "webdl";

#define WEBDL_STACK      16384        /* PSRAM: TLS handshake + streaming reads on this task */
#define WEBDL_PRIO       5
#define WEBDL_CHUNK      4096         /* PSRAM read buffer                                   */
#define WEBDL_LINK_MS    15000        /* how long to wait for a STA link on start            */
#define WEBDL_NAME_MAX   96
#define WEBDL_PATH_MAX   256
#define WEBDL_URL_MAX    512

/* ---- state (worker writes, UI reads plain) --------------------------------------------------- */
static TaskHandle_t          s_task;
static volatile nocsif_webdl_state_t s_state = NOCSIF_WEBDL_IDLE;
static volatile int          s_progress;                 /* 0..100, or -1 if unknown length */
static const char           *s_status = "";
static char                  s_saved[WEBDL_NAME_MAX];     /* basename of the last saved file  */
static char                  s_url[WEBDL_URL_MAX];        /* the requested URL (worker-read)  */
static volatile bool         s_req;                       /* a download is queued             */

/* ---- filename helpers ------------------------------------------------------------------------ */

/* FAT-safe: keep letters/digits and a small punct set; everything else -> '_'. */
static char sanitize_ch(char c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
    if (c == '.' || c == '-' || c == '_' || c == ' ' || c == '(' || c == ')') return c;
    return '_';
}

/* Derive a filename from the URL's last path segment (query/fragment stripped, sanitized). */
static void derive_name(const char *url, char *out, size_t n)
{
    const char *q   = strpbrk(url, "?#");
    const char *end = q ? q : url + strlen(url);
    const char *seg = end;
    while (seg > url && *(seg - 1) != '/') seg--;         /* back up to the last '/'          */
    size_t len = (size_t)(end - seg);
    size_t o = 0;
    for (size_t i = 0; i < len && o < n - 1; i++) {
        char c = sanitize_ch(seg[i]);
        out[o++] = c;
    }
    out[o] = '\0';
    /* trim leading/trailing spaces + dots that FAT dislikes */
    while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '.')) out[--o] = '\0';
    if (out[0] == '\0') strlcpy(out, "download.bin", n);
}

static bool path_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* Resolve a non-colliding path in /sd/storage: name, then "base (1).ext", "base (2).ext", … Must be
 * called with the /sd lock held (it stats candidate paths). */
static void unique_path(const char *name, char *out, size_t n)
{
    snprintf(out, n, NOCSIF_WEBDL_DIR "/%s", name);
    if (!path_exists(out)) return;

    /* split base / extension (extension = last dot that isn't the first char) */
    char base[WEBDL_NAME_MAX];
    char ext[WEBDL_NAME_MAX];
    const char *dot = strrchr(name, '.');
    if (dot && dot != name) {
        size_t bl = (size_t)(dot - name);
        if (bl >= sizeof base) bl = sizeof base - 1;
        memcpy(base, name, bl); base[bl] = '\0';
        strlcpy(ext, dot, sizeof ext);       /* includes the leading '.' */
    } else {
        strlcpy(base, name, sizeof base);
        ext[0] = '\0';
    }
    for (int k = 1; k < 1000; k++) {
        snprintf(out, n, NOCSIF_WEBDL_DIR "/%s (%d)%s", base, k, ext);
        if (!path_exists(out)) return;
    }
    /* give up gracefully — overwrite the base name */
    snprintf(out, n, NOCSIF_WEBDL_DIR "/%s", name);
}

/* ---- the download ---------------------------------------------------------------------------- */

static bool wait_link(void)
{
    if (nocsif_wifi_connected()) return true;
    nocsif_wifi_request_enable(true);
    int waited = 0;
    while (!nocsif_wifi_connected() && waited < WEBDL_LINK_MS) {
        vTaskDelay(pdMS_TO_TICKS(250));
        waited += 250;
    }
    return nocsif_wifi_connected();
}

static void do_download(void)
{
    s_state    = NOCSIF_WEBDL_RUNNING;
    s_progress = -1;
    s_status   = "connecting\xE2\x80\xA6";

    if (s_url[0] == '\0') { s_status = "no URL"; s_state = NOCSIF_WEBDL_FAILED; return; }
    if (!wait_link())     { s_status = "no WiFi link"; s_state = NOCSIF_WEBDL_FAILED; return; }

    /* Own the card for the whole transfer (away from File Share); the FAT lock is taken per chunk. */
    esp_err_t ce = nocsif_usb_gadget_claim_sd(2000);
    if (ce == ESP_ERR_INVALID_STATE) { s_status = "File Share has the card"; s_state = NOCSIF_WEBDL_FAILED; return; }
    if (ce != ESP_OK)                { s_status = "microSD unavailable";     s_state = NOCSIF_WEBDL_FAILED; return; }

    char name[WEBDL_NAME_MAX];
    char dest[WEBDL_PATH_MAX];
    char part[WEBDL_PATH_MAX];
    derive_name(s_url, name, sizeof name);

    FILE *f = NULL;
    if (nocsif_sdcard_lock(3000)) {
        mkdir("/sd/nocsif", 0777);           /* parent (shared with the rest of the app) */
        mkdir(NOCSIF_WEBDL_DIR, 0777);       /* create /sd/nocsif/storage on first use (ignore EEXIST) */
        unique_path(name, dest, sizeof dest);
        snprintf(part, sizeof part, "%s.part", dest);
        f = fopen(part, "wb");
        nocsif_sdcard_unlock();
    }
    if (!f) { nocsif_usb_gadget_release_sd(); s_status = "cannot write to the card"; s_state = NOCSIF_WEBDL_FAILED; return; }

    ESP_LOGI(TAG, "GET %s -> %s", s_url, dest);
    s_status = "downloading\xE2\x80\xA6";

    esp_http_client_config_t cfg = {
        .url               = s_url,
        .method            = HTTP_METHOD_GET,
        .timeout_ms        = 20000,
        .buffer_size       = 2048,
        .buffer_size_tx    = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,   /* https; harmless for http */
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    uint8_t *buf = heap_caps_malloc(WEBDL_CHUNK, MALLOC_CAP_SPIRAM);
    const char *err = NULL;
    int64_t clen = 0;
    uint32_t total = 0;

    if (!c || !buf) {
        err = "out of memory";
    } else if (esp_http_client_open(c, 0) != ESP_OK) {
        err = "connection failed";
    } else {
        clen = esp_http_client_fetch_headers(c);          /* -1 / 0 if chunked / unknown */
        int status = esp_http_client_get_status_code(c);
        if (status != 200) {
            err = (status == 404) ? "not found (404)" :
                  (status == 403) ? "forbidden (403)" : "server error";
            ESP_LOGW(TAG, "http status %d", status);
        }
    }

    while (!err) {
        int n = esp_http_client_read(c, (char *)buf, WEBDL_CHUNK);
        if (n < 0) { err = "read error"; break; }
        if (n == 0) break;                                 /* complete (EOF) */
        if (!nocsif_sdcard_lock(3000)) { err = "card busy"; break; }
        size_t w = fwrite(buf, 1, (size_t)n, f);
        nocsif_sdcard_unlock();
        if (w != (size_t)n) { err = "card write failed (full?)"; break; }
        total += (uint32_t)n;
        if (clen > 0) s_progress = (int)((uint64_t)total * 100u / (uint64_t)clen);
    }

    if (c) { esp_http_client_close(c); esp_http_client_cleanup(c); }
    if (buf) heap_caps_free(buf);
    if (nocsif_sdcard_lock(3000)) { fclose(f); nocsif_sdcard_unlock(); } else { fclose(f); }
    f = NULL;

    if (!err && clen > 0 && total != (uint32_t)clen) err = "download incomplete";
    if (!err && total == 0)                          err = "empty download";

    if (!err && nocsif_sdcard_lock(3000)) {
        remove(dest);
        if (rename(part, dest) != 0) err = "cannot save file";
        nocsif_sdcard_unlock();
    } else if (!err) {
        err = "card busy";
    }
    if (err && nocsif_sdcard_lock(3000)) { remove(part); nocsif_sdcard_unlock(); }
    nocsif_usb_gadget_release_sd();

    if (err) {
        ESP_LOGE(TAG, "download aborted: %s (%u bytes)", err, (unsigned)total);
        s_status = err;
        s_state  = NOCSIF_WEBDL_FAILED;
        return;
    }
    const char *base = strrchr(dest, '/');
    strlcpy(s_saved, base ? base + 1 : dest, sizeof s_saved);
    ESP_LOGW(TAG, "downloaded %u bytes -> %s", (unsigned)total, dest);
    s_progress = 100;
    s_status   = "done";
    s_state    = NOCSIF_WEBDL_DONE;
}

static void webdl_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_req) {
            s_req = false;
            do_download();
        }
    }
}

/* ---- public API ------------------------------------------------------------------------------ */
esp_err_t nocsif_webdl_init(void)
{
    if (s_task) return ESP_OK;
    if (nocsif_reliability_safe_mode()) {
        ESP_LOGW(TAG, "safe mode — downloader disabled");
        return ESP_OK;
    }
    /* PSRAM stack: network + card only, never flash / DMA from its own stack (same rule as ota.c's
     * web worker), so the 16 KB does not compete for the scarce internal-DMA hole. */
    if (xTaskCreateWithCaps(webdl_task, "nocsif_webdl", WEBDL_STACK, NULL, WEBDL_PRIO, &s_task,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "worker create failed");
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "downloader ready (idle until a URL is posted)");
    return ESP_OK;
}

bool nocsif_webdl_available(void) { return s_task != NULL; }

void nocsif_webdl_start(const char *url)
{
    if (s_task == NULL || url == NULL || url[0] == '\0') return;
    if (s_state == NOCSIF_WEBDL_RUNNING) return;          /* one at a time */
    strlcpy(s_url, url, sizeof s_url);
    s_progress = -1;
    s_state    = NOCSIF_WEBDL_RUNNING;                    /* latch immediately so a double-tap is a no-op */
    s_status   = "starting\xE2\x80\xA6";
    s_req      = true;
    xTaskNotifyGive(s_task);
}

bool nocsif_webdl_busy(void) { return s_state == NOCSIF_WEBDL_RUNNING; }

nocsif_webdl_state_t nocsif_webdl_state(void) { return s_state; }
int          nocsif_webdl_progress(void)   { return s_progress; }
const char  *nocsif_webdl_status(void)     { return s_status[0] ? s_status : "ready"; }
const char  *nocsif_webdl_saved_name(void) { return s_saved; }
