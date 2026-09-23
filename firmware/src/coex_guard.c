/*
 * coex_guard.c — compile-time guard that protects the load-bearing radio-coexistence sdkconfig
 * keys. Part of Phase 0 of the RAM remediation
 * (docs/design/meta-prompts/ram-org-remediation.md; docs/RAM-BUDGET.md conflict C5
 * "stale/regenerated sdkconfig reverts a key").
 *
 * THE RISK (conflict C5): a build using a stale generated sdkconfig, or a fullclean regenerated
 * from an incomplete defaults file, can silently revert a RAM-critical key — putting the 96 KB LVGL
 * object pool back into internal .bss (breaking BLE+WiFi coexistence), forcing every small
 * allocation internal (draining the internal-DMA pool), or shrinking the BLE controller's activity
 * table. The symptom (BLE malloc failures, watchdog reboots under heavy use) reads like a runtime
 * bug, and nothing used to catch the revert.
 *
 * THE FIX: these #error checks read the generated sdkconfig macros at compile time. If any key has
 * drifted from its intended value, the build fails with a named error message, so a silent revert
 * can no longer ship. This is the "re-check the load-bearing sdkconfig keys" step from the plan,
 * enforced by the compiler instead of relying on someone to remember. Keep the intended values here
 * in sync with firmware/sdkconfig.defaults.
 *
 * (The matching RUNTIME check is main.c's boot heartbeat, which prints the internal-DMA largest-
 * block gauge and a one-line coex-config summary at boot; see docs/RAM-BUDGET.md
 * "How to verify on-device" — boot RAM% 54.2 is correct, 39.2 means something is broken.)
 */
#include "sdkconfig.h"
#include "coex.h"

/* 1 — the LVGL object heap must live in PSRAM (lv_mem_psram.c). Without it, the ~96 KB pool sits
 *     in internal .bss and BLE + WiFi can no longer coexist. */
#if !defined(CONFIG_LV_USE_CUSTOM_MALLOC)
#error "coex guard: CONFIG_LV_USE_CUSTOM_MALLOC reverted — LVGL heap would return to internal .bss and break BLE+WiFi coexistence. Restore =y in sdkconfig.defaults and regen."
#endif

/* 2 — allocations of 4096 bytes or less go internal; larger ones prefer PSRAM. Reverting to the
 *     16384 default drains the scarce internal-DMA pool (freeing task stacks would buy nothing). */
#if !defined(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL) || (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL != 4096)
#error "coex guard: CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL must be 4096 — a revert drains the int-DMA pool. Restore =4096 in sdkconfig.defaults and regen."
#endif

/* 3 — WiFi RX block-ack window. STATIC_RX is cut to 4 (so RX_BA_WIN must stay <= 2*4 = 8); an
 *     unpinned regen defaults it higher, which trips wifi_init.c's own build-time #error. Pinned to
 *     6 in sdkconfig.defaults. */
#if !defined(CONFIG_ESP_WIFI_RX_BA_WIN) || (CONFIG_ESP_WIFI_RX_BA_WIN != 6)
#error "coex guard: CONFIG_ESP_WIFI_RX_BA_WIN must be 6 — an unpinned regen defaults higher and violates RX_BA_WIN <= 2*STATIC_RX. Restore =6 in sdkconfig.defaults and regen."
#endif

/* 4 — the BLE controller's activity table. Each slot costs 828 B out of the ~31.7 KB contiguous
 *     block the boot reserve claims; 3 slots lets a phone advertisement and an observer scan
 *     coexist. Changing this changes the required reserve size — re-verify the BLE gate
 *     (NOCSIF_RADIO_MIN_DMA_BLE) if it changes. */
#if defined(CONFIG_BT_ENABLED)
#if !defined(CONFIG_BT_CTRL_BLE_MAX_ACT) || (CONFIG_BT_CTRL_BLE_MAX_ACT != 3)
#error "coex guard: CONFIG_BT_CTRL_BLE_MAX_ACT must be 3 — it sizes the reserved BLE controller block (3 x 828 B). Restore =3 in sdkconfig.defaults, or update the BLE gate, then regen."
#endif
#endif

/* 5 — CONFIG_FATFS_ALLOC_PREFER_EXTRAM must stay =n (#228), and the SD path must not depend on where
 *     FatFs buffers land. What this key REALLY moves (measured 2026-09-23, docs/LESSONS.md): only the
 *     small FatFs scratch (the 512 B LFN buffer) — the ~25 KB volume context (FATFS window + FIL
 *     buffers, CONFIG_FATFS_SECTOR_4096) is over the ALWAYSINTERNAL limit and lives in PSRAM either way,
 *     so every FatFs sector reaches sdmmc with a PSRAM buffer regardless. That is why the SD crash fix
 *     does not rely on this key: sd_bounce.c serves spi_master's bounce, and sdcard.c's diskio driver
 *     stages PSRAM sectors through one RTC-fast-memory sector. Keep the key pinned so the config stays
 *     deliberate (a STALE generated sdkconfig is the way it silently flips; see conflict C5). In
 *     sdkconfig a `=n` bool is left undefined, so the guard is "must be undefined". */
#ifdef CONFIG_FATFS_ALLOC_PREFER_EXTRAM
#error "coex guard: CONFIG_FATFS_ALLOC_PREFER_EXTRAM must be n (left unset) — =y puts FatFs buffers in PSRAM, so every SD sector bounces through int-DMA and fragments the pool under BLE+WiFi. Set =n in sdkconfig.defaults AND delete the generated sdkconfig.* so it regenerates."
#endif
