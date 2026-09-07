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

/* The image on the microSD card: `nocsif/firmware/firmware.bin` — the same folder shape the public GitHub
 * mirror uses (§4.10). Drop it there via File Share or any card reader, or let "Download" fetch it. The
 * pre-§4.10 location /sd/nocsif/firmware.bin is still accepted when the folder copy is absent. */
#define NOCSIF_OTA_SD_DIR       "/sd/nocsif/firmware"
#define NOCSIF_OTA_SD_PATH      "/sd/nocsif/firmware/firmware.bin"
#define NOCSIF_OTA_SD_PATH_OLD  "/sd/nocsif/firmware.bin"

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
 * a no-op if a job is already running or no valid image was found by the last scan. Refused (status
 * says so) below 30 % battery unless USB power is present — a brown-out mid-flash is the one failure the
 * A/B rollback cannot undo. */
void        nocsif_ota_request_install_sd(void);

/* ---- §4.10 GitHub source ------------------------------------------------------------------ *
 * The watch pulls updates from the PUBLIC mirror: raw.githubusercontent.com/<repo>/main/nocsif/firmware/
 * {manifest.json, firmware.bin}. MANUAL ONLY — nothing runs on a timer (operator call). "Check" fetches
 * the manifest and compares its version with the running image (esp_app_desc.version; both come from
 * `git describe`, so any published build that differs from the running one counts as available).
 * "Download" streams firmware.bin to NOCSIF_OTA_SD_PATH (replacing the card copy) while hashing it,
 * verifies size + sha256, then re-scans the card so the shipped Install path takes over. Both run on a
 * PSRAM-stacked worker — TLS needs no DMA now that mbedTLS allocates from PSRAM (gotcha-tls-under-wifi);
 * a parked STA is woken for them. */
#define NOCSIF_OTA_REPO_DEFAULT "silverwolf2r/Nocsif-Firmware"
typedef enum {
    NOCSIF_OTA_WEB_IDLE = 0,
    NOCSIF_OTA_WEB_CHECKING,
    NOCSIF_OTA_WEB_UPTODATE,      /* manifest version == running version                 */
    NOCSIF_OTA_WEB_AVAILABLE,     /* a different version is published (version/size below) */
    NOCSIF_OTA_WEB_DOWNLOADING,   /* streaming to the card (progress valid)              */
    NOCSIF_OTA_WEB_DOWNLOADED,    /* on the card, size + sha256 verified — Install next   */
    NOCSIF_OTA_WEB_FAILED,        /* the status string says why                          */
} nocsif_ota_web_state_t;
void        nocsif_ota_request_web_check(void);      /* non-blocking; LVGL-safe                    */
void        nocsif_ota_request_web_download(void);   /* non-blocking; needs AVAILABLE              */
nocsif_ota_web_state_t nocsif_ota_web_state(void);
int         nocsif_ota_web_progress(void);           /* 0..100 while DOWNLOADING                   */
const char *nocsif_ota_web_status_str(void);         /* short literal for the Update screen        */
const char *nocsif_ota_web_version(void);            /* the published version ("" until checked)   */
uint32_t    nocsif_ota_web_size(void);               /* the published image size (0 until checked) */
bool        nocsif_ota_web_busy(void);               /* CHECKING / DOWNLOADING — a Governor holder */
const char *nocsif_ota_repo(void);                   /* "owner/repo" (persisted "ota_repo")        */
void        nocsif_ota_set_repo(const char *repo);   /* persist; the next Check uses it            */
const char *nocsif_ota_running_version(void);        /* esp_app_desc.version of the running image  */

#ifdef __cplusplus
}
#endif
