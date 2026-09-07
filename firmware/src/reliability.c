/*
 * NocSif — reliability hardening (Phase A). See reliability.h for the contract.
 *
 * Depends on sdkconfig: CONFIG_ESP_TASK_WDT_PANIC=y (a hang reboots instead of freezing),
 * CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y + DATA_FORMAT_ELF (a crash's backtrace survives), and the
 * 'coredump' partition (partitions.csv).
 */
#include "reliability.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_task_wdt.h"
#include "esp_core_dump.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"

static const char *TAG = "reliab";

/* On-device verification hook (default 0 — ships off). At 1, arming the UI-liveness watchdog also
 * schedules a one-shot ~12 s timer that BLOCKS the LVGL task forever (vTaskDelay(portMAX_DELAY)) —
 * a blocked wedge that yields the CPU, so the idle task stays healthy and the stock idle-task WDT
 * CANNOT see it. Only the UI-liveness subscription below trips the WDT -> panic + reboot with the
 * LVGL task named. This is the DMA-hang class of freeze; use it to prove the auto-recovery, then
 * set back to 0. */
#define NOCSIF_REL_HANG_TEST 0

/* Dedicated NVS namespace, separate from settings' "nocsif" so reliability bookkeeping can never
 * collide with user settings. Keys are <=15 chars (NVS limit). */
#define REL_NVS_NS          "nocsif_rel"
#define REL_KEY_STREAK      "streak"      /* u8: consecutive crash-class boots without a healthy run */
#define REL_KEY_LASTCRASH   "lastcrash"   /* str: last formatted crash record (persists over reboots)  */

/* Boot-loop guard: this many crash-class boots in a row (none surviving the healthy dwell) trips
 * safe mode for the next boot. */
#define REL_SAFE_MODE_THRESHOLD  3

/* UI-liveness watchdog: how often the LVGL task pets the Task-WDT. Must be comfortably below the
 * WDT timeout (8 s, sdkconfig) so a healthy UI never trips it, yet small enough that a wedge is
 * caught within ~timeout after the last pet. */
#define REL_UI_WDT_PET_MS   1000

/* Compact record buffer. Sized for "<reason> task=<16> pc=0x######## bt=<8x 0x########> elf=########". */
#define REL_CRASH_BUF_SZ    200

static bool         s_safe_mode      = false;
static bool         s_healthy_marked = false;
static bool         s_liveness_armed = false;
static volatile bool s_liveness_paused = false;   /* suspend(): the pet-timer must not call a WDT it is unsubscribed from */
static TaskHandle_t s_lvgl_task      = NULL;   /* the WDT-watched LVGL task, for suspend/resume */
static const char  *s_reason_str     = "unknown";
static char         s_last_crash[REL_CRASH_BUF_SZ] = {0};

/* One NVS handle kept open for the app's lifetime (opened in boot_check, reused by mark_healthy).
 * NVS handles are not task-bound; both callers run on the app_main task anyway. */
static nvs_handle_t s_nvs      = 0;
static bool         s_nvs_open = false;

/* ---- helpers ---------------------------------------------------------------------------------- */

/* Bring NVS up (idempotent — settings_init calls nvs_flash_init() again later and gets ESP_OK).
 * Mirrors settings.c's erase-and-retry on a corrupt/format-changed partition. */
static esp_err_t rel_nvs_ensure(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs needs erase (%s) — erasing + retrying", esp_err_to_name(e));
        ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_flash_erase());
        e = nvs_flash_init();
    }
    return e;
}

static const char *reason_to_str(esp_reset_reason_t r)
{
    switch (r) {
        case ESP_RST_POWERON:   return "power-on";
        case ESP_RST_EXT:       return "ext-reset";
        case ESP_RST_SW:        return "sw-restart";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "int-wdt";
        case ESP_RST_TASK_WDT:  return "task-wdt";
        case ESP_RST_WDT:       return "other-wdt";
        case ESP_RST_DEEPSLEEP: return "deep-sleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "sdio";
        case ESP_RST_USB:       return "usb";
        case ESP_RST_JTAG:      return "jtag";
        case ESP_RST_EFUSE:     return "efuse-err";
        case ESP_RST_PWR_GLITCH: return "pwr-glitch";
        case ESP_RST_CPU_LOCKUP: return "cpu-lockup";
        default:                return "unknown";
    }
}

/* Reset reasons that mean "our code hung or crashed" — these accrue toward the boot-loop guard.
 * Brownout is a power event (a dying cell) that safe mode can't fix, so it is recorded but does NOT
 * count toward safe mode (we don't want a low battery to strand the watch in safe mode). */
static bool reason_is_crash(esp_reset_reason_t r)
{
    switch (r) {
        case ESP_RST_PANIC:
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:
        case ESP_RST_CPU_LOCKUP:
            return true;
        default:
            return false;
    }
}

/* Fold a stored core dump into s_last_crash, persist it, log it, and erase the image. have_dump is
 * the authoritative "a dump is present" flag (computed once in boot_check). On a crash with no
 * readable dump (e.g. a brownout, or a corrupted image), records a minimal reason-only line. */
static void rel_record_crash(esp_reset_reason_t r, bool have_dump)
{
    const char *reason = reason_to_str(r);
    int n = 0;

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    if (have_dump) {
        esp_core_dump_summary_t *sum = calloc(1, sizeof(*sum));
        if (sum && esp_core_dump_get_summary(sum) == ESP_OK) {
            n = snprintf(s_last_crash, sizeof(s_last_crash), "%s task=%s pc=0x%08x",
                         reason, sum->exc_task, (unsigned)sum->exc_pc);
            if (n < 0) n = 0;
            /* backtrace: up to 8 PCs (host addr2line decodes them against the app ELF sha below) */
            uint32_t depth = sum->exc_bt_info.depth;
            if (depth > 8) depth = 8;
            if (depth && n < (int)sizeof(s_last_crash)) {
                n += snprintf(s_last_crash + n, sizeof(s_last_crash) - n, " bt=");
                for (uint32_t i = 0; i < depth && n < (int)sizeof(s_last_crash); i++) {
                    n += snprintf(s_last_crash + n, sizeof(s_last_crash) - n, "0x%08x ",
                                  (unsigned)sum->exc_bt_info.bt[i]);
                }
            }
            if (n < (int)sizeof(s_last_crash)) {
                /* app_elf_sha256 is a NUL-terminated ASCII hex string; first 8 chars pin the build. */
                n += snprintf(s_last_crash + n, sizeof(s_last_crash) - n, "elf=%.8s",
                              (const char *)sum->app_elf_sha256);
            }
        }
        free(sum);
    }
#else
    (void)have_dump;
#endif

    /* Erase whatever dump we read so the next crash can write a fresh one (a stale image would mask
     * it). esp_core_dump_image_erase() is available regardless of the data-format config. */
    if (have_dump) esp_core_dump_image_erase();

    if (n == 0) {  /* crash-class but no usable dump */
        snprintf(s_last_crash, sizeof(s_last_crash), "%s (no core dump)", reason);
    }

    ESP_LOGE(TAG, "CRASH RECORD: %s", s_last_crash);
    if (s_nvs_open) {
        if (nvs_set_str(s_nvs, REL_KEY_LASTCRASH, s_last_crash) == ESP_OK) {
            nvs_commit(s_nvs);
        }
    }
}

/* ---- public API ------------------------------------------------------------------------------- */

void nocsif_reliability_boot_check(void)
{
    esp_reset_reason_t r = esp_reset_reason();
    s_reason_str = reason_to_str(r);

    /* A stored core dump is definitive proof a crash occurred — more reliable than the reset reason
     * alone (which can miss a case, or leave a stale image if a prior erase failed). Treat either
     * signal as a crash so the dump is always recorded + erased. */
    bool have_dump = false;
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    have_dump = (esp_core_dump_image_check() == ESP_OK);
#endif
    bool crash = reason_is_crash(r) || have_dump;

    if (rel_nvs_ensure() != ESP_OK) {
        ESP_LOGE(TAG, "nvs unavailable — reliability bookkeeping disabled this boot (reason=%s)", s_reason_str);
        return;  /* leave safe mode off; the WDT/coredump still work, only the persisted counters don't */
    }
    if (nvs_open(REL_NVS_NS, NVS_READWRITE, &s_nvs) == ESP_OK) {
        s_nvs_open = true;
    } else {
        ESP_LOGW(TAG, "nvs_open(%s) failed — streak + last-crash not persisted", REL_NVS_NS);
    }

    /* Carry the previous crash record forward so the Diagnostics screen always shows the most recent
     * one, even across clean reboots. */
    if (s_nvs_open) {
        size_t len = sizeof(s_last_crash);
        if (nvs_get_str(s_nvs, REL_KEY_LASTCRASH, s_last_crash, &len) != ESP_OK) {
            s_last_crash[0] = '\0';
        }
    }

    uint8_t streak = 0;
    if (s_nvs_open) nvs_get_u8(s_nvs, REL_KEY_STREAK, &streak);

    if (crash) {
        rel_record_crash(r, have_dump);      /* overwrites s_last_crash + persists it */
        if (streak < 0xFF) streak++;
        if (s_nvs_open) { nvs_set_u8(s_nvs, REL_KEY_STREAK, streak); nvs_commit(s_nvs); }
        ESP_LOGE(TAG, "crash-class boot (%s); crash streak = %u", s_reason_str, streak);
    } else {
        ESP_LOGI(TAG, "boot reason: %s (crash streak = %u)", s_reason_str, streak);
    }

    s_safe_mode = (streak >= REL_SAFE_MODE_THRESHOLD);
    if (s_safe_mode) {
        ESP_LOGE(TAG, "*** SAFE MODE *** %u crash-class boots without a healthy run — "
                      "risky/heavy subsystems disabled this boot", streak);
    }
}

bool nocsif_reliability_safe_mode(void)
{
    return s_safe_mode;
}

void nocsif_reliability_mark_healthy(void)
{
    if (s_healthy_marked) return;
    s_healthy_marked = true;

    if (!s_nvs_open) return;
    uint8_t streak = 0;
    nvs_get_u8(s_nvs, REL_KEY_STREAK, &streak);
    if (streak != 0) {
        if (nvs_set_u8(s_nvs, REL_KEY_STREAK, 0) == ESP_OK) {
            nvs_commit(s_nvs);
            ESP_LOGI(TAG, "healthy dwell reached — crash streak cleared (was %u)", streak);
        }
    }
}

/* Runs on the LVGL task (LVGL timers are serviced there). Subscribes the LVGL task to the Task-WDT
 * on first fire, then pets it each tick. If the LVGL task wedges (blocked in the flush wait or
 * spinning in glyph render), this stops firing -> the WDT elapses -> panic + reboot with the LVGL
 * task in the backtrace. The timer keeps firing while the display is asleep (LVGL timers run
 * independent of panel state), so screen-off does not false-trip it. */
#if NOCSIF_REL_HANG_TEST
static void rel_hang_test_cb(lv_timer_t *t)
{
    lv_timer_del(t);
    ESP_LOGE(TAG, "HANG TEST: blocking the LVGL task forever — idle stays healthy, so ONLY the "
                  "UI-liveness watchdog should catch this and reboot (~%ds).", 8 + REL_UI_WDT_PET_MS / 1000);
    vTaskDelay(portMAX_DELAY);  /* blocked wedge: CPU yielded, idle-task WDT can't see it */
}
#endif

static void rel_ui_wdt_timer_cb(lv_timer_t *t)
{
    (void)t;
    static bool subscribed = false;
    if (!subscribed) {
        esp_err_t e = esp_task_wdt_add(NULL);   /* NULL = the current (LVGL) task */
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE /* already subscribed */) {
            ESP_LOGW(TAG, "ui-liveness: task_wdt_add failed (%s)", esp_err_to_name(e));
            return;
        }
        subscribed = true;
        s_lvgl_task = xTaskGetCurrentTaskHandle();   /* remember it so suspend() can unsubscribe it */
        ESP_LOGI(TAG, "ui-liveness: LVGL task subscribed to task-wdt");
    }
    if (!s_liveness_paused) esp_task_wdt_reset();   /* while suspended the task is unsubscribed — a reset
                                                     * would only log "task not found" every pet (§4.10) */
}

void nocsif_reliability_ui_liveness_suspend(bool suspend)
{
    if (!s_liveness_armed || s_lvgl_task == NULL) return;
    /* Unsubscribe / re-subscribe the LVGL task by handle (safe from any task; the WDT API is locked).
     * The pet-timer skips its reset while suspended; resume re-adds the task so the next pet counts. */
    if (suspend) {
        s_liveness_paused = true;
        esp_task_wdt_delete(s_lvgl_task);
    } else {
        esp_task_wdt_add(s_lvgl_task);   /* re-add starts a fresh window; the pet-timer resumes petting */
        s_liveness_paused = false;
    }
}

void nocsif_reliability_ui_liveness_arm(void)
{
    if (s_liveness_armed) return;
    /* lv_timer_create touches LVGL state — take the port lock (the cb itself runs under the port). */
    if (!lvgl_port_lock(1000)) {
        ESP_LOGW(TAG, "ui-liveness: lvgl lock timeout — watchdog NOT armed");
        return;
    }
    lv_timer_t *t = lv_timer_create(rel_ui_wdt_timer_cb, REL_UI_WDT_PET_MS, NULL);
#if NOCSIF_REL_HANG_TEST
    lv_timer_t *ht = lv_timer_create(rel_hang_test_cb, 12000, NULL);
    if (ht) lv_timer_set_repeat_count(ht, 1);  /* one-shot */
#endif
    lvgl_port_unlock();
    if (t) {
        s_liveness_armed = true;
        ESP_LOGI(TAG, "ui-liveness watchdog armed (pet=%dms)", REL_UI_WDT_PET_MS);
    } else {
        ESP_LOGW(TAG, "ui-liveness: lv_timer_create failed — watchdog NOT armed");
    }
}

const char *nocsif_reliability_last_crash_str(void)
{
    return s_last_crash;
}

const char *nocsif_reliability_reset_reason_str(void)
{
    return s_reason_str;
}
