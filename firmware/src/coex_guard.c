/*
 * coex_guard.c — build-time guard for the load-bearing coexistence sdkconfig keys.
 * Phase 0 of the RAM remediation (docs/design/meta-prompts/ram-org-remediation.md;
 * docs/RAM-BUDGET.md conflict C5 "stale/regenerated sdkconfig reverts a key").
 *
 * THE LANDMINE (C5): a build from a stale generated sdkconfig, or a fullclean regen
 * from an incomplete defaults file, can SILENTLY revert a RAM-critical key — putting
 * the 96 KB LVGL object pool back in internal .bss (breaks BLE+WiFi coexistence),
 * forcing every <=16 KB alloc internal (drains the int-DMA pool), or shrinking the
 * BLE controller's activity table. The symptom (BLE Malloc-failed, heavy-app WDT
 * reboots) looks like a runtime bug; nothing used to re-check the keys.
 *
 * THE GUARD: these #error checks read the GENERATED sdkconfig macros at compile time.
 * If any key drifts from its intended value, the BUILD FAILS with a named message —
 * a silent revert can no longer ship. This is the "re-grep the four load-bearing
 * sdkconfig keys" contract from the plan, enforced by the compiler instead of by a
 * human remembering to look. Keep the intended values in lockstep with
 * firmware/sdkconfig.defaults.
 *
 * (The matching RUNTIME check is the main.c boot heartbeat's int-dma largest gauge +
 * the one-line coex-config echo it prints at boot; see docs/RAM-BUDGET.md
 * "How to verify on-device": boot RAM% 54.2 correct vs 39.2 broken.)
 */
#include "sdkconfig.h"
#include "coex.h"

/* 1 — LVGL object heap must live in PSRAM (lv_mem_psram.c). Without it the ~96 KB
 *     pool sits in internal .bss and BLE + WiFi cannot coexist. */
#if !defined(CONFIG_LV_USE_CUSTOM_MALLOC)
#error "coex guard: CONFIG_LV_USE_CUSTOM_MALLOC reverted — LVGL heap would return to internal .bss and break BLE+WiFi coexistence. Restore =y in sdkconfig.defaults and regen."
#endif

/* 2 — Allocations <=4096 go internal, larger prefer PSRAM. A revert to the 16384
 *     default drains the scarce int-DMA pool (freeing stacks then buys nothing). */
#if !defined(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL) || (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL != 4096)
#error "coex guard: CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL must be 4096 — a revert drains the int-DMA pool. Restore =4096 in sdkconfig.defaults and regen."
#endif

/* 3 — WiFi RX block-ack window. STATIC_RX is cut to 4 (ceiling RX_BA_WIN <= 2*4 = 8);
 *     an unpinned regen defaults it higher and trips the wifi_init.c build #error.
 *     Pinned to 6 (see sdkconfig.defaults). */
#if !defined(CONFIG_ESP_WIFI_RX_BA_WIN) || (CONFIG_ESP_WIFI_RX_BA_WIN != 6)
#error "coex guard: CONFIG_ESP_WIFI_RX_BA_WIN must be 6 — an unpinned regen defaults higher and violates RX_BA_WIN <= 2*STATIC_RX. Restore =6 in sdkconfig.defaults and regen."
#endif

/* 4 — BLE controller activity table. Each slot is 828 B folded into the ~31.7 KB
 *     contiguous block the reserve claims at boot; 3 lets the phone advert + an
 *     observer scan coexist. Changing it changes the reserve size — re-verify the
 *     BLE gate (NOCSIF_RADIO_MIN_DMA_BLE) if you do. */
#if defined(CONFIG_BT_ENABLED)
#if !defined(CONFIG_BT_CTRL_BLE_MAX_ACT) || (CONFIG_BT_CTRL_BLE_MAX_ACT != 3)
#error "coex guard: CONFIG_BT_CTRL_BLE_MAX_ACT must be 3 — it sizes the reserved BLE controller block (3 x 828 B). Restore =3 in sdkconfig.defaults, or update the BLE gate, then regen."
#endif
#endif
