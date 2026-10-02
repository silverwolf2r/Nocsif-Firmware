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
#include "netstore.h"          /* pair a grabbed portal page with its network in the Networks folder */

static const char *TAG = "webdl";

#define WEBDL_PORTAL_DIR "/sd/nocsif/wifi/portals"   /* same folder the Web Portal picker lists */

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
static volatile uint32_t     s_bytes;                     /* bytes written so far (resume_off + body) */
static volatile uint64_t     s_full;                      /* total target size in bytes, 0 if unknown */
static const char           *s_status = "";
static char                  s_saved[WEBDL_NAME_MAX];     /* basename of the last saved file  */
static char                  s_url[WEBDL_URL_MAX];        /* the requested URL (worker-read)  */
static volatile bool         s_req;                       /* a download is queued             */
static volatile bool         s_cancel;                    /* user asked to stop the in-flight download */
static volatile bool         s_portal_req;                /* a captive-portal grab is queued  */
static char                  s_portal_ssid[33];           /* SSID to name + associate the page */
static char                  s_dir[WEBDL_PATH_MAX] = NOCSIF_WEBDL_DIR;  /* destination dir for the download */
static volatile bool         s_resume;                    /* resume a prior .part via an HTTP Range request */

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

/* Resolve a non-colliding path in `dir`: name, then "base (1).ext", "base (2).ext", … Must be
 * called with the /sd lock held (it stats candidate paths). */
static void unique_path(const char *dir, const char *name, char *out, size_t n)
{
    snprintf(out, n, "%s/%s", dir, name);
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
        snprintf(out, n, "%s/%s (%d)%s", dir, base, k, ext);
        if (!path_exists(out)) return;
    }
    /* give up gracefully — overwrite the base name */
    snprintf(out, n, "%s/%s", dir, name);
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
    s_bytes    = 0;
    s_full     = 0;
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

    /* Resume (big OS images over WiFi): a prior .part at a STABLE path is continued with an HTTP Range
     * request. Non-resume downloads keep the collision-avoiding unique name + a fresh ".part". */
    off_t resume_off = 0;
    FILE *f = NULL;
    if (nocsif_sdcard_lock(3000)) {
        mkdir("/sd/nocsif", 0777);           /* parent (shared with the rest of the app) */
        mkdir(s_dir, 0777);                  /* create the destination dir on first use (ignore EEXIST) */
        if (s_resume) {
            snprintf(dest, sizeof dest, "%s/%s", s_dir, name);
            snprintf(part, sizeof part, "%s.part", dest);
            struct stat st;
            if (stat(part, &st) == 0 && st.st_size > 0) resume_off = st.st_size;
        } else {
            unique_path(s_dir, name, dest, sizeof dest);
            snprintf(part, sizeof part, "%s.part", dest);
        }
        f = fopen(part, resume_off > 0 ? "ab" : "wb");
        nocsif_sdcard_unlock();
    }
    if (!f) { nocsif_usb_gadget_release_sd(); s_status = "cannot write to the card"; s_state = NOCSIF_WEBDL_FAILED; return; }

    ESP_LOGI(TAG, "GET %s -> %s (resume@%lld)", s_url, dest, (long long)resume_off);
    s_status = "downloading\xE2\x80\xA6";

    esp_http_client_config_t cfg = {
        .url               = s_url,
        .method            = HTTP_METHOD_GET,
        .timeout_ms        = 30000,                   /* slack for slow reads under recon/int-DMA contention */
        .buffer_size       = 2048,
        .buffer_size_tx    = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,   /* https; harmless for http */
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c && resume_off > 0) {
        char range[48];
        snprintf(range, sizeof range, "bytes=%lld-", (long long)resume_off);
        esp_http_client_set_header(c, "Range", range);
    }
    uint8_t *buf = heap_caps_malloc(WEBDL_CHUNK, MALLOC_CAP_SPIRAM);
    const char *err = NULL;
    int64_t clen = 0;
    uint64_t full = 0;                                  /* total target size (resume_off + body) */
    uint32_t total = (uint32_t)resume_off;             /* bytes on disk so far (fits: images < 4 GB) */
    uint32_t last_log = total;                          /* last console throughput log mark */

    if (!c || !buf) {
        err = "out of memory";
    } else if (esp_http_client_open(c, 0) != ESP_OK) {
        err = "connection failed";
    } else {
        clen = esp_http_client_fetch_headers(c);          /* -1 / 0 if chunked / unknown */
        int status = esp_http_client_get_status_code(c);
        /* Follow redirects (e.g. download.tails.net -> mirror pool -> a mirror). The manual
         * open/fetch_headers/read streaming flow does NOT auto-follow, so chase the Location header
         * ourselves: set_redirection updates the URL, then close + re-open. Any Range header we set
         * persists across the hop. Bounded to avoid a redirect loop. */
        for (int hops = 0; !err && hops < 6 &&
             (status == 301 || status == 302 || status == 303 || status == 307 || status == 308); hops++) {
            ESP_LOGI(TAG, "redirect %d (status %d) — following Location", hops + 1, status);
            esp_http_client_set_redirection(c);
            esp_http_client_close(c);
            if (esp_http_client_open(c, 0) != ESP_OK) { err = "connection failed"; break; }
            clen   = esp_http_client_fetch_headers(c);
            status = esp_http_client_get_status_code(c);
        }
        if (err) {
            /* redirect handling already set the error */
        } else if (resume_off > 0 && status == 200) {
            /* server ignored the Range (sent the whole file) — restart from scratch to avoid corruption */
            ESP_LOGW(TAG, "resume: server ignored Range (200) — restarting");
            if (nocsif_sdcard_lock(3000)) { fclose(f); f = fopen(part, "wb"); nocsif_sdcard_unlock(); }
            else { fclose(f); f = NULL; }
            resume_off = 0;
            total = 0;
            if (!f) err = "cannot write to the card";
        } else if (status != 200 && status != 206) {
            err = (status == 404) ? "not found (404)" :
                  (status == 403) ? "forbidden (403)" : "server error";
            ESP_LOGW(TAG, "http status %d", status);
        }
        if (!err && clen > 0) full = (uint64_t)resume_off + (uint64_t)clen;   /* 206: clen=remainder */
        s_full  = full;
        s_bytes = total;
    }

    while (!err) {
        if (s_cancel) break;                               /* user cancelled: stop reading (keeps .part) */
        int n = esp_http_client_read(c, (char *)buf, WEBDL_CHUNK);
        if (n < 0) { err = "read error"; break; }
        if (n == 0) break;                                 /* complete (EOF) */
        if (!nocsif_sdcard_lock(3000)) { err = "card busy"; break; }
        size_t w = fwrite(buf, 1, (size_t)n, f);
        nocsif_sdcard_unlock();
        if (w != (size_t)n) { err = "card write failed (full?)"; break; }
        total += (uint32_t)n;
        s_bytes = total;
        if (full > 0) s_progress = (int)((uint64_t)total * 100u / full);
        if (total - last_log >= 4u * 1024u * 1024u) {      /* ~4 MB console throughput log */
            last_log = total;
            ESP_LOGI(TAG, "downloading %u / %llu KB (%d%%)", (unsigned)(total / 1024),
                     (unsigned long long)(full / 1024), s_progress);
        }
    }

    if (c) { esp_http_client_close(c); esp_http_client_cleanup(c); }
    if (buf) heap_caps_free(buf);
    if (f) { if (nocsif_sdcard_lock(3000)) { fclose(f); nocsif_sdcard_unlock(); } else { fclose(f); } }
    f = NULL;

    if (!err && full > 0 && (uint64_t)total != full) err = "download incomplete";
    if (!err && total == 0)                          err = "empty download";

    /* On an incomplete resumable transfer, KEEP the .part so the next run continues it. Other failures
     * discard the partial. */
    bool keep_part = (err != NULL && s_resume && total > 0);

    if (!err && nocsif_sdcard_lock(3000)) {
        remove(dest);
        if (rename(part, dest) != 0) err = "cannot save file";
        nocsif_sdcard_unlock();
    } else if (!err) {
        err = "card busy";
    }
    if (err && !keep_part && nocsif_sdcard_lock(3000)) { remove(part); nocsif_sdcard_unlock(); }
    nocsif_usb_gadget_release_sd();

    if (err) {
        ESP_LOGE(TAG, "download aborted: %s (%u bytes)%s", err, (unsigned)total,
                 keep_part ? " — .part kept for resume" : "");
        s_status = keep_part ? "interrupted — tap Download to resume" : err;
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

/* ---- captive-portal grab (GET http://<gateway>/ -> /sd/nocsif/wifi/portals/<ssid>.html) ------- *
 * Reuses the same PSRAM worker / claim-card / per-chunk-lock discipline as do_download, but writes a
 * fixed SSID-named page into the Web Portal folder and pairs it with the network afterwards. Meant to be
 * called while the watch is CONNECTED to the target network (the caller gates on nocsif_wifi_connected). */
static void do_portal_grab(void)
{
    s_state    = NOCSIF_WEBDL_RUNNING;
    s_progress = -1;
    s_status   = "connecting\xE2\x80\xA6";

    if (s_portal_ssid[0] == '\0') { s_status = "no network";  s_state = NOCSIF_WEBDL_FAILED; return; }
    if (!nocsif_wifi_connected()) { s_status = "no WiFi link"; s_state = NOCSIF_WEBDL_FAILED; return; }
    const char *gw = nocsif_wifi_gateway_str();
    if (!gw || gw[0] == '\0')     { s_status = "no gateway";   s_state = NOCSIF_WEBDL_FAILED; return; }
    char url[80];
    snprintf(url, sizeof url, "http://%s/", gw);

    esp_err_t ce = nocsif_usb_gadget_claim_sd(2000);
    if (ce == ESP_ERR_INVALID_STATE) { s_status = "File Share has the card"; s_state = NOCSIF_WEBDL_FAILED; return; }
    if (ce != ESP_OK)                { s_status = "microSD unavailable";     s_state = NOCSIF_WEBDL_FAILED; return; }

    char name[WEBDL_NAME_MAX];
    size_t o = 0;
    for (size_t i = 0; s_portal_ssid[i] && o < sizeof name - 6; i++) name[o++] = sanitize_ch(s_portal_ssid[i]);
    name[o] = '\0';
    while (o > 0 && (name[o - 1] == ' ' || name[o - 1] == '.')) name[--o] = '\0';
    if (name[0] == '\0') strlcpy(name, "portal", sizeof name);
    strlcat(name, ".html", sizeof name);

    char dest[WEBDL_PATH_MAX], part[WEBDL_PATH_MAX];
    FILE *f = NULL;
    if (nocsif_sdcard_lock(3000)) {
        mkdir("/sd/nocsif", 0777);
        mkdir("/sd/nocsif/wifi", 0777);
        mkdir(WEBDL_PORTAL_DIR, 0777);
        snprintf(dest, sizeof dest, WEBDL_PORTAL_DIR "/%s", name);
        snprintf(part, sizeof part, "%s.part", dest);
        f = fopen(part, "wb");
        nocsif_sdcard_unlock();
    }
    if (!f) { nocsif_usb_gadget_release_sd(); s_status = "cannot write to the card"; s_state = NOCSIF_WEBDL_FAILED; return; }

    ESP_LOGI(TAG, "portal GET %s -> %s", url, dest);
    s_status = "fetching portal\xE2\x80\xA6";

    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_GET,
        .timeout_ms        = 12000,
        .buffer_size       = 2048,
        .buffer_size_tx    = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,   /* harmless for http; a portal may 302 to https */
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    uint8_t *buf = heap_caps_malloc(WEBDL_CHUNK, MALLOC_CAP_SPIRAM);
    const char *err = NULL;
    uint32_t total = 0;

    if (!c || !buf) {
        err = "out of memory";
    } else if (esp_http_client_open(c, 0) != ESP_OK) {
        err = "connection failed";
    } else {
        esp_http_client_fetch_headers(c);             /* a captive portal usually 200s the page body */
    }

    while (!err) {
        int n = esp_http_client_read(c, (char *)buf, WEBDL_CHUNK);
        if (n < 0) { err = "read error"; break; }
        if (n == 0) break;
        if (!nocsif_sdcard_lock(3000)) { err = "card busy"; break; }
        size_t w = fwrite(buf, 1, (size_t)n, f);
        nocsif_sdcard_unlock();
        if (w != (size_t)n) { err = "card write failed"; break; }
        total += (uint32_t)n;
        if (total > 256u * 1024u) break;              /* portals are small — cap a runaway body */
    }

    if (c) { esp_http_client_close(c); esp_http_client_cleanup(c); }
    if (buf) heap_caps_free(buf);
    if (nocsif_sdcard_lock(3000)) { fclose(f); nocsif_sdcard_unlock(); } else { fclose(f); }

    if (!err && total == 0) err = "empty page";
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
        ESP_LOGE(TAG, "portal grab aborted: %s (%u bytes)", err, (unsigned)total);
        s_status = err;
        s_state  = NOCSIF_WEBDL_FAILED;
        return;
    }
    strlcpy(s_saved, name, sizeof s_saved);
    nocsif_net_set_portal(s_portal_ssid, name);       /* pair the page with the network file */
    ESP_LOGW(TAG, "portal %u bytes -> %s (paired with %s)", (unsigned)total, dest, s_portal_ssid);
    s_progress = 100;
    s_status   = "portal saved";
    s_state    = NOCSIF_WEBDL_DONE;
}

static void webdl_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_portal_req) {
            s_portal_req = false;
            do_portal_grab();
        } else if (s_req) {
            s_req = false;
            /* Auto-retry + resume: a big OS image over a contended WiFi link (concurrent recon — a LoRa
             * band survey, GPS cycling — fragments the scarce int-DMA the lean WiFi RX needs) can stall and
             * abort mid-transfer. Re-run do_download (it resumes from the .part via HTTP Range) as long as
             * each attempt makes progress; give up only after several consecutive no-progress attempts.
             * Non-resumable downloads (Download to SD) are left as a single attempt. */
            uint32_t prev = 0;
            int stalls = 0;
            for (;;) {
                do_download();
                if (s_cancel) {                              /* user hit Cancel: stop (the .part is kept) */
                    s_cancel = false;
                    s_status = "cancelled";
                    s_state  = NOCSIF_WEBDL_IDLE;
                    break;
                }
                if (s_state == NOCSIF_WEBDL_DONE || !s_resume) {
                    break;                                   /* completed, or not a resumable image */
                }
                bool progressed = (s_bytes > prev);
                prev = s_bytes;
                if (progressed) {
                    stalls = 0;
                } else if (++stalls >= 5) {
                    ESP_LOGE(TAG, "download gave up: 5 attempts with no progress (stuck at %u KB)",
                             (unsigned)(s_bytes / 1024));
                    break;                                   /* genuinely stuck */
                }
                ESP_LOGW(TAG, "download stalled at %u KB — auto-resuming (no-progress streak %d)",
                         (unsigned)(s_bytes / 1024), stalls);
                s_state  = NOCSIF_WEBDL_RUNNING;             /* keep the UI in "running" across the retry */
                s_status = "resuming\xE2\x80\xA6";
                vTaskDelay(pdMS_TO_TICKS(2500));             /* brief backoff before the reconnect */
            }
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

void nocsif_webdl_start_to(const char *url, const char *dir, bool resume)
{
    if (s_task == NULL || url == NULL || url[0] == '\0') return;
    if (s_state == NOCSIF_WEBDL_RUNNING) return;          /* one at a time */
    strlcpy(s_url, url, sizeof s_url);
    strlcpy(s_dir, (dir && dir[0]) ? dir : NOCSIF_WEBDL_DIR, sizeof s_dir);
    s_resume   = resume;
    s_cancel   = false;
    s_progress = -1;
    s_state    = NOCSIF_WEBDL_RUNNING;                    /* latch immediately so a double-tap is a no-op */
    s_status   = "starting\xE2\x80\xA6";
    s_req      = true;
    xTaskNotifyGive(s_task);
}

void nocsif_webdl_start(const char *url)
{
    nocsif_webdl_start_to(url, NOCSIF_WEBDL_DIR, false);
}

void nocsif_webdl_cancel(void)
{
    if (s_state == NOCSIF_WEBDL_RUNNING) {
        s_cancel = true;                                  /* the worker stops at the next chunk / retry */
        s_status = "cancelling\xE2\x80\xA6";
    }
}

void nocsif_webdl_grab_portal(const char *ssid)
{
    if (s_task == NULL || ssid == NULL || ssid[0] == '\0') return;
    if (s_state == NOCSIF_WEBDL_RUNNING) return;          /* one transfer at a time */
    strlcpy(s_portal_ssid, ssid, sizeof s_portal_ssid);
    s_progress   = -1;
    s_state      = NOCSIF_WEBDL_RUNNING;                  /* latch so a double-tap is a no-op */
    s_status     = "starting\xE2\x80\xA6";
    s_portal_req = true;
    xTaskNotifyGive(s_task);
}

bool nocsif_webdl_busy(void) { return s_state == NOCSIF_WEBDL_RUNNING; }

nocsif_webdl_state_t nocsif_webdl_state(void) { return s_state; }
int          nocsif_webdl_progress(void)   { return s_progress; }
uint32_t     nocsif_webdl_bytes(void)       { return s_bytes; }
uint64_t     nocsif_webdl_total_bytes(void) { return s_full; }
const char  *nocsif_webdl_status(void)     { return s_status[0] ? s_status : "ready"; }
const char  *nocsif_webdl_saved_name(void) { return s_saved; }
