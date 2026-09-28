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
#include "esp_system.h"                        /* esp_reset_reason: power-cycle the card first after a crash */
#include "esp_rom_gpio.h"                      /* esp_rom_gpio_connect_out_signal: hand SCK/MOSI back to SPI3 */
#include "esp_rom_sys.h"                       /* esp_rom_delay_us */
#include "soc/spi_periph.h"                    /* spi_periph_signal: SPI3's SCK/MOSI matrix signals */
#include "ff.h"                                /* FatFs BYTE/WORD/DWORD */
#include "diskio_impl.h"                       /* ff_diskio_register: our heap-free FatFs sector driver */
#include "diskio_sdmmc.h"                      /* ff_diskio_get_pdrv_card: which FatFs drive wraps s_card */
#include "sd_bounce.h"                         /* the static spi_master bounce pool (crash fix) */
#include "power.h"                              /* nocsif_power_sd_rail: power-cycle ALDO1 on a retry / re-probe */
#include "xl9555.h"                             /* the socket's card-detect switch (XL9555_IO_SD_DETECT) */

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

/* SPI clock after init (the card is probed at 400 kHz first). It was 4 MHz ("shared routing unverified
 * above this"), which caps File Share at ~400 KB/s — and Windows reads the whole FAT (~8 MB on this 64 GB
 * card) when it mounts the drive. 20 MHz (the SD default-speed maximum in SPI mode) is verified on-device
 * with hash-checked bulk reads and writes. */
#define SD_MAX_FREQ_KHZ 20000

/* A single sdmmc_card_init miss used to lose the card for the whole session (one-shot init, no retry) —
 * a marginal contact, or a card left wedged by a WARM reset (the CPU resets but ALDO1 keeps the card
 * powered, so CMD0 alone may not clear it). It now gets up to this many attempts, each retry after a real
 * card power-cycle (sd_power_cycle) — a firmware stand-in for the physical reseat that used to be needed. */
#define SD_INIT_ATTEMPTS  2
/* Wall-clock budget for ONE sdmmc_card_init, enforced in sd_do_transaction. A card that answers CMD0/CMD8
 * but never leaves the idle state keeps IDF's ACMD41 loop polling 300 times — ~27 s per attempt measured,
 * so three attempts held the /sd lock for 82 s. The SD spec gives ACMD41 1 s; a healthy card here is up in
 * ~0.85 s. Past the budget every command fails fast and the init unwinds within milliseconds. */
#define SD_INIT_BUDGET_MS 2000
#define SD_RAIL_OFF_MS    500      /* ALDO1 off with every SD line LOW: long enough for the card's VDD to collapse */
#define SD_RAIL_RAMP_MS   10       /* ALDO1 back on: let VDD ramp before CS/MOSI are driven high again */
#define SD_RAIL_ON_MS     90       /* then settle before the first command */
#define SD_BRINGUP_LOCK_MS 3000    /* wait for the /sd lock before an init attempt */

static const char *TAG = "sdcard";

/* s_card is allocated on the first successful init and never moves or frees after that: the File-Share
 * MSC storage keeps this pointer, so a card that is pulled and re-inserted is re-initialised IN PLACE.
 * s_card_ok says whether the card in the slot is initialised and usable right now. */
static sdmmc_card_t *s_card;
static volatile bool s_card_ok;
static bool s_bus_ready;
static bool s_sdspi_ready;
static bool s_host_ready;                  /* the sdspi device + host struct are set up (reused across re-probes) */
static sdspi_dev_handle_t s_dev;           /* the shared-bus sdspi device handle */
static sdmmc_host_t s_host;                /* card-init host config (do_transaction bounce interposer installed) */
static volatile int64_t s_init_deadline_us; /* != 0 while a budgeted card init runs (SD_INIT_BUDGET_MS) */
static volatile bool s_init_over_budget;    /* the running init hit that budget */

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
 * heap — the allocation whose failure used to memcpy-from-NULL and reboot the watch. It also enforces the
 * card-init time budget: IDF's init has no overall timeout, so once SD_INIT_BUDGET_MS has passed every
 * command fails here without touching the bus and sdmmc_card_init returns within a few calls. */
static esp_err_t sd_do_transaction(int slot, sdmmc_command_t *cmdinfo)
{
    if (s_init_deadline_us != 0 && esp_timer_get_time() >= s_init_deadline_us) {
        s_init_over_budget = true;
        return ESP_ERR_TIMEOUT;
    }
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
    return (s_card_ok && sdmmc_get_status(s_card) == ESP_OK) ? 0 : STA_NOINIT;
}

static DSTATUS sd_disk_status(BYTE pdrv)
{
    (void)pdrv;
    return s_card_ok ? 0 : STA_NOINIT;          /* IDF's default: no per-op status poll */
}

static DRESULT sd_disk_read(BYTE pdrv, BYTE *buff, DWORD sector, UINT count)
{
    (void)pdrv;
    if (!s_card_ok) {
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
    if (!s_card_ok) {
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
    if (!s_card_ok) {
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
    if (!s_card_ok) {
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

/* A REAL microSD power-cycle — the firmware stand-in for a physical reseat: resets a card that a warm
 * reset left wedged, or that never answered on a marginal contact. Switching ALDO1 off alone does NOT
 * depower the card: CS (held high between commands) and MOSI keep feeding it through its I/O protection
 * diodes, so it sags instead of resetting (measured: a bare 120 ms ALDO1 blip never revived a wedged
 * card). So for the off window every SD line is driven LOW — CS is the plain GPIO sdspi already drives,
 * SCK/MOSI are taken off SPI3 as GPIO outputs, MISO's pull-up becomes a pull-down — then everything is
 * restored the way spi_bus_initialize routed it. SCK/MOSI are shared with LoRa/NFC: the caller holds the
 * /sd lock (every LoRa/NFC bus op takes it), and their CS stay high, so a low SCK/MOSI is invisible to
 * them. Costs ~600 ms. */
static void sd_power_cycle(void)
{
    ESP_LOGW(TAG, "SD power-cycle: SD lines low + ALDO1 off %d ms, then on + %d ms settle",
             SD_RAIL_OFF_MS, SD_RAIL_RAMP_MS + SD_RAIL_ON_MS);
    gpio_set_level(SD_PIN_CS, 0);
    gpio_set_level(SD_PIN_SCK, 0);
    gpio_set_level(SD_PIN_MOSI, 0);
    gpio_set_direction(SD_PIN_SCK, GPIO_MODE_OUTPUT);     /* routes the pin to its GPIO out (low), off SPI3 */
    gpio_set_direction(SD_PIN_MOSI, GPIO_MODE_OUTPUT);
    gpio_pullup_dis(SD_PIN_MISO);
    gpio_pulldown_en(SD_PIN_MISO);
    nocsif_power_sd_rail(false);
    vTaskDelay(pdMS_TO_TICKS(SD_RAIL_OFF_MS));
    nocsif_power_sd_rail(true);
    vTaskDelay(pdMS_TO_TICKS(SD_RAIL_RAMP_MS));
    /* Restore — the same calls spi_bus_initialize made for GPIO-matrix pins. */
    gpio_pulldown_dis(SD_PIN_MISO);
    gpio_pullup_en(SD_PIN_MISO);
    gpio_set_direction(SD_PIN_MOSI, GPIO_MODE_INPUT_OUTPUT);
    esp_rom_gpio_connect_out_signal(SD_PIN_MOSI, spi_periph_signal[SD_SPI_HOST].spid_out, false, false);
    gpio_set_direction(SD_PIN_SCK, GPIO_MODE_INPUT_OUTPUT);
    esp_rom_gpio_connect_out_signal(SD_PIN_SCK, spi_periph_signal[SD_SPI_HOST].spiclk_out, false, false);
    gpio_set_level(SD_PIN_CS, 1);                         /* deselected, as sdspi leaves it between commands */
    vTaskDelay(pdMS_TO_TICKS(SD_RAIL_ON_MS));
}

/* Bring up the shared SPI3 bus, the sdspi host, and our sdspi device + host struct (with the bounce-pool
 * do_transaction interposer). Idempotent — each stage is guarded so a re-probe reuses what boot built.
 * The neighbour CS pins are parked HIGH only at the FIRST bus init: at runtime LoRa/NFC own their CS via
 * spi_master and re-driving those GPIOs would fight the driver. */
static esp_err_t sd_ensure_bus_and_device(void)
{
    esp_err_t err;
    if (!s_bus_ready) {
        /* Deselect the other devices on the shared bus before touching it (once, at first init). */
        park_cs_high(SD_PIN_NFC_CS);
        park_cs_high(SD_PIN_LORA_CS);
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
        /* MISO pull-up. Before EVERY command sdspi polls MISO with the card deselected and waits up to
         * 40 ms for it to read high — "should not be needed if correct pull-up resistors are used"
         * (sdspi_host.h). A deselected MISO with no pull-up reads low, so each command pays the full
         * 40 ms. The level readings below show whether this board needs it. */
        const int miso_bare = gpio_get_level(SD_PIN_MISO);
        gpio_pullup_en(SD_PIN_MISO);
        esp_rom_delay_us(20);
        ESP_LOGI(TAG, "MISO idle level (all CS high): %d bare, %d with the internal pull-up",
                 miso_bare, gpio_get_level(SD_PIN_MISO));
    }
    if (!s_sdspi_ready) {
        err = sdspi_host_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "sdspi_host_init failed: %s", esp_err_to_name(err));
            return err;
        }
        s_sdspi_ready = true;
    }
    if (!s_host_ready) {
        sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
        slot_cfg.gpio_cs = SD_PIN_CS;
        slot_cfg.host_id = SD_SPI_HOST;
        err = sdspi_host_init_device(&slot_cfg, &s_dev);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "sdspi_host_init_device(CS=%d) failed: %s", SD_PIN_CS, esp_err_to_name(err));
            return err;
        }
        sdmmc_host_t host = SDSPI_HOST_DEFAULT();   /* macro is a brace-initializer; use init context */
        host.slot = s_dev;                          /* card init talks over this sdspi device handle */
        host.max_freq_khz = SD_MAX_FREQ_KHZ;
        /* Install the bounce-pool interposer + heap-free staging BEFORE card init: sdmmc_card_init copies
         * this host into the card, so the card's own init commands and every later FatFs / bridge /
         * File-Share transfer run through sd_do_transaction. */
        nocsif_sd_bounce_init();
        sd_disk_tmp_claim();
        host.do_transaction = sd_do_transaction;
        s_host = host;                              /* struct copy (assignment of a struct is valid C) */
        s_host_ready = true;
    }
    return ESP_OK;
}

/* One sdmmc_card_init under the SD_INIT_BUDGET_MS deadline (enforced in sd_do_transaction). */
static esp_err_t sd_card_init_budgeted(sdmmc_card_t *card, int attempt, bool cycled)
{
    ESP_LOGI(TAG, "initialising SD card (CS=%d, %d kHz), attempt %d/%d%s...",
             SD_PIN_CS, SD_MAX_FREQ_KHZ, attempt, SD_INIT_ATTEMPTS, cycled ? " (power-cycled)" : "");
    const int64_t t0 = esp_timer_get_time();
    s_init_over_budget = false;
    s_init_deadline_us = t0 + (int64_t)SD_INIT_BUDGET_MS * 1000;
    esp_err_t err = sdmmc_card_init(&s_host, card);
    s_init_deadline_us = 0;
    const unsigned ms = (unsigned)((esp_timer_get_time() - t0) / 1000);
    if (err == ESP_OK) {
        const double mb = ((double)card->csd.capacity * card->csd.sector_size) / (1024.0 * 1024.0);
        ESP_LOGI(TAG, "SD card up (raw) in %u ms: %s, %d sectors x %d B = %.0f MB (FAT owned by USB-MSC in "
                      "gadget mode)%s", ms, card->cid.name, card->csd.capacity, card->csd.sector_size,
                 mb, attempt > 1 ? " [recovered on retry]" : "");
    } else {
        ESP_LOGE(TAG, "sdmmc_card_init attempt %d/%d failed after %u ms: %s%s", attempt, SD_INIT_ATTEMPTS, ms,
                 esp_err_to_name(err), s_init_over_budget ? " (stopped at the init time budget)" : "");
    }
    return err;
}

/* Try to bring the raw card up over the ready sdspi device, retrying after a real power-cycle. Each
 * attempt (power-cycle + budgeted init, <= ~2.6 s) runs under the /sd lock — LoRa/NFC share SPI3 and
 * sd_power_cycle drives its lines — and the lock is released between attempts. The first card is built in
 * a private struct and published only once init succeeded; a re-inserted card is re-initialised into the
 * existing s_card (its address is shared with the MSC storage) while s_card_ok is false, so nothing reads
 * a half-initialised card. cycle_first power-cycles before even the first attempt (a re-probe, or a boot
 * after a crash). */
static esp_err_t sd_bringup_card(bool cycle_first)
{
    const bool fresh = (s_card == NULL);
    sdmmc_card_t *card = fresh ? calloc(1, sizeof(sdmmc_card_t)) : s_card;
    if (card == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= SD_INIT_ATTEMPTS; attempt++) {
        if (!nocsif_sdcard_lock(SD_BRINGUP_LOCK_MS)) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        if (s_card_ok) {                                  /* a concurrent re-probe already brought it up */
            nocsif_sdcard_unlock();
            if (fresh) free(card);
            return ESP_OK;
        }
        const bool cycle = (attempt > 1) || cycle_first;
        if (cycle) {
            sd_power_cycle();
        }
        err = sd_card_init_budgeted(card, attempt, cycle);
        if (err == ESP_OK) {
            s_card    = card;
            s_card_ok = true;
        }
        nocsif_sdcard_unlock();
        if (err == ESP_OK) {
            return ESP_OK;
        }
    }
    ESP_LOGE(TAG, "SD card not detected after %d attempts — check ALDO1 rail, wiring, card seated (FAT32)",
             SD_INIT_ATTEMPTS);
    if (fresh) free(card);
    return err;
}

/* A crash reset keeps ALDO1 up and can leave the card wedged mid-transaction — power-cycle it first. */
static bool sd_boot_after_crash(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_BROWNOUT:
        return true;
    default:
        return false;
    }
}

esp_err_t nocsif_sdcard_init(void)
{
    /* Create the /sd access mutex once, here at boot (single-threaded) — before the USB-MSC
     * monitor and the DuckyScript worker tasks exist, so nocsif_sdcard_lock() never has to. */
    if (s_sd_lock == NULL) {
        s_sd_lock = xSemaphoreCreateMutex();
    }
    if (s_card_ok) {
        return ESP_OK;
    }
    esp_err_t err = sd_ensure_bus_and_device();
    if (err != ESP_OK) {
        return err;
    }
    /* Card-detect first: an empty slot costs nothing (no init, no retry, no power-cycle) and the USB worker
     * brings a card up the moment one is inserted. The detect line was an output in older builds and the
     * expander keeps its config across a warm reset, so make it an input here. */
    nocsif_xl9555_set_input(XL9555_IO_SD_DETECT);
    const int det = nocsif_sdcard_detect();
    ESP_LOGI(TAG, "card-detect (XL9555 IO%d): %s", XL9555_IO_SD_DETECT,
             det > 0 ? "card inserted" : det == 0 ? "slot EMPTY" : "unreadable (probing anyway)");
    if (det == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    /* main.c powered ALDO1 + settled it, so attempt 1 normally uses the rail as-is (the proven fast path)
     * and only the retry power-cycles; after a crash reset the card is power-cycled first. */
    const bool crashed = sd_boot_after_crash();
    if (crashed) {
        ESP_LOGW(TAG, "boot after a crash reset: power-cycling the microSD before init");
    }
    return sd_bringup_card(crashed);
}

esp_err_t nocsif_sdcard_reprobe(void)
{
    if (s_sd_lock == NULL) {
        return ESP_ERR_INVALID_STATE;      /* nocsif_sdcard_init never ran */
    }
    if (s_card_ok) {
        return ESP_OK;                     /* a card is already up — never disturb it (keeps s_msc valid) */
    }
    if (!nocsif_sdcard_lock(SD_BRINGUP_LOCK_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = sd_ensure_bus_and_device();
    nocsif_sdcard_unlock();
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGW(TAG, "SD re-probe: no card up — power-cycling the card and retrying init");
    return sd_bringup_card(true);          /* recovering: power-cycle before the first attempt */
}

int nocsif_sdcard_detect(void)
{
    bool level;
    if (nocsif_xl9555_get_level(XL9555_IO_SD_DETECT, &level) != ESP_OK) {
        return -1;
    }
    return level ? 0 : 1;                  /* the socket switch pulls the line LOW with a card in */
}

void nocsif_sdcard_mark_removed(void)
{
    if (!s_card_ok) {
        return;
    }
    const bool locked = nocsif_sdcard_lock(2000);   /* let an in-flight app transfer finish first */
    s_card_ok = false;
    if (locked) {
        nocsif_sdcard_unlock();
    }
    ESP_LOGW(TAG, "microSD removed — card marked absent (re-inserting brings it back, no reboot)");
}

sdmmc_card_t *nocsif_sdcard_card(void)
{
    return s_card_ok ? s_card : NULL;
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
