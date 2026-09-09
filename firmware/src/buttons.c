/*
 * NocSif physical-button input service (UI-shell P4.1 decode; P4.4 actions). See buttons.h.
 *
 * A single worker task polls both buttons at ~50 Hz and dispatches decoded events into the UI
 * action API (ui.c), which itself marshals them onto the LVGL task — this task never touches
 * widgets directly.
 *   - PWR: nocsif_power_pwrkey_poll() reads the AXP2101's latched PWRKEY event bits (the PMU
 *     already classifies short vs long). A double is synthesized here from two shorts within
 *     DOUBLE_US, but only while a PWR double-press shortcut is bound — otherwise a short fires
 *     immediately, keeping the common PWR tap (screen off/on, pop-to-Home) instant.
 *   - FN = GPIO0: debounced by requiring N consecutive equal samples. Custom mapping (2026-08-10,
 *     different from the original P4 spec's "single = launch"): FN single = back one screen, FN
 *     double = launch the FN shortcut app (default "wifi"). Since both actions share one button,
 *     FN always uses the double-press synthesis path, so a single (back) only fires after the
 *     double-press window elapses.
 *
 * Runs on its own worker task; every action is marshalled onto the LVGL task by ui.c. Requires
 * nocsif_power_init() first, and nocsif_ui_init() before the actions have any effect — they're
 * no-ops until the UI exists.
 */
#include "buttons.h"
#include "freertos/idf_additions.h" /* xTaskCreateWithCaps — places the worker stack in PSRAM */
#include "esp_heap_caps.h"          /* MALLOC_CAP_SPIRAM */
#include "esp_memory_utils.h"       /* esp_ptr_external_ram — checks where the stack landed */

#include <stdint.h>
#include <stdbool.h>

#include "power.h"                 /* nocsif_power_pwrkey_config / _poll + NOCSIF_PWRKEY_* + _vbus_read */
#include "audio.h"                 /* nocsif_audio_usb_cue — plays a chime on a USB-plug rising edge */
#include "ui.h"                    /* nocsif_ui_btn_pwr_short/_long/_double, _fn_short/_double */
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "buttons";

#define FN_GPIO           GPIO_NUM_0   /* FN button; also the boot strap — runtime input only */
#define POLL_MS           20           /* poll tick, roughly 50 Hz                            */
#define DEBOUNCE_SAMPLES  3            /* ~60 ms of stability required before accepting an FN change */
/* Double-press window used by PWR (when armed) and always by FN. A shorter window makes FN
 * single/back snappier but the double harder to land; a longer one eases the double but adds
 * latency to back. Tunable on-device. */
#define DOUBLE_US         (350 * 1000)
#define FN_LONG_US        (600 * 1000) /* FN hold time counted as a long press (currently unused) */

static TaskHandle_t s_task;

/* PWR double-press detection only arms once a shortcut is actually bound (set by the UI). While
 * disarmed (the default), a PWR short fires immediately with no double-press wait. FN's double is
 * always armed, since its double press launches the FN shortcut. */
static volatile bool s_pwr_double_armed;

void nocsif_buttons_set_pwr_double_enabled(bool enabled)
{
    s_pwr_double_armed = enabled;
}

/* ---- software single/double-press synthesis (shared by PWR and FN) ----------------
 * A "short" starts out provisional: a second short arriving within DOUBLE_US promotes it to a
 * double, otherwise the window lapses and it becomes a single. *pending_us holds the unconfirmed
 * short's timestamp (0 = none pending). The dispatch callbacks are passed in so both buttons reuse
 * the same logic. */
typedef void (*btn_cb_t)(void);

static void short_or_double(int64_t now, int64_t *pending_us, btn_cb_t double_cb)
{
    if (*pending_us != 0 && (now - *pending_us) <= DOUBLE_US) {
        *pending_us = 0;
        double_cb();
    } else {
        *pending_us = now;   /* provisional single — flush_short confirms it once the window lapses */
    }
}

static void flush_short(int64_t now, int64_t *pending_us, btn_cb_t single_cb)
{
    if (*pending_us != 0 && (now - *pending_us) > DOUBLE_US) {
        *pending_us = 0;
        single_cb();
    }
}

static void buttons_task(void *arg)
{
    (void)arg;
    volatile uint8_t probe;                          /* on-stack byte, used to check where the stack lives */
    ESP_LOGI(TAG, "worker up: stack in %s", esp_ptr_external_ram((void *)&probe) ? "PSRAM" : "INTERNAL");
    int64_t pwr_pending = 0, fn_pending = 0;

    /* FN debounce/timing state. GPIO0 idles HIGH (released). */
    int     fn_stable = 1, fn_last_raw = 1, fn_cnt = 0;
    bool    fn_down = false, fn_long_logged = false;
    int64_t fn_press_us = 0;

    /* USB-plug cue state: VBUS is polled at ~2 Hz and a chime plays on a rising (unplug->plug)
     * edge. The first reading only establishes the baseline, so booting with USB already attached
     * doesn't chime. */
    bool    vbus_last = false, vbus_have = false;
    int     vbus_div = 0;

    for (;;) {
        int64_t now = esp_timer_get_time();

        /* Flush any matured pending shorts before decoding this tick's new events: if a second
         * short lands in the exact tick its window matures, flushing first fires the earlier one
         * as a single (rather than letting the decode below overwrite — and lose — it). This also
         * un-strands a pending short if the PWR binding gets cleared mid-window. */
        flush_short(now, &pwr_pending, nocsif_ui_btn_pwr_short);
        flush_short(now, &fn_pending,  nocsif_ui_btn_fn_short);

        /* ---- PWR (AXP2101 PWRKEY, polled over I2C) ---- */
        uint8_t ev = 0;
        if (nocsif_power_pwrkey_poll(&ev) == ESP_OK && ev != 0) {
            if (ev & NOCSIF_PWRKEY_LONG) {
                pwr_pending = 0;                    /* a long press cancels any pending short */
                nocsif_ui_btn_pwr_long();
            } else if (ev & NOCSIF_PWRKEY_SHORT) {
                if (s_pwr_double_armed) {
                    short_or_double(now, &pwr_pending, nocsif_ui_btn_pwr_double);
                } else {
                    nocsif_ui_btn_pwr_short();      /* fires immediately — no double bound */
                }
            }
        }

        /* ---- FN (GPIO0, debounced) — single = back, double = launch shortcut ---- */
        int raw = gpio_get_level(FN_GPIO);
        if (raw == fn_last_raw) {
            if (fn_cnt < DEBOUNCE_SAMPLES) fn_cnt++;
        } else {
            fn_last_raw = raw;
            fn_cnt = 1;
        }
        if (fn_cnt >= DEBOUNCE_SAMPLES && raw != fn_stable) {
            fn_stable = raw;                        /* transition accepted */
            if (fn_stable == 0) {                   /* pressed (active-low) */
                fn_down = true;
                fn_long_logged = false;
                fn_press_us = now;
            } else if (fn_down) {                   /* released */
                fn_down = false;
                if (!fn_long_logged && (now - fn_press_us) < FN_LONG_US) {
                    /* Provisional short: flush_short above fires nocsif_ui_btn_fn_short (back)
                     * once the window lapses; a second short inside it fires the double (launch). */
                    short_or_double(now, &fn_pending, nocsif_ui_btn_fn_double);
                }
            }
        }
        if (fn_down && !fn_long_logged && (now - fn_press_us) >= FN_LONG_US) {
            fn_long_logged = true;
            ESP_LOGI(TAG, "FN long press (reserved — no action yet)");
        }

        /* ---- USB-plug cue (VBUS rising edge, polled at ~2 Hz over I2C) ---- */
        if (++vbus_div >= (500 / POLL_MS)) {
            vbus_div = 0;
            bool vb;
            if (nocsif_power_vbus_read(&vb) == ESP_OK) {
                if (vbus_have && !vbus_last && vb) {
                    nocsif_audio_usb_cue();     /* gated internally by the usb-sound setting + master mute */
                }
                vbus_last = vb;
                vbus_have = true;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

esp_err_t nocsif_buttons_init(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }

    /* FN = GPIO0 configured as a runtime input. GPIO0 doubles as the boot download-mode strap, but
     * strapping is only sampled at reset, so configuring it as an input afterward is safe. Never
     * set it as an output, and never drive it low. Idles HIGH via a pull-up; a press pulls it LOW. */
    const gpio_config_t fn = {
        .pin_bit_mask = 1ULL << FN_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&fn);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FN gpio_config(GPIO0) failed: %s", esp_err_to_name(err));
        return err;
    }

    /* PWR = AXP2101 PWRKEY: takes it away from the PMU's hardware auto-power-off and enables its
     * IRQ latch. If the PMU isn't attached, continue anyway — FN still works. */
    err = nocsif_power_pwrkey_config();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PWRKEY config failed: %s (is nocsif_power_init() done?)",
                 esp_err_to_name(err));
    }

    /* Stack in PSRAM: this worker only polls the PMU PWRKEY over I2C and the FN GPIO, and marshals
     * every action onto the LVGL task (lv_async_call) — no on-task NVS/flash access, no DMA from
     * its stack — so its 4 KB can safely come from PSRAM instead of the contended internal-DMA
     * pool. The task is never deleted. */
    if (xTaskCreateWithCaps(buttons_task, "buttons", 4096, NULL, 4, &s_task, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "buttons task create failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "button service up (P4.4 actions): PWR=AXP2101 PWRKEY (poll 0x49), "
                  "FN=GPIO0 (idle level=%d, single=back double=shortcut)", gpio_get_level(FN_GPIO));
    return ESP_OK;
}
