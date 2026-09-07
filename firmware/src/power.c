/*
 * NocSif — AXP2101 PMU power rails (M1). See power.h.
 *
 * Register facts (source: lewisxhe/XPowersLib AXP2101Constants.h + XPowersAXP2101.hpp):
 *   0x90 LDO_ONOFF_CTRL0 : b0=ALDO1 b1=ALDO2 b2=ALDO3 b3=ALDO4 enables
 *   0x92 ALDO1 voltage   : bits[4:0] = (mV-500)/100  (the microSD rail, M4-P1)
 *   0x93 ALDO2 voltage   : bits[4:0] = (mV-500)/100, range 500..3500mV, bits[7:5] reserved
 */
#include "power.h"

#include <stdint.h>
#include <stdio.h>             /* snprintf (battery string cache) */
#include <string.h>           /* strcpy */

#include "i2c_scan.h"          /* nocsif_i2c_bus() */
#include "driver/i2c_master.h"
#include "esp_log.h"

#define AXP2101_ADDR            0x34
#define AXP2101_SCL_HZ          400000
#define AXP2101_TIMEOUT_MS      100

#define AXP2101_REG_LDO_ONOFF0  0x90
#define AXP2101_REG_ALDO1_VOL   0x92
#define AXP2101_REG_ALDO2_VOL   0x93
#define AXP2101_REG_ALDO3_VOL   0x94        /* ALDO3 voltage (bits[4:0] = (mV-500)/100), the SX1262 LoRa rail (M9) */
#define AXP2101_REG_ALDO4_VOL   0x95        /* ALDO4 voltage (bits[4:0] = (mV-500)/100), the sensor/IMU rail (M11) */
#define AXP2101_REG_BLDO1_VOL   0x96        /* BLDO1 voltage (bits[4:0] = (mV-500)/100), the u-blox GNSS rail (M8) */
#define AXP2101_REG_BLDO2_VOL   0x97        /* BLDO2 voltage (bits[4:0] = (mV-500)/100), the speaker-amp rail (M11-E1) */
#define AXP2101_REG_DLDO1_VOL   0x99        /* DLDO1 voltage (bits[4:0] = (mV-500)/100), the NFC rail (M6) */
#define AXP2101_ALDO1_EN_BIT    (1u << 0)   /* 0x01 */
#define AXP2101_ALDO2_EN_BIT    (1u << 1)   /* 0x02 */
#define AXP2101_ALDO3_EN_BIT    (1u << 2)   /* 0x04 — 0x90 b2 = ALDO3 enable (LDO_ONOFF_CTRL0), LoRa (M9) */
#define AXP2101_ALDO4_EN_BIT    (1u << 3)   /* 0x08 — 0x90 b3 = ALDO4 enable (LDO_ONOFF_CTRL0) */
#define AXP2101_BLDO1_EN_BIT    (1u << 4)   /* 0x10 — 0x90 b4 = BLDO1 enable (LDO_ONOFF_CTRL0), GNSS (M8) */
#define AXP2101_BLDO2_EN_BIT    (1u << 5)   /* 0x20 — 0x90 b5 = BLDO2 enable (LDO_ONOFF_CTRL0) */
#define AXP2101_DLDO1_EN_BIT    (1u << 7)   /* 0x80 — 0x90 b7 = DLDO1 enable (LDO_ONOFF_CTRL0) */
#define AXP2101_ALDO_VOL_MASK   0x1F        /* low 5 bits = voltage code (shared ALDO/DLDO encoding) */
#define AXP2101_ALDO_CODE_3V3   0x1C        /* (3300-500)/100 = 28 */

/* PWRKEY / IRQ registers (P4.1). 0x22 PWROFF_EN: b1 = "long PWRKEY hold is a
 * power-off source" (clear it so firmware owns the button). 0x27 IRQ_OFF_ON_LEVEL:
 * b5:4 IRQLEVEL = long-press IRQ threshold {00=1s,01=1.5s,10=2s,11=2.5s}; b3:2
 * OFFLEVEL and b1:0 ONLEVEL are left as programmed. INTEN2 0x41 / INTSTS2 0x49:
 * b3 ponsp(short) b2 ponlp(long) b1 down b0 up; status is write-1-to-clear. */
#define AXP2101_REG_PWROFF_EN   0x22
#define AXP2101_REG_ONOFF_LEVEL 0x27
#define AXP2101_REG_INTEN2      0x41
#define AXP2101_REG_INTSTS1     0x48
#define AXP2101_REG_INTSTS2     0x49
#define AXP2101_REG_INTSTS3     0x4A
#define AXP2101_PWROFF_LONG_BIT 0x02        /* 0x22 b1 = long-hold power-off source */
#define AXP2101_IRQLEVEL_MASK   0x30        /* 0x27 b5:4 */
#define AXP2101_IRQLEVEL_1S5    0x10        /* 01 << 4 = 1.5 s long-press threshold */
#define AXP2101_PWRKEY_BITS     0x0F        /* 0x41/0x49 b3:b0 = short/long/down/up */

/* Software power-off / restart command register (P4.4). 0x10 COMMON_CONFIG: b0 = "Soft
 * PWROFF" (the deliberate software power-off; RWAC/auto-clear), b1 = PMU-level restart
 * (POWEROFF/POWON — reserved; the UI "Restart" uses esp_restart() so the rails stay up).
 * Do NOT touch b5 (internal off-discharge, POR default 1) — hence read-modify-write. This
 * is a SEPARATE register from 0x22 (PWROFF_EN, the hardware auto-off SOURCE enable), so the
 * P4.1 clearing of 0x22 b1 does not block this command. Verified against the AXP2101
 * datasheet V1.0 §6.5.4.3/§6.13.2.7 + XPowersLib shutdown() (2026-08-10). */
#define AXP2101_REG_COMMON_CFG  0x10
#define AXP2101_SOFT_PWROFF_BIT (1u << 0)   /* 0x10 b0 = soft power-off */

/* Battery fuel gauge + charge state (P4.3). Verified against the AXP2101 datasheet V1.0
 * (6.11 E-Gauge, 6.13.2 register tables), XPowersLib, and LilyGoLib usage (2026-08-10).
 *   0x00 STATUS1 (RO): b3 battery-present, b5 VBUS-good.
 *   0x01 STATUS2 (RO): b2:0 charger FSM (tri/pre/CC/CV/done/stop), b6:5 current direction
 *                      (00=standby, 01=charging, 10=discharging).
 *   0x18 MODULE_EN (RW): b3 fuel-gauge module enable (POR default 1).
 *   0x30 ADC_CH_CTRL (RW): b0 battery-voltage ADC enable — feeds the gauge (POR default 1).
 *   0x68 BAT_DET (RW): b0 battery-detection enable (POR default 1).
 *   0xA4 BAT_PERCENT (RO): state-of-charge 0..100, direct uint8 (NOT the VBAT voltage ADC). */
#define AXP2101_REG_STATUS1     0x00
#define AXP2101_REG_STATUS2     0x01
#define AXP2101_REG_MODULE_EN   0x18
#define AXP2101_REG_ADC_CH_CTRL 0x30
#define AXP2101_REG_BAT_DET     0x68
#define AXP2101_REG_BAT_PERCENT 0xA4
#define AXP2101_STATUS1_BATT    (1u << 3)   /* 0x00 b3 = battery present */
#define AXP2101_STATUS1_VBUS    (1u << 5)   /* 0x00 b5 = VBUS good (USB present) */
/* (0x01 b2:0 is the charger FSM — tri/pre/CC/CV/done/stop — not decoded here: the UI needs
 *  only the b6:5 current-direction enum below. Add an FSM decode if a "full/done" state is
 *  ever surfaced.) */
#define AXP2101_CHG_DIR_SHIFT   5           /* 0x01 b6:5 = battery current direction */
#define AXP2101_CHG_DIR_MASK    0x03
#define AXP2101_GAUGE_EN_BIT    (1u << 3)   /* 0x18 b3 */
#define AXP2101_ADC_VBAT_EN     (1u << 0)   /* 0x30 b0 */
#define AXP2101_BAT_DET_EN      (1u << 0)   /* 0x68 b0 */
#define AXP2101_BATT_PCT_MAX    100         /* reject >100 (0xFF) as gauge-not-ready */

static const char *TAG = "axp2101";

static i2c_master_dev_handle_t s_dev;

/* Battery cache (P4.3) — refreshed by nocsif_power_batt_tick, read by the cheap getters. */
static int                s_batt_pct = -1;         /* SOC 0..100, -1 = unknown        */
static char               s_batt_str[8] = "--%";   /* getter output ("NN%" / "--%")   */
static nocsif_chg_state_t s_chg = NOCSIF_CHG_UNKNOWN;
static bool               s_vbus;                    /* USB VBUS present               */

static esp_err_t reg_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, AXP2101_TIMEOUT_MS);
}

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof buf, AXP2101_TIMEOUT_MS);
}

/* read-modify-write: (cur & ~mask) | bits */
static esp_err_t reg_update(uint8_t reg, uint8_t mask, uint8_t bits)
{
    uint8_t cur;
    esp_err_t err = reg_read(reg, &cur);
    if (err != ESP_OK) return err;
    uint8_t next = (uint8_t)((cur & ~mask) | bits);
    if (next == cur) return ESP_OK;
    return reg_write(reg, next);
}

esp_err_t nocsif_power_init(void)
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
        .device_address = AXP2101_ADDR,
        .scl_speed_hz = AXP2101_SCL_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (err != ESP_OK) {
        s_dev = NULL;
        ESP_LOGE(TAG, "add_device(0x%02X) failed: %s", AXP2101_ADDR, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "AXP2101 PMU attached at 0x%02X", AXP2101_ADDR);
    return ESP_OK;
}

esp_err_t nocsif_power_display_rail(bool on)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err;
    if (on) {
        /* Voltage BEFORE enable, so the rail comes up at exactly 3.3V. */
        if ((err = reg_update(AXP2101_REG_ALDO2_VOL, AXP2101_ALDO_VOL_MASK,
                              AXP2101_ALDO_CODE_3V3)) != ESP_OK) {
            ESP_LOGE(TAG, "set ALDO2 voltage failed: %s", esp_err_to_name(err));
            return err;
        }
        if ((err = reg_update(AXP2101_REG_LDO_ONOFF0, AXP2101_ALDO2_EN_BIT,
                              AXP2101_ALDO2_EN_BIT)) != ESP_OK) {
            ESP_LOGE(TAG, "enable ALDO2 failed: %s", esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(TAG, "ALDO2 display rail ON @ 3.3V");
    } else {
        if ((err = reg_update(AXP2101_REG_LDO_ONOFF0, AXP2101_ALDO2_EN_BIT, 0)) != ESP_OK) {
            return err;
        }
        ESP_LOGI(TAG, "ALDO2 display rail OFF");
    }
    return ESP_OK;
}

esp_err_t nocsif_power_sd_rail(bool on)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err;
    if (on) {
        /* Voltage BEFORE enable, so the microSD rail comes up at exactly 3.3V. */
        if ((err = reg_update(AXP2101_REG_ALDO1_VOL, AXP2101_ALDO_VOL_MASK,
                              AXP2101_ALDO_CODE_3V3)) != ESP_OK) {
            ESP_LOGE(TAG, "set ALDO1 voltage failed: %s", esp_err_to_name(err));
            return err;
        }
        if ((err = reg_update(AXP2101_REG_LDO_ONOFF0, AXP2101_ALDO1_EN_BIT,
                              AXP2101_ALDO1_EN_BIT)) != ESP_OK) {
            ESP_LOGE(TAG, "enable ALDO1 failed: %s", esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(TAG, "ALDO1 microSD rail ON @ 3.3V");
    } else {
        if ((err = reg_update(AXP2101_REG_LDO_ONOFF0, AXP2101_ALDO1_EN_BIT, 0)) != ESP_OK) {
            return err;
        }
        ESP_LOGI(TAG, "ALDO1 microSD rail OFF");
    }
    return ESP_OK;
}

esp_err_t nocsif_power_nfc_rail(bool on)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err;
    if (on) {
        /* Voltage BEFORE enable, so the NFC rail comes up at exactly 3.3V (same discipline
         * as the display/SD rails). DLDO1 uses the same (mV-500)/100 5-bit encoding. */
        if ((err = reg_update(AXP2101_REG_DLDO1_VOL, AXP2101_ALDO_VOL_MASK,
                              AXP2101_ALDO_CODE_3V3)) != ESP_OK) {
            ESP_LOGE(TAG, "set DLDO1 voltage failed: %s", esp_err_to_name(err));
            return err;
        }
        if ((err = reg_update(AXP2101_REG_LDO_ONOFF0, AXP2101_DLDO1_EN_BIT,
                              AXP2101_DLDO1_EN_BIT)) != ESP_OK) {
            ESP_LOGE(TAG, "enable DLDO1 failed: %s", esp_err_to_name(err));
            return err;
        }
        /* Read back 0x90/0x99 so the DLDO1 register mapping (b7 enable, 0x99 volt) is
         * on-device-verifiable — XPowersLib isn't vendored to cross-check at build time. */
        uint8_t onoff = 0, vol = 0;
        reg_read(AXP2101_REG_LDO_ONOFF0, &onoff);
        reg_read(AXP2101_REG_DLDO1_VOL, &vol);
        ESP_LOGI(TAG, "DLDO1 NFC rail ON @ 3.3V (0x90=0x%02X b7=%d, 0x99=0x%02X code=%d)",
                 onoff, (onoff & AXP2101_DLDO1_EN_BIT) ? 1 : 0, vol, vol & AXP2101_ALDO_VOL_MASK);
    } else {
        if ((err = reg_update(AXP2101_REG_LDO_ONOFF0, AXP2101_DLDO1_EN_BIT, 0)) != ESP_OK) {
            return err;
        }
        ESP_LOGI(TAG, "DLDO1 NFC rail OFF");
    }
    return ESP_OK;
}

esp_err_t nocsif_power_sensor_rail(bool on)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err;
    if (on) {
        /* Voltage BEFORE enable, so the sensor rail comes up at exactly 3.3V (same discipline
         * as the display/SD/NFC rails). ALDO4 uses the same (mV-500)/100 5-bit encoding. */
        if ((err = reg_update(AXP2101_REG_ALDO4_VOL, AXP2101_ALDO_VOL_MASK,
                              AXP2101_ALDO_CODE_3V3)) != ESP_OK) {
            ESP_LOGE(TAG, "set ALDO4 voltage failed: %s", esp_err_to_name(err));
            return err;
        }
        if ((err = reg_update(AXP2101_REG_LDO_ONOFF0, AXP2101_ALDO4_EN_BIT,
                              AXP2101_ALDO4_EN_BIT)) != ESP_OK) {
            ESP_LOGE(TAG, "enable ALDO4 failed: %s", esp_err_to_name(err));
            return err;
        }
        /* Read back 0x90/0x95 so the ALDO4 register mapping (b3 enable, 0x95 volt) is
         * on-device-verifiable — XPowersLib isn't vendored to cross-check at build time. */
        uint8_t onoff = 0, vol = 0;
        reg_read(AXP2101_REG_LDO_ONOFF0, &onoff);
        reg_read(AXP2101_REG_ALDO4_VOL, &vol);
        ESP_LOGI(TAG, "ALDO4 sensor rail ON @ 3.3V (0x90=0x%02X b3=%d, 0x95=0x%02X code=%d)",
                 onoff, (onoff & AXP2101_ALDO4_EN_BIT) ? 1 : 0, vol, vol & AXP2101_ALDO_VOL_MASK);
    } else {
        if ((err = reg_update(AXP2101_REG_LDO_ONOFF0, AXP2101_ALDO4_EN_BIT, 0)) != ESP_OK) {
            return err;
        }
        ESP_LOGI(TAG, "ALDO4 sensor rail OFF");
    }
    return ESP_OK;
}

esp_err_t nocsif_power_speaker_rail(bool on)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err;
    if (on) {
        /* Voltage BEFORE enable, so the amp rail comes up at exactly 3.3V (same discipline as the
         * display/SD/NFC/sensor rails). BLDO2 uses the same (mV-500)/100 5-bit encoding. The MAX98357A
         * is powered only while a sound plays (audio.c toggles this), so this is called on each cue. */
        if ((err = reg_update(AXP2101_REG_BLDO2_VOL, AXP2101_ALDO_VOL_MASK,
                              AXP2101_ALDO_CODE_3V3)) != ESP_OK) {
            ESP_LOGE(TAG, "set BLDO2 voltage failed: %s", esp_err_to_name(err));
            return err;
        }
        if ((err = reg_update(AXP2101_REG_LDO_ONOFF0, AXP2101_BLDO2_EN_BIT,
                              AXP2101_BLDO2_EN_BIT)) != ESP_OK) {
            ESP_LOGE(TAG, "enable BLDO2 failed: %s", esp_err_to_name(err));
            return err;
        }
    } else {
        if ((err = reg_update(AXP2101_REG_LDO_ONOFF0, AXP2101_BLDO2_EN_BIT, 0)) != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t nocsif_power_lora_rail(bool on)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err;
    if (on) {
        /* Voltage BEFORE enable, so the LoRa rail comes up at exactly 3.3V (same discipline as the
         * display/SD/NFC/sensor rails). ALDO3 uses the same (mV-500)/100 5-bit encoding. 0x94 is the
         * datasheet-sequential ALDO slot (0x92/0x93/0x94/0x95 = ALDO1..4); the readback below makes
         * the mapping on-device-verifiable — XPowersLib isn't vendored to cross-check at build time. */
        if ((err = reg_update(AXP2101_REG_ALDO3_VOL, AXP2101_ALDO_VOL_MASK,
                              AXP2101_ALDO_CODE_3V3)) != ESP_OK) {
            ESP_LOGE(TAG, "set ALDO3 voltage failed: %s", esp_err_to_name(err));
            return err;
        }
        if ((err = reg_update(AXP2101_REG_LDO_ONOFF0, AXP2101_ALDO3_EN_BIT,
                              AXP2101_ALDO3_EN_BIT)) != ESP_OK) {
            ESP_LOGE(TAG, "enable ALDO3 failed: %s", esp_err_to_name(err));
            return err;
        }
        uint8_t onoff = 0, vol = 0;
        reg_read(AXP2101_REG_LDO_ONOFF0, &onoff);
        reg_read(AXP2101_REG_ALDO3_VOL, &vol);
        ESP_LOGI(TAG, "ALDO3 LoRa rail ON @ 3.3V (0x90=0x%02X b2=%d, 0x94=0x%02X code=%d)",
                 onoff, (onoff & AXP2101_ALDO3_EN_BIT) ? 1 : 0, vol, vol & AXP2101_ALDO_VOL_MASK);
    } else {
        if ((err = reg_update(AXP2101_REG_LDO_ONOFF0, AXP2101_ALDO3_EN_BIT, 0)) != ESP_OK) {
            return err;
        }
        ESP_LOGI(TAG, "ALDO3 LoRa rail OFF");
    }
    return ESP_OK;
}

esp_err_t nocsif_power_gnss_rail(bool on)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err;
    if (on) {
        /* Voltage BEFORE enable, so the GNSS rail comes up at exactly 3.3V (same discipline as the
         * other rails; LilyGoLib sets BLDO1 = 3300 mV for the u-blox). BLDO1 uses the same
         * (mV-500)/100 5-bit encoding. 0x96 is the datasheet-sequential BLDO slot (0x96 BLDO1,
         * 0x97 BLDO2); the readback below makes the mapping on-device-verifiable. */
        if ((err = reg_update(AXP2101_REG_BLDO1_VOL, AXP2101_ALDO_VOL_MASK,
                              AXP2101_ALDO_CODE_3V3)) != ESP_OK) {
            ESP_LOGE(TAG, "set BLDO1 voltage failed: %s", esp_err_to_name(err));
            return err;
        }
        if ((err = reg_update(AXP2101_REG_LDO_ONOFF0, AXP2101_BLDO1_EN_BIT,
                              AXP2101_BLDO1_EN_BIT)) != ESP_OK) {
            ESP_LOGE(TAG, "enable BLDO1 failed: %s", esp_err_to_name(err));
            return err;
        }
        uint8_t onoff = 0, vol = 0;
        reg_read(AXP2101_REG_LDO_ONOFF0, &onoff);
        reg_read(AXP2101_REG_BLDO1_VOL, &vol);
        ESP_LOGI(TAG, "BLDO1 GNSS rail ON @ 3.3V (0x90=0x%02X b4=%d, 0x96=0x%02X code=%d)",
                 onoff, (onoff & AXP2101_BLDO1_EN_BIT) ? 1 : 0, vol, vol & AXP2101_ALDO_VOL_MASK);
    } else {
        if ((err = reg_update(AXP2101_REG_LDO_ONOFF0, AXP2101_BLDO1_EN_BIT, 0)) != ESP_OK) {
            return err;
        }
        ESP_LOGI(TAG, "BLDO1 GNSS rail OFF");
    }
    return ESP_OK;
}

esp_err_t nocsif_power_pwrkey_config(void)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err;
    uint8_t off_before = 0, lvl_before = 0, off_after = 0, lvl_after = 0;
    reg_read(AXP2101_REG_PWROFF_EN, &off_before);
    reg_read(AXP2101_REG_ONOFF_LEVEL, &lvl_before);

    /* 1. Firmware owns the button: clear the "long PWRKEY hold = power-off" source so no
     *    hold length hardware-powers-off the watch. The long-press IRQ (ponlp) still fires;
     *    software decides what a long press does. Do NOT trust the EFUSE default here. */
    if ((err = reg_update(AXP2101_REG_PWROFF_EN, AXP2101_PWROFF_LONG_BIT, 0)) != ESP_OK) {
        ESP_LOGE(TAG, "clear PWROFF_EN b1 failed: %s", esp_err_to_name(err));
        return err;
    }
    /* 2. Long-press IRQ threshold = 1.5 s (leave OFFLEVEL/ONLEVEL as programmed). */
    if ((err = reg_update(AXP2101_REG_ONOFF_LEVEL, AXP2101_IRQLEVEL_MASK,
                          AXP2101_IRQLEVEL_1S5)) != ESP_OK) {
        ESP_LOGE(TAG, "set IRQLEVEL failed: %s", esp_err_to_name(err));
        return err;
    }
    /* 3. Drain stale latched IRQs (all three status banks are write-1-to-clear). */
    reg_write(AXP2101_REG_INTSTS1, 0xFF);
    reg_write(AXP2101_REG_INTSTS2, 0xFF);
    reg_write(AXP2101_REG_INTSTS3, 0xFF);
    /* 4. Enable the PWRKEY short/long/edge IRQ sources. Not strictly needed while we poll
     *    0x49 (it latches regardless), but harmless and lets the GPIO7 IRQ line assert later. */
    reg_update(AXP2101_REG_INTEN2, AXP2101_PWRKEY_BITS, AXP2101_PWRKEY_BITS);

    reg_read(AXP2101_REG_PWROFF_EN, &off_after);
    reg_read(AXP2101_REG_ONOFF_LEVEL, &lvl_after);
    ESP_LOGI(TAG, "PWRKEY cfg: PWROFF_EN(0x22) 0x%02X->0x%02X (long-off %s), "
                  "ONOFF_LVL(0x27) 0x%02X->0x%02X (IRQLEVEL code %d)",
             off_before, off_after,
             (off_after & AXP2101_PWROFF_LONG_BIT) ? "STILL SET!" : "cleared",
             lvl_before, lvl_after, (lvl_after & AXP2101_IRQLEVEL_MASK) >> 4);
    return ESP_OK;
}

esp_err_t nocsif_power_off(void)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Unique final log line BEFORE the write (the rails drop essentially the moment the PMU
     * latches b0, so nothing after this is guaranteed to run / flush). On the bench: COM7
     * de-enumerates and the screen goes black right here = the command reached the PMU. */
    ESP_LOGW(TAG, "AXP2101 soft power-off now (0x10 b0)");
    /* RMW set of just b0: preserves b5 (internal off-discharge, POR 1) and the rest of 0x10.
     * The bit auto-clears and cannot be read back, so this is fire-and-forget. */
    esp_err_t err = reg_update(AXP2101_REG_COMMON_CFG, AXP2101_SOFT_PWROFF_BIT,
                               AXP2101_SOFT_PWROFF_BIT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "soft power-off write failed: %s", esp_err_to_name(err));
    }
    return err;   /* on success the SoC loses power before this returns */
}

esp_err_t nocsif_power_pwrkey_poll(uint8_t *events)
{
    if (events) {
        *events = 0;
    }
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t v;
    esp_err_t err = reg_read(AXP2101_REG_INTSTS2, &v);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t pk = v & AXP2101_PWRKEY_BITS;
    if (pk) {
        reg_write(AXP2101_REG_INTSTS2, pk);   /* write-1-clear only the PWRKEY bits */
        if (events) {
            *events = pk;
        }
    }
    return ESP_OK;
}

/* ---- AXP2101 battery fuel gauge + charge state (P4.3) ---------------------- */
static const char *chg_name(nocsif_chg_state_t s)
{
    switch (s) {
    case NOCSIF_CHG_CHARGING:    return "charging";
    case NOCSIF_CHG_DISCHARGING: return "discharging";
    case NOCSIF_CHG_STANDBY:     return "standby";
    default:                     return "unknown";
    }
}

esp_err_t nocsif_power_gauge_config(void)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Ensure the gauge / battery-detect / VBAT-ADC enables are set. All three are POR
     * default-on, but OTP state isn't trusted (same discipline as the rails) and this mirrors
     * LilyGo's boot. Each reg_update touches ONLY its one bit and no-ops when already set — so
     * neighbours (e.g. 0x18 b1 cell-charge-enable, b0 watchdog) are preserved. Best-effort:
     * the gauge is default-on, so a write failure here is not fatal to the readout. */
    reg_update(AXP2101_REG_MODULE_EN,   AXP2101_GAUGE_EN_BIT, AXP2101_GAUGE_EN_BIT);
    reg_update(AXP2101_REG_BAT_DET,     AXP2101_BAT_DET_EN,   AXP2101_BAT_DET_EN);
    reg_update(AXP2101_REG_ADC_CH_CTRL, AXP2101_ADC_VBAT_EN,  AXP2101_ADC_VBAT_EN);

    /* Prime the cache so the first header render shows a real % (not "--%"). */
    nocsif_power_batt_tick();
    ESP_LOGI(TAG, "battery gauge: %s %d%% (vbus=%d)", chg_name(s_chg), s_batt_pct, s_vbus);
    return ESP_OK;
}

void nocsif_power_batt_tick(void)
{
    if (s_dev == NULL) {
        return;
    }
    uint8_t s1 = 0;
    /* STATUS1 first — presence gates the percent read. On a transient I2C error keep the last
     * good cache (leave everything unchanged) rather than flashing "--%". */
    if (reg_read(AXP2101_REG_STATUS1, &s1) != ESP_OK) {
        return;
    }
    bool present = (s1 & AXP2101_STATUS1_BATT) != 0;
    s_vbus = (s1 & AXP2101_STATUS1_VBUS) != 0;

    /* STATUS2 b6:5 = current direction. A read error just leaves the last charge state. */
    nocsif_chg_state_t prev = s_chg;
    uint8_t s2 = 0;
    if (reg_read(AXP2101_REG_STATUS2, &s2) == ESP_OK) {
        switch ((s2 >> AXP2101_CHG_DIR_SHIFT) & AXP2101_CHG_DIR_MASK) {
        case 1:  s_chg = NOCSIF_CHG_CHARGING;    break;
        case 2:  s_chg = NOCSIF_CHG_DISCHARGING; break;
        case 0:  s_chg = NOCSIF_CHG_STANDBY;     break;
        default: s_chg = NOCSIF_CHG_UNKNOWN;     break;   /* 3 = reserved */
        }
    }

    /* State-of-charge percent — only trustworthy with a battery present; reject >100 (0xFF /
     * gauge unsettled) as unknown. On a transient I2C error keep the LAST-GOOD value (same
     * discipline as the STATUS1/STATUS2 reads above) rather than flashing "--%" for one tick
     * (which, guarded update-on-change under full_refresh, would repaint the whole frame). */
    if (!present) {
        s_batt_pct = -1;
        strcpy(s_batt_str, "--%");
    } else {
        uint8_t pct = 0;
        if (reg_read(AXP2101_REG_BAT_PERCENT, &pct) == ESP_OK) {
            if (pct <= AXP2101_BATT_PCT_MAX) {
                s_batt_pct = pct;
                snprintf(s_batt_str, sizeof s_batt_str, "%u%%", (unsigned)pct);
            } else {
                s_batt_pct = -1;                 /* gauge unsettled (0xFF / >100) */
                strcpy(s_batt_str, "--%");
            }
        }
        /* else: transient I2C read error -> keep the last-good cache */
    }

    /* Log only on a charge-state change (plug/unplug), never per tick. */
    if (s_chg != prev) {
        ESP_LOGI(TAG, "battery %s: %d%% (vbus=%d)", chg_name(s_chg), s_batt_pct, s_vbus);
    }
}

int nocsif_power_batt_pct(void)
{
    return s_batt_pct;
}

const char *nocsif_power_batt_str(void)
{
    return s_batt_str;
}

nocsif_chg_state_t nocsif_power_charging(void)
{
    return s_chg;
}

bool nocsif_power_vbus_present(void)
{
    return s_vbus;
}

esp_err_t nocsif_power_vbus_read(bool *present)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t s1 = 0;
    esp_err_t err = reg_read(AXP2101_REG_STATUS1, &s1);
    if (err == ESP_OK && present) {
        *present = (s1 & AXP2101_STATUS1_VBUS) != 0;
        s_vbus = *present;   /* keep the cache fresh too (cheap side benefit) */
    }
    return err;
}
