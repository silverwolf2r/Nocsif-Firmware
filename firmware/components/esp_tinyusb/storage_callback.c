/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * NocSif addition (Bootable OS): read-only, callback-backed MSC storage medium.
 * See include_private/storage_callback.h.
 */

#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "msc_storage.h"
#include "storage_callback.h"

static const char *TAG = "storage_callback";

/* Single-instance state (one served image at a time; same model as storage_sdmmc.c's _scard). */
static tinyusb_msc_read_cb_t s_read_cb;
static void                 *s_ctx;
static uint32_t              s_total_sectors;
static uint32_t              s_sector_size;

/* Never FAT-mounted for the app: the host owns the served image; the app only feeds bytes. */
static esp_err_t cb_mount(BYTE pdrv)
{
    (void)pdrv;
    return ESP_OK;
}

static esp_err_t cb_unmount(void)
{
    return ESP_OK;
}

static esp_err_t cb_read(uint32_t lba, uint32_t offset, size_t size, void *dest)
{
    if (s_read_cb == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return s_read_cb(s_ctx, lba, offset, size, dest);
}

/* Read-only: the served boot image is immutable (no persistence). */
static esp_err_t cb_write(uint32_t lba, uint32_t offset, size_t size, const void *src)
{
    (void)lba;
    (void)offset;
    (void)size;
    (void)src;
    ESP_LOGW(TAG, "write refused: served image is read-only");
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t cb_get_info(storage_info_t *info)
{
    info->total_sectors = s_total_sectors;
    info->sector_size   = s_sector_size;
    return ESP_OK;
}

static void cb_close(void)
{
    s_read_cb = NULL;
    s_ctx = NULL;
    s_total_sectors = 0;
    s_sector_size = 0;
}

/* Classified as a generic block medium (SDMMC): the deferred-write overflow guard in
 * tinyusb_msc.c only special-cases SPIFLASH, and writes are refused here regardless. */
static const storage_medium_t callback_storage_medium = {
    .type     = STORAGE_MEDIUM_TYPE_SDMMC,
    .mount    = &cb_mount,
    .unmount  = &cb_unmount,
    .read     = &cb_read,
    .write    = &cb_write,
    .get_info = &cb_get_info,
    .close    = &cb_close,
};

esp_err_t storage_callback_open_medium(tinyusb_msc_read_cb_t read_cb, void *ctx,
                                       uint32_t total_sectors, uint32_t sector_size,
                                       const storage_medium_t **medium)
{
    ESP_RETURN_ON_FALSE(medium != NULL, ESP_ERR_INVALID_ARG, TAG, "medium pointer can't be NULL");
    ESP_RETURN_ON_FALSE(read_cb != NULL, ESP_ERR_INVALID_ARG, TAG, "read_cb can't be NULL");
    ESP_RETURN_ON_FALSE(total_sectors != 0 && sector_size != 0, ESP_ERR_INVALID_ARG, TAG, "bad geometry");

    s_read_cb = read_cb;
    s_ctx = ctx;
    s_total_sectors = total_sectors;
    s_sector_size = sector_size;
    *medium = &callback_storage_medium;
    return ESP_OK;
}
