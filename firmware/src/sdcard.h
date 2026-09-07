/*
 * NocSif — microSD over SDSPI (M4-P1 raw bring-up, M4-P3 hands the card to USB-MSC)
 *
 * The T-Watch Ultra microSD sits on the SHARED SPI bus (MOSI=34, MISO=33, SCK=35,
 * CS=21), shared with NFC (CS=4) and LoRa (CS=36). The display AMOLED is on a
 * SEPARATE QSPI bus on SPI2_HOST, so the SD takes SPI3_HOST. Power = AXP2101 ALDO1
 * (nocsif_power_sd_rail), which must be on before init.
 *
 * As of P3 this module only brings the card up to a RAW sdmmc_card_t (no FAT mount):
 * the esp_tinyusb MSC helper owns the FAT mount + the single-owner USB<->app handoff
 * (see usb_gadget.c). Mounting FAT here too would double-own the volume and corrupt it.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdmmc_cmd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up SPI3_HOST on the shared bus (parking NFC/LoRa CS high) and initialise the
 * SD card to a raw sdmmc_card_t — NO FAT mount. Requires nocsif_power_sd_rail(true)
 * first. Idempotent. */
esp_err_t nocsif_sdcard_init(void);

/* The initialised card handle, or NULL if init failed. The USB-MSC storage wraps this
 * same sdmmc_card_t (P3). */
sdmmc_card_t *nocsif_sdcard_card(void);

/* Log a directory listing of `path` (e.g. "/sd"). Only valid while the FAT volume is
 * mounted for the app — i.e. when USB-MSC ownership is MOUNT_APP (not exposed to a host).
 * Used to prove a host-copied file is visible to firmware after the host ejects. Takes the
 * /sd access lock internally. */
void nocsif_sdcard_list(const char *path);

/* Serialise app-side FAT access to the shared /sd volume. More than one task reading the
 * card at once (the USB-MSC monitor listing + the DuckyScript reader) can race inside FatFs,
 * so both wrap their file I/O in this lock. Returns false on timeout, or if called before
 * nocsif_sdcard_init() created the lock. */
bool nocsif_sdcard_lock(uint32_t timeout_ms);
void nocsif_sdcard_unlock(void);

#ifdef __cplusplus
}
#endif
