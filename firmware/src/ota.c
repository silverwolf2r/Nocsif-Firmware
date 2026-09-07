/*
 * NocSif — over-the-air firmware update (M-OTA). See ota.h for the design.
 *
 * One worker task, woken by a task-notification, does the two blocking jobs off the LVGL thread:
 *   SCAN    — claim /sd, stat + header-validate NOCSIF_OTA_SD_PATH, publish present/size/version.
 *   INSTALL — claim /sd, stream the .bin into the inactive slot (esp_ota_*) with a live progress %,
 *             mark it bootable, and reboot into it. The bootloader boots it as PENDING_VERIFY;
 *             nocsif_ota_confirm() (main heartbeat, ~30 s) cancels the rollback once it runs healthy.
 *
 * Status fields are single-writer (this worker) / single-reader (the LVGL poll) plain reads — the
 * same lock-free convention the wifi/mic screens use. s_status points at string literals only (a
 * pointer store is atomic on this target), so there is no torn-string race.
 */
#include "ota.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_app_desc.h"

#include "sdcard.h"          /* nocsif_sdcard_lock/unlock — serialize app-side FAT access */
#include "usb_gadget.h"      /* nocsif_usb_gadget_claim_sd/release_sd — own /sd vs USB-MSC */
#include "reliability.h"     /* nocsif_reliability_ui_liveness_suspend — the flash ops starve LVGL */

static const char *TAG = "nocsif_ota";

/* ESP application image: byte 0 is the image magic; the app descriptor sits at a fixed offset right
 * after the image header (24 B) + first segment header (8 B). */
#define OTA_IMG_MAGIC        0xE9u
#define OTA_APP_DESC_OFFSET  32
#define OTA_HDR_READ         (OTA_APP_DESC_OFFSET + sizeof(esp_app_desc_t))   /* bytes to sniff */

/* Streaming chunk (worker-only; keeps the task stack light — the whole image is never buffered). */
#define OTA_CHUNK            4096

typedef enum { REQ_NONE = 0, REQ_SCAN, REQ_INSTALL } ota_req_t;

static TaskHandle_t                 s_task;
static volatile ota_req_t           s_req;
static volatile nocsif_ota_state_t  s_state    = NOCSIF_OTA_IDLE;
static volatile int                 s_progress;                   /* 0..100 */
static const char * volatile        s_status   = "";              /* literals only */
static volatile bool                s_sd_present;
static volatile uint32_t            s_sd_size;
static char                         s_sd_ver[32];                 /* worker-written, UI-read */

/* 64-byte aligned + DMA-capable internal RAM so the SD read DMAs STRAIGHT into it with no bounce:
 * spi_master only allocates an internal-DMA bounce buffer when the target is not DMA-capable OR not
 * aligned, and under int-DMA fragmentation that alloc can fault. Aligning s_buf means FatFs's direct
 * multi-sector DATA read needs no bounce. NOTE: WiFi is no longer yielded for the read (the yield
 * machinery was deleted with the resident-BLE-controller model, RAM-BUDGET.md remake #5), so this
 * aligned staging buffer is the sole guarantee the streaming read stays bounce-free. Only the small FAT
 * directory reads inside fopen use FatFs's own window buffer; verify an OTA install with WiFi ASSOCIATED
 * on-device (RAM-BUDGET.md conflict C6). */
static uint8_t                      s_buf[OTA_CHUNK] __attribute__((aligned(64)));  /* worker-only */

/* ---- helpers --------------------------------------------------------------------------------- */

/* Sniff an open image file: confirm it is an ESP app image and (optionally) extract its version.
 * Leaves the file position past the header — the caller rewinds before streaming. */
static bool image_sniff(FILE *f, char *ver, size_t vern)
{
    uint8_t hdr[OTA_HDR_READ];
    if (fseek(f, 0, SEEK_SET) != 0) return false;
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr) return false;
    if (hdr[0] != OTA_IMG_MAGIC) return false;
    const esp_app_desc_t *d = (const esp_app_desc_t *)(hdr + OTA_APP_DESC_OFFSET);
    if (d->magic_word != ESP_APP_DESC_MAGIC_WORD) return false;
    if (ver && vern) {
        snprintf(ver, vern, "%s", d->version);
    }
    return true;
}

/* Take /sd away from USB-MSC AND the app FAT lock. Returns true on success; on failure sets
 * s_status/s_state(fail_state) and returns false with nothing held. */
static bool sd_grab(nocsif_ota_state_t fail_state)
{
    if (nocsif_usb_gadget_claim_sd(2000) != ESP_OK) {
        s_status = "microSD unavailable";
        s_state  = fail_state;
        return false;
    }
    if (!nocsif_sdcard_lock(3000)) {
        nocsif_usb_gadget_release_sd();
        s_status = "microSD busy";
        s_state  = fail_state;
        return false;
    }
    return true;
}

static void sd_drop(void)
{
    nocsif_sdcard_unlock();
    nocsif_usb_gadget_release_sd();
}

/* ---- jobs ------------------------------------------------------------------------------------ */

static void do_scan(void)
{
    s_state = NOCSIF_OTA_SCANNING;
    s_status = "scanning card\xE2\x80\xA6";
    s_sd_present = false;
    s_sd_size = 0;
    s_sd_ver[0] = '\0';

    if (!sd_grab(NOCSIF_OTA_IDLE)) return;

    const char *result = "no firmware.bin on card";
    FILE *f = fopen(NOCSIF_OTA_SD_PATH, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        char ver[32] = "";
        const esp_partition_t *tgt = esp_ota_get_next_update_partition(NULL);
        if (sz <= 0 || !image_sniff(f, ver, sizeof ver)) {
            result = "firmware.bin is not a valid image";
        } else if (tgt && (uint32_t)sz > tgt->size) {
            result = "image too large for slot";
        } else {
            s_sd_size = (uint32_t)sz;
            snprintf(s_sd_ver, sizeof s_sd_ver, "%s", ver);
            s_sd_present = true;
            result = "image ready";
        }
        fclose(f);
    }
    sd_drop();
    s_status = result;
    s_state = NOCSIF_OTA_IDLE;
}

static void do_install(void)
{
    s_state = NOCSIF_OTA_RUNNING;
    s_progress = 0;
    s_status = "preparing\xE2\x80\xA6";

    const esp_partition_t *tgt = esp_ota_get_next_update_partition(NULL);
    if (!tgt) { s_status = "no OTA slot in table"; s_state = NOCSIF_OTA_FAILED; return; }

    bool             wdt_suspended = false;
    bool             sd_held       = false;
    bool             have_handle   = false;
    esp_ota_handle_t h             = 0;
    FILE            *f             = NULL;
    const char      *err           = NULL;
    long             sz            = 0;
    size_t           written       = 0;
    esp_err_t        e             = ESP_OK;

    /* The streaming read below DMAs each chunk straight into the aligned s_buf (bounce-free); WiFi is no
     * longer yielded for it (the yield machinery was deleted with the resident-BLE model). See the s_buf
     * note re: verifying an install with WiFi associated on-device.
     *
     * Pause the UI-liveness watchdog for the duration: the flash erase/write below repeatedly disables
     * the cache the LVGL task's pet-timer runs from, so it cannot pet even though nothing is wrong —
     * the watchdog would otherwise panic mid-install (the other crash root cause). Resumed on every
     * exit below (on success we reboot anyway). */
    nocsif_reliability_ui_liveness_suspend(true);
    wdt_suspended = true;

    if (!sd_grab(NOCSIF_OTA_FAILED)) goto fail_kept_status;   /* sd_grab already set s_status */
    sd_held = true;

    f = fopen(NOCSIF_OTA_SD_PATH, "rb");
    if (!f) { err = "no firmware.bin on card"; goto fail; }

    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || !image_sniff(f, NULL, 0)) { err = "not a firmware image"; goto fail; }
    if ((uint32_t)sz > tgt->size)            { err = "image too large for slot"; goto fail; }
    fseek(f, 0, SEEK_SET);   /* rewind past the sniff read before streaming */

    /* OTA_WITH_SEQUENTIAL_WRITES: do NOT erase the whole slot up front (that single multi-second
     * blocking erase is what froze the UI + tripped the watchdog). Each sector is erased lazily inside
     * esp_ota_write, in small chunks the loop below yields between — the progress bar keeps animating. */
    e = esp_ota_begin(tgt, OTA_WITH_SEQUENTIAL_WRITES, &h);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin(%s): %s", tgt->label, esp_err_to_name(e));
        err = "OTA begin failed"; goto fail;
    }
    have_handle = true;

    ESP_LOGW(TAG, "OTA: writing %ld bytes to %s (sequential erase-on-write)", sz, tgt->label);
    s_status = "writing\xE2\x80\xA6";
    for (;;) {
        size_t got = fread(s_buf, 1, sizeof s_buf, f);
        if (got == 0) break;                       /* EOF (or read error — caught by the size check) */
        e = esp_ota_write(h, s_buf, got);          /* erases this sector then writes it */
        if (e != ESP_OK) { ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(e));
                           err = "flash write failed"; goto fail; }
        written += got;
        s_progress = (int)((uint64_t)written * 100u / (size_t)sz);
        if ((written & 0x7FFFu) == 0) vTaskDelay(1);   /* ~every 32 KB: let the UI redraw the progress */
    }
    fclose(f); f = NULL;
    if (written != (size_t)sz) { err = "short read from card"; goto fail; }

    e = esp_ota_end(h);            /* validates the written image (checksum / signature) */
    have_handle = false;
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(e));
        err = (e == ESP_ERR_OTA_VALIDATE_FAILED) ? "image failed validation" : "finalize failed";
        goto fail;
    }
    e = esp_ota_set_boot_partition(tgt);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition(%s): %s", tgt->label, esp_err_to_name(e));
        err = "set-boot failed"; goto fail;
    }

    sd_drop();   /* release the card before the reboot */
    nocsif_reliability_ui_liveness_suspend(false);   /* flash work done — re-arm the UI watchdog */
    s_progress = 100;
    s_status = "installed \xE2\x80\x94 rebooting";
    s_state  = NOCSIF_OTA_SUCCESS;
    ESP_LOGW(TAG, "OTA installed to %s (%u bytes). Rebooting into it (PENDING_VERIFY — confirms once "
                  "the UI is up, else rolls back).", tgt->label, (unsigned)written);
    vTaskDelay(pdMS_TO_TICKS(1600));   /* let the UI paint SUCCESS before the reboot */
    esp_restart();
    return;

fail:
    s_status = err ? err : "install failed";
fail_kept_status:
    if (f)            fclose(f);
    if (have_handle)  esp_ota_abort(h);
    if (sd_held)      sd_drop();
    if (wdt_suspended) nocsif_reliability_ui_liveness_suspend(false);   /* re-arm the UI watchdog */
    s_state = NOCSIF_OTA_FAILED;
    ESP_LOGE(TAG, "OTA install aborted: %s", s_status);
}

static void ota_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        ota_req_t r = s_req;
        s_req = REQ_NONE;
        if (r == REQ_SCAN)         do_scan();
        else if (r == REQ_INSTALL) do_install();
    }
}

/* ---- public API ------------------------------------------------------------------------------ */

esp_err_t nocsif_ota_init(void)
{
    if (s_task != NULL) return ESP_OK;
    if (xTaskCreate(ota_task, "ota", 8192, NULL, 4, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "failed to create OTA task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void nocsif_ota_confirm(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (!run) return;
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            ESP_LOGI(TAG, "OTA image on %s confirmed valid — rollback cancelled", run->label);
        } else {
            ESP_LOGW(TAG, "esp_ota_mark_app_valid_cancel_rollback failed");
        }
    }
}

void nocsif_ota_request_sd_scan(void)
{
    if (s_task == NULL || s_state == NOCSIF_OTA_RUNNING) return;
    s_req = REQ_SCAN;
    xTaskNotifyGive(s_task);
}

void nocsif_ota_request_install_sd(void)
{
    if (s_task == NULL || s_state == NOCSIF_OTA_RUNNING || !s_sd_present) return;
    s_req = REQ_INSTALL;
    xTaskNotifyGive(s_task);
}

nocsif_ota_state_t nocsif_ota_state(void)   { return s_state; }
int         nocsif_ota_progress(void)       { return s_progress; }
const char *nocsif_ota_status_str(void)     { return s_status ? s_status : ""; }
bool        nocsif_ota_sd_present(void)     { return s_sd_present; }
uint32_t    nocsif_ota_sd_size(void)        { return s_sd_size; }
const char *nocsif_ota_sd_version(void)     { return s_sd_ver; }

const char *nocsif_ota_running_label(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    return run ? run->label : "?";
}

bool nocsif_ota_pending_verify(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (!run) return false;
    esp_ota_img_states_t st;
    return esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
}
