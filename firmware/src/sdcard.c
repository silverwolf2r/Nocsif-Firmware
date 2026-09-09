/*
 * NocSif — microSD card driver implementation. See sdcard.h.
 *
 * Runs the same sdspi_host_init -> sdspi_host_init_device -> sdmmc_card_init sequence
 * that a full FAT mount would use internally, but stops short of mounting FAT — that's
 * left to usb_gadget.c, which hands this raw card handle to the USB mass-storage helper.
 */
#include "sdcard.h"

#include <stdlib.h>
#include <dirent.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"

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

bool nocsif_sdcard_lock(uint32_t timeout_ms)
{
    if (s_sd_lock == NULL) {
        return false;   /* not created yet — init() hasn't run */
    }
    return xSemaphoreTake(s_sd_lock, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void nocsif_sdcard_unlock(void)
{
    if (s_sd_lock != NULL) {
        xSemaphoreGive(s_sd_lock);
    }
}

/* Configure a shared-bus chip-select pin as output and drive it high (deselected). */
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
