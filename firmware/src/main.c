/*
 * NocSif — LilyGo T-Watch Ultra firmware (native C++/ESP-IDF core, Option B)
 *
 * M0:   prove the board boots, the quad PSRAM is exposed, USB-Serial/JTAG works.
 * M0.1: scan the I2C bus (SDA=3/SCL=2) — confirm PMU/expander/RTC/etc. respond.
 * M1:   CO5300 QSPI AMOLED bring-up — power sequence + first pixels.
 * M2:   CST9217 capacitive touch — polled reads, native coordinates.
 * M3:   LVGL v9 (esp_lvgl_port) on the panel + touch — minimal demo UI.
 * M4-P1: microSD FAT mount over the shared SPI bus (no USB yet).
 * TinyUSB device classes (CDC/MSC/HID), radio and NFC land in M4+.
 */
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "i2c_scan.h"
#include "power.h"
#include "xl9555.h"
#include "display.h"
#include "touch.h"
#include "ui.h"
#include "sdcard.h"
#include "usb_gadget.h"
#include "ducky.h"
#include "buttons.h"
#include "rtc.h"
#include "settings.h"
#include "reliability.h"
#include "coex.h"             /* NOCSIF_DMA_CAPS + nocsif_int_dma_largest/_free — shared int-DMA gauge */
#include "radio_state.h"      /* RAM Phase 2 §4.13: shared live radio-state accessor (CC-tile self-test) */
#include "logbook.h"
#include "nfc.h"              /* M6-P1: optional boot NFC self-test (compile-gated, default off) */
#include "lora.h"             /* M9: optional boot LoRa (SX1262) proof-of-life (compile-gated, default off) */
#include "gnss.h"             /* M8: optional boot GNSS (u-blox/LS550G) proof-of-life (compile-gated, default off) */
#include "wifi.h"             /* M5-P1: WiFi station worker (lazy radio bring-up) */
#include "ble.h"              /* M7: phone companion — boot auto-connect to the last saved phone */
#include "imu.h"              /* M11-A1: BHI260AP inertial sensor hub worker */
#include "audio.h"            /* M11-E1: MAX98357A I2S speaker worker (tones / cues) */
#include "mic.h"              /* M11-E1·2: PDM microphone worker (level meter) */
#include "weather.h"          /* §4.1: Weather worker (Open-Meteo over WiFi) */
#include "pm.h"               /* M11 power: CPU dynamic frequency scaling (DFS) */
#include "ota.h"              /* M-OTA: A/B firmware update worker + rollback-confirm */
#include "display_io.h"       /* RAM Phase A1: PSRAM-direct panel IO telemetry (DMA underruns) */
#include "governor.h"         /* §4.6 Connectivity Governor P1: WiFi power policy tick */

static const char *TAG = "nocsif";

/* RAM remediation Phase A — COEXV: per-stage int-DMA snapshots through app_main (the method of
 * docs/DMA-COEXISTENCE-VERIFICATION.md) plus a steady-state co-residence probe in the heartbeat
 * (GNSS + LoRa bring-up + an 8 KB contiguous claim as the USB File-Share entry proxy). Compile-gated,
 * default OFF; build with -DNOCSIF_COEXV=1 to measure a build. Every line carries COEXV_TAG so the
 * RUNNING binary is verifiable by content (findstr COEXV_A1 firmware.bin), never by the cached
 * compile-time banner (gotcha-display-dma-hang). */
#ifndef NOCSIF_COEXV
#define NOCSIF_COEXV 0
#endif
#if NOCSIF_COEXV
#define COEXV_TAG "COEXV_A1"
#define COEXV_SNAP(stage) ESP_LOGW(TAG, COEXV_TAG " stage=%-18s free=%u largest=%u", (stage), \
        (unsigned)nocsif_int_dma_free(), (unsigned)nocsif_int_dma_largest())
#else
#define COEXV_SNAP(stage) ((void)0)
#endif

/* §4.10 — TLS-under-WiFi gate (Phase 0). Compile-gated, ships OFF: build with -DNOCSIF_TLS_PROBE=1. Once
 * the STA links (BLE resident, everything else as shipped) a PSRAM-stacked task HTTPS-GETs a small file from
 * raw.githubusercontent.com through the mbedTLS CA bundle and logs the outcome + the int-DMA curve — the
 * go / no-go for pulling firmware from GitHub. Every line carries TLSPROBE so the running binary is
 * verifiable by content (findstr TLSPROBE firmware.bin). */
#ifndef NOCSIF_TLS_PROBE
#define NOCSIF_TLS_PROBE 0
#endif
#if NOCSIF_TLS_PROBE
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#include "wifi.h"
#define TLSPROBE_URL "https://raw.githubusercontent.com/lieff/minimp3/master/LICENSE"
static void tls_probe_task(void *arg)
{
    (void)arg;
    int waited = 0;
    while (!nocsif_wifi_connected() && waited < 90) { vTaskDelay(pdMS_TO_TICKS(1000)); waited++; }
    if (!nocsif_wifi_connected()) {
        ESP_LOGW(TAG, "TLSPROBE no STA link after %d s — skipped", waited);
        vTaskDeleteWithCaps(NULL);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(3000));                       /* let the link settle (modem-sleep, DHCP) */
    ESP_LOGW(TAG, "TLSPROBE start: %s; int-dma free=%u largest=%u", TLSPROBE_URL,
             (unsigned)nocsif_int_dma_free(), (unsigned)nocsif_int_dma_largest());
    esp_http_client_config_t cfg = {
        .url               = TLSPROBE_URL,
        .method            = HTTP_METHOD_GET,
        .timeout_ms        = 15000,
        .buffer_size       = 2048,
        .buffer_size_tx    = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    int64_t t0 = esp_timer_get_time();
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_err_t err = c ? esp_http_client_open(c, 0) : ESP_FAIL;
    ESP_LOGW(TAG, "TLSPROBE open -> %s in %lld ms; int-dma free=%u largest=%u", esp_err_to_name(err),
             (long long)((esp_timer_get_time() - t0) / 1000), (unsigned)nocsif_int_dma_free(), (unsigned)nocsif_int_dma_largest());
    int status = -1, total = 0;
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(c);
        status = esp_http_client_get_status_code(c);
        char *buf = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM);
        int n;
        while (buf && (n = esp_http_client_read(c, buf, 2048)) > 0) total += n;
        heap_caps_free(buf);
        esp_http_client_close(c);
    }
    if (c) esp_http_client_cleanup(c);
    ESP_LOGW(TAG, "TLSPROBE %s: status %d, %d bytes in %lld ms; int-dma free=%u largest=%u",
             (err == ESP_OK && status == 200 && total > 0) ? "PASS" : "FAIL", status, total,
             (long long)((esp_timer_get_time() - t0) / 1000), (unsigned)nocsif_int_dma_free(), (unsigned)nocsif_int_dma_largest());
    vTaskDeleteWithCaps(NULL);
}
#endif

/* Reliability on-device verification hook (default 0 — ships off). At 1, app_main aborts ~1.5 s
 * into boot (unless already in safe mode) to exercise the core-dump crash record + the boot-loop
 * guard: three rapid aborts in a row trip safe mode, which then SKIPS this test (the watch boots
 * normally), proving the loop is broken. Set back to 0 after verifying. */
#define NOCSIF_REL_CRASH_TEST 0

/* UI-shell P5 — the M3 colour self-test (RED/GREEN/BLUE/WHITE bands + a 2 s hold) validated the
 * CO5300 BGR flip during bring-up. It now sits BEFORE the LVGL boot splash, so at 1 it flashes for
 * 2 s on every cold boot. Default OFF (0) so the splash is the first thing seen; flip to 1 to
 * eyeball the bands + the B,G,R swap after a display change. */
#define NOCSIF_BOOT_BAND_TEST 0

static void nocsif_banner(void)
{
    printf("\n");
    printf("        *   .        .      *        .     *\n");
    printf("   .        *      N o c S i f      *    .\n");
    printf("     *  .        .        *      .      *\n");
    printf("   T-Watch Ultra firmware   v%s\n", NOCSIF_VERSION);
    printf("        .      *       .        *    .\n\n");
}

void app_main(void)
{
    nocsif_banner();

    /* Reliability hardening (Phase A / A2) — bring the persistent logbook up FIRST (after the
     * decorative banner) so it tees the ESP_LOG stream to flash from here on: the reliability
     * crash record, the reset reason, and the whole boot sequence all persist for untethered
     * reading in System > Diagnostics. Non-fatal if the 'logs' partition is missing. */
    nocsif_logbook_init();

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "chip: ESP32-S3 rev v%d.%d, %d core(s), features=0x%08x",
             chip.revision / 100, chip.revision % 100, chip.cores, (unsigned)chip.features);

    size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psram) {
        ESP_LOGI(TAG, "PSRAM: %u bytes (%.2f MB) exposed - quad SPIRAM OK",
                 (unsigned)psram, psram / (1024.0 * 1024.0));
    } else {
        ESP_LOGE(TAG, "PSRAM: NONE exposed — check quad SPIRAM sdkconfig (M0 gate FAILED)");
    }
    ESP_LOGI(TAG, "internal free heap: %u bytes", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    /* Reliability hardening (Phase A) — run FIRST, before any heavy init. Classifies the reset
     * reason, folds a stored core dump into the last-crash record (NVS + serial), and runs the
     * boot-loop guard. safe==true means the guard tripped (repeated crash-class boots) → this boot
     * skips the risky/heavy subsystems (a non-detached USB default) so the watch reaches a usable
     * state instead of crash-looping. Future milestones (e.g. M6 NFC) gate their init on this too. */
    nocsif_reliability_boot_check();
    bool safe = nocsif_reliability_safe_mode();

#if NOCSIF_REL_CRASH_TEST
    if (!safe) {
        ESP_LOGE(TAG, "CRASH TEST: aborting in 1.5s to exercise the coredump + boot-loop guard "
                      "(reason=%s)", nocsif_reliability_reset_reason_str());
        vTaskDelay(pdMS_TO_TICKS(1500));   /* let the log flush; keep the loop from spinning too fast */
        abort();                            /* → panic → core dump → reboot; 3 in a row trip safe mode */
    }
#endif

    /* UI-shell P4.4 — persistent settings store (NVS). Bring it up early so the button
     * bindings (FN target, PWR double-press shortcut) are loaded before the button service.
     * Non-fatal: settings fall back to defaults (RAM only) if NVS can't init. */
    if (nocsif_settings_init() != ESP_OK) {
        ESP_LOGE(TAG, "P4.4 settings init FAILED — using defaults (see errors above).");
    }

    /* M11 power — apply the persisted CPU DFS policy (Settings > Power > Power saver). Needs settings;
     * a no-op unless "dfs_en" is on. Early so any downclock applies through the rest of boot. */
    nocsif_pm_init();

    /* M0.1 — bring up the shared I2C bus and scan for the on-board devices. */
    if (nocsif_i2c_init() == ESP_OK) {
        nocsif_i2c_scan();
    }

    /* M1 — CO5300 display bring-up. Power the display rail FIRST (AXP2101 ALDO2
     * 3.3V + XL9555 IO7), then init the panel and draw a test pattern. */
    nocsif_power_init();
    nocsif_power_display_rail(true);              /* AXP2101 ALDO2 = 3.3V */

    nocsif_xl9555_init();
    nocsif_xl9555_display_power(true);            /* XL9555 IO7 = display power on */
    nocsif_xl9555_touch_reset(true);             /* IO10 released (explicit) */
    nocsif_xl9555_haptic_enable(true);           /* IO6 (matches LilyGo; harmless) */
    vTaskDelay(pdMS_TO_TICKS(50));                /* let the 3.3V rail settle before reset */

    bool display_ok = (nocsif_display_init() == ESP_OK);
    if (display_ok) {
#if NOCSIF_BOOT_BAND_TEST
        /* Red-fill sanity check for the M3 BGR flip: the bands must STILL read
         * RED / GREEN / BLUE / WHITE top->bottom (panel now BGR element order,
         * raw draw path writes B,G,R to compensate). Hold it briefly so it's
         * eyeballable before LVGL paints the UI over it. (P5: gated off by default
         * so the boot splash — not a 2 s colour flash — is the first frame.) */
        nocsif_display_test_bands();
        ESP_LOGI(TAG, "M3 colour sanity: bands must read RED / GREEN / BLUE / WHITE "
                      "top->bottom. If red reads blue, the BGR flip / B,G,R swap is wrong.");
        vTaskDelay(pdMS_TO_TICKS(2000));
#endif
    } else {
        ESP_LOGE(TAG, "M1 display init FAILED — see errors above.");
    }

    /* M2 — CST9217 touch (already un-gated at cold boot; reset released above).
     * NOTE: no polling task here anymore. The LVGL input device (nocsif_ui_init)
     * is now the SOLE reader of nocsif_touch_read — a second reader would race to
     * consume reports and drop touches. */
    bool touch_ok = (nocsif_touch_init() == ESP_OK);
    if (!touch_ok) {
        ESP_LOGE(TAG, "M2 touch init FAILED — see errors above.");
    }

    /* UI-shell P4.2 — PCF85063A real-time clock (shared I2C bus, 0x51). Bring it up BEFORE the
     * UI so the very first header render (Home) shows a real time, not "--:--". Seeds itself
     * from the build timestamp if the oscillator-stop flag says the time is unreliable. */
    if (nocsif_rtc_init() != ESP_OK) {
        ESP_LOGE(TAG, "P4.2 RTC init FAILED — header clock will show --:-- (see errors above).");
    }

    /* UI-shell P4.3 — AXP2101 battery fuel gauge. Ensure the gauge/detect/ADC enables are set and
     * prime the battery cache BEFORE the UI so the first header render shows a real %, not "--%".
     * The PMU is already attached (nocsif_power_init above). Non-fatal: the getters just report
     * "--%" until a later tick if this fails. */
    if (nocsif_power_gauge_config() != ESP_OK) {
        ESP_LOGE(TAG, "P4.3 battery gauge config FAILED — header battery will show --%% (see above).");
    }

    /* M7 — claim the BLE controller's memory FIRST, before ANYTHING brings WiFi up.
     *
     * This ordering is load-bearing. The BT controller needs ~30 KB of CONTIGUOUS internal DMA, and
     * once esp_wifi_init has run the largest free hole is capped (measured: 27.6 KB full profile,
     * 31.7 KB lean) no matter how much TOTAL memory is free — fragmentation, not shortage. Right here
     * the heap is still one unbroken run (~86 KB), so the controller gets its block and WiFi then
     * allocates around it.
     *
     * It must precede nocsif_ui_init(): the Control Center restores its persisted Wi-Fi toggle during
     * UI bring-up (ui.c, "make the radio MATCH the persisted Wi-Fi toggle at boot") and that enables
     * the radio ~1.8 s in — before any later hook could run. Deliberately NOT gated on ui_ok: the
     * radio does not need the UI, and by the time ui_ok is known WiFi has already taken the memory.
     * Also arms WiFi's lean buffer profile so it fits in what remains.
     *
     * UNCONDITIONAL (not gated on the persisted Bluetooth toggle): the controller is claimed + held
     * resident even when Bluetooth is "off", so turning it on later is an instant logical re-enable and
     * never a reboot (operator constraint #1) — a released block can't be re-claimed once WiFi is up.
     * WiFi is therefore always armed lean (the block is always held). Only safe mode skips it. */
    COEXV_SNAP("pre-BLE");            /* pristine pool (display/touch/RTC up): ~86-90 KB largest */
    if (!safe) {
        nocsif_ble_init();
        nocsif_wifi_set_lean(true);   /* set before the worker exists; applies at first bring-up */
        nocsif_ble_boot_reserve();    /* claims the block resident (blocks ~4 s); safe-mode no-op inside */
        COEXV_SNAP("post-ble-reserve"); /* BLE controller claimed first: 59,392 on the pre-A1 build */
        /* RAM Phase 2 (#12/C7) — claim the audio I2S TX DMA next, from the pool that still has ~53 KB
         * contiguous after BLE, BEFORE nocsif_ui_init() below runs the first esp_wifi_init (which caps
         * the hole ~30 KB and, at steady state, ~3 KB). The ~3.8 KB of I2S descriptors (4, trimmed for the
         * IMU margin) are internal-DMA only (no PSRAM route), so ORDER is the only lever — this is why the Signal-Hunt cue / boot
         * chime lost the race before. Channel left disabled (no rail/clock); tx_ready gates the cue. */
        nocsif_audio_boot_reserve();
        COEXV_SNAP("post-audio-reserve");
        /* RAM Phase A3 follow-up — claim the USB File-Share entry block HERE, from the same pristine
         * region as the BLE + I2S reserves, rather than after ui_init: claimed late it lands wherever TLSF
         * has a hole and can split the post-WiFi tail (measured: post-imu largest 30,720 vs 22,528 across
         * two otherwise-equal builds). Released just before the first tinyusb_driver_install. */
        nocsif_usb_gadget_boot_reserve();
        COEXV_SNAP("post-usb-reserve");
    }

    /* §4.1 Weather worker — created HERE, before nocsif_ui_init()'s esp_wifi_init, for the same reason as
     * the reserves above: its 8 KB stack must stay internal (it writes NVS on-task) and, created after WiFi,
     * it carved the post-WiFi tail (measured post-imu 30,720 -> 22,528 = exactly this stack once the USB
     * reserve had taken the holes it used to fit in). Creating it is cheap (settings + a task; the first
     * fetch is lazy and gates on a station link at fetch time, so the WiFi worker need not exist yet).
     * Safe-mode-gated inside. */
    nocsif_weather_init();
    COEXV_SNAP("post-weather-init");

    /* M3 — LVGL v9 on the CO5300 panel + CST9217 touch. Needs both up. */
    bool ui_ok = false;
    if (display_ok && touch_ok) {
        if (nocsif_ui_init() == ESP_OK) {
            ui_ok = true;
            ESP_LOGI(TAG, "M3 LVGL up — tap the button; the bottom label tracks live touch coords.");
        } else {
            ESP_LOGE(TAG, "M3 LVGL init FAILED — see errors above.");
        }
    } else {
        ESP_LOGE(TAG, "M3 LVGL skipped — display_ok=%d touch_ok=%d", display_ok, touch_ok);
    }
    /* ui_init = taskLVGL (16 KB internal) + esp_wifi_init (the CC toggle restore) — and, pre-A1, the
     * 29.5 KB flush stage. Measured 30,720 largest here on the pre-A1 build. */
    COEXV_SNAP("post-ui-init");

    /* P5 — display brightness is held at 0 from nocsif_display_init() until the UI reveals it after
     * flushing the boot splash (no power-on white flash). If the UI didn't come up, reveal the
     * black-filled panel here so a display-OK-but-UI-failed boot isn't a mysteriously dark screen. */
    if (!ui_ok && display_ok) {
        nocsif_display_set_brightness(0xFF);
    }

    /* M4-P1/P3 — microSD over SDSPI (shared SPI bus). Power ALDO1 FIRST, let the 3.3V rail
     * settle, then bring the card up to a RAW sdmmc_card_t. FAT is NOT mounted here: the
     * USB-MSC helper owns the FAT mount + the host<->app handoff in gadget mode (usb_gadget.c). */
    nocsif_power_sd_rail(true);                   /* AXP2101 ALDO1 = 3.3V (microSD) */
    vTaskDelay(pdMS_TO_TICKS(50));                /* let the rail settle before I/O */
    bool sd_ok = (nocsif_sdcard_init() == ESP_OK);
    if (!sd_ok) {
        ESP_LOGE(TAG, "M4-P1 SD init FAILED — see errors above (MSC will be unavailable).");
    }
    /* P5 — resolve the boot splash 'storage' line from the real SD result (honest 'no card' on the
     * cardless path). FAT itself is mounted later by the USB-MSC helper; here the raw card is up. */
    nocsif_ui_boot_report_storage(sd_ok, sd_ok ? "ready" : "no card");

    /* M4-P2/P3/P4 — TinyUSB gadget worker, idle until the on-screen "USB Gadget" toggle.
     * Deferred so the USB-Serial/JTAG console (COM7) stays live for normal flash/log;
     * only a deliberate tap hands the USB PHY to USB-OTG and brings up the CDC+MSC+HID
     * composite (CDC console, /sd drive, and a USB HID keyboard). */
    nocsif_usb_gadget_init();
    COEXV_SNAP("post-usb-init");      /* 6 KB worker stack + ~8 KB MSC storage object (int-DMA) */

    /* UI-shell P4.5.4 — apply the persisted default USB mode (groundwork). The store currently
     * holds DETACHED, so boot stays detached and the USB-Serial/JTAG console (COM7) remains live
     * for flashing + logs; a future Settings control can persist a different default via
     * nocsif_settings_set_i32("usb_def_mode", <mode>). A non-DETACHED default would install TinyUSB
     * here at boot (COM7 dark until a reboot into Detached). request_mode is the same non-blocking
     * path a UI tap uses; the worker applies it off the UI thread. NVS key is <=15 chars. */
    int32_t usb_def = nocsif_settings_get_i32("usb_def_mode", NOCSIF_USB_MODE_DETACHED);
    /* Reliability: a non-detached default installs TinyUSB at boot (memory pressure — the DMA-hang
     * class), so in safe mode force detached to keep the boot light and COM7 live for recovery. */
    if (usb_def != NOCSIF_USB_MODE_DETACHED && !safe) {
        nocsif_usb_gadget_request_mode((nocsif_usb_mode_t)usb_def);
    } else if (usb_def != NOCSIF_USB_MODE_DETACHED && safe) {
        ESP_LOGW(TAG, "safe mode: ignoring non-detached USB default (%d) this boot", (int)usb_def);
    }
    /* P5 — resolve the boot splash 'usb' line. Boot stays detached (COM7/USB-Serial-JTAG console
     * live) unless a non-detached default is persisted (and not suppressed by safe mode). */
    nocsif_ui_boot_report_usb((usb_def == NOCSIF_USB_MODE_DETACHED || safe) ? "detached" : "starting");

    /* M4-P4 DuckyScript + M-OTA workers are created LAZILY, by the screens that use them (see ui.c:
     * build_hid / build_ble_hid / the macro picker, and build_ota). Their task stacks (6144 + 8192)
     * are internal DMA-capable RAM — the SAME pool the BLE controller needs ~30 KB of and WiFi takes
     * ~58 KB of — and both workers are idle unless you are actually running a macro or installing
     * firmware. Creating them on demand keeps that RAM available so BLE and WiFi can coexist.
     * Every request/getter in those modules is NULL-task-safe, and their _init is idempotent.
     * NOTE: nocsif_ota_confirm() below does NOT need the OTA worker (it is a direct esp_ota call),
     * so a freshly-OTA'd image still cancels its rollback on this boot. */

    /* M5-P1 — WiFi station worker, idle until a scan/toggle request. Creating it is cheap
     * (a task + a command queue; NO radio yet) — the heavy esp_wifi bring-up is lazy on the
     * first enable/scan and gated on safe mode, so boot stays light and the coexistence risk
     * is isolated to first use. The Control-Center WiFi toggle + the WiFi scan screen both
     * drive this worker. No-op in safe mode (nocsif_wifi_available() stays false). */
    nocsif_wifi_init();

    /* §4.1 — Weather worker: created earlier (before nocsif_ui_init) so its internal stack sits with the
     * boot reserves instead of in the post-WiFi tail; see the note there. The first fetch is lazy, on a
     * screen-open / manual refresh, and only when a station link exists. */

    /* §4.6 Connectivity Governor P1 — the WiFi power policy tick (park an idle unlinked STA, modem-sleep
     * when linked+idle, wake on demand). Activity/power only — never a driver deinit. Needs the WiFi +
     * weather workers above; safe-mode no-op inside. */
    nocsif_gov_init();
#if NOCSIF_TLS_PROBE
    xTaskCreateWithCaps(tls_probe_task, "tlsprobe", 12288, NULL, 3, NULL, MALLOC_CAP_SPIRAM);   /* §4.10 gate */
#endif

    /* M11-A1 — BHI260AP inertial sensor hub. Starts a worker that powers ALDO4, probes 0x28 and
     * uploads+boots the sensor-hub firmware, then streams the accelerometer. Independent of the
     * BLE/WiFi single radio (I2C only), so it composes freely. Backgrounded (the ~2-3 s firmware
     * upload runs off this thread) and gated on safe mode internally, so boot stays light. Brought
     * up eagerly (not lazily) because the end-state — wrist-raise auto-wake, motion — wants the IMU
     * running whenever the watch is worn. */
    nocsif_imu_init();
    COEXV_SNAP("post-imu");           /* weather (8 KB) + IMU (6 KB + FIFO) internal stacks */

    /* M11-E1 — audio output worker (MAX98357A speaker on BLDO2). Cheap to create (a task + queue;
     * the I2S channel is allocated lazily on the first sound), independent of the radio (its own I2S
     * port + rail). With the DRV2605 haptic hardware-dead on this unit, this is the alerting channel.
     * A boot chime is queued below once the UI is up so it lands after the noisy init settles. */
    nocsif_audio_init();

    /* M11-E1·2 — PDM microphone worker: created LAZILY by the screens that record (ui.c build_mic +
     * the Voice Memos recorder), for the same internal-DMA reason as ducky/OTA above — its 6144-byte
     * stack is idle unless you are actually capturing. nocsif_mic_available() returns false until
     * then, which the mic rows already handle ("n/a"). */

    /* UI-shell P4.1/P4.4 — physical-button input service. Decodes PWR (AXP2101 PWRKEY over
     * I2C) + FN (GPIO0) on its own worker task and dispatches them into the UI action API
     * (FN -> launch the FN target; PWR -> screen off/on, pop-to-Home, power menu, shortcut).
     * Needs the PMU attached (nocsif_power_init) and the UI up (nocsif_ui_init) above.
     * GATE ON ui_ok: taking over the PWR button (nocsif_power_pwrkey_config clears the PMU's
     * hardware long-hold power-off, so firmware owns it) is only safe when the UI is up to
     * SERVICE the button — otherwise a headless boot would have neither hardware nor software
     * power-off. If the UI didn't come up, leave the button service off (PMU hardware power-off
     * stays intact). */
    if (ui_ok) {
        nocsif_buttons_init();
        /* Arm PWR double-press detection only if a shortcut is bound (else a PWR short is instant). */
        nocsif_buttons_set_pwr_double_enabled(nocsif_settings_pwr_double()[0] != '\0');
    } else {
        ESP_LOGW(TAG, "buttons: UI not up — leaving the PMU hardware power-off intact (no button service)");
    }

    /* Reliability hardening (Phase A) — arm the UI-liveness watchdog now that the LVGL port is up.
     * It subscribes the LVGL task to the Task-WDT so a BLOCKED render wedge (the DMA-hang class,
     * which does not starve the idle task the stock WDT watches) auto-reboots instead of freezing. */
    if (ui_ok) {
        nocsif_reliability_ui_liveness_arm();
        /* M-OTA — the UI is up and has rendered the home screen, and the liveness watchdog now guards
         * it: that is a strong "this image boots and works" signal, so confirm a freshly-OTA'd image
         * here (cancel the bootloader's pending rollback) rather than waiting the full ~30 s dwell —
         * a much smaller window in which a stray reset could revert the update. No-op unless this boot
         * is a PENDING_VERIFY OTA image. A UI-less boot never reaches here, so a broken image still
         * rolls back; the reliability safe-mode backstops any crash that appears only later. */
        nocsif_ota_confirm();
    }

    /* M7 phone companion — boot auto-connect. The controller was already brought up (and advertising
     * started) by nocsif_ble_boot_reserve() above, ahead of WiFi, so a saved phone reconnects on its
     * own from here — the watch is the peripheral and iOS reconnects to a bonded accessory once it
     * advertises. This call is the belt-and-suspenders path for the case where the reservation was
     * skipped or the worker came up late; it is idempotent (a live session is left alone). */
    if (ui_ok && !safe && nocsif_ble_bt_enabled() && nocsif_ble_phone_count() > 0) {
        ESP_LOGI(TAG, "phone: boot auto-connect (Bluetooth on, %d saved)", nocsif_ble_phone_count());
        nocsif_ble_phone_boot_autostart();
    }

    /* M6-P1 NFC hardware self-test hook (grab-and-run bring-up test) — proves the RFAL SPI port
     * (NFC rail + SPI3 2nd-device + rfalNfcInitialize) and measures the RF front-end / antenna
     * health WITHOUT a screen tap or a tag. Default OFF (like the reliability NOCSIF_*_TEST
     * hooks); build -DNOCSIF_NFC_BOOT_SELFTEST=1 to run it and read the verdict over serial.
     *   >>> NOTE (2026-08-16): NFC is UNVERIFIED end-to-end — the original T-Watch Ultra unit has
     *       a HARDWARE fault in the NFC transmit path (chip + all rails healthy, but tx_on never
     *       asserts, no carrier, no over-current; LilyGo's own firmware fails identically).
     *       Awaiting an RMA/replacement watch. On a NEW unit, flash with this flag and watch for
     *       the 'NFC HW SELF-TEST' block + 'VERDICT' line (see nfc.cpp hw_selftest / RESUME.md).
     * Gated on ui_ok + !safe. Normal builds are unaffected — NFC stays lazy on first screen entry. */
#ifndef NOCSIF_NFC_BOOT_SELFTEST
#define NOCSIF_NFC_BOOT_SELFTEST 0
#endif
#if NOCSIF_NFC_BOOT_SELFTEST
    if (ui_ok && !safe) {
        ESP_LOGW(TAG, "M6-P1 NFC boot self-test: RF front-end / antenna health check + discovery (no tap needed)");
        nocsif_nfc_init();
        vTaskDelay(pdMS_TO_TICKS(300));
        nocsif_nfc_request_selftest();
    }
#endif

    /* M9 SX1262 LoRa boot self-test hook (grab-and-run) — powers ALDO3, selects the built-in antenna
     * (XL9555 IO11), brings the SX1262 up via RadioLib @915 MHz, and TRANSMITS a few test packets to
     * confirm the TX path (TX_DONE). ⚠ This EMITS on 915 MHz ISM (brief; own device / own spectrum) —
     * it is NOT a passive probe. Default OFF; build -DNOCSIF_LORA_BOOT_SELFTEST=1 to run it and read
     * the 'LoRa TX TEST' block + 'VERDICT' line over serial (see lora.cpp). (Slice 1 already confirmed
     * the chip is alive; this is slice 2a's TX path.) Gated on ui_ok + !safe — normal builds unaffected. */
#ifndef NOCSIF_LORA_BOOT_SELFTEST
#define NOCSIF_LORA_BOOT_SELFTEST 0
#endif
#if NOCSIF_LORA_BOOT_SELFTEST
    if (ui_ok && !safe) {
        ESP_LOGW(TAG, "M9 LoRa boot self-test: SX1262 bring-up + TX test @915 MHz (emits briefly)");
        nocsif_lora_init();
        vTaskDelay(pdMS_TO_TICKS(300));
        nocsif_lora_request_selftest();
    }
#endif

    /* M9 LoRa Channel-Activity boot self-test — brings the SX1262 up and runs a few band sweeps
     * across 902–928 MHz, logging the per-channel RSSI spectrum + a home-channel CAD verdict. Unlike
     * the TX test this is PASSIVE (RX / CAD only, no emission) and fully solo-verifiable — a quiet band
     * reads a noise floor and CAD returns free. Default OFF; build -DNOCSIF_LORA_ACTIVITY_SELFTEST=1 to
     * run it and read the 'CHANNEL-ACTIVITY SELF-TEST' block + 'VERDICT' over serial (see lora.cpp).
     * Gated on ui_ok + !safe — normal builds unaffected. */
#ifndef NOCSIF_LORA_ACTIVITY_SELFTEST
#define NOCSIF_LORA_ACTIVITY_SELFTEST 0
#endif
#if NOCSIF_LORA_ACTIVITY_SELFTEST
    if (ui_ok && !safe) {
        ESP_LOGW(TAG, "M9 LoRa boot self-test: channel-activity band scan 902-928 MHz (passive RX/CAD)");
        nocsif_lora_init();
        vTaskDelay(pdMS_TO_TICKS(300));
        nocsif_lora_request_activity_selftest();
    }
#endif

    /* M9 LoRa Band-Survey boot self-test — brings the SX1262 up (widened to BW500) and runs a few
     * fine 52-bin sweeps across 902-928 MHz, logging the noise floor + the detected-signal list.
     * PASSIVE (RX only, no emission), fully solo-verifiable. Default OFF; build
     * -DNOCSIF_LORA_SURVEY_SELFTEST=1 to read the 'BAND-SURVEY SELF-TEST' block + 'VERDICT' (see
     * lora.cpp). Gated on ui_ok + !safe — normal builds unaffected. */
#ifndef NOCSIF_LORA_SURVEY_SELFTEST
#define NOCSIF_LORA_SURVEY_SELFTEST 0
#endif
#if NOCSIF_LORA_SURVEY_SELFTEST
    if (ui_ok && !safe) {
        ESP_LOGW(TAG, "M9 LoRa boot self-test: band survey 902-928 MHz, 52x500 kHz bins (passive RX)");
        nocsif_lora_init();
        vTaskDelay(pdMS_TO_TICKS(300));
        nocsif_lora_request_survey_selftest();
    }
#endif

    /* M9 LoRa Signal-Hunt boot self-test — parks the SX1262 on 915 MHz and reads the RSSI a few times,
     * logging the "getting warmer" envelope. PASSIVE (RX only, no emission). This is the engine behind
     * hunting a Band-Survey-detected signal by energy. Default OFF; build -DNOCSIF_LORA_HUNT_SELFTEST=1
     * to read the 'SIGNAL-HUNT SELF-TEST' block + 'VERDICT' (see lora.cpp). Gated on ui_ok + !safe. */
#ifndef NOCSIF_LORA_HUNT_SELFTEST
#define NOCSIF_LORA_HUNT_SELFTEST 0
#endif
#if NOCSIF_LORA_HUNT_SELFTEST
    if (ui_ok && !safe) {
        ESP_LOGW(TAG, "M9 LoRa boot self-test: signal hunt @915 MHz — RSSI envelope (passive RX)");
        nocsif_lora_init();
        vTaskDelay(pdMS_TO_TICKS(300));
        nocsif_lora_request_hunt_selftest();
    }
#endif

    /* M8 GNSS proof-of-life hook (grab-and-run bring-up test) — powers BLDO1, opens UART1, and
     * sweeps bauds x pin orders listening for NMEA/UBX (a live receiver streams within ~1 s, no
     * fix needed). Default OFF; build -DNOCSIF_GNSS_BOOT_SELFTEST=1 to run it + read the verdict.
     *   >>> WHY: the GPS on this unit was reported dead by the operator (suspected flex-cluster
     *       fault with NFC/haptic), but that was an observation, not a probe — and the M9 LoRa
     *       check overturned a similar suspicion. This gives the GPS a hard, instrumented verdict.
     *       Watch for the 'GNSS HW PROOF-OF-LIFE' block + the 'VERDICT' line (see gnss.c hw_selftest).
     * Gated on ui_ok + !safe. Normal builds are unaffected — GNSS stays idle. */
#ifndef NOCSIF_GNSS_BOOT_SELFTEST
#define NOCSIF_GNSS_BOOT_SELFTEST 0
#endif
#if NOCSIF_GNSS_BOOT_SELFTEST
    if (ui_ok && !safe) {
        ESP_LOGW(TAG, "M8 GNSS boot proof-of-life: u-blox/LS550G UART liveness check (no fix needed)");
        nocsif_gnss_init();
        vTaskDelay(pdMS_TO_TICKS(300));
        nocsif_gnss_request_selftest();
    }
#endif

    /* M11-E1 — boot chime: the watch says "up". Queued (non-blocking) after the heavy init has
     * settled; the audio worker powers the amp, plays the rising two-tone, and drops the rail.
     * No-op when sound is muted (Settings > Display > Sounds) or in safe mode (keep boot silent). */
    if (!safe) {
        nocsif_audio_boot_cue();   /* gated by the "Boot sound" setting + the master mute */
    }

    /* RAM remediation Phase 0 — coexistence-config echo (the runtime half of the coex_guard.c
     * build-time check). Prints the load-bearing sdkconfig keys + the current int-DMA largest so a
     * COM7 watcher can confirm coexistence health at a glance (docs/RAM-BUDGET.md "How to verify
     * on-device": boot RAM% 54.2 correct vs 39.2 broken). The build already FAILS if any key drifts;
     * this makes the live values visible next to the heartbeat gauge. */
    ESP_LOGI(TAG, "coex config: LV_CUSTOM_MALLOC=%d ALWAYSINTERNAL=%d RX_BA_WIN=%d BLE_MAX_ACT=%d; "
                  "boot int-dma free=%u largest=%u",
#ifdef CONFIG_LV_USE_CUSTOM_MALLOC
             1,
#else
             0,
#endif
             CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL, CONFIG_ESP_WIFI_RX_BA_WIN,
#ifdef CONFIG_BT_CTRL_BLE_MAX_ACT
             CONFIG_BT_CTRL_BLE_MAX_ACT,
#else
             0,
#endif
             (unsigned)nocsif_int_dma_free(), (unsigned)nocsif_int_dma_largest());

    /* RAM remediation Phase 1 — on-device SIGNAL-HUNT TRI-RADIO ACCEPTANCE proof. From the heartbeat
     * loop (so the int-DMA hole has actually collapsed to the steady-state ~2816 with BLE resident + WiFi
     * up + phone linked), cycle the Signal-Hunt radio BLE -> WiFi -> LoRa -> BLE -> WiFi -> LoRa -> BLE,
     * one mode per beat, byte-identical to ui.c hunt_set_radio. Proves each radio does REAL recon at
     * fragmentation, switching NEVER refuses/crashes/reboots, and the RESIDENT BLE controller + phone
     * link stay usable throughout. Default OFF; build -DNOCSIF_HUNT_CYCLE_SELFTEST=1. */
#ifndef NOCSIF_HUNT_CYCLE_SELFTEST
#define NOCSIF_HUNT_CYCLE_SELFTEST 0
#endif

    /* RAM remediation Phase 2 (remake #7) — PSRAM WORKER-STACK proof. The audio/mic/ducky worker task
     * stacks were moved to PSRAM (xTaskCreateWithCaps + MALLOC_CAP_SPIRAM). This hook (a) spawns the two
     * LAZY workers (mic, ducky) at STEADY-STATE fragmentation — a 6 KB INTERNAL stack could not be
     * claimed when the largest int-DMA hole is ~3 KB, so a successful spawn there proves the "first-use
     * spawn fails under fragmentation" class is closed — and (b) reads each worker's on-stack probe to
     * confirm the stack pointer is in external RAM. Default OFF; -DNOCSIF_PSRAM_STACK_SELFTEST=1. */
#ifndef NOCSIF_PSRAM_STACK_SELFTEST
#define NOCSIF_PSRAM_STACK_SELFTEST 0
#endif

    /* RAM remediation Phase 2 (remake #12 / conflict C7) — SIGNAL-HUNT AUDIO CUE proof. The I2S TX DMA
     * is boot-reserved (nocsif_audio_boot_reserve, before esp_wifi_init). At a steady-state beat the
     * largest int-DMA hole is ~3 KB — FAR below the ~5.7 KB a fresh i2s open would need — so tx_ready=1
     * here proves the channel was pre-claimed and survived full fragmentation, and the two tones are
     * audible confirmation. Default OFF; -DNOCSIF_AUDIO_CUE_SELFTEST=1. */
#ifndef NOCSIF_AUDIO_CUE_SELFTEST
#define NOCSIF_AUDIO_CUE_SELFTEST 0
#endif

    /* RAM remediation Phase 2 (remake #10 / conflict C13) — HID-WITHOUT-TEARDOWN proof. Entering/leaving
     * BLE keyboard mode is now a pure advert swap (the GATT server is permanent, the controller resident).
     * This hook cycles nocsif_ble_hid_request_start() -> _request_release() at steady state and asserts
     * the largest int-DMA block never swings (a real nimble_teardown+bring_up would free+re-claim the
     * ~31.7 KB controller block), nocsif_ble_needs_restart() stays 0, and the phone (ANCS) link re-arms
     * after release. Default OFF; -DNOCSIF_HID_CYCLE_SELFTEST=1. */
#ifndef NOCSIF_HID_CYCLE_SELFTEST
#define NOCSIF_HID_CYCLE_SELFTEST 0
#endif

    /* RAM remediation Phase 2 (§4.13 / remake #6) — CC RADIO-TILE proof. The Control-Center BLE/WiFi
     * tiles now DRIVE the real radios (nocsif_ble_bt_set_enabled / nocsif_wifi_request_enable) and
     * REFLECT them through the shared nocsif_radio_state accessor. This hook drives the same setters the
     * tiles do and asserts the accessor tracks each toggle, the BLE toggle is a pure logical activity
     * change (largest int-DMA stays flat, controller stays resident, needs_restart stays 0 — never a
     * crash/refuse/restart), and the phone link survives a BLE off->on. Default OFF;
     * -DNOCSIF_RADIO_TILE_SELFTEST=1. */
#ifndef NOCSIF_RADIO_TILE_SELFTEST
#define NOCSIF_RADIO_TILE_SELFTEST 0
#endif

    for (int beat = 0;; beat++) {
        /* int-dma = internal DMA-capable RAM (what the display's per-flush SPI bounce buffer + USB
         * draw from). 'largest' is the biggest CONTIGUOUS block — a full-frame flush chunk must fit
         * in it, so if this dips under the chunk size a flush fails and wedges LVGL (P4.5.3b hang). */
        ESP_LOGI(TAG, "heartbeat %d  free heap=%u  int-dma free=%u largest=%u", beat,
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)nocsif_int_dma_free(),
                 (unsigned)nocsif_int_dma_largest());
#if NOCSIF_PSRAM_STACK_SELFTEST
        if (!safe) {
            if (beat == 2) {               /* steady state: spawn the two lazy workers under fragmentation */
                nocsif_log_dma_free("PSRAM-STACK: before mic/ducky spawn");
                esp_err_t em = nocsif_mic_init();       /* both idempotent + NULL-safe */
                esp_err_t ed = nocsif_ducky_init();
                ESP_LOGW(TAG, "PSRAM-STACK: mic_init=%s ducky_init=%s (spawn at largest=%u)",
                         esp_err_to_name(em), esp_err_to_name(ed),
                         (unsigned)nocsif_int_dma_largest());
            } else if (beat == 3) {        /* one beat later: read each worker's on-stack PSRAM probe */
                bool a = nocsif_audio_stack_is_psram();
                bool m = nocsif_mic_stack_is_psram();
                bool d = nocsif_ducky_stack_is_psram();
                ESP_LOGW(TAG, "PSRAM-STACK VERDICT: %s  audio=%d mic=%d ducky=%d  largest=%u",
                         (a && m && d) ? "PASS" : "FAIL", (int)a, (int)m, (int)d,
                         (unsigned)nocsif_int_dma_largest());
                nocsif_log_dma_free("PSRAM-STACK: after");
            }
        }
#endif
#if NOCSIF_AUDIO_CUE_SELFTEST
        if (!safe && beat == 3) {      /* steady state: BLE resident + WiFi up, largest ~3 KB */
            ESP_LOGW(TAG, "AUDIO-CUE VERDICT: %s  tx_ready=%d int-dma largest=%u (a fresh open needs ~5760)",
                     nocsif_audio_tx_ready() ? "PASS" : "FAIL", (int)nocsif_audio_tx_ready(),
                     (unsigned)nocsif_int_dma_largest());
            nocsif_audio_tone(880, 60, 70);      /* audible confirmation the pre-claimed channel plays */
            nocsif_audio_tone(1319, 90, 70);
        }
#endif
#if NOCSIF_HID_CYCLE_SELFTEST
        if (!safe && beat >= 3) {
            static unsigned s_hid_l0; static int s_hid_maxdev; static bool s_hid_restart_seen;
            int s = beat - 3;
            unsigned l = (unsigned)nocsif_int_dma_largest();
            if (s >= 1) {                        /* track the largest swing + any restart flag across the cycle */
                int dev = (int)l - (int)s_hid_l0; if (dev < 0) dev = -dev;
                if (dev > s_hid_maxdev) s_hid_maxdev = dev;
                if (nocsif_ble_needs_restart()) s_hid_restart_seen = true;
            }
            switch (s) {
            case 0: s_hid_l0 = l; s_hid_maxdev = 0; s_hid_restart_seen = false;
                    ESP_LOGW(TAG, "HID-CYCLE baseline: largest=%u ancs=%d", l, (int)nocsif_ble_ancs_state());
                    break;
            case 1: ESP_LOGW(TAG, "HID-CYCLE: enter keyboard (request_start)");
                    nocsif_ble_hid_request_start();
                    break;
            case 2: ESP_LOGW(TAG, "HID-CYCLE after start: hid_state=%d largest=%u (base %u) needs_restart=%d",
                             (int)nocsif_ble_hid_state(), l, s_hid_l0, (int)nocsif_ble_needs_restart());
                    ESP_LOGW(TAG, "HID-CYCLE: leave keyboard (request_release)");
                    nocsif_ble_request_release();
                    break;
            case 3: ESP_LOGW(TAG, "HID-CYCLE after release: ancs=%d hid_state=%d largest=%u (base %u)",
                             (int)nocsif_ble_ancs_state(), (int)nocsif_ble_hid_state(), l, s_hid_l0);
                    break;
            case 4: ESP_LOGW(TAG, "HID-CYCLE VERDICT: %s  max|dlargest|=%d needs_restart_seen=%d ancs=%d "
                                  "(a NimBLE teardown would swing largest ~31 KB)",
                             (s_hid_maxdev < 4096 && !s_hid_restart_seen) ? "PASS" : "FAIL",
                             s_hid_maxdev, (int)s_hid_restart_seen, (int)nocsif_ble_ancs_state());
                    break;
            default: break;
            }
        }
#endif
#if NOCSIF_RADIO_TILE_SELFTEST
        if (!safe && beat >= 3) {
            static unsigned s_rt_l0; static int s_rt_maxdev; static bool s_rt_lost, s_rt_track_ok = true;
            int s = beat - 3;
            nocsif_radio_state_t rs; nocsif_radio_state(&rs);
            unsigned l = (unsigned)nocsif_int_dma_largest();
            if (s >= 1) {
                int dev = (int)l - (int)s_rt_l0; if (dev < 0) dev = -dev;
                if (dev > s_rt_maxdev) s_rt_maxdev = dev;
                if (nocsif_ble_needs_restart() || !rs.ble_controller_resident) s_rt_lost = true;
            }
            switch (s) {
            case 0: s_rt_l0 = l; s_rt_maxdev = 0; s_rt_lost = false; s_rt_track_ok = true;
                    ESP_LOGW(TAG, "RTILE baseline: ble_on=%d wifi_on=%d resident=%d largest=%u",
                             (int)rs.ble_logical_on, (int)rs.wifi_sta_on, (int)rs.ble_controller_resident, l);
                    break;
            case 1: ESP_LOGW(TAG, "RTILE: BLE off (logical)"); nocsif_ble_bt_set_enabled(false);
                    break;
            case 2: if (rs.ble_logical_on) s_rt_track_ok = false;   /* accessor must show off */
                    ESP_LOGW(TAG, "RTILE after BLE off: ble_on=%d(want0) resident=%d largest=%u restart=%d",
                             (int)rs.ble_logical_on, (int)rs.ble_controller_resident, l,
                             (int)nocsif_ble_needs_restart());
                    ESP_LOGW(TAG, "RTILE: BLE on (logical)"); nocsif_ble_bt_set_enabled(true);
                    break;
            case 3: if (!rs.ble_logical_on) s_rt_track_ok = false;  /* accessor must show on */
                    ESP_LOGW(TAG, "RTILE after BLE on: ble_on=%d(want1) resident=%d ancs=%d largest=%u",
                             (int)rs.ble_logical_on, (int)rs.ble_controller_resident,
                             (int)nocsif_ble_ancs_state(), l);
                    ESP_LOGW(TAG, "RTILE: WiFi off (STA)"); nocsif_wifi_request_enable(false);
                    break;
            case 4: ESP_LOGW(TAG, "RTILE after WiFi off: wifi_on=%d largest=%u", (int)rs.wifi_sta_on, l);
                    ESP_LOGW(TAG, "RTILE: WiFi on (STA)"); nocsif_wifi_request_enable(true);
                    break;
            case 5: ESP_LOGW(TAG, "RTILE after WiFi on: wifi_on=%d largest=%u", (int)rs.wifi_sta_on, l);
                    break;
            case 6: ESP_LOGW(TAG, "RTILE VERDICT: %s  ble_accessor_tracked=%d max|dlargest|=%d "
                                  "restart/lost_resident=%d  final ble_on=%d wifi_on=%d ancs=%d",
                             (s_rt_track_ok && !s_rt_lost && s_rt_maxdev < 4096) ? "PASS" : "FAIL",
                             (int)s_rt_track_ok, s_rt_maxdev, (int)s_rt_lost,
                             (int)rs.ble_logical_on, (int)rs.wifi_sta_on, (int)nocsif_ble_ancs_state());
                    break;
            default: break;
            }
        }
#endif
#if NOCSIF_COEXV
        {
            /* Per-beat curve (WiFi association lands asynchronously on the WiFi worker, so tag it once)
             * + the display IO's DMA-underrun counter (must stay 0 — validates the 80 MHz pclk with
             * PSRAM-direct DMA), then ONE steady-state co-residence probe: bring up what a normal boot
             * leaves down — GNSS + LoRa — and claim an 8 KB contiguous block as the USB File-Share entry
             * proxy (TinyUSB task stack + CDC rings + FAT remount). Same method/order as
             * docs/DMA-COEXISTENCE-VERIFICATION.md, so the numbers compare across builds. */
            static bool s_assoc_seen, s_probe_done;
            const bool conn = nocsif_wifi_connected();
            if (conn && !s_assoc_seen) {
                s_assoc_seen = true;
                COEXV_SNAP("post-wifi-assoc");
            }
            ESP_LOGW(TAG, COEXV_TAG " beat=%d wifi_conn=%d free=%u largest=%u disp_tx_fail=%u chunks=%u", beat,
                     (int)conn, (unsigned)nocsif_int_dma_free(), (unsigned)nocsif_int_dma_largest(),
                     (unsigned)nocsif_display_io_tx_fail(), (unsigned)nocsif_display_io_color_chunks());
            if (!safe && !s_probe_done && (conn || beat >= 8)) {
                s_probe_done = true;
                COEXV_SNAP("COEXALL-baseline");
                esp_err_t eg = nocsif_gnss_init();
                if (eg == ESP_OK) nocsif_gnss_set_live(true);
                vTaskDelay(pdMS_TO_TICKS(200));
                ESP_LOGW(TAG, COEXV_TAG " COEXALL +gnss: init=%s avail=%d free=%u largest=%u",
                         esp_err_to_name(eg), (int)nocsif_gnss_available(),
                         (unsigned)nocsif_int_dma_free(), (unsigned)nocsif_int_dma_largest());
                esp_err_t el = nocsif_lora_init();
                vTaskDelay(pdMS_TO_TICKS(200));
                ESP_LOGW(TAG, COEXV_TAG " COEXALL +lora: init=%s avail=%d free=%u largest=%u",
                         esp_err_to_name(el), (int)nocsif_lora_available(),
                         (unsigned)nocsif_int_dma_free(), (unsigned)nocsif_int_dma_largest());
                void *usb = heap_caps_malloc(8192, NOCSIF_DMA_CAPS);
                ESP_LOGW(TAG, COEXV_TAG " COEXALL usb8k proxy (File-Share entry): %s free=%u largest=%u",
                         usb ? "OK would-enter" : "NULL would-REFUSE",
                         (unsigned)nocsif_int_dma_free(), (unsigned)nocsif_int_dma_largest());
                if (usb) heap_caps_free(usb);
                ESP_LOGW(TAG, COEXV_TAG " COEXALL VERDICT: gnss=%d lora=%d usb8k=%d disp_tx_fail=%u | final free=%u largest=%u",
                         (int)nocsif_gnss_available(), (int)nocsif_lora_available(), (int)(usb != NULL),
                         (unsigned)nocsif_display_io_tx_fail(),
                         (unsigned)nocsif_int_dma_free(), (unsigned)nocsif_int_dma_largest());
            }
        }
#endif
#if NOCSIF_HUNT_CYCLE_SELFTEST
        {
            static const int HSEQ[] = { 0, 1, 2, 0, 1, 2, 0 };   /* 0=BLE 1=WiFi 2=LoRa (BLE→WiFi→LoRa ×2) */
            const int HLEN = (int)(sizeof HSEQ / sizeof HSEQ[0]);
            int si = beat - 3;                                    /* first switch at beat 3 (steady state) */
            /* Verify the mode armed on the PREVIOUS beat is doing REAL recon (had ~5 s to establish). */
            if (!safe && si >= 1 && si <= HLEN) {
                int pv = HSEQ[si - 1];
                if (pv == 0)
                    ESP_LOGW(TAG, "HUNT verify BLE : scan_active=%d devs=%d largest=%u",
                             (int)nocsif_ble_scan_active(), nocsif_ble_dev_count(),
                             (unsigned)nocsif_int_dma_largest());
                else if (pv == 1)
                    ESP_LOGW(TAG, "HUNT verify WiFi: mon_active=%d frames=%lu aps=%d largest=%u",
                             (int)nocsif_wifi_monitor_active(), (unsigned long)nocsif_wifi_monitor_total(),
                             nocsif_wifi_mon_ap_count(), (unsigned)nocsif_int_dma_largest());
                else {
                    nocsif_lora_survey_t sv = {0};
                    nocsif_lora_survey_snapshot(&sv);
                    ESP_LOGW(TAG, "HUNT verify LoRa: surveying=%d sweeps=%lu floor=%d largest=%u",
                             (int)nocsif_lora_surveying(), (unsigned long)sv.sweeps, sv.floor,
                             (unsigned)nocsif_int_dma_largest());
                }
            }
            /* Switch the Signal-Hunt radio (byte-identical to hunt_set_radio). ancs state proves the
             * phone/BLE link is still alive across the switch — BLE usable throughout, never a refuse. */
            if (!safe && si >= 0 && si < HLEN) {
                int m = HSEQ[si];
                ESP_LOGW(TAG, "HUNT switch #%d -> %-4s; BLE phone(ancs)=%d largest=%u", si,
                         m == 0 ? "BLE" : m == 1 ? "WiFi" : "LoRa",
                         (int)nocsif_ble_ancs_state(), (unsigned)nocsif_int_dma_largest());
                if (m == 0) {                            /* BLE observer on the RESIDENT controller */
                    nocsif_lora_set_survey(false); nocsif_lora_hunt_stop();
                    nocsif_wifi_request_parse(false); nocsif_wifi_request_monitor(false);
                    nocsif_ble_request_scan(true);
                } else if (m == 1) {                     /* WiFi monitor — STA stays associated */
                    nocsif_ble_request_scan(false);
                    nocsif_lora_set_survey(false); nocsif_lora_hunt_stop();
                    nocsif_wifi_request_parse(true); nocsif_wifi_request_monitor_hop(true);
                } else {                                 /* LoRa survey — SX1262, PSRAM stack */
                    nocsif_ble_request_scan(false);
                    nocsif_wifi_request_parse(false); nocsif_wifi_request_monitor(false);
                    if (nocsif_lora_init() == ESP_OK && !nocsif_lora_surveying() && !nocsif_lora_hunting())
                        nocsif_lora_set_survey(true);
                }
            }
        }
#endif
        /* Re-scan every ~30 s so devices that appear as rails/resets are toggled
         * during later bring-up show up without a reflash. */
        if (beat > 0 && beat % 6 == 0) {
            nocsif_i2c_scan_compact();
        }
        /* Reliability: after a healthy ~30 s dwell, clear the crash streak so only *rapid repeated*
         * crashes (that never reach this point) trip safe mode. Idempotent — safe to call again. */
        if (beat == 6) {
            nocsif_reliability_mark_healthy();
            /* M-OTA — a belt-and-suspenders re-confirm at the healthy dwell (the primary confirm runs
             * right after the UI comes up, above). Idempotent: a no-op once the image is already
             * marked valid, or when this boot is not a PENDING_VERIFY OTA image. */
            nocsif_ota_confirm();
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
