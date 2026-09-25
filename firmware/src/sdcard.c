/*
 * NocSif — microSD card driver implementation. See sdcard.h.
 *
 * Runs the same sdspi_host_init -> sdspi_host_init_device -> sdmmc_card_init sequence
 * that a full FAT mount would use internally, but stops short of mounting FAT — that's
 * left to usb_gadget.c, which hands this raw card handle to the USB mass-storage helper.
 */
#include "sdcard.h"

#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "driver/sdmmc_host.h"
#include "esp_private/spi_common_internal.h"   /* spi_bus_get_dma_ctx/_attr: the MEASURED bounce alignment */
#include "esp_memory_utils.h"                  /* esp_ptr_external_ram: pick the direct vs staged sector path */
#include "esp_timer.h"                         /* esp_timer_get_time: lock-hold + sector-read telemetry */
#include "esp_log.h"
#include "ff.h"                                /* FatFs BYTE/WORD/DWORD */
#include "diskio_impl.h"                       /* ff_diskio_register: our heap-free FatFs sector driver */
#include "diskio_sdmmc.h"                      /* ff_diskio_get_pdrv_card: which FatFs drive wraps s_card */
#include "sd_bounce.h"                         /* the static spi_master bounce pool (crash fix) */

/* SPI bus shared with the other radio/NFC peripherals. */
#define SD_SPI_HOST     SPI3_HOST
#define SD_PIN_MOSI     34
#define SD_PIN_MISO     33
#define SD_PIN_SCK      35
#define SD_PIN_CS       21
/* The other devices' chip-select pins on this bus, parked high (deselected) below so a
 * floating CS can't let a second peripheral drive MISO and corrupt SD traffic. */
#define SD_PIN_NFC_CS   4
#define SD_PIN_LORA_CS  36

/* Conservative clock rate for the shared bus routing. */
#define SD_MAX_FREQ_KHZ 4000

static const char *TAG = "sdcard";

static sdmmc_card_t *s_card;
static bool s_bus_ready;
static bool s_sdspi_ready;

/* Guards app-side FAT access; created on first use inside nocsif_sdcard_init(). */
static SemaphoreHandle_t s_sd_lock;
/* Lock-hold telemetry (loudness pass): who holds the lock and since when, so a streamed play that finds
 * the card busy can name the holder, and a hold longer than SD_LOCK_LONG_MS is logged with its owner. */
#define SD_LOCK_LONG_MS 250
static const char *volatile s_sd_holder = "";
static int64_t              s_sd_held_since;
/* Sector-read telemetry: direct (internal buffer, multi-sector) vs staged (a sector at a time) calls,
 * sectors and time — read through nocsif_sdcard_read_stats so a play can report its own reads. */
static uint32_t s_rd_direct, s_rd_staged, s_rd_sectors, s_rd_us;

bool nocsif_sdcard_lock(uint32_t timeout_ms)
{
    if (s_sd_lock == NULL) {
        return false;   /* not created yet — init() hasn't run */
    }
    if (xSemaphoreTake(s_sd_lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return false;
    }
    s_sd_holder     = pcTaskGetName(NULL);
    s_sd_held_since = esp_timer_get_time();
    return true;
}

void nocsif_sdcard_unlock(void)
{
    if (s_sd_lock != NULL) {
        const uint32_t held_ms = (uint32_t)((esp_timer_get_time() - s_sd_held_since) / 1000);
        if (held_ms >= SD_LOCK_LONG_MS) {
            ESP_LOGW(TAG, "/sd lock held %u ms by %s", (unsigned)held_ms, s_sd_holder);
        }
        s_sd_holder = "";
        xSemaphoreGive(s_sd_lock);
    }
}

const char *nocsif_sdcard_lock_holder(void)
{
    return s_sd_holder;
}

void nocsif_sdcard_read_stats(uint32_t *direct_calls, uint32_t *staged_calls, uint32_t *sectors, uint32_t *us)
{
    if (direct_calls) *direct_calls = s_rd_direct;
    if (staged_calls) *staged_calls = s_rd_staged;
    if (sectors)      *sectors      = s_rd_sectors;
    if (us)           *us           = s_rd_us;
}

/* --- SD bounce-crash fix (sd_bounce.c; docs/LESSONS.md 2026-09-23) ----------------------------------
 * EVERY SD host command funnels through card->host.do_transaction — the app's FatFs I/O, the desktop
 * bridge, esp_tinyusb's File-Share MSC (it wraps this same sdmmc_card_t), and even sdmmc_card_init.
 * This interposer brackets the real sdspi transaction so that, for its duration, spi_master's per-
 * transfer DMA bounce buffers come from the static pool in sd_bounce.c instead of the starved int-DMA
 * heap — the allocation whose failure used to memcpy-from-NULL and reboot the watch. */
static esp_err_t sd_do_transaction(int slot, sdmmc_command_t *cmdinfo)
{
    if (!nocsif_sd_bounce_enter()) {
        return ESP_ERR_TIMEOUT;       /* previous command wedged in the SPI layer: fail this one, don't hang */
    }
    esp_err_t err = sdspi_host_do_transaction(slot, cmdinfo);
    nocsif_sd_bounce_exit();
    return err;
}

/* --- FatFs sector I/O without the heap ----------------------------------------------------------------
 * CONFIG_FATFS_SECTOR_4096 makes the FatFs volume context (FATFS window + max_files x FIL buffers) ~25 KB,
 * so it lives in PSRAM no matter what CONFIG_FATFS_ALLOC_PREFER_EXTRAM says (anything over the 4 KB
 * ALWAYSINTERNAL limit goes external). Every FatFs sector therefore reaches IDF's sdmmc_read/write_sectors
 * with a PSRAM buffer, and IDF stages it through a heap_caps_malloc(512, MALLOC_CAP_DMA) temp — NULL-checked
 * (so a graceful ESP_ERR_NO_MEM, "sdmmc_read_sectors: not enough mem"), but under BLE+WiFi starvation that
 * 512 B claim fails routinely, and then f_open / readdir / fread all fail. This driver replaces IDF's
 * diskio for the SD volume: a buffer that is internal + 4-aligned takes the direct multi-sector path (IDF
 * then needs no temp at all — e.g. OTA's aligned s_buf), anything else is staged one sector at a time
 * through ONE staging sector claimed once at init — the same sector-at-a-time algorithm IDF uses, minus
 * the per-call heap claim. The staging sector is never a DMA target itself (sdspi DMAs into its own
 * block_buf and memcpy's; a write from a non-DRAM buffer is copied through block_buf too), so it is taken
 * from the RTC-fast-memory heap (MALLOC_CAP_RTCRAM: 8 KB the int-DMA pool never sees), falling back to
 * internal DRAM only if that is unavailable. With the spi_master bounce served by sd_bounce.c, an SD
 * sector now needs ZERO int-DMA heap end to end. */
#define SD_DISK_TMP_BYTES 512
static uint8_t *s_disk_tmp;                        /* one sector; RTC-fast heap (preferred) or internal */

static void sd_disk_tmp_claim(void)
{
    if (s_disk_tmp != NULL) {
        return;
    }
    const char *where = "RTC-fast heap";
    s_disk_tmp = heap_caps_aligned_alloc(16, SD_DISK_TMP_BYTES, MALLOC_CAP_RTCRAM);
    if (s_disk_tmp == NULL) {
        where = "internal DRAM heap (RTC-fast unavailable)";
        s_disk_tmp = heap_caps_aligned_alloc(16, SD_DISK_TMP_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (s_disk_tmp == NULL) {
        ESP_LOGE(TAG, "FatFs staging sector: no memory — PSRAM-buffer sectors will use IDF's per-call temp");
        return;
    }
    ESP_LOGI(TAG, "FatFs staging sector: %u B in %s (rtc-fast free now %u B)", (unsigned)SD_DISK_TMP_BYTES,
             where, (unsigned)heap_caps_get_free_size(MALLOC_CAP_RTCRAM));
}

static bool sd_buf_direct_ok(const void *buf, size_t len)
{
    /* Same rule as sdmmc_read_sectors' fast path: internal, 4-aligned address and length. */
    return !esp_ptr_external_ram(buf) && (((uintptr_t)buf & 3u) == 0) && ((len & 3u) == 0);
}

static DSTATUS sd_disk_init(BYTE pdrv)
{
    (void)pdrv;
    return (s_card != NULL && sdmmc_get_status(s_card) == ESP_OK) ? 0 : STA_NOINIT;
}

static DSTATUS sd_disk_status(BYTE pdrv)
{
    (void)pdrv;
    return (s_card != NULL) ? 0 : STA_NOINIT;   /* IDF's default: no per-op status poll */
}

static DRESULT sd_disk_read(BYTE pdrv, BYTE *buff, DWORD sector, UINT count)
{
    (void)pdrv;
    if (s_card == NULL) {
        return RES_NOTRDY;
    }
    const size_t ss = s_card->csd.sector_size;
    esp_err_t err = ESP_OK;
    const int64_t t0 = esp_timer_get_time();
    if (s_disk_tmp == NULL || ss > SD_DISK_TMP_BYTES || sd_buf_direct_ok(buff, ss * count)) {
        err = sdmmc_read_sectors(s_card, buff, sector, count);           /* direct DMA path, no temp */
        s_rd_direct++;
    } else {
        for (UINT i = 0; i < count && err == ESP_OK; i++) {
            err = sdmmc_read_sectors(s_card, s_disk_tmp, sector + i, 1);  /* not PSRAM, aligned: direct */
            if (err == ESP_OK) {
                memcpy(buff + i * ss, s_disk_tmp, ss);
            }
        }
        s_rd_staged++;
    }
    s_rd_sectors += count;
    s_rd_us      += (uint32_t)(esp_timer_get_time() - t0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "disk read %lu+%u failed: %s", (unsigned long)sector, count, esp_err_to_name(err));
        return RES_ERROR;
    }
    return RES_OK;
}

static DRESULT sd_disk_write(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count)
{
    (void)pdrv;
    if (s_card == NULL) {
        return RES_NOTRDY;
    }
    const size_t ss = s_card->csd.sector_size;
    esp_err_t err = ESP_OK;
    if (s_disk_tmp == NULL || ss > SD_DISK_TMP_BYTES || sd_buf_direct_ok(buff, ss * count)) {
        err = sdmmc_write_sectors(s_card, buff, sector, count);
    } else {
        for (UINT i = 0; i < count && err == ESP_OK; i++) {
            memcpy(s_disk_tmp, buff + i * ss, ss);
            err = sdmmc_write_sectors(s_card, s_disk_tmp, sector + i, 1);
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "disk write %lu+%u failed: %s", (unsigned long)sector, count, esp_err_to_name(err));
        return RES_ERROR;
    }
    return RES_OK;
}

static DRESULT sd_disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    (void)pdrv;
    if (s_card == NULL) {
        return RES_NOTRDY;
    }
    switch (cmd) {                                   /* mirrors IDF's ff_sdmmc_ioctl */
    case CTRL_SYNC:        return RES_OK;
    case GET_SECTOR_COUNT: *((DWORD *)buff) = s_card->csd.capacity;    return RES_OK;
    case GET_SECTOR_SIZE:  *((WORD *)buff)  = s_card->csd.sector_size; return RES_OK;
    default:               return RES_ERROR;         /* GET_BLOCK_SIZE / TRIM: not supported, as in IDF */
    }
}

void nocsif_sdcard_diskio_attach(void)
{
    if (s_card == NULL) {
        return;
    }
    BYTE pdrv = ff_diskio_get_pdrv_card(s_card);      /* set by esp_tinyusb's mount (ff_diskio_register_sdmmc) */
    if (pdrv == 0xff) {
        ESP_LOGW(TAG, "diskio attach: card is not FAT-mounted for the app right now — nothing to route");
        return;
    }
    static const ff_diskio_impl_t impl = {
        .init   = sd_disk_init,
        .status = sd_disk_status,
        .read   = sd_disk_read,
        .write  = sd_disk_write,
        .ioctl  = sd_disk_ioctl,
    };
    ff_diskio_register(pdrv, &impl);                  /* replaces IDF's copy for this drive */
    ESP_LOGI(TAG, "FatFs drive %u: sector I/O routed through the heap-free staged path", (unsigned)pdrv);
}

/* Log what spi_master will actually bounce on SPI3, straight from the driver's own DMA context (the
 * numbers the fix is sized to; keep them in the boot log so a future IDF change is visible). */
static void log_spi3_bounce_rule(void)
{
    const spi_dma_ctx_t *dma = spi_bus_get_dma_ctx(SD_SPI_HOST);
    const spi_bus_attr_t *attr = spi_bus_get_attr(SD_SPI_HOST);
    if (dma == NULL || attr == NULL) {
        return;
    }
    ESP_LOGI(TAG, "SPI3 DMA bounce rule: tx_align=%u rx_align=%u cache_align_int=%u max_transfer=%d "
                  "(sdspi's 514 B last-block read bounces on rx_align -> served by the sd_bounce pool)",
             (unsigned)dma->dma_align_tx_int, (unsigned)dma->dma_align_rx_int,
             (unsigned)attr->cache_align_int, attr->max_transfer_sz);
}

/* Drive an unused shared-bus CS pin high (deselect) as a plain push-pull output. */
static void park_cs_high(int gpio)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(gpio, 1);
}

esp_err_t nocsif_sdcard_init(void)
{
    /* Create the access mutex here, before any other task that might call the lock exists. */
    if (s_sd_lock == NULL) {
        s_sd_lock = xSemaphoreCreateMutex();
    }
    if (s_card != NULL) {
        return ESP_OK;
    }

    /* Deselect the other devices on the shared bus before touching it. */
    park_cs_high(SD_PIN_NFC_CS);
    park_cs_high(SD_PIN_LORA_CS);

    esp_err_t err;
    if (!s_bus_ready) {
        const spi_bus_config_t bus_cfg = {
            .mosi_io_num = SD_PIN_MOSI,
            .miso_io_num = SD_PIN_MISO,
            .sclk_io_num = SD_PIN_SCK,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 4096,
        };
        err = spi_bus_initialize(SD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {  /* INVALID_STATE just means it's already up */
            ESP_LOGE(TAG, "spi_bus_initialize(SPI3) failed: %s", esp_err_to_name(err));
            return err;
        }
        s_bus_ready = true;
        ESP_LOGI(TAG, "SPI3 bus up (MOSI=%d MISO=%d SCK=%d), NFC/LoRa CS parked high",
                 SD_PIN_MOSI, SD_PIN_MISO, SD_PIN_SCK);
        log_spi3_bounce_rule();
    }

    if (!s_sdspi_ready) {
        err = sdspi_host_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "sdspi_host_init failed: %s", esp_err_to_name(err));
            return err;
        }
        s_sdspi_ready = true;
    }

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = SD_PIN_CS;
    slot_cfg.host_id = SD_SPI_HOST;
    sdspi_dev_handle_t dev;
    err = sdspi_host_init_device(&slot_cfg, &dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sdspi_host_init_device(CS=%d) failed: %s", SD_PIN_CS, esp_err_to_name(err));
        return err;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = dev;                       /* card init will talk over this device handle */
    host.max_freq_khz = SD_MAX_FREQ_KHZ;
    /* Install the bounce-pool interposer BEFORE card init: sdmmc_card_init copies this host struct into
     * the card (card->host = *host), so the card's own init commands, every later FatFs / bridge /
     * File-Share transfer, and any re-init all run through sd_do_transaction. */
    nocsif_sd_bounce_init();
    sd_disk_tmp_claim();
    host.do_transaction = sd_do_transaction;

    s_card = calloc(1, sizeof(sdmmc_card_t));
    if (s_card == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "initialising SD card (CS=%d, %d kHz)...", SD_PIN_CS, SD_MAX_FREQ_KHZ);
    err = sdmmc_card_init(&host, s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sdmmc_card_init failed: %s — check ALDO1 rail, wiring, card seated (FAT32)",
                 esp_err_to_name(err));
        free(s_card);
        s_card = NULL;
        return err;
    }

    const double mb = ((double)s_card->csd.capacity * s_card->csd.sector_size) / (1024.0 * 1024.0);
    ESP_LOGI(TAG, "SD card up (raw): %s, %d sectors x %d B = %.0f MB "
                  "(FAT owned by USB-MSC in gadget mode)",
             s_card->cid.name, s_card->csd.capacity, s_card->csd.sector_size, mb);
    return ESP_OK;
}

sdmmc_card_t *nocsif_sdcard_card(void)
{
    return s_card;
}

void nocsif_sdcard_list(const char *path)
{
    if (!nocsif_sdcard_lock(2000)) {
        ESP_LOGW(TAG, "list(%s): could not take /sd lock", path);
        return;
    }
    DIR *dir = opendir(path);
    if (dir == NULL) {
        ESP_LOGW(TAG, "opendir(%s) failed (is the volume mounted for the app?)", path);
        nocsif_sdcard_unlock();
        return;
    }
    ESP_LOGI(TAG, "%s listing:", path);
    struct dirent *e;
    int count = 0;
    while ((e = readdir(dir)) != NULL) {
        ESP_LOGI(TAG, "  %s%s", e->d_name, (e->d_type == DT_DIR) ? "/" : "");
        count++;
    }
    closedir(dir);
    ESP_LOGI(TAG, "%s: %d entries", path, count);
    nocsif_sdcard_unlock();
}
