/*
 * NocSif — XL9555 I2C GPIO expander (M1). See xl9555.h.
 */
#include "xl9555.h"

#include <stdint.h>

#include "i2c_scan.h"          /* nocsif_i2c_bus() */
#include "driver/i2c_master.h"
#include "esp_log.h"

#define XL9555_ADDR        0x20
#define XL9555_SCL_HZ      400000
#define XL9555_TIMEOUT_MS  100

/* PCA9555 register map. Port 0 covers IO0..IO7, port 1 covers IO8..IO15. */
#define XL9555_REG_OUTPUT0 0x02
#define XL9555_REG_OUTPUT1 0x03
#define XL9555_REG_CONFIG0 0x06   /* 1 = input (default), 0 = output */
#define XL9555_REG_CONFIG1 0x07

static const char *TAG = "xl9555";

static i2c_master_dev_handle_t s_dev;

static esp_err_t reg_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, XL9555_TIMEOUT_MS);
}

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof buf, XL9555_TIMEOUT_MS);
}

esp_err_t nocsif_xl9555_init(void)
{
    if (s_dev != NULL) {
        return ESP_OK;
    }
    i2c_master_bus_handle_t bus = nocsif_i2c_bus();
    if (bus == NULL) {
        ESP_LOGE(TAG, "shared I2C bus not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = XL9555_ADDR,
        .scl_speed_hz = XL9555_SCL_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (err != ESP_OK) {
        s_dev = NULL;
        ESP_LOGE(TAG, "add_device(0x%02X) failed: %s", XL9555_ADDR, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "XL9555 expander attached at 0x%02X", XL9555_ADDR);
    return ESP_OK;
}

esp_err_t nocsif_xl9555_set_output(uint8_t io, bool level)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (io > 15) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t bit     = (uint8_t)(1u << (io & 0x7));
    const uint8_t out_reg = (io < 8) ? XL9555_REG_OUTPUT0 : XL9555_REG_OUTPUT1;
    const uint8_t cfg_reg = (io < 8) ? XL9555_REG_CONFIG0 : XL9555_REG_CONFIG1;

    uint8_t out, cfg;
    esp_err_t err;

    /* Set the output latch to the desired level BEFORE switching the pin to an
     * output, so it never briefly drives the wrong level. */
    if ((err = reg_read(out_reg, &out)) != ESP_OK) return err;
    out = level ? (uint8_t)(out | bit) : (uint8_t)(out & ~bit);
    if ((err = reg_write(out_reg, out)) != ESP_OK) return err;

    /* Direction: clear the bit (0 = output). */
    if ((err = reg_read(cfg_reg, &cfg)) != ESP_OK) return err;
    cfg = (uint8_t)(cfg & ~bit);
    if ((err = reg_write(cfg_reg, cfg)) != ESP_OK) return err;

    return ESP_OK;
}

esp_err_t nocsif_xl9555_display_power(bool on)
{
    esp_err_t err = nocsif_xl9555_set_output(XL9555_IO_DISPLAY_EN, on);
    ESP_LOGI(TAG, "display power (IO%d) %s: %s", XL9555_IO_DISPLAY_EN,
             on ? "ON" : "OFF", esp_err_to_name(err));
    return err;
}

esp_err_t nocsif_xl9555_touch_reset(bool released)
{
    return nocsif_xl9555_set_output(XL9555_IO_TOUCH_RST, released);
}

esp_err_t nocsif_xl9555_haptic_enable(bool on)
{
    return nocsif_xl9555_set_output(XL9555_IO_HAPTIC_EN, on);
}
