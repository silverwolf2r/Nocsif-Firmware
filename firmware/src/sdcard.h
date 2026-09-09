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

/* Log a directory listing of `path`. Only meaningful while the FAT volume is mounted for
 * the app rather than exposed to a USB host. Takes the card lock internally. */
void nocsif_sdcard_list(const char *path);

/* Serialise app-side FAT access to the card: more than one task touching the filesystem
 * at once can race inside FatFs, so every caller wraps its file I/O in this lock. Returns
 * false on timeout, or if called before init() has created the lock. */
bool nocsif_sdcard_lock(uint32_t timeout_ms);
void nocsif_sdcard_unlock(void);

#ifdef __cplusplus
}
#endif
