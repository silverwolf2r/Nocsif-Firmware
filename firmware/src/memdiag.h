/*
 * memdiag.h — internal-RAM diagnostics (NocSif).
 *
 * The scarce resource on this watch is internal DMA-capable RAM (coex.h). When the Wi-Fi driver logs
 * `wifi:mem fail` / `m f null` the pool sampled a second later usually looks fine — the failures are
 * transient bursts, so a periodic gauge never sees them. This module records every failed heap
 * allocation at the moment it happens (size, caps, task, the pool state, a short backtrace) into a
 * PSRAM ring, logs one edge line per second while failures continue, and walks the internal heap on
 * request (used / free block histograms). Bridge: `mem`, `mem a=fails`, `mem ext=N`, `mem a=wifi`.
 *
 * It also owns the live "malloc() prefers internal below N bytes" threshold
 * (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL at boot; heap_caps_malloc_extmem_enable at runtime), so a
 * bring-up can drop it temporarily and the bridge can A/B it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NOCSIF_MEMDIAG_PCS   8                /* backtrace depth kept per failure */
#define NOCSIF_MEMDIAG_BINS  6                /* walk histogram: <=64, <=256, <=1K, <=2K, <=4K, >4K */

typedef struct {
    uint32_t t_ms;                            /* uptime at the failure                                 */
    uint32_t size;                            /* requested bytes                                       */
    uint32_t caps;                            /* caps of the last attempt (MALLOC_CAP_*)               */
    uint32_t int_free;                        /* internal free at the failure                          */
    uint32_t dma_free, dma_largest;           /* internal DMA-capable pool (coex.h) at the failure     */
    char     task[12];                        /* the failing task                                      */
    uint32_t pc[NOCSIF_MEMDIAG_PCS];          /* caller chain (decode with addr2line on the ELF)       */
} nocsif_memdiag_fail_t;

typedef struct {
    uint32_t used_n[NOCSIF_MEMDIAG_BINS], used_b[NOCSIF_MEMDIAG_BINS];
    uint32_t free_n[NOCSIF_MEMDIAG_BINS], free_b[NOCSIF_MEMDIAG_BINS];
    uint32_t big_used[4];                     /* the four largest used blocks (bytes), largest first   */
} nocsif_memdiag_walk_t;

/* Register the failed-allocation hook + the 1 s edge logger. Call once, early in app_main. */
void nocsif_memdiag_init(void);

uint32_t nocsif_memdiag_fail_count(void);     /* every failed allocation since boot                    */
uint32_t nocsif_memdiag_fail_skipped(void);   /* failures seen from an ISR / with the cache off (counted only) */
/* i = 0 is the newest record. Returns false when there is no such record. */
bool     nocsif_memdiag_fail_get(int i, nocsif_memdiag_fail_t *out);

/* Used / free block histogram of the heaps matching `caps` (e.g. MALLOC_CAP_INTERNAL). No allocation. */
void nocsif_memdiag_walk(uint32_t caps, nocsif_memdiag_walk_t *out);

/* malloc() requests below `limit` bytes try internal RAM first (the rest PSRAM first). */
void   nocsif_memdiag_extmem_set(size_t limit);
size_t nocsif_memdiag_extmem_get(void);

#ifdef __cplusplus
}
#endif
