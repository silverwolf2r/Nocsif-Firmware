/*
 * sd_bounce.h — the microSD SPI bounce pool (NocSif). See sd_bounce.c for the story.
 *
 * ONE sentence: every SD read ends with a 514-byte SPI transfer that ESP-IDF's spi_master MUST bounce
 * through a freshly heap-allocated internal-DMA buffer, and when that allocation fails (BLE + WiFi
 * have starved the contiguous int-DMA pool to ~1 KB) IDF's cleanup path memcpy's from NULL and the
 * watch reboots. This module gives spi_master those bounce buffers from a tiny static pool — only for
 * the task that is inside an SD transaction — so the heap is never asked, the allocation can never
 * fail, and SD I/O keeps working under full radio load.
 *
 * Wiring: sdcard.c installs an interposer on the card's `host.do_transaction` that brackets the real
 * `sdspi_host_do_transaction` with nocsif_sd_bounce_enter()/_exit(); platformio.ini adds
 * `-Wl,--wrap=heap_caps_aligned_alloc -Wl,--wrap=free` so spi_master's bounce alloc / free land in the
 * __wrap_* functions defined in sd_bounce.c.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t served;      /* bounce buffers handed out from the pool (the crash-path allocs we absorbed) */
    uint32_t fallback;    /* SD-transaction DMA requests too big / too aligned for a slot -> real heap */
    uint32_t exhausted;   /* requests that fit a slot but found none free -> real heap (should stay 0) */
    uint32_t heap_fail;   /* SD-transaction requests the real heap ALSO refused (the old crash precondition) */
    uint32_t peak;        /* most slots in use at once (expected max 2: tx + rx of one transaction) */
} nocsif_sd_bounce_stats_t;

/* Create the transaction mutex. Call once at boot (single-threaded), before the card's first
 * command — nocsif_sdcard_init() does. Idempotent. */
void nocsif_sd_bounce_init(void);

/* Bracket ONE SD host transaction (the do_transaction interposer in sdcard.c). enter() serialises
 * transactions across tasks and marks the caller as the task whose spi_master bounce allocs are
 * served from the pool; exit() clears the mark and logs (rate-limited) if anything fell through.
 * enter() returns false (and logs the holder) if the previous transaction has not finished within a
 * bound well past any single SD command — the caller must then NOT run the command (and not call
 * exit()); it reports the I/O error up instead of blocking into the task watchdog. */
bool nocsif_sd_bounce_enter(void);
void nocsif_sd_bounce_exit(void);

/* Snapshot of the counters (bridge `status` telemetry). */
void nocsif_sd_bounce_stats(nocsif_sd_bounce_stats_t *out);

/* Static pool footprint in bytes (docs/RAM-BUDGET.md row). */
size_t nocsif_sd_bounce_pool_bytes(void);

#ifdef __cplusplus
}
#endif
