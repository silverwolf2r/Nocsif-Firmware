/*
 * NocSif — public API for over-the-air firmware updates.
 *
 * Installs a firmware.bin into the inactive A/B partition and reboots into it. The
 * bootloader's rollback feature boots a fresh image as unverified and automatically
 * reverts to the previous slot unless nocsif_ota_confirm() is called once the app proves
 * itself stable — so a bad update that crashes or hangs is self-healing.
 *
 * The image comes either from the microSD card or is pulled from a GitHub mirror over
 * WiFi first. All the calls here just post a request to a background worker and return
 * immediately; progress and status are exposed through the getters for the UI to poll.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Where the update image is expected on the microSD card; the older flat-file location is
 * still accepted as a fallback if the folder copy isn't present. */
#define NOCSIF_OTA_SD_DIR       "/sd/nocsif/firmware"
#define NOCSIF_OTA_SD_PATH      "/sd/nocsif/firmware/firmware.bin"
#define NOCSIF_OTA_SD_PATH_OLD  "/sd/nocsif/firmware.bin"

/* State of a card-install job. */
typedef enum {
    NOCSIF_OTA_IDLE = 0,   /* nothing in progress */
    NOCSIF_OTA_SCANNING,   /* checking the card for an image */
    NOCSIF_OTA_RUNNING,    /* writing to the inactive slot */
    NOCSIF_OTA_SUCCESS,    /* written and marked bootable; about to reboot */
    NOCSIF_OTA_FAILED,     /* the job failed; see nocsif_ota_status_str() */
} nocsif_ota_state_t;

/* Start the background worker task. Cheap and safe to call even in safe mode, since OTA
 * is itself a recovery path. Idempotent. */
esp_err_t nocsif_ota_init(void);

/* Confirm the running image is healthy, cancelling its pending rollback. Does nothing if
 * the running slot isn't actually pending verification. Call once the app has proven
 * itself stable after boot. */
void nocsif_ota_confirm(void);

/* ---- status getters (cheap, safe to poll from the UI) -------------------------------- */
nocsif_ota_state_t nocsif_ota_state(void);
int         nocsif_ota_progress(void);       /* 0..100 during an install */
const char *nocsif_ota_status_str(void);     /* short human-readable status line */

/* Which slot is running, and whether it's still pending confirmation. */
const char *nocsif_ota_running_label(void);
bool        nocsif_ota_pending_verify(void);

/* ---- microSD-card update source -------------------------------------------------------- */
/* Ask the worker to re-check the card for an update image; result lands in the getters below. */
void        nocsif_ota_request_sd_scan(void);
bool        nocsif_ota_sd_present(void);     /* a usable image was found on the card */
uint32_t    nocsif_ota_sd_size(void);        /* its size in bytes, 0 if none */
const char *nocsif_ota_sd_version(void);     /* its embedded version string, "" if none */

/* Ask the worker to flash the card image and reboot into it. Refused if a job is already
 * running, no image was found, or the battery is low and no USB power is present (a
 * brown-out mid-flash is the one failure rollback can't undo). */
void        nocsif_ota_request_install_sd(void);

/* ---- GitHub download source ------------------------------------------------------------ *
 * Manual only — the watch never checks on a timer. "Check" fetches a manifest and compares
 * its version against the running build. "Download" streams the image to the card, verifies
 * its checksum, then hands off to the card-scan path above. */
#define NOCSIF_OTA_REPO_DEFAULT "silverwolf2r/Nocsif-Firmware"
typedef enum {
    NOCSIF_OTA_WEB_IDLE = 0,
    NOCSIF_OTA_WEB_CHECKING,
    NOCSIF_OTA_WEB_UPTODATE,      /* already running the published version */
    NOCSIF_OTA_WEB_AVAILABLE,     /* a newer/different version is published */
    NOCSIF_OTA_WEB_DOWNLOADING,   /* streaming the image to the card */
    NOCSIF_OTA_WEB_DOWNLOADED,    /* verified on the card, ready to install */
    NOCSIF_OTA_WEB_FAILED,        /* see nocsif_ota_web_status_str() */
} nocsif_ota_web_state_t;
void        nocsif_ota_request_web_check(void);
void        nocsif_ota_request_web_download(void);   /* needs state == AVAILABLE first */
nocsif_ota_web_state_t nocsif_ota_web_state(void);
int         nocsif_ota_web_progress(void);           /* 0..100 during a download */
const char *nocsif_ota_web_status_str(void);
const char *nocsif_ota_web_version(void);            /* published version, "" until checked */
uint32_t    nocsif_ota_web_size(void);               /* published size in bytes, 0 until checked */
bool        nocsif_ota_web_busy(void);               /* a check or download is in progress */
const char *nocsif_ota_repo(void);                   /* current "owner/repo" source */
void        nocsif_ota_set_repo(const char *repo);   /* change and persist the source repo */
const char *nocsif_ota_running_version(void);        /* version string of the running image */

#ifdef __cplusplus
}
#endif
