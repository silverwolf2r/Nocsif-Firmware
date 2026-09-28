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

/* Re-probe a microSD that was absent at boot (or lost since): power-cycle the card (ALDO1 off with every
 * SD line driven low) and re-run card init WITHOUT a reboot, so a reseated card is detected live. The
 * firmware power-cycle stands in for the physical reseat that used to be required after a marginal init or
 * a warm-reset-wedged card. No-op (returns ESP_OK) when a card is already up — it never disturbs a working
 * card, so the USB-MSC storage's card pointer can never dangle. Takes the /sd lock per attempt (<= ~2.6 s
 * each, 2 attempts); blocks up to ~6 s worst case, so call it OFF the LVGL task. Returns ESP_OK once a
 * card is up, else the init error. usb_gadget.c wraps this (nocsif_usb_gadget_sd_rescan) to also
 * (re)create the File-Share MSC storage once a card appears. */
esp_err_t nocsif_sdcard_reprobe(void);

/* The socket's card-detect switch (XL9555 IO10): 1 = a card is in the slot, 0 = empty, -1 = the expander
 * could not be read. Only says a card is PRESENT — nocsif_sdcard_card() says whether it is initialised. */
int nocsif_sdcard_detect(void);

/* The card was pulled: mark it absent (nocsif_sdcard_card() returns NULL, FatFs I/O fails fast) without
 * freeing it — the File-Share storage keeps the card pointer, and nocsif_sdcard_reprobe() re-initialises a
 * re-inserted card in place. */
void nocsif_sdcard_mark_removed(void);

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
