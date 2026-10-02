/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * NocSif addition (Bootable OS): a callback-backed, READ-ONLY MSC storage medium.
 * Instead of exposing a whole SD card or a wear-levelled flash region, the host's
 * SCSI READ(10) is served by an application-supplied reader — so the firmware can
 * present a single image FILE on the shared card (or any byte source) to the host
 * as a bootable USB disk, without touching the card's layout. Writes are refused
 * (the served image is immutable — "no persistence"). See tinyusb_msc.h
 * (tinyusb_msc_new_storage_callback) and firmware/src/bootos.c for the reader.
 */
#pragma once

#include "esp_err.h"
#include "msc_storage.h"     /* storage_medium_t */
#include "tinyusb_msc.h"     /* tinyusb_msc_read_cb_t */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Open a read-only, callback-backed storage medium.
 *
 * The medium reports a fixed geometry (total_sectors x sector_size) and forwards every
 * READ(10) to read_cb(ctx, lba, offset, size, dest). It is never FAT-mounted for the app
 * (mount/unmount are no-ops) and rejects all writes. A single instance is supported at a
 * time (matches serving one boot image), mirroring storage_sdmmc.c's single-card model.
 *
 * @param[in]  read_cb        Application reader. Returns ESP_OK on a full read.
 * @param[in]  ctx            Opaque argument passed back to read_cb.
 * @param[in]  total_sectors  Number of addressable sectors (image size / sector_size).
 * @param[in]  sector_size    Logical sector size in bytes (typically 512).
 * @param[out] medium         Receives the medium function table.
 */
esp_err_t storage_callback_open_medium(tinyusb_msc_read_cb_t read_cb, void *ctx,
                                       uint32_t total_sectors, uint32_t sector_size,
                                       const storage_medium_t **medium);

#ifdef __cplusplus
}
#endif
