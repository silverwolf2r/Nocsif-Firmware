/*
 * NocSif — CST9217 capacitive touch controller (M2). See touch.h.
 *
 * The Hynitron CST92xx family (CST9217 = single-touch variant) is NOT the CST816
 * protocol: it uses 16-bit BIG-ENDIAN register addresses (two address bytes on
 * the wire before every read/write). Derived from lewisxhe/SensorLib
 * TouchDrvCST92xx.cpp — the exact driver LilyGo's own firmware uses — and
 * cross-checked byte-for-byte against the ESPHome cst9220 component. On this
 * board the controller answers at 0x1A (SensorLib's 0x5A default collides with
 * the DRV2605 haptic), confirmed by our own I2C scan.
 *
 * Polled steady-state read (no INT pin needed for bring-up):
 *   1. write reg addr {0xD0,0x00} then read 15 bytes (REG_READ 0xD000);
 *   2. write {0xD0,0x00,0xAB} back — the frame-ACK handshake; REQUIRED, or the
 *      controller stops emitting new frames after the first;
 *   3. gate the frame: buf[6]==0xAB (ACK marker) && buf[0]!=0xAB && buf[0]!=0x00;
 *   4. count = buf[5] & 0x7F; per finger i, pdat = buf + i*5 + (i?2:0) — the
 *      count/ACK bytes sit BETWEEN finger0 and finger1 — then decode 12-bit
 *      X=(pdat[1]<<4)|(pdat[3]>>4), Y=(pdat[2]<<4)|(pdat[3]&0x0F), accepting a
 *      point only when its event nibble (pdat[0]&0x0F) == 0x06 (contact).
 */
#include "touch.h"

#include <stdint.h>
#include <stddef.h>

#include "i2c_scan.h"          /* nocsif_i2c_bus() */
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define TOUCH_ADDR         0x1A
#define TOUCH_SCL_HZ       400000
#define TOUCH_TIMEOUT_MS   50

/* CST92xx 16-bit registers (big-endian on the wire). */
#define CST_REG_READ       0xD000   /* touch report block */
#define CST_REG_CMDMODE    0xD101   /* enter command/debug mode (attribute reads) */
#define CST_REG_CHIPID     0xD204   /* chipType (bytes 3:2) + projectID (bytes 1:0) */
#define CST_ACK            0xAB      /* frame-ACK marker at buf[6] and write-back value */
#define CST_EVENT_CONTACT  0x06      /* event nibble meaning finger-down/contact */
#define CST_MAX_FINGERS    2         /* SensorLib caps the CST92xx driver at 2 */
#define CST_REPORT_LEN     (CST_MAX_FINGERS * 5 + 5)   /* 15 bytes */
#define CST9217_CHIP_ID    0x9217

/* Optional chip-id confirmation at init. It requires entering command mode
 * (write 0xD101), which on divergent board firmware could disturb report mode,
 * so it is OFF by default — the milestone is proving touch reads, and the I2C
 * probe already confirms the controller is alive. Set to 1 to log the chip id. */
#define TOUCH_READ_CHIP_ID 0

static const char *TAG = "cst9217";

static i2c_master_dev_handle_t s_dev;

static inline void be16(uint16_t reg, uint8_t out[2])
{
    out[0] = (uint8_t)(reg >> 8);
    out[1] = (uint8_t)reg;
}

/* Write the 16-bit register address (big-endian), then read `len` bytes back in
 * one transaction (repeated start) — SensorLib's writeThenRead. */
static esp_err_t read_reg(uint16_t reg, uint8_t *buf, size_t len)
{
    uint8_t addr[2];
    be16(reg, addr);
    return i2c_master_transmit_receive(s_dev, addr, sizeof addr, buf, len, TOUCH_TIMEOUT_MS);
}

#if TOUCH_READ_CHIP_ID
/* Best-effort, diagnostic only: never gate init on it (attribute reads can be
 * unreliable at the 0x1A remap). */
static void log_chip_id(void)
{
    uint8_t cmd[2];
    be16(CST_REG_CMDMODE, cmd);                 /* address-only write enters command mode */
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

    /* The controller ACKs at cold boot (touch reset is released via the XL9555
     * before we get here). Confirm it answers so a wiring/reset fault is loud. */
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

/* Set for the frame in which the controller flags its built-in cover-screen gesture (see the
 * gate below). Reflects the LAST read only (cleared at each read entry). Read via
 * nocsif_touch_cover() by the same (LVGL) task that calls nocsif_touch_read — no locking needed. */
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

    /* Frame-ACK handshake: write 0xAB back to REG_READ, unconditionally and
     * BEFORE validating, exactly as SensorLib — without it the controller stops
     * producing new frames after the first read. */
    uint8_t ack[3];
    be16(CST_REG_READ, ack);
    ack[2] = CST_ACK;
    err = i2c_master_transmit(s_dev, ack, sizeof ack, TOUCH_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }

    /* Frame-validity gate: buf[6] is the ACK marker; buf[0]==0xAB or 0x00 is a
     * stale/empty frame (no touch). A clean read with no touch is not an error. */
    if (buf[6] != CST_ACK || buf[0] == CST_ACK || buf[0] == 0x00) {
        return ESP_OK;
    }
    /* A cover-screen / home-button gesture is flagged in point0's status byte
     * [4] (bit7); it carries no coordinate, so report zero points. Surface it (palm-to-sleep,
     * P4.6) — the controller's own cover detection is more reliable than counting points. */
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
        /* count (buf[5]) and ACK (buf[6]) sit between finger0 and finger1, so
         * finger0 is at offset 0 and finger1 at offset 7. */
        const uint8_t *p = buf + (i * 5) + (i == 0 ? 0 : 2);
        const uint8_t id    = (uint8_t)(p[0] >> 4);
        const uint8_t event = (uint8_t)(p[0] & 0x0F);
        if (event != CST_EVENT_CONTACT || id >= CST_MAX_FINGERS) {
            continue;
        }
        pts[out].x  = (uint16_t)((p[1] << 4) | (p[3] >> 4));   /* 12-bit X */
        pts[out].y  = (uint16_t)((p[2] << 4) | (p[3] & 0x0F)); /* 12-bit Y */
        pts[out].id = id;
        out++;
    }

    if (count != NULL) {
        *count = out;
    }
    return ESP_OK;
}
