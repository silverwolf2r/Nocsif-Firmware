/*
 * NocSif — Bootable OS (USB): serve an OS image FILE on the shared microSD to a PC as a
 * read-only bootable USB disk, WITHOUT taking over the card.
 *
 * The image (a whole-disk .img or an isohybrid .iso) lives as an ordinary file in a NocSif
 * folder on the normal FAT card (NOCSIF_BOOTOS_DIR), alongside everything else. When the
 * user picks it, usb_gadget presents that file's bytes to the host as a read-only USB disk
 * (via the vendored esp_tinyusb callback medium, tinyusb_msc_new_storage_callback) — so the
 * PC boots it exactly like a dd'd USB stick, the card is never repartitioned, and nothing is
 * written back (no persistence).
 *
 * Hardware ceiling to remember: the ESP32-S3 USB is full-speed (~650 KB/s), so booting a
 * live OS off this is correct but slow. See docs/design / RESUME for the feature plan.
 *
 * Phase 0 scope: arm one image, serve it, stop on request. The Bootable-OS list UI,
 * bootability detection and the Tails downloader are Phase 1.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Folder on the shared FAT card where OS images are dropped / downloaded. The watch creates it. */
#define NOCSIF_BOOTOS_DIR   "/sd/nocsif/bootos"

/* Create NOCSIF_BOOTOS_DIR if missing (needs /sd mounted for the app). Idempotent; non-fatal. */
esp_err_t nocsif_bootos_init(void);

/* Begin serving the image at `path` (an .iso/.img on /sd) to the host as a read-only bootable
 * disk. Opens the file, computes its geometry (size / 512), and asks the usb_gadget worker to
 * enumerate it. Safe from an LVGL callback (the USB switch runs off-thread on the worker).
 * Returns:
 *   ESP_OK                armed; the worker is bringing the disk up
 *   ESP_ERR_INVALID_STATE /sd not available, or already serving
 *   ESP_ERR_NOT_FOUND     file missing / unreadable
 *   ESP_ERR_INVALID_SIZE  file too small / not a whole-disk image
 */
esp_err_t nocsif_bootos_serve(const char *path);

/* Stop serving and close the image (detaches the USB disk from the host). Safe any time. */
void nocsif_bootos_stop(void);

/* True while an image is armed/served. LVGL-safe. */
bool nocsif_bootos_active(void);

/* The path currently being served, or "" when idle. LVGL-safe (points at a static buffer). */
const char *nocsif_bootos_current(void);

/* Find the first *.iso / *.img in NOCSIF_BOOTOS_DIR (case-insensitive) into `buf`.
 * Returns true and fills `buf` (a full "/sd/..." path) if one is found. Needs /sd mounted. */
bool nocsif_bootos_find_first(char *buf, size_t buflen);

/* Bootability of an image, from a quick peek at its first sectors (Phase 1). This is a heuristic
 * label, not a guarantee — it tells the user whether an image even has a block-level boot structure. */
typedef enum {
    NOCSIF_BOOTOS_BOOTABLE = 0,  /* whole-disk .img or isohybrid .iso (MBR/GPT partition table) — boots */
    NOCSIF_BOOTOS_CD_ONLY,       /* pure ISO9660, no block-level boot structure — won't boot served this way */
    NOCSIF_BOOTOS_UNKNOWN,       /* no recognizable boot signature */
} nocsif_bootos_kind_t;

/* Short ASCII label for a kind (list UI): "bootable" / "CD-only ISO" / "unknown". */
const char *nocsif_bootos_kind_str(nocsif_bootos_kind_t kind);

/* Classify the image at `path` by peeking sector 0 (MBR), LBA1 (GPT) and 0x8000 (ISO9660). Needs /sd. */
nocsif_bootos_kind_t nocsif_bootos_probe(const char *path);

/* One image in the drop folder. */
typedef struct {
    char                 name[128];   /* file name only (no path) */
    uint64_t             size;        /* bytes */
    nocsif_bootos_kind_t kind;        /* bootability label */
} nocsif_bootos_entry_t;

/* Enumerate *.iso/*.img in NOCSIF_BOOTOS_DIR into `out` (up to `max`), probing each; returns the count.
 * Needs /sd. Each entry is name + size + bootability. */
int nocsif_bootos_enumerate(nocsif_bootos_entry_t *out, int max);

#ifdef __cplusplus
}
#endif
