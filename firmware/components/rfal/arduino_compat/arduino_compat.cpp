/*
 * NocSif — Arduino-compatibility shim implementation over ESP-IDF.
 * See Arduino.h / SPI.h / Wire.h for the rationale.  This is the ONLY place the
 * vendored ST25R3916 / RFAL library touches the SoC; everything else is portable C++.
 */
#include "Arduino.h"
#include "SPI.h"

#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"          /* esp_rom_delay_us */
#include "esp_heap_caps.h"        /* DMA-capable bounce buffer */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ---- GPIO ---------------------------------------------------------------------- */
extern "C" void pinMode(int pin, int mode)
{
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << pin;
    cfg.intr_type    = GPIO_INTR_DISABLE;
    switch (mode) {
        case OUTPUT:
            cfg.mode = GPIO_MODE_OUTPUT;
            break;
        case INPUT_PULLUP:
            cfg.mode = GPIO_MODE_INPUT;
            cfg.pull_up_en = GPIO_PULLUP_ENABLE;
            break;
        case INPUT_PULLDOWN:
            cfg.mode = GPIO_MODE_INPUT;
            cfg.pull_down_en = GPIO_PULLDOWN_ENABLE;
            break;
        case INPUT:
        default:
            cfg.mode = GPIO_MODE_INPUT;
            break;
    }
    gpio_config(&cfg);
}

extern "C" void digitalWrite(int pin, int level)
{
    gpio_set_level((gpio_num_t)pin, level ? 1 : 0);
}

extern "C" int digitalRead(int pin)
{
    return gpio_get_level((gpio_num_t)pin) ? HIGH : LOW;
}

/* ---- timing -------------------------------------------------------------------- */
extern "C" uint32_t millis(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

extern "C" uint32_t micros(void)
{
    return (uint32_t)esp_timer_get_time();
}

extern "C" void delay(uint32_t ms)
{
    if (ms == 0) {
        taskYIELD();
        return;
    }
    /* Short delays (< 10 ms) are settle/timing-critical during chip bring-up and would
     * round to 0 ticks under vTaskDelay at a coarse tick rate, so busy-wait them exactly.
     * The nfc worker is a low-priority task, so a few ms of busy-wait is harmless. Longer
     * delays sleep cooperatively (+1 tick guards against sub-tick truncation). */
    if (ms < 10) {
        esp_rom_delay_us(ms * 1000U);
    } else {
        vTaskDelay(pdMS_TO_TICKS(ms) + 1);
    }
}

extern "C" void delayMicroseconds(uint32_t us)
{
    esp_rom_delay_us(us);
}

extern "C" void yield(void)
{
    taskYIELD();
}

/* ---- interrupts (no-op: this fork polls the IRQ line — see Arduino.h) ----------- */
extern "C" void attachInterrupt(int pin, void (*isr)(void), int mode)
{
    (void)pin; (void)isr; (void)mode;
}

extern "C" void detachInterrupt(int pin)
{
    (void)pin;
}

/* ---- SPI (esp-idf spi_device transport) ---------------------------------------- */
void SPIClass::beginTransaction(SPISettings settings)
{
    (void)settings;   /* clock/mode are fixed on the esp-idf device (nfc.cpp) */
    if (_dev != nullptr) {
        /* Hold the shared SPI3 bus for the whole CS-asserted window so an SD transaction
         * on the same bus cannot interleave between the command byte and the data. */
        spi_device_acquire_bus(_dev, portMAX_DELAY);
    }
}

void SPIClass::endTransaction()
{
    if (_dev != nullptr) {
        spi_device_release_bus(_dev);
    }
}

uint8_t SPIClass::transfer(uint8_t data)
{
    if (_dev == nullptr) {
        return 0;
    }
    spi_transaction_t t = {};
    t.flags     = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;   /* no DMA buffer for 1 byte */
    t.length    = 8;
    t.tx_data[0] = data;
    if (spi_device_polling_transmit(_dev, &t) != ESP_OK) {
        return 0;
    }
    return t.rx_data[0];
}

/* The shared SPI3 bus runs with DMA (SPI_DMA_CH_AUTO), so a block transfer's buffer must be
 * DMA-capable AND word-aligned. RFAL hands us small *stack* buffers (often unaligned), which
 * the DMA path rejects — the vendor never hit this because their ESP32-S3 default is bit-banged
 * soft-SPI. So route every block transfer through a DMA-capable, 4-byte-aligned bounce buffer
 * (ST25R3916 FIFO depth is 512; +8 covers DMA rx round-up). NFC runs single-threaded on the
 * worker, so one static bounce is race-free. */
#define NOCSIF_SPI_BOUNCE_SZ 520
static uint8_t *s_spi_bounce;

void SPIClass::transfer(void *buf, size_t count)
{
    if (_dev == nullptr || buf == nullptr || count == 0) {
        return;
    }
    if (s_spi_bounce == nullptr) {
        s_spi_bounce = (uint8_t *)heap_caps_aligned_alloc(
            4, NOCSIF_SPI_BOUNCE_SZ, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (s_spi_bounce == nullptr) {
            return;   /* out of DMA memory — leave buf untouched (read returns stale) */
        }
    }
    size_t off = 0;
    while (off < count) {
        size_t chunk = count - off;
        if (chunk > NOCSIF_SPI_BOUNCE_SZ) {
            chunk = NOCSIF_SPI_BOUNCE_SZ;   /* defensive: RFAL never exceeds 512 in one call */
        }
        memcpy(s_spi_bounce, (uint8_t *)buf + off, chunk);
        spi_transaction_t t = {};
        t.length    = chunk * 8;
        t.tx_buffer = s_spi_bounce;
        t.rx_buffer = s_spi_bounce;   /* full-duplex, in-place — Arduino transfer() semantics */
        if (spi_device_polling_transmit(_dev, &t) == ESP_OK) {
            memcpy((uint8_t *)buf + off, s_spi_bounce, chunk);
        }
        off += chunk;
    }
}
