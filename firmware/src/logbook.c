/*
 * NocSif — persistent rolling log (Phase A / A2). See logbook.h for the contract.
 *
 * Ring layout: the 'logs' partition is divided into 4 KB sectors, each
 * written whole with a header {magic, seq, len} followed by up to
 * (4096-12) bytes of newline-delimited log text. Sequence numbers increase
 * monotonically; the newest sector has the highest seq. Writing a sector
 * erases it first, so a sector is only ever fully-valid or (mid-erase)
 * invalid-magic — never half-updated.
 */
#include "logbook.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_partition.h"

static const char *TAG = "logbook";

#define LB_SECTOR_SZ      4096u
#define LB_MAGIC          0x474F4C4Eu          /* "NLOG" little-endian */
#define LB_POLL_MS        2000                 /* flush-task poll period */
#define LB_FLUSH_IDLE_MS  30000                /* commit a dirty buffer at least this often */

typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint32_t len;   /* payload bytes actually used (<= LB_PAYLOAD) */
} lb_hdr_t;

#define LB_PAYLOAD    (LB_SECTOR_SZ - sizeof(lb_hdr_t))   /* 4084 */
#define LB_HIGH_WATER (LB_PAYLOAD * 3 / 4)                /* commit early once 75% full */

static const esp_partition_t *s_part;
static int       s_num_sectors;
static int       s_head;                 /* sector index to write next */
static uint32_t  s_next_seq = 1;
static bool      s_ready;

/* RAM accumulator, guarded by a spinlock (the tee runs from any logging task). */
static portMUX_TYPE s_spin = portMUX_INITIALIZER_UNLOCKED;
static char      s_ram[LB_PAYLOAD];
static uint32_t  s_ram_len;

static char      s_snap[LB_PAYLOAD];     /* commit scratch (flush task only) */
static SemaphoreHandle_t s_commit_mtx;   /* serializes commits (task + explicit flush) */
static vprintf_like_t    s_orig_vprintf; /* the console logger we tee to */
static volatile bool     s_clear_req;    /* set by clear(); the flush task does the slow erase */

/* ---- RAM accumulation (spinlock-guarded, no flash) --------------------------------------------- */

/* Appends n bytes to the RAM buffer; drops the write if it would overflow (the flush task drains regularly). */
static void lb_ram_put(const char *data, uint32_t n)
{
    if (!s_ready || n == 0) return;
    portENTER_CRITICAL(&s_spin);
    if (s_ram_len + n <= sizeof(s_ram)) {   /* drop-if-full: the flush task drains it every ~2s */
        memcpy(s_ram + s_ram_len, data, n);
        s_ram_len += n;
    }
    portEXIT_CRITICAL(&s_spin);
}

/* ---- flash commit ------------------------------------------------------------------------------ */

/* Snapshots the RAM buffer and writes it to the head sector (erase + header + payload), then advances the ring. */
static void lb_commit(void)
{
    if (!s_ready) return;
    xSemaphoreTake(s_commit_mtx, portMAX_DELAY);

    uint32_t len;
    portENTER_CRITICAL(&s_spin);
    len = s_ram_len;
    if (len) { memcpy(s_snap, s_ram, len); s_ram_len = 0; }
    portEXIT_CRITICAL(&s_spin);

    if (len) {
        size_t off = (size_t)s_head * LB_SECTOR_SZ;
        lb_hdr_t h = { LB_MAGIC, s_next_seq, len };
        uint32_t plen = (len + 3u) & ~3u;               /* 4-byte-aligned write size */
        if (plen > len) memset(s_snap + len, 0, plen - len);
        if (esp_partition_erase_range(s_part, off, LB_SECTOR_SZ) == ESP_OK &&
            esp_partition_write(s_part, off, &h, sizeof(h)) == ESP_OK &&
            esp_partition_write(s_part, off + sizeof(h), s_snap, plen) == ESP_OK) {
            s_next_seq++;
            s_head = (s_head + 1) % s_num_sectors;
        } else {
            ESP_LOGW(TAG, "sector commit failed @ %u (log line dropped)", (unsigned)off);
        }
    }
    xSemaphoreGive(s_commit_mtx);
}

/* Erases the whole ring. Slow (~50 ms/sector x N sectors), so it runs only
 * on the flush task — never on the LVGL task, where it would block long
 * enough to trip the UI-liveness watchdog. */
static void lb_do_clear(void)
{
    xSemaphoreTake(s_commit_mtx, portMAX_DELAY);
    portENTER_CRITICAL(&s_spin);
    s_ram_len = 0;
    portEXIT_CRITICAL(&s_spin);
    for (int i = 0; i < s_num_sectors; i++) {
        esp_partition_erase_range(s_part, (size_t)i * LB_SECTOR_SZ, LB_SECTOR_SZ);
    }
    s_head = 0;
    s_next_seq = 1;
    xSemaphoreGive(s_commit_mtx);
    ESP_LOGI(TAG, "logbook cleared");
}

/* Low-priority task: services a pending clear, otherwise commits the RAM
 * buffer once it is large enough or has sat dirty long enough. */
static void lb_flush_task(void *arg)
{
    (void)arg;
    uint32_t dirty_ms = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(LB_POLL_MS));
        if (s_clear_req) { s_clear_req = false; lb_do_clear(); dirty_ms = 0; continue; }
        uint32_t len;
        portENTER_CRITICAL(&s_spin);
        len = s_ram_len;
        portEXIT_CRITICAL(&s_spin);
        if (len == 0) { dirty_ms = 0; continue; }
        dirty_ms += LB_POLL_MS;
        if (len >= LB_HIGH_WATER || dirty_ms >= LB_FLUSH_IDLE_MS) {
            lb_commit();
            dirty_ms = 0;
        }
    }
}

/* ---- ESP_LOG tee ------------------------------------------------------------------------------- */

/* vprintf hook installed via esp_log_set_vprintf: forwards to the console as
 * before, and also formats the line into the RAM ring buffer. */
static int lb_vprintf(const char *fmt, va_list ap)
{
    /* Console first (consumes ap); capture from a copy. */
    va_list ap2;
    va_copy(ap2, ap);
    int r = s_orig_vprintf ? s_orig_vprintf(fmt, ap) : vprintf(fmt, ap);
    char line[256];
    int n = vsnprintf(line, sizeof(line), fmt, ap2);
    va_end(ap2);
    if (n > 0) lb_ram_put(line, (uint32_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1));
    return r;
}

/* ---- public API -------------------------------------------------------------------------------- */

void nocsif_logbook_init(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "logs");
    if (!s_part) {
        ESP_LOGE(TAG, "'logs' partition not found — persistent log disabled");
        return;
    }
    s_num_sectors = (int)(s_part->size / LB_SECTOR_SZ);
    if (s_num_sectors < 2) {
        ESP_LOGE(TAG, "'logs' partition too small (%u B) — persistent log disabled", (unsigned)s_part->size);
        return;
    }
    s_commit_mtx = xSemaphoreCreateMutex();
    if (!s_commit_mtx) { ESP_LOGE(TAG, "mutex alloc failed — persistent log disabled"); return; }

    /* Scan sector headers for the highest sequence number = the newest sector; resume after it. */
    uint32_t max_seq = 0; int max_idx = -1;
    for (int i = 0; i < s_num_sectors; i++) {
        lb_hdr_t h;
        if (esp_partition_read(s_part, (size_t)i * LB_SECTOR_SZ, &h, sizeof(h)) == ESP_OK &&
            h.magic == LB_MAGIC) {
            if (max_idx < 0 || h.seq > max_seq) { max_seq = h.seq; max_idx = i; }
        }
    }
    if (max_idx >= 0) { s_head = (max_idx + 1) % s_num_sectors; s_next_seq = max_seq + 1; }
    else              { s_head = 0; s_next_seq = 1; }

    s_ready = true;
    s_orig_vprintf = esp_log_set_vprintf(lb_vprintf);   /* tee console -> ring */
    xTaskCreate(lb_flush_task, "logflush", 3584, NULL, 1, NULL);

    ESP_LOGI(TAG, "logbook up: 'logs' %u KB, %d sectors, head=%d next_seq=%u",
             (unsigned)(s_part->size / 1024), s_num_sectors, s_head, (unsigned)s_next_seq);
}

void nocsif_logbook_append(const char *line)
{
    if (!s_ready || !line) return;
    uint32_t n = (uint32_t)strlen(line);
    if (n) lb_ram_put(line, n);
    lb_ram_put("\n", 1);   /* one append == one line */
}

void nocsif_logbook_flush(void)
{
    lb_commit();
}

size_t nocsif_logbook_read_tail(char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return 0;
    out[0] = '\0';
    if (!s_ready) return 0;

    /* Assemble [newest flushed sector payload][current RAM buffer] into a
     * temp, then return its tail. That is the freshest content the ring holds. */
    char *tmp = malloc((size_t)LB_PAYLOAD * 2 + 1);
    if (!tmp) return 0;
    size_t tlen = 0;

    uint32_t max_seq = 0; int max_idx = -1;
    for (int i = 0; i < s_num_sectors; i++) {
        lb_hdr_t h;
        if (esp_partition_read(s_part, (size_t)i * LB_SECTOR_SZ, &h, sizeof(h)) == ESP_OK &&
            h.magic == LB_MAGIC) {
            if (max_idx < 0 || h.seq > max_seq) { max_seq = h.seq; max_idx = i; }
        }
    }
    if (max_idx >= 0) {
        lb_hdr_t h;
        if (esp_partition_read(s_part, (size_t)max_idx * LB_SECTOR_SZ, &h, sizeof(h)) == ESP_OK &&
            h.magic == LB_MAGIC && h.len <= LB_PAYLOAD) {
            if (esp_partition_read(s_part, (size_t)max_idx * LB_SECTOR_SZ + sizeof(h), tmp, h.len) == ESP_OK)
                tlen = h.len;
        }
    }
    portENTER_CRITICAL(&s_spin);
    uint32_t rl = s_ram_len;
    if (rl) { memcpy(tmp + tlen, s_ram, rl); tlen += rl; }
    portEXIT_CRITICAL(&s_spin);

    size_t start = (tlen > out_sz - 1) ? tlen - (out_sz - 1) : 0;
    size_t cpy = tlen - start;
    memcpy(out, tmp + start, cpy);
    out[cpy] = '\0';
    free(tmp);
    return cpy;
}

void nocsif_logbook_clear(void)
{
    /* Non-blocking: just request it. The flush task does the slow
     * multi-sector erase off the caller's thread (the Diagnostics "Clear"
     * runs this on the LVGL task, which must not stall). */
    if (s_ready) s_clear_req = true;
}
