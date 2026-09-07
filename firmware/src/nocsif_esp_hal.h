/*
 * NocSif — RadioLib hardware-abstraction layer for the ESP32-S3 (M9 LoRa).
 *
 * RadioLib's shipped EspHal example is ESP32-only (register-bangs SPI2) and would collide with
 * our display (SPI2) + SD (SPI3) drivers. This HAL instead uses the high-level esp_driver_spi
 * (spi_master) API on the SHARED SPI3 bus, exactly like the SX1262 proof-of-life did — so LoRa
 * coexists with the microSD (+ NFC) under nocsif_sdcard_lock (held by lora.cpp around each radio
 * operation, not here). CS is left to RadioLib (spics_io_num = -1; RadioLib toggles it via
 * digitalWrite), matching how the RFAL/NFC shim drives a manual CS on the same bus.
 *
 * Header-only; included ONLY by lora.cpp (C++). Not for the C sources.
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

/* Platform-specific values handed to the RadioLibHal base (see EspHal.h reference). */
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
        /* Idempotent: returns INVALID_STATE if another driver already installed the ISR service. */
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
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {   /* INVALID_STATE = SD already brought it up */
            ESP_LOGE("lora.hal", "spi_bus_initialize failed: %s", esp_err_to_name(e));
            return;
        }
        spi_device_interface_config_t dev = {};
        dev.clock_speed_hz = _hz;
        dev.mode = 0;                 /* SX126x = SPI mode 0 */
        dev.spics_io_num = -1;        /* RadioLib drives CS via digitalWrite */
        dev.queue_size = 4;
        e = spi_bus_add_device(_host, &dev, &_dev);
        if (e != ESP_OK) {
            ESP_LOGE("lora.hal", "spi_bus_add_device failed: %s", esp_err_to_name(e));
            _dev = nullptr;
        }
    }

    void spiBeginTransaction() override {}

    /* Bounce through internal (DMA-capable) buffers: RadioLib may hand us a flash-resident tx
     * pointer (a const payload) or a >64 B payload — both would break a direct spi_master DMA. */
    void spiTransfer(uint8_t *out, size_t len, uint8_t *in) override {
        if (!_dev || len == 0) return;
        if (len <= sizeof _txb) {
            if (out) std::memcpy(_txb, out, len); else std::memset(_txb, 0, len);
            spi_transaction_t t = {};
            t.length = len * 8;
            t.tx_buffer = _txb;
            t.rxlength = len * 8;
            t.rx_buffer = _rxb;
            spi_device_polling_transmit(_dev, &t);
            if (in) std::memcpy(in, _rxb, len);
        } else {
            /* Not expected for SX126x (max ~258 B); direct path as a fallback. */
            spi_transaction_t t = {};
            t.length = len * 8;
            t.tx_buffer = out;
            t.rxlength = len * 8;
            t.rx_buffer = in;
            spi_device_polling_transmit(_dev, &t);
        }
    }

    void spiEndTransaction() override {}

    void spiEnd() override {
        if (_dev) { spi_bus_remove_device(_dev); _dev = nullptr; }
    }

    /* Called from RadioLib's blocking wait loops — yield so the idle task (task-WDT) is fed. */
    void yield() override { taskYIELD(); }

  private:
    spi_host_device_t _host;
    int _sck, _miso, _mosi;
    uint32_t _hz;
    spi_device_handle_t _dev = nullptr;
    uint8_t _txb[260];
    uint8_t _rxb[260];
};
