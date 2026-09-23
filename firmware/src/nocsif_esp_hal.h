/*
 * NocSif — RadioLib hardware-abstraction layer for the SX1262 LoRa radio.
 *
 * RadioLib needs a HAL implementation to talk to the platform. Rather than use its stock
 * ESP32 example (which bit-bangs a bus our display already owns), this HAL rides the normal
 * spi_master driver on the SPI3 bus shared with the microSD/NFC, leaving chip-select toggling
 * to RadioLib itself. Header-only; included from lora.cpp only.
 */
#pragma once

#include <RadioLib.h>

#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/spi_common.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

/* Pin-mode/edge constants RadioLibHal expects the platform HAL to define. */
#define NOCSIF_HAL_LOW      0x0
#define NOCSIF_HAL_HIGH     0x1
#define NOCSIF_HAL_INPUT    0x01
#define NOCSIF_HAL_OUTPUT   0x03
#define NOCSIF_HAL_RISING   0x01
#define NOCSIF_HAL_FALLING  0x02

class NocsifEspHal : public RadioLibHal {
  public:
    NocsifEspHal(spi_host_device_t host, int sck, int miso, int mosi, uint32_t hz)
      : RadioLibHal(NOCSIF_HAL_INPUT, NOCSIF_HAL_OUTPUT, NOCSIF_HAL_LOW, NOCSIF_HAL_HIGH,
                    NOCSIF_HAL_RISING, NOCSIF_HAL_FALLING),
        _host(host), _sck(sck), _miso(miso), _mosi(mosi), _hz(hz) {}

    void init() override { spiBegin(); }
    void term() override { spiEnd(); }

    void pinMode(uint32_t pin, uint32_t mode) override {
        if (pin == RADIOLIB_NC) return;
        gpio_config_t c = {};
        c.pin_bit_mask = 1ULL << pin;
        c.mode = (mode == NOCSIF_HAL_OUTPUT) ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT;
        c.pull_up_en = GPIO_PULLUP_DISABLE;
        c.pull_down_en = GPIO_PULLDOWN_DISABLE;
        c.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&c);
    }

    void digitalWrite(uint32_t pin, uint32_t value) override {
        if (pin == RADIOLIB_NC) return;
        gpio_set_level((gpio_num_t)pin, value);
    }

    uint32_t digitalRead(uint32_t pin) override {
        if (pin == RADIOLIB_NC) return 0;
        return (uint32_t)gpio_get_level((gpio_num_t)pin);
    }

    void attachInterrupt(uint32_t interruptNum, void (*cb)(void), uint32_t mode) override {
        if (interruptNum == RADIOLIB_NC) return;
        /* Safe to call more than once — returns INVALID_STATE if already installed elsewhere. */
        gpio_install_isr_service(0);
        gpio_int_type_t t = (mode == NOCSIF_HAL_RISING)  ? GPIO_INTR_POSEDGE
                          : (mode == NOCSIF_HAL_FALLING) ? GPIO_INTR_NEGEDGE
                                                         : GPIO_INTR_ANYEDGE;
        gpio_set_intr_type((gpio_num_t)interruptNum, t);
        gpio_isr_handler_add((gpio_num_t)interruptNum, (gpio_isr_t)cb, nullptr);
        gpio_intr_enable((gpio_num_t)interruptNum);
    }

    void detachInterrupt(uint32_t interruptNum) override {
        if (interruptNum == RADIOLIB_NC) return;
        gpio_isr_handler_remove((gpio_num_t)interruptNum);
        gpio_set_intr_type((gpio_num_t)interruptNum, GPIO_INTR_DISABLE);
    }

    void delay(RadioLibTime_t ms) override {
        if (ms == 0) { taskYIELD(); return; }
        vTaskDelay(pdMS_TO_TICKS(ms));
    }

    void delayMicroseconds(RadioLibTime_t us) override {
        if (us) esp_rom_delay_us(us);
    }

    RadioLibTime_t millis() override {
        return (RadioLibTime_t)(esp_timer_get_time() / 1000ULL);
    }

    RadioLibTime_t micros() override {
        return (RadioLibTime_t)esp_timer_get_time();
    }

    long pulseIn(uint32_t pin, uint32_t state, RadioLibTime_t timeout) override {
        if (pin == RADIOLIB_NC) return 0;
        RadioLibTime_t start = micros();
        while (digitalRead(pin) == state) {
            if ((micros() - start) > timeout) return 0;
        }
        return (long)(micros() - start);
    }

    void spiBegin() override {
        if (_dev) return;
        spi_bus_config_t bus = {};
        bus.mosi_io_num = _mosi;
        bus.miso_io_num = _miso;
        bus.sclk_io_num = _sck;
        bus.quadwp_io_num = -1;
        bus.quadhd_io_num = -1;
        bus.max_transfer_sz = 4096;
        esp_err_t e = spi_bus_initialize(_host, &bus, SPI_DMA_CH_AUTO);
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {   /* INVALID_STATE just means it's already up */
            ESP_LOGE("lora.hal", "spi_bus_initialize failed: %s", esp_err_to_name(e));
            return;
        }
        spi_device_interface_config_t dev = {};
        dev.clock_speed_hz = _hz;
        dev.mode = 0;                 /* SX1262 uses SPI mode 0 */
        dev.spics_io_num = -1;        /* let RadioLib toggle CS itself */
        dev.queue_size = 4;
        e = spi_bus_add_device(_host, &dev, &_dev);
        if (e != ESP_OK) {
            ESP_LOGE("lora.hal", "spi_bus_add_device failed: %s", esp_err_to_name(e));
            _dev = nullptr;
        }
        /* Guaranteed internal DMA-capable SPI buffers (see the _txb/_rxb note) — alloc once, kept across
         * bring-ups until spiEnd. 260 B fits even a heavily fragmented int-DMA pool. */
        if (!_txb) _txb = (uint8_t *)heap_caps_malloc(SPIBUF, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (!_rxb) _rxb = (uint8_t *)heap_caps_malloc(SPIBUF, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (!_txb || !_rxb) ESP_LOGE("lora.hal", "SPI DMA buffer alloc failed (int-DMA starved)");
    }

    void spiBeginTransaction() override {}

    /* Copy through fixed internal DMA-capable buffers, since RadioLib may pass a pointer
     * (e.g. into flash) that the SPI driver's DMA can't read directly. */
    void spiTransfer(uint8_t *out, size_t len, uint8_t *in) override {
        if (!_dev || len == 0) return;
        if (!_txb || !_rxb || len > SPIBUF) {
            /* No internal DMA buffer (alloc failed) or an unexpectedly large frame: DON'T attempt a
             * transfer from a possibly-PSRAM pointer (a spi_master DMA bounce alloc can panic under
             * int-DMA starvation) — hand RadioLib a benign zeroed read instead. */
            if (in) std::memset(in, 0, len);
            return;
        }
        if (out) std::memcpy(_txb, out, len); else std::memset(_txb, 0, len);
        spi_transaction_t t = {};
        t.length   = len * 8;
        t.tx_buffer = _txb;
        t.rxlength = len * 8;
        t.rx_buffer = _rxb;
        if (spi_device_polling_transmit(_dev, &t) == ESP_OK) {
            if (in) std::memcpy(in, _rxb, len);
        } else if (in) {
            std::memset(in, 0, len);
        }
    }

    void spiEndTransaction() override {}

    void spiEnd() override {
        if (_dev) { spi_bus_remove_device(_dev); _dev = nullptr; }
        if (_txb) { heap_caps_free(_txb); _txb = nullptr; }
        if (_rxb) { heap_caps_free(_rxb); _rxb = nullptr; }
    }

    /* RadioLib calls this in its blocking wait loops; yield to keep the watchdog fed. */
    void yield() override { taskYIELD(); }

  private:
    spi_host_device_t _host;
    int _sck, _miso, _mosi;
    uint32_t _hz;
    spi_device_handle_t _dev = nullptr;
    static constexpr size_t SPIBUF = 260;   /* SX126x max frame ~258 B */
    /* SPI buffers MUST be internal DMA-capable. The NocsifEspHal object is new'd (and the LoRa worker
     * stack lives in PSRAM), so members here can land in PSRAM — and a PSRAM tx/rx pointer makes the
     * spi_master allocate a DMA bounce buffer PER TRANSFER, which fails and PANICS the LoRa task once
     * BLE + WiFi have eaten the contiguous int-DMA pool. These are allocated internal-DMA once in
     * spiBegin (freed in spiEnd), so no runtime bounce is ever needed. */
    uint8_t *_txb = nullptr;
    uint8_t *_rxb = nullptr;
};
