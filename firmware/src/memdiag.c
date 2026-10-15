/*
 * memdiag.c — internal-RAM diagnostics (see memdiag.h).
 *
 * The failed-allocation hook runs in whatever task failed, right after the heap gave up (no heap lock
 * held). It must be safe there: IRAM, no allocation, no logging (the log tee itself allocates), and it
 * only records when the flash cache is on and it is not in an ISR — otherwise it just counts. The ring
 * lives in PSRAM (allocated once at init), so the diagnostics cost no internal RAM beyond a few words.
 * One esp_timer tick a second turns new records into a single log line.
 */
#include "memdiag.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_debug_helpers.h"
#include "esp_cpu_utils.h"
#include "esp_private/cache_utils.h"        /* spi_flash_cache_enabled — never touch PSRAM with the cache off */
#include "sdkconfig.h"

#include "coex.h"                           /* NOCSIF_DMA_CAPS — the shared int-DMA gauge */

static const char *TAG = "memdiag";

#define MD_RING 16

static nocsif_memdiag_fail_t *s_ring;       /* PSRAM, MD_RING records */
static volatile uint32_t s_fail_n;          /* every failure (also those not recorded) */
static volatile uint32_t s_fail_skip;       /* ISR / cache-off failures: counted, not recorded */
static volatile uint32_t s_rec_n;           /* records written (ring head = s_rec_n % MD_RING) */
static uint32_t s_logged_n;                 /* s_fail_n at the last edge line */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static size_t s_extmem = CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL;
static esp_timer_handle_t s_tick;

static void IRAM_ATTR memdiag_fail_cb(size_t size, uint32_t caps, const char *fn)
{
    (void)fn;
    s_fail_n++;
    if (xPortInIsrContext() || !spi_flash_cache_enabled() || s_ring == NULL) {
        s_fail_skip++;
        return;
    }
    nocsif_memdiag_fail_t e;
    memset(&e, 0, sizeof e);
    e.t_ms        = (uint32_t)(esp_timer_get_time() / 1000);
    e.size        = (uint32_t)size;
    e.caps        = caps;
    e.int_free    = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    e.dma_free    = (uint32_t)heap_caps_get_free_size(NOCSIF_DMA_CAPS);
    e.dma_largest = (uint32_t)heap_caps_get_largest_free_block(NOCSIF_DMA_CAPS);
    const char *tn = pcTaskGetName(NULL);
    if (tn) strlcpy(e.task, tn, sizeof e.task);
    /* Caller chain: frame 0 is this hook, then the heap's own failure path; keep the first
     * NOCSIF_MEMDIAG_PCS and let addr2line on the host sort the heap frames from the caller. */
    esp_backtrace_frame_t f;
    esp_backtrace_get_start(&f.pc, &f.sp, &f.next_pc);
    f.exc_frame = NULL;
    int n = 0;
    e.pc[n++] = esp_cpu_process_stack_pc(f.pc);
    while (n < NOCSIF_MEMDIAG_PCS && f.next_pc != 0) {
        if (!esp_backtrace_get_next_frame(&f)) break;
        e.pc[n++] = esp_cpu_process_stack_pc(f.pc);
    }
    portENTER_CRITICAL(&s_mux);
    s_ring[s_rec_n % MD_RING] = e;
    s_rec_n++;
    portEXIT_CRITICAL(&s_mux);
}

/* One line per second while new failures keep arriving: how many, and the newest one in full. */
static void memdiag_tick(void *arg)
{
    (void)arg;
    const uint32_t n = s_fail_n;
    if (n == s_logged_n) return;
    const uint32_t delta = n - s_logged_n;
    s_logged_n = n;
    nocsif_memdiag_fail_t e;
    if (!nocsif_memdiag_fail_get(0, &e)) {
        ESP_LOGW(TAG, "%u alloc fail(s) (+%u), none recorded (ISR / cache off)", (unsigned)n, (unsigned)delta);
        return;
    }
    ESP_LOGW(TAG, "%u alloc fail(s) (+%u) - last %u B caps 0x%x task %s @%u ms; at fail int %u, int-dma free %u "
                  "largest %u; now int-dma free %u largest %u; bt 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x",
             (unsigned)n, (unsigned)delta, (unsigned)e.size, (unsigned)e.caps, e.task, (unsigned)e.t_ms,
             (unsigned)e.int_free, (unsigned)e.dma_free, (unsigned)e.dma_largest,
             (unsigned)nocsif_int_dma_free(), (unsigned)nocsif_int_dma_largest(),
             (unsigned)e.pc[2], (unsigned)e.pc[3], (unsigned)e.pc[4], (unsigned)e.pc[5], (unsigned)e.pc[6],
             (unsigned)e.pc[7]);
}

void nocsif_memdiag_init(void)
{
    if (s_ring == NULL) {
        s_ring = heap_caps_calloc(MD_RING, sizeof *s_ring, MALLOC_CAP_SPIRAM);
    }
    heap_caps_register_failed_alloc_callback(memdiag_fail_cb);
    if (s_tick == NULL) {
        const esp_timer_create_args_t a = { .callback = memdiag_tick, .name = "memdiag" };
        if (esp_timer_create(&a, &s_tick) == ESP_OK) esp_timer_start_periodic(s_tick, 1000000);
    }
    ESP_LOGI(TAG, "failed-allocation recorder on (ring %s), malloc internal-first below %u B",
             s_ring ? "PSRAM" : "MISSING", (unsigned)s_extmem);
}

uint32_t nocsif_memdiag_fail_count(void)   { return s_fail_n; }
uint32_t nocsif_memdiag_fail_skipped(void) { return s_fail_skip; }

bool nocsif_memdiag_fail_get(int i, nocsif_memdiag_fail_t *out)
{
    if (!s_ring || !out || i < 0) return false;
    bool ok = false;
    portENTER_CRITICAL(&s_mux);
    const uint32_t have = s_rec_n < MD_RING ? s_rec_n : MD_RING;
    if ((uint32_t)i < have) {
        *out = s_ring[(s_rec_n - 1 - (uint32_t)i) % MD_RING];
        ok = true;
    }
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

static int bin_of(size_t sz)
{
    if (sz <= 64)   return 0;
    if (sz <= 256)  return 1;
    if (sz <= 1024) return 2;
    if (sz <= 2048) return 3;
    if (sz <= 4096) return 4;
    return 5;
}

/* Runs under each heap's lock: count only, never allocate or log. */
static bool walk_cb(walker_heap_into_t heap, walker_block_info_t blk, void *user)
{
    (void)heap;
    nocsif_memdiag_walk_t *w = (nocsif_memdiag_walk_t *)user;
    const int b = bin_of(blk.size);
    if (blk.used) {
        w->used_n[b]++; w->used_b[b] += (uint32_t)blk.size;
        uint32_t s = (uint32_t)blk.size;
        for (int i = 0; i < 4; i++) {
            if (s > w->big_used[i]) { uint32_t t = w->big_used[i]; w->big_used[i] = s; s = t; }
        }
    } else {
        w->free_n[b]++; w->free_b[b] += (uint32_t)blk.size;
    }
    return true;
}

void nocsif_memdiag_walk(uint32_t caps, nocsif_memdiag_walk_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    heap_caps_walk(caps, walk_cb, out);
}

void nocsif_memdiag_extmem_set(size_t limit)
{
    s_extmem = limit;
    heap_caps_malloc_extmem_enable(limit);
}

size_t nocsif_memdiag_extmem_get(void) { return s_extmem; }
