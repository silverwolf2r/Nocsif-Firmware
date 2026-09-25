/*
 * NocSif — public API for microSD card access over SPI.
 *
 * Brings the card up on the SPI bus it shares with the NFC and LoRa chips (the display
 * has its own separate bus). Power must already be on before calling init. This module
 * only initialises the raw card handle — it deliberately does not mount a FAT filesystem,
 * since usb_gadget.c owns that mount and the single-owner handoff between the app and a
 * connected USB host; mounting here too would corrupt the volume.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdmmc_cmd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up the shared SPI bus (parking the other devices' chip selects high) and
 * initialise the card to a raw handle, with no FAT mount. Requires the SD power rail
 * already on. Idempotent. */
esp_err_t nocsif_sdcard_init(void);

/* The initialised card handle, or NULL if init hasn't succeeded. The USB mass-storage
 * gadget wraps this same handle. */
sdmmc_card_t *nocsif_sdcard_card(void);

/* Route the FAT volume's sector I/O through the heap-free staged path (see sdcard.c "FatFs sector I/O
 * without the heap"). Call right after every FAT mount of the card for the app — esp_tinyusb re-registers
 * IDF's own diskio on each mount, so usb_gadget.c calls this after the boot mount and after every
 * File-Share -> app handoff. No-op (with a warning) when the card is not mounted for the app. */
void nocsif_sdcard_diskio_attach(void);

/* Log a directory listing of `path` (e.g. "/sd"). Only valid while the FAT volume is
 * mounted for the app — i.e. when USB-MSC ownership is MOUNT_APP (not exposed to a host).
 * Used to prove a host-copied file is visible to firmware after the host ejects. Takes the
 * /sd access lock internally. */
void nocsif_sdcard_list(const char *path);

/* Serialise app-side FAT access to the card: more than one task touching the filesystem
 * at once can race inside FatFs, so every caller wraps its file I/O in this lock. Returns
 * false on timeout, or if called before init() has created the lock. */
bool nocsif_sdcard_lock(uint32_t timeout_ms);
void nocsif_sdcard_unlock(void);

/* Telemetry (loudness pass): the task name currently holding the /sd lock ("" when free — a snapshot,
 * for a log line, not for logic), and the FatFs sector-read counters since boot: direct (internal,
 * multi-sector) vs staged (one sector at a time) calls, sectors, and time in the driver. A hold longer
 * than 250 ms is logged with its owner at unlock. */
const char *nocsif_sdcard_lock_holder(void);
void        nocsif_sdcard_read_stats(uint32_t *direct_calls, uint32_t *staged_calls, uint32_t *sectors, uint32_t *us);

#ifdef __cplusplus
}
#endif
