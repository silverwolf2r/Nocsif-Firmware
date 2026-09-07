/*
 * coex.h — load-bearing radio-coexistence policy (NocSif). Phase 0 of the RAM
 * remediation (docs/design/meta-prompts/ram-org-remediation.md; docs/RAM-BUDGET.md
 * remake #1/#14; docs/ORGANIZATION.md "Radio internal-DMA coexistence gate").
 *
 * ONE place measures the scarce resource and ONE table holds every per-radio
 * bring-up threshold, so every gate/log across ble.c / wifi.c / ui.c / lora.cpp /
 * main.c measures the SAME quantity. Field history: docs/LESSONS.md
 * ([[gotcha-ble-wifi-coexistence]] / [[project-ble-release-regression]] /
 * [[project-f2-signal-hunt]]).
 *
 * The one sentence: the killer is CONTIGUITY, not free bytes. After esp_wifi_init
 * the largest int-DMA hole is capped (~30 KB) no matter how much TOTAL is free,
 * and runtime teardown NEVER defragments it back toward the pristine ~90 KB
 * (measured: 15,872 WiFi-up / 21,504 WiFi-released / 9,216 airplane). So the BLE
 * controller's ~31.7 KB block is claimed FIRST at boot and held resident for the
 * whole session; these thresholds gate that boot claim — a runtime re-claim is
 * impossible (21,504 < 31,744) and must never be attempted.
 *
 * Dependency-free leaf header (only esp_heap_caps.h / esp_log.h): safe to include
 * from any module, including bare hardware drivers.
 */
#pragma once

#include "esp_heap_caps.h"
#include "esp_log.h"

/* The scarce pool: internal SRAM that can back a DMA descriptor/buffer. */
#define NOCSIF_DMA_CAPS  (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)

/* Largest CONTIGUOUS free int-DMA block — the ONLY number that governs a radio
 * bring-up (a controller/stack block must fit in ONE run). Gate on this. */
static inline size_t nocsif_int_dma_largest(void)
{
    return heap_caps_get_largest_free_block(NOCSIF_DMA_CAPS);
}

/* Total free int-DMA bytes — telemetry only. Do NOT gate on this: freeing bytes
 * does not widen the contiguous hole (measured — freeing ~10 KB moved free
 * 28k->38k and left largest unchanged). */
static inline size_t nocsif_int_dma_free(void)
{
    return heap_caps_get_free_size(NOCSIF_DMA_CAPS);
}

/* One-line int-DMA telemetry through the shared gauge, matching the hand-written
 * "<context>; int-dma free=%u largest=%u" lines it replaces. Uses the including
 * file's `TAG` (every NocSif module defines `static const char *TAG`). */
#define nocsif_log_dma_free(ctx) \
    ESP_LOGI(TAG, "%s; int-dma free=%u largest=%u", (ctx), \
             (unsigned)nocsif_int_dma_free(), (unsigned)nocsif_int_dma_largest())

/* ---- per-radio bring-up gates (contiguous int-DMA required, in ONE run) ------ *
 * MEASURED on this board, not guessed (docs/RAM-BUDGET.md "Measurement & uncertainty"):
 *
 *   BLE  : largest 27,648 -> "BLE_INIT: Malloc failed" (a failed
 *          esp_bt_controller_init then blows the interrupt watchdog and panics);
 *          31,736 -> controller up + host synced. 31,744 sits between the two,
 *          above every observed failure and below every observed success. The
 *          controller block is claimed ONCE from the pristine ~90 KB boot pool and
 *          held resident for the whole BT-master session — a runtime re-claim is
 *          IMPOSSIBLE (WiFi-released largest tops out at 21,504) and must never be
 *          attempted. MAX_ACT coupling: the block folds in CONFIG_BT_CTRL_BLE_MAX_ACT
 *          activity slots (828 B each), so re-verify this gate if MAX_ACT changes
 *          (see coex_guard.c).
 *
 *   LORA : the SX1262 worker's stack. Kept for reference/telemetry only — the LoRa
 *          worker stack now lives in PSRAM (docs/RAM-BUDGET.md remake #8), so this
 *          gate is NO LONGER applied to spawn it. LoRa fits alongside BLE + WiFi.
 *
 *   FLOOR: the historical fragmentation wall for other int-DMA claimers.
 *
 *   USB  : the one-time tinyusb_driver_install (the first USB mode pick): esp_tinyusb creates its
 *          device task with an INTERNAL 4 KB stack (xTaskCreatePinnedToCore, no caps option) plus its
 *          context, the CDC-ACM rings (512+512) and the MSC APP->USB FAT handoff — ~5-7 KB of runtime
 *          internal memory. Pre-Phase-A the steady-state largest was ~2 KB, so File Share REFUSED
 *          (docs/DMA-COEXISTENCE-VERIFICATION.md §2). RAM Phase A3 makes the entry deterministic:
 *          usb_gadget.c claims this much from the pristine boot pool and frees it immediately before the
 *          install, so the install lands in that hole no matter what fragmented since; the gate below is
 *          then only ever failed when the reserve itself was never claimed (safe mode / boot alloc
 *          failure) and is reported honestly on the USB screen.
 */
#define NOCSIF_RADIO_MIN_DMA_BLE    31744
#define NOCSIF_RADIO_MIN_DMA_LORA   12288
#define NOCSIF_RADIO_MIN_DMA_FLOOR  24576
#define NOCSIF_RADIO_MIN_DMA_USB    8192
