/*
 * CST9217 capacitive touch controller driver implementation. See touch.h.
 *
 * The CST92xx family (the CST9217 is its single-touch member) uses a different wire
 * protocol from CST816: every read/write is preceded by a 16-bit BIG-ENDIAN register
 * address. This implementation is ported from lewisxhe/SensorLib's TouchDrvCST92xx.cpp
 * (the same driver LilyGo ships) and checked byte-for-byte against the ESPHome cst9220
 * component. On this board the controller responds at 0x1A instead of SensorLib's usual
 * 0x5A default, because 0x5A is already taken by the DRV2605 haptic driver here; 0x1A
 * was confirmed with our own I2C bus scan.
 *
 * Steady-state polled read sequence (no INT pin required):
 *   1. write the register address {0xD0,0x00}, then read 15 bytes back (REG_READ 0xD000);
 *   2. write {0xD0,0x00,0xAB} back to the controller as a frame-ACK handshake; this is
 *      mandatory, since skipping it makes the controller stop sending new frames after
 *      the first one;
 *   3. validate the frame: buf[6] must equal the ACK marker 0xAB, and buf[0] must be
 *      neither 0xAB nor 0x00 (both indicate a stale/empty frame);
 *   4. finger count = buf[5] & 0x7F; each finger's data starts at buf + i*5 + (i?2:0),
 *      because the count and ACK bytes sit between finger 0 and finger 1's data. Decode
 *      each finger's 12-bit X/Y as X=(pdat[1]<<4)|(pdat[3]>>4), Y=(pdat[2]<<4)|(pdat[3]&0x0F),
 *      accepting the point only when its event nibble (pdat[0]&0x0F) equals 0x06 (contact).
 */
#include "touch.h"

#include <stdint.h>
#include <stddef.h>

#include "i2c_scan.h"          /* for nocsif_i2c_bus() */
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define TOUCH_ADDR         0x1A
#define TOUCH_SCL_HZ       400000
#define TOUCH_TIMEOUT_MS   50

/* CST92xx registers are 16-bit and sent big-endian on the wire. */
#define CST_REG_READ       0xD000   /* the touch report block */
#define CST_REG_CMDMODE    0xD101   /* enters command/debug mode, needed to read attribute registers */
#define CST_REG_CHIPID     0xD204   /* chip type in bytes 3:2, project id in bytes 1:0 */
#define CST_ACK            0xAB      /* the ACK marker byte found at buf[6], and the value we write back */
#define CST_EVENT_CONTACT  0x06      /* event nibble value meaning finger-down / contact */
#define CST_MAX_FINGERS    2         /* SensorLib's CST92xx driver only tracks up to 2 fingers */
#define CST_REPORT_LEN     (CST_MAX_FINGERS * 5 + 5)   /* total report length: 2 fingers x 5 bytes each + a 5-byte header */
#define CST9217_CHIP_ID    0x9217

/* Optional chip-id readback at init, off by default. Reading it means entering
 * command mode (write to 0xD101), which risks disturbing report mode on firmware
 * variants we haven't tested; since the I2C probe already confirms the controller is
 * alive, this check isn't needed to prove touch works. Flip to 1 to log the chip id. */
#define TOUCH_READ_CHIP_ID 0

static const char *TAG = "cst9217";

static i2c_master_dev_handle_t s_dev;

static inline void be16(uint16_t reg, uint8_t out[2])
{
    out[0] = (uint8_t)(reg >> 8);
    out[1] = (uint8_t)reg;
}

/* Write the 16-bit big-endian register address, then read `len` bytes back in a
 * single repeated-start transaction, matching SensorLib's writeThenRead. */
static esp_err_t read_reg(uint16_t reg, uint8_t *buf, size_t len)
{
    uint8_t addr[2];
    be16(reg, addr);
    return i2c_master_transmit_receive(s_dev, addr, sizeof addr, buf, len, TOUCH_TIMEOUT_MS);
}

#if TOUCH_READ_CHIP_ID
/* Diagnostic only, never used to gate init: attribute reads can be unreliable
 * once the controller has been remapped to 0x1A. */
static void log_chip_id(void)
{
    uint8_t cmd[2];
    be16(CST_REG_CMDMODE, cmd);                 /* writing just the address enters command mode */
    if (i2c_master_transmit(s_dev, cmd, sizeof cmd, TOUCH_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "chip-id: command-mode write failed (continuing)");
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    uint8_t b[4] = {0};
    if (read_reg(CST_REG_CHIPID, b, sizeof b) != ESP_OK) {
        ESP_LOGW(TAG, "chip-id: read failed (continuing)");
        return;
    }
    uint16_t chip = (uint16_t)((b[3] << 8) | b[2]);
    uint16_t proj = (uint16_t)((b[1] << 8) | b[0]);
    if (chip == CST9217_CHIP_ID) {
        ESP_LOGI(TAG, "chip id 0x%04X (CST9217), project 0x%04X", chip, proj);
    } else {
        ESP_LOGW(TAG, "chip id 0x%04X (expected 0x9217; attribute read may be unreliable at 0x1A)", chip);
    }
}
#endif

esp_err_t nocsif_touch_init(void)
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
        .device_address = TOUCH_ADDR,
        .scl_speed_hz = TOUCH_SCL_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (err != ESP_OK) {
        s_dev = NULL;
        ESP_LOGE(TAG, "add_device(0x%02X) failed: %s", TOUCH_ADDR, esp_err_to_name(err));
        return err;
    }

    /* The controller should ACK right after cold boot, since the XL9555 has already
     * released touch reset by this point. Log loudly if it doesn't, since that usually
     * means a wiring or reset problem. */
    if (i2c_master_probe(bus, TOUCH_ADDR, TOUCH_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "0x%02X did not ACK — touch reset (XL9555 IO10) released?", TOUCH_ADDR);
    }

#if TOUCH_READ_CHIP_ID
    log_chip_id();
#endif

    ESP_LOGI(TAG, "CST9217 attached at 0x%02X (polled, 16-bit BE regs, %d-byte reports)",
             TOUCH_ADDR, CST_REPORT_LEN);
    return ESP_OK;
}

/* Latches true for the frame in which the controller reports its built-in
 * cover-screen gesture (see the gate below); cleared again at the start of every
 * read. Read through nocsif_touch_cover() from the same LVGL task that calls
 * nocsif_touch_read(), so no locking is needed. */
static bool s_cover;

bool nocsif_touch_cover(void)
{
    return s_cover;
}

esp_err_t nocsif_touch_read(nocsif_touch_point_t *pts, int max, int *count)
{
    s_cover = false;
    if (count != NULL) {
        *count = 0;
    }
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pts == NULL || max <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t buf[CST_REPORT_LEN] = {0};
    esp_err_t err = read_reg(CST_REG_READ, buf, sizeof buf);
    if (err != ESP_OK) {
        return err;
    }

    /* Frame-ACK handshake: unconditionally write 0xAB back to REG_READ before doing
     * any validation, matching SensorLib exactly. Skipping this makes the controller
     * stop producing new frames after the first read. */
    uint8_t ack[3];
    be16(CST_REG_READ, ack);
    ack[2] = CST_ACK;
    err = i2c_master_transmit(s_dev, ack, sizeof ack, TOUCH_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }

    /* Frame-validity check: buf[6] must carry the ACK marker, and buf[0] of 0xAB or
     * 0x00 means a stale or empty frame. A clean no-touch read is not an error. */
    if (buf[6] != CST_ACK || buf[0] == CST_ACK || buf[0] == 0x00) {
        return ESP_OK;
    }
    /* A cover-screen / home-button gesture sets bit 7 of point0's status byte [4].
     * It has no coordinate, so we report zero touch points and instead surface it via
     * s_cover for the palm-to-sleep feature — trusting the controller's own gesture
     * flag is more reliable than trying to infer a cover from point counts. */
    if ((buf[4] & 0xF0) && (buf[4] >> 7) == 0x01) {
        s_cover = true;
        return ESP_OK;
    }

    int n = buf[5] & 0x7F;
    if (n <= 0 || n > CST_MAX_FINGERS) {
        return ESP_OK;
    }

    int out = 0;
    for (int i = 0; i < n && out < max; i++) {
        /* The count and ACK bytes sit between finger 0 and finger 1's data, so finger 0
         * starts at offset 0 but finger 1 starts at offset 7, not 5. */
        const uint8_t *p = buf + (i * 5) + (i == 0 ? 0 : 2);
        const uint8_t id    = (uint8_t)(p[0] >> 4);
        const uint8_t event = (uint8_t)(p[0] & 0x0F);
        if (event != CST_EVENT_CONTACT || id >= CST_MAX_FINGERS) {
            continue;
        }
        pts[out].x  = (uint16_t)((p[1] << 4) | (p[3] >> 4));   /* 12-bit X coordinate */
        pts[out].y  = (uint16_t)((p[2] << 4) | (p[3] & 0x0F)); /* 12-bit Y coordinate */
        pts[out].id = id;
        out++;
    }

    if (count != NULL) {
        *count = out;
    }
    return ESP_OK;
}
