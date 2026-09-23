/*
 * sd_bounce.c — a static, transaction-scoped bounce pool for the microSD SPI path (NocSif).
 *
 * THE CRASH (fully symbolized against the matching ELF, docs/LESSONS.md 2026-09-23):
 *   ROM memcpy <- uninstall_priv_desc (spi_master.c:1176) <- setup_priv_desc (:1253)
 *   <- spi_device_queue_trans <- spi_device_transmit <- start_command_read_blocks (sdspi_host.c:829)
 *   <- sdspi_host_do_transaction <- sdmmc_send_cmd <- sdmmc_read_sectors_dma
 *   Any SD read or write (a write reads FAT/dir sectors first) panicked the watch once BLE + WiFi
 *   (+ GNSS + LoRa) had starved the contiguous internal-DMA pool to ~1 KB.
 *
 * WHY IT BOUNCES AT ALL — measured, not guessed (the boot log's "SPI3 DMA bounce rule" line):
 *   spi_common.c configures both GDMA channels with max_data_burst_size=32; on the ESP32-S3 that sets
 *   the RX channel's internal-memory alignment to 4 (gdma.c GDMA_LL_AHB_RX_BURST_NEEDS_ALIGNMENT), TX
 *   stays 1, and internal SRAM has no data cache (cache_align_int = 0). setup_dma_priv_buffer then
 *   bounces an RX buffer whenever (address | length) & 3 != 0. sdspi never DMAs into the caller's
 *   buffer: every block is received into its own private 516-byte block_buf (4-byte aligned, heap),
 *   with length = data + 4 for a middle block (516: aligned, no bounce) but data + 2 for the LAST
 *   block of every read (514: length & 3 = 2 -> bounce). So the bounce is baked into IDF's sdspi read
 *   path by LENGTH, not by any buffer we can supply — a caller-side aligned buffer cannot remove it, and
 *   neither can card->host.check_buffer_alignment (its "graceful" branch reads into a 512 B temp that
 *   sdspi copies through the very same block_buf transfer; it only ADDS a 512 B heap claim first).
 *
 * WHY THE FAILURE IS A CRASH, NOT AN ERROR: setup_priv_desc only stores buffer_to_rcv after BOTH the
 *   tx and rx bounce setups succeed; on the rx alloc failing it jumps to clean_up -> uninstall_priv_desc,
 *   which sees buffer_to_rcv (still NULL) != rx_buffer and memcpy's rxlen bytes FROM NULL. That is an
 *   ESP-IDF latent bug we cannot patch from app code (static functions), triggered by our int-DMA
 *   exhaustion.
 *
 * WHY NOT A HEAP RESERVE: this firmware's BLE + WiFi coexistence is tuned to the byte — a 4 KB int-DMA
 *   block held through boot broke WiFi association and halved the steady-state largest hole, and a
 *   reserve freed for the op and re-claimed after is racy (WiFi allocs in the same window) and one-shot
 *   (nothing contiguous to re-claim at steady state). See docs/RAM-BUDGET.md conflict #6.
 *
 * THE FIX: spi_master gets its bounce buffers from HERE instead of the heap. Two static slots in .bss
 *   (internal DRAM = DMA-capable, 16-byte aligned): a 528 B slot for the block_buf bounce (516 B: the
 *   514 B last block rounded up to 4) and a 64 B slot for the tiny command / token bounces a PSRAM-stack
 *   caller's transaction adds (sdspi_hw_cmd_t, poll bytes: <= 16 B each). At most two bounce buffers are
 *   alive in one transaction (tx + rx of a single transfer), and the only pairing is small + small or
 *   small + large, so 592 B covers every sdspi transaction deterministically. GNU ld `--wrap`
 *   (platformio.ini) routes every `heap_caps_aligned_alloc` and `free` in the image through the __wrap_*
 *   functions below. The alloc wrapper serves a slot ONLY when the calling task is the one inside an SD
 *   transaction (marked by the do_transaction interposer in sdcard.c) and the request is a small
 *   internal-DMA one; everything else falls straight through to the real heap. The free wrapper returns
 *   pool addresses to the bitmap and passes everything else to the real free. Net effect: the SD bounce
 *   allocation cannot fail, so IDF's NULL-deref cleanup is never reached; SD I/O succeeds at any int-DMA
 *   level (with sdcard.c's heap-free FatFs sector staging); and SD bounces stop churning the heap.
 *
 * COST: 592 B .bss + ~300 B IRAM (the wrappers sit in IRAM like the functions they wrap: spi_master's
 *   ISR-attributed setup path calls the alloc, WiFi's IRAM paths call free) + one 32-byte call frame per
 *   free()/heap_caps_aligned_alloc() system-wide. The hot path is a single compare (owner NULL / pointer
 *   not in the pool); the slot bitmap is lock-free (atomic or/and), so no critical section is ever taken.
 *   Every internal byte matters here (ui.c's LoRa Signal-Alerts watch only arms the radio while the
 *   largest int-DMA hole is >= 4096 B, and the old build cleared that by a few hundred bytes), which is
 *   why gnss.c's 3 KB wardrive dedup table moved to PSRAM in the same change and sdcard.c stages FatFs
 *   sectors through the RTC-fast-memory heap rather than DRAM.
 */
#include "sd_bounce.h"

#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "sd_bounce";

/* Slot 0: sdspi's block_buf bounce (514 B up-aligned to 516) with slack. Slot 1: the tiny command /
 * token bounces (sdspi_hw_cmd_t is < 20 B; poll transfers are 1-8 B; RX lengths up-align to 4). */
#define SD_BOUNCE_BIG_SIZE     528
#define SD_BOUNCE_SMALL_SIZE   64
#define SD_BOUNCE_ALIGN        16     /* serves any alignment spi_master asks for internal memory (1 / 4) */
#define SD_BOUNCE_XFER_WAIT_MS 6000   /* > sdspi's longest single-command timeout, < the 8 s task-WDT */

/* The pool: plain static .bss = internal DRAM (DMA-capable on the S3). NOT PSRAM (spi_master would
 * refuse it as a DMA target), NOT RTC memory (GDMA cannot reach it), NOT the heap (the whole point). */
static uint8_t s_big[SD_BOUNCE_BIG_SIZE]     __attribute__((aligned(SD_BOUNCE_ALIGN)));
static uint8_t s_small[SD_BOUNCE_SMALL_SIZE] __attribute__((aligned(SD_BOUNCE_ALIGN)));

#define SLOT_BIG    0x1u
#define SLOT_SMALL  0x2u
static uint32_t                     s_used;           /* slot bitmap, lock-free (atomic or / and) */
static volatile TaskHandle_t        s_owner;          /* task inside an SD transaction; NULL = none */
static SemaphoreHandle_t            s_xfer_mutex;     /* serialises transactions (protects s_owner) */
static nocsif_sd_bounce_stats_t     s_stats;          /* telemetry; written by the owner task only */
static uint32_t                     s_logged_exhausted, s_logged_heap_fail, s_logged_fallback;

extern void *__real_heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps);
extern void  __real_free(void *ptr);
void *__wrap_heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps);
void  __wrap_free(void *ptr);

void nocsif_sd_bounce_init(void)
{
    if (s_xfer_mutex == NULL) {
        s_xfer_mutex = xSemaphoreCreateMutex();
    }
    ESP_LOGI(TAG, "SD bounce pool: %u + %u B static, serving spi_master's per-transfer DMA bounces for "
                  "the SD transaction in flight",
             (unsigned)SD_BOUNCE_BIG_SIZE, (unsigned)SD_BOUNCE_SMALL_SIZE);
}

bool nocsif_sd_bounce_enter(void)
{
    if (s_xfer_mutex != NULL) {
        /* One SD command is bounded by sdspi's own per-command timeouts (<= the 5 s write timeout), so a
         * wait this long means the holder is wedged inside the SPI layer — refuse (the caller sees an I/O
         * error) instead of blocking this task past the 8 s task-WDT, and say who holds it. */
        if (xSemaphoreTake(s_xfer_mutex, pdMS_TO_TICKS(SD_BOUNCE_XFER_WAIT_MS)) != pdTRUE) {
            TaskHandle_t holder = xSemaphoreGetMutexHolder(s_xfer_mutex);
            ESP_LOGE(TAG, "SD transaction mutex held >%u ms by %s — refusing this command (task %s)",
                     (unsigned)SD_BOUNCE_XFER_WAIT_MS, holder ? pcTaskGetName(holder) : "?",
                     pcTaskGetName(NULL));
            return false;
        }
    }
    s_owner = xTaskGetCurrentTaskHandle();
    return true;
}

void nocsif_sd_bounce_exit(void)
{
    s_owner = NULL;
    /* Task context (not the IRAM wrappers): safe to log. Rate-limited to "something changed". */
    if (s_stats.exhausted != s_logged_exhausted || s_stats.heap_fail != s_logged_heap_fail ||
        s_stats.fallback != s_logged_fallback) {
        s_logged_exhausted = s_stats.exhausted;
        s_logged_heap_fail = s_stats.heap_fail;
        s_logged_fallback  = s_stats.fallback;
        ESP_LOGW(TAG, "bounce pool fell through: fallback=%u exhausted=%u heap_fail=%u (served=%u peak=%u) "
                      "— a request did not fit the pool; heap_fail>0 is the old crash precondition",
                 (unsigned)s_stats.fallback, (unsigned)s_stats.exhausted, (unsigned)s_stats.heap_fail,
                 (unsigned)s_stats.served, (unsigned)s_stats.peak);
    }
    if (s_xfer_mutex != NULL) {
        xSemaphoreGive(s_xfer_mutex);
    }
}

void nocsif_sd_bounce_stats(nocsif_sd_bounce_stats_t *out)
{
    *out = s_stats;   /* 32-bit fields; a torn read of telemetry is harmless */
}

size_t nocsif_sd_bounce_pool_bytes(void)
{
    return sizeof(s_big) + sizeof(s_small);
}

/* ---- the linker wraps (IRAM: called from spi_master's IRAM setup path and from IRAM free() users) ---- */

/* Try to take one slot (bit); returns true if it was free. Lock-free, ISR-safe. */
static inline bool IRAM_ATTR slot_take(uint32_t bit)
{
    return (__atomic_fetch_or(&s_used, bit, __ATOMIC_ACQ_REL) & bit) == 0;
}

void *IRAM_ATTR __wrap_heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps)
{
    TaskHandle_t owner = s_owner;
    /* Hot path: no SD transaction in flight (owner NULL) -> one compare, straight to the heap. */
    if (owner == NULL || !(caps & MALLOC_CAP_DMA) || (caps & MALLOC_CAP_SPIRAM) ||
        xTaskGetCurrentTaskHandle() != owner) {
        return __real_heap_caps_aligned_alloc(alignment, size, caps);
    }

    /* The SD transaction's own internal-DMA request: this is spi_master's bounce buffer. Small requests
     * prefer the small slot, anything up to the big slot takes the big one; either falls back to the
     * other if it fits and is free. */
    if (alignment <= SD_BOUNCE_ALIGN && size <= SD_BOUNCE_BIG_SIZE) {
        void *slot = NULL;
        if (size <= SD_BOUNCE_SMALL_SIZE && slot_take(SLOT_SMALL)) {
            slot = s_small;
        } else if (slot_take(SLOT_BIG)) {
            slot = s_big;
        }
        if (slot != NULL) {
            s_stats.served++;
            uint32_t in_use = (uint32_t)__builtin_popcount(s_used);
            if (in_use > s_stats.peak) {
                s_stats.peak = in_use;
            }
            return slot;
        }
        s_stats.exhausted++;
    } else {
        s_stats.fallback++;
    }

    /* Did not fit / pool full: today's behaviour (the heap), counted so it is visible if it ever happens. */
    void *p = __real_heap_caps_aligned_alloc(alignment, size, caps);
    if (p == NULL) {
        s_stats.heap_fail++;
    }
    return p;
}

void IRAM_ATTR __wrap_free(void *ptr)
{
    /* Two compares on the hot path; NULL never matches (the pool is at a non-zero address). */
    if (ptr == (void *)s_big) {
        __atomic_fetch_and(&s_used, ~SLOT_BIG, __ATOMIC_ACQ_REL);
        return;
    }
    if (ptr == (void *)s_small) {
        __atomic_fetch_and(&s_used, ~SLOT_SMALL, __ATOMIC_ACQ_REL);
        return;
    }
    __real_free(ptr);
}
