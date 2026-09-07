/*
 * NocSif — over-the-air firmware update (M-OTA).
 *
 * A/B slot updates with automatic rollback. The partition table (partitions.csv) carries two equal
 * app slots (ota_0 / ota_1) + an otadata region; CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE makes the
 * bootloader boot a freshly-installed image as PENDING_VERIFY and roll back to the previous slot
 * unless the app confirms it healthy. That confirm is nocsif_ota_confirm(), called from the main
 * heartbeat once a stable, serviceable state is reached (the same ~30 s dwell that clears the
 * reliability crash streak) — so an image that crashes or hangs before then auto-reverts.
 *
 * Source (this slice): a firmware.bin on the microSD card (dropped there via File Share). A worker
 * task claims /sd away from USB-MSC, validates the image header, streams it into the inactive slot
 * with a live progress %, marks it bootable, and reboots. All request calls are non-blocking and
 * LVGL-safe (they notify the worker); status is published for the UI to poll.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The default image path on the microSD card. Drop firmware.bin here (via Settings > USB > File
 * Share, or any card reader) for the Update screen to find it. */
#define NOCSIF_OTA_SD_PATH   "/sd/nocsif/firmware.bin"

/* Install-job state (LVGL-safe read via nocsif_ota_state). */
typedef enum {
    NOCSIF_OTA_IDLE = 0,   /* nothing running                                         */
    NOCSIF_OTA_SCANNING,   /* probing the card for an image                           */
    NOCSIF_OTA_RUNNING,    /* writing an image to the inactive slot (progress valid)  */
    NOCSIF_OTA_SUCCESS,    /* image written + set bootable; the watch reboots shortly */
    NOCSIF_OTA_FAILED,     /* the last job failed (nocsif_ota_status_str has why)     */
} nocsif_ota_state_t;

/* Create the idle worker task (task + notify; NO flash/SD touched until a request). Idempotent;
 * call once at boot. NOT safe-mode-gated — OTA is a recovery path, so it must work in safe mode. */
esp_err_t nocsif_ota_init(void);

/* Cancel the pending rollback for the running image (mark it valid). No-op unless the running slot
 * is actually PENDING_VERIFY — so it is harmless on a directly-flashed (esptool) build and on every
 * normal boot. Call once the firmware has proven itself healthy (main.c heartbeat dwell). */
void nocsif_ota_confirm(void);

/* ---- status (all LVGL-safe plain reads) --------------------------------------------------- */
nocsif_ota_state_t nocsif_ota_state(void);
int         nocsif_ota_progress(void);       /* 0..100 while RUNNING / SUCCESS                 */
const char *nocsif_ota_status_str(void);     /* short human line for the current state          */

/* Running-image identity + rollback posture, for the Update screen header. */
const char *nocsif_ota_running_label(void);  /* "ota_0" / "ota_1" (the active slot)             */
bool        nocsif_ota_pending_verify(void); /* running image is not yet confirmed (rollback armed) */

/* ---- microSD source ------------------------------------------------------------------------ */
/* Ask the worker to (re)probe the card for NOCSIF_OTA_SD_PATH. Result lands in the getters below;
 * state goes SCANNING -> IDLE. Non-blocking. */
void        nocsif_ota_request_sd_scan(void);
bool        nocsif_ota_sd_present(void);     /* a readable, valid-looking image is on the card   */
uint32_t    nocsif_ota_sd_size(void);        /* its size in bytes (0 if none)                    */
const char *nocsif_ota_sd_version(void);     /* the app version embedded in the card image, or "" */

/* Ask the worker to install the microSD image into the inactive slot and reboot into it. Non-blocking;
 * a no-op if a job is already running or no valid image was found by the last scan. */
void        nocsif_ota_request_install_sd(void);

#ifdef __cplusplus
}
#endif
