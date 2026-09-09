/*
 * coex.h — the shared radio-coexistence policy for NocSif. Part of Phase 0 of the RAM
 * remediation work (docs/design/meta-prompts/ram-org-remediation.md; docs/RAM-BUDGET.md
 * remake #1/#14; docs/ORGANIZATION.md "Radio internal-DMA coexistence gate").
 *
 * Centralizes the measurement of the scarce resource and the per-radio bring-up thresholds, so
 * every gate/log across ble.c / wifi.c / ui.c / lora.cpp / main.c is reading the SAME quantity the
 * same way. See docs/LESSONS.md for field history
 * ([[gotcha-ble-wifi-coexistence]] / [[project-ble-release-regression]] / [[project-f2-signal-hunt]]).
 *
 * The short version: what kills a radio bring-up is CONTIGUITY, not total free bytes. Once
 * esp_wifi_init runs, the largest internal-DMA hole caps out around 30 KB no matter how much total
 * memory is free, and runtime teardown never defragments it back toward the pristine ~90 KB
 * (measured: 15,872 with WiFi up / 21,504 with WiFi released / 9,216 in airplane mode). So the BLE
 * controller's ~31.7 KB block is claimed FIRST at boot and held for the whole session — a runtime
 * re-claim later is impossible (21,504 < 31,744) and must never be attempted; these thresholds
 * gate that boot-time claim.
 *
 * A dependency-free leaf header (only esp_heap_caps.h / esp_log.h), so it's safe to include from
 * any module, even a bare hardware driver.
 */
#pragma once

#include "esp_heap_caps.h"
#include "esp_log.h"

/* The scarce pool: internal SRAM that can back a DMA descriptor or buffer. */
#define NOCSIF_DMA_CAPS  (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)

/* Largest CONTIGUOUS free block in the internal-DMA pool — the one number that actually governs a
 * radio bring-up, since a controller/stack block must fit in a single run. Gate on this. */
static inline size_t nocsif_int_dma_largest(void)
{
    return heap_caps_get_largest_free_block(NOCSIF_DMA_CAPS);
}

/* Total free internal-DMA bytes — telemetry only, never a gate: freeing bytes elsewhere doesn't
 * widen the contiguous hole (measured — freeing ~10 KB moved total-free from 28k to 38k while
 * leaving the largest block unchanged). */
static inline size_t nocsif_int_dma_free(void)
{
    return heap_caps_get_free_size(NOCSIF_DMA_CAPS);
}

/* One-line internal-DMA telemetry, matching the "<context>; int-dma free=%u largest=%u" lines it
 * replaces. Relies on the including file's own `TAG` (every NocSif module defines one). */
#define nocsif_log_dma_free(ctx) \
    ESP_LOGI(TAG, "%s; int-dma free=%u largest=%u", (ctx), \
             (unsigned)nocsif_int_dma_free(), (unsigned)nocsif_int_dma_largest())

/* ---- per-radio bring-up gates (contiguous int-DMA required, in ONE run) ------ *
 * These numbers were MEASURED on this board (docs/RAM-BUDGET.md "Measurement & uncertainty"), not
 * guessed:
 *
 *   BLE  : at a largest block of 27,648 B, esp_bt_controller_init fails with "BLE_INIT: Malloc
 *          failed" and then trips the interrupt watchdog; at 31,736 B, the controller comes up and
 *          the host syncs. 31,744 sits between those two — above every observed failure, below
 *          every observed success. The controller's block is claimed ONCE from the pristine ~90 KB
 *          boot pool and held resident for the whole BT-master session, since a runtime re-claim is
 *          IMPOSSIBLE once WiFi has fragmented the heap (WiFi-released largest tops out at
 *          21,504 B). If CONFIG_BT_CTRL_BLE_MAX_ACT changes, this gate's size assumption changes
 *          too (each activity slot is 828 B) — re-verify it (see coex_guard.c).
 *
 *   LORA : the SX1262 worker's stack size. Kept only for reference/telemetry — the LoRa worker
 *          stack now lives in PSRAM (docs/RAM-BUDGET.md remake #8), so this gate is no longer
 *          actually applied when spawning it. LoRa fits fine alongside BLE + WiFi.
 *
 *   FLOOR: the historical fragmentation wall observed for other internal-DMA claimers.
 *
 *   USB  : covers the one-time tinyusb_driver_install (the first USB mode pick). esp_tinyusb
 *          creates its device task with a fixed 4 KB internal stack plus its own context, the
 *          CDC-ACM rings (512+512 B), and the MSC APP->USB FAT handoff — roughly 5-7 KB of runtime
 *          internal memory. Before RAM remediation, the steady-state largest block was only ~2 KB,
 *          so File Share would refuse (docs/DMA-COEXISTENCE-VERIFICATION.md §2). RAM Phase A3 makes
 *          entry deterministic: usb_gadget.c claims and immediately frees this much from the
 *          pristine boot pool right before the install, so it lands in that same hole regardless of
 *          later fragmentation — this gate then only fails if that reserve itself never happened
 *          (safe mode / a boot allocation failure), which is reported honestly on the USB screen.
 */
#define NOCSIF_RADIO_MIN_DMA_BLE    31744
#define NOCSIF_RADIO_MIN_DMA_LORA   12288
#define NOCSIF_RADIO_MIN_DMA_FLOOR  24576
#define NOCSIF_RADIO_MIN_DMA_USB    8192
