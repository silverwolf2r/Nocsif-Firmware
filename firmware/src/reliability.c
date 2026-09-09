/*
 * NocSif — crash detection and recovery implementation. See reliability.h.
 *
 * Relies on sdkconfig options: task-watchdog panic-on-timeout, and core-dump-to-flash
 * with ELF-format backtraces, plus a dedicated 'coredump' partition.
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

/* Debug-only self-test switch, normally off. When set to 1, arming the watchdog also
 * schedules a one-shot timer that deliberately blocks the LVGL task forever, letting you
 * confirm the UI-liveness watchdog actually catches a wedge (the stock idle-task
 * watchdog wouldn't, since a blocked task still yields the CPU). Revert to 0 after testing. */
#define NOCSIF_REL_HANG_TEST 0

/* Separate NVS namespace from the app's settings store, so this bookkeeping can never
 * collide with a user-facing setting key. */
#define REL_NVS_NS          "nocsif_rel"
#define REL_KEY_STREAK      "streak"      /* consecutive crash-class boots without a healthy run */
#define REL_KEY_LASTCRASH   "lastcrash"   /* last formatted crash description, persisted */

/* This many crash-class boots in a row, none reaching the healthy dwell, trips safe mode. */
#define REL_SAFE_MODE_THRESHOLD  3

/* How often the LVGL task pets the watchdog: often enough that a wedge is caught soon
 * after it happens, but comfortably below the watchdog's own timeout. */
#define REL_UI_WDT_PET_MS   1000

/* Room for a formatted crash summary: reason, task name, PC, a short backtrace, build hash. */
#define REL_CRASH_BUF_SZ    200

static bool         s_safe_mode      = false;
static bool         s_healthy_marked = false;
static bool         s_liveness_armed = false;
static volatile bool s_liveness_paused = false;   /* true while suspended, so the pet-timer skips its reset */
static TaskHandle_t s_lvgl_task      = NULL;   /* the task being watched, for suspend/resume */
static const char  *s_reason_str     = "unknown";
static char         s_last_crash[REL_CRASH_BUF_SZ] = {0};

/* Kept open for the app's lifetime once opened in boot_check(); reused later by mark_healthy(). */
static nvs_handle_t s_nvs      = 0;
static bool         s_nvs_open = false;

/* ---- helpers ---------------------------------------------------------------------------------- */

/* Bring up NVS, erasing and retrying once if the partition is corrupt or a new format. */
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

/* Reset reasons that mean the firmware itself hung or crashed, and so count toward the
 * boot-loop guard. Brownout is deliberately excluded — a dying battery isn't something
 * safe mode can fix, and shouldn't be able to strand the watch in it. */
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

/* Build s_last_crash from a stored core dump (if there is one), persist and log it, and
 * erase the dump image afterward. Falls back to a bare reason string if there's no
 * usable dump (e.g. a brownout, or a corrupted image). */
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
            /* Record up to 8 backtrace addresses; a host tool can decode them against the build. */
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
                /* First 8 hex characters of the build hash are enough to identify the build. */
                n += snprintf(s_last_crash + n, sizeof(s_last_crash) - n, "elf=%.8s",
                              (const char *)sum->app_elf_sha256);
            }
        }
        free(sum);
    }
#else
    (void)have_dump;
#endif

    /* Clear the dump image so it doesn't mask the next crash's dump. */
    if (have_dump) esp_core_dump_image_erase();

    if (n == 0) {  /* a crash happened, but no usable dump was available */
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

    /* A stored dump image is itself proof a crash happened, even if the reset reason alone
     * missed it, so either signal counts as a crash. */
    bool have_dump = false;
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    have_dump = (esp_core_dump_image_check() == ESP_OK);
#endif
    bool crash = reason_is_crash(r) || have_dump;

    if (rel_nvs_ensure() != ESP_OK) {
        ESP_LOGE(TAG, "nvs unavailable — reliability bookkeeping disabled this boot (reason=%s)", s_reason_str);
        return;  /* the watchdog and core dump still work; only the persisted counters are lost */
    }
    if (nvs_open(REL_NVS_NS, NVS_READWRITE, &s_nvs) == ESP_OK) {
        s_nvs_open = true;
    } else {
        ESP_LOGW(TAG, "nvs_open(%s) failed — streak + last-crash not persisted", REL_NVS_NS);
    }

    /* Load the previous crash record so it's still available even after a clean reboot. */
    if (s_nvs_open) {
        size_t len = sizeof(s_last_crash);
        if (nvs_get_str(s_nvs, REL_KEY_LASTCRASH, s_last_crash, &len) != ESP_OK) {
            s_last_crash[0] = '\0';
        }
    }

    uint8_t streak = 0;
    if (s_nvs_open) nvs_get_u8(s_nvs, REL_KEY_STREAK, &streak);

    if (crash) {
        rel_record_crash(r, have_dump);      /* updates and persists s_last_crash */
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

/* Runs on the LVGL task via its own timer mechanism. Subscribes to the task watchdog on
 * first fire, then pets it every tick after. If the LVGL task ever wedges, this simply
 * stops firing and the watchdog reboots the device with LVGL named in the backtrace.
 * Keeps firing while the display is asleep, so screen-off never falsely trips it. */
#if NOCSIF_REL_HANG_TEST
static void rel_hang_test_cb(lv_timer_t *t)
{
    lv_timer_del(t);
    ESP_LOGE(TAG, "HANG TEST: blocking the LVGL task forever — idle stays healthy, so ONLY the "
                  "UI-liveness watchdog should catch this and reboot (~%ds).", 8 + REL_UI_WDT_PET_MS / 1000);
    vTaskDelay(portMAX_DELAY);  /* blocks forever but yields, so the idle-task watchdog misses it */
}
#endif

static void rel_ui_wdt_timer_cb(lv_timer_t *t)
{
    (void)t;
    static bool subscribed = false;
    if (!subscribed) {
        esp_err_t e = esp_task_wdt_add(NULL);   /* subscribes the calling (LVGL) task */
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE /* already subscribed */) {
            ESP_LOGW(TAG, "ui-liveness: task_wdt_add failed (%s)", esp_err_to_name(e));
            return;
        }
        subscribed = true;
        s_lvgl_task = xTaskGetCurrentTaskHandle();   /* saved for suspend()/resume() to use later */
        ESP_LOGI(TAG, "ui-liveness: LVGL task subscribed to task-wdt");
    }
    if (!s_liveness_paused) esp_task_wdt_reset();   /* skip the reset while suspended, since the task
                                                     * isn't subscribed and a reset would just log an error */
}

void nocsif_reliability_ui_liveness_suspend(bool suspend)
{
    if (!s_liveness_armed || s_lvgl_task == NULL) return;
    /* Add/remove the LVGL task from the watchdog by handle; safe to call from any task. */
    if (suspend) {
        s_liveness_paused = true;
        esp_task_wdt_delete(s_lvgl_task);
    } else {
        esp_task_wdt_add(s_lvgl_task);   /* starts a fresh window; the pet-timer resumes normally */
        s_liveness_paused = false;
    }
}

void nocsif_reliability_ui_liveness_arm(void)
{
    if (s_liveness_armed) return;
    /* Creating an LVGL timer touches LVGL state, so take the port lock around it. */
    if (!lvgl_port_lock(1000)) {
        ESP_LOGW(TAG, "ui-liveness: lvgl lock timeout — watchdog NOT armed");
        return;
    }
    lv_timer_t *t = lv_timer_create(rel_ui_wdt_timer_cb, REL_UI_WDT_PET_MS, NULL);
#if NOCSIF_REL_HANG_TEST
    lv_timer_t *ht = lv_timer_create(rel_hang_test_cb, 12000, NULL);
    if (ht) lv_timer_set_repeat_count(ht, 1);  /* fire only once */
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
