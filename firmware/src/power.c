/*
 * NocSif — AXP2101 PMU driver implementation. See power.h.
 *
 * Register 0x90 holds the enable bit for each rail; each rail also has its own voltage
 * register encoding millivolts as (mV-500)/100 in the low 5 bits. Enabling a rail always
 * writes the voltage first, then sets the enable bit, so it never briefly runs at a stale
 * voltage from a previous boot.
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
#define AXP2101_REG_ALDO1_VOL   0x92        /* microSD rail voltage */
#define AXP2101_REG_ALDO2_VOL   0x93        /* display rail voltage */
#define AXP2101_REG_ALDO3_VOL   0x94        /* LoRa rail voltage */
#define AXP2101_REG_ALDO4_VOL   0x95        /* sensor/IMU rail voltage */
#define AXP2101_REG_BLDO1_VOL   0x96        /* GNSS rail voltage */
#define AXP2101_REG_BLDO2_VOL   0x97        /* speaker-amp rail voltage */
#define AXP2101_REG_DLDO1_VOL   0x99        /* NFC rail voltage */
#define AXP2101_ALDO1_EN_BIT    (1u << 0)
#define AXP2101_ALDO2_EN_BIT    (1u << 1)
#define AXP2101_ALDO3_EN_BIT    (1u << 2)
#define AXP2101_ALDO4_EN_BIT    (1u << 3)
#define AXP2101_BLDO1_EN_BIT    (1u << 4)
#define AXP2101_BLDO2_EN_BIT    (1u << 5)
#define AXP2101_DLDO1_EN_BIT    (1u << 7)
#define AXP2101_ALDO_VOL_MASK   0x1F        /* voltage-code bits, same layout on every rail register */
#define AXP2101_ALDO_CODE_3V3   0x1C        /* voltage code for 3.3V */

/* Power-button and IRQ registers. PWROFF_EN controls whether a long hold is itself a
 * hardware power-off trigger (cleared so firmware decides instead); ONOFF_LEVEL sets the
 * long-press timing threshold; the INTSTS/INTEN registers latch and enable button IRQs,
 * write-1-to-clear. */
#define AXP2101_REG_PWROFF_EN   0x22
#define AXP2101_REG_ONOFF_LEVEL 0x27
#define AXP2101_REG_INTEN2      0x41
#define AXP2101_REG_INTSTS1     0x48
#define AXP2101_REG_INTSTS2     0x49
#define AXP2101_REG_INTSTS3     0x4A
#define AXP2101_PWROFF_LONG_BIT 0x02        /* long-hold-as-power-off-source bit */
#define AXP2101_IRQLEVEL_MASK   0x30
#define AXP2101_IRQLEVEL_1S5    0x10        /* 1.5s long-press threshold */
#define AXP2101_PWRKEY_BITS     0x0F        /* short/long/down/up event bits */

/* Software power-off register: bit 0 commands the deliberate soft power-off used here.
 * A separate register from PWROFF_EN above, so disabling the hardware auto-off doesn't
 * block this command. Read-modify-write to avoid disturbing the discharge-config bit. */
#define AXP2101_REG_COMMON_CFG  0x10
#define AXP2101_SOFT_PWROFF_BIT (1u << 0)

/* Battery fuel gauge + charge state registers:
 *   STATUS1: battery-present and VBUS-good flags.
 *   STATUS2: charge-current direction (standby/charging/discharging).
 *   MODULE_EN / ADC_CH_CTRL / BAT_DET: enable bits for the gauge, its voltage ADC, and
 *     battery detection (all default on, but not trusted without an explicit set).
 *   BAT_PERCENT: state-of-charge as a direct 0-100 value. */
#define AXP2101_REG_STATUS1     0x00
#define AXP2101_REG_STATUS2     0x01
#define AXP2101_REG_MODULE_EN   0x18
#define AXP2101_REG_ADC_CH_CTRL 0x30
#define AXP2101_REG_BAT_DET     0x68
#define AXP2101_REG_BAT_PERCENT 0xA4
#define AXP2101_STATUS1_BATT    (1u << 3)   /* battery present */
#define AXP2101_STATUS1_VBUS    (1u << 5)   /* USB power present */
/* The charger's tri/pre/CC/CV/done/stop state machine bits aren't decoded here — only
 * the charge-direction bits below are currently surfaced to the UI. */
#define AXP2101_CHG_DIR_SHIFT   5
#define AXP2101_CHG_DIR_MASK    0x03
#define AXP2101_GAUGE_EN_BIT    (1u << 3)
#define AXP2101_ADC_VBAT_EN     (1u << 0)
#define AXP2101_BAT_DET_EN      (1u << 0)
#define AXP2101_BATT_PCT_MAX    100         /* a reading above this means the gauge isn't ready */

/* Charger config + status block — READ-ONLY decode (see power.h). Addresses + encodings from the
 * AXP2101 datasheet / XPowersLib (pinned 2026-10-01); confirm against a nocsif_power_charge_dump()
 * readback before a WRITE path trusts them. Only STATUS2 (0x01) is otherwise read already. */
#define AXP2101_REG_BATFET      0x12        /* BATFET / charge-enable control (raw-dumped; write TBD) */
#define AXP2101_REG_VINLIM      0x15        /* VBUS input voltage limit (VINDPM) — raw-dumped         */
#define AXP2101_REG_IINLIM      0x16        /* VBUS input current limit, b2:0                         */
#define AXP2101_REG_ICC_CHG     0x62        /* constant charge current limit, b4:0                    */
#define AXP2101_REG_CV_CHG      0x64        /* charge target / CV voltage, b2:0                        */
#define AXP2101_REG_ADC_VBAT_H  0x34        /* VBAT ADC data: b5:0 = result bits [13:8]                */
#define AXP2101_REG_ADC_VBAT_L  0x35        /* VBAT ADC data: bits [7:0]                               */
#define AXP2101_IINLIM_MASK     0x07        /* 0x16 b2:0 */
#define AXP2101_ICC_MASK        0x1F        /* 0x62 b4:0 */
#define AXP2101_CV_MASK         0x07        /* 0x64 b2:0 */
#define AXP2101_CHG_FSM_MASK    0x07        /* 0x01 b2:0 = charger state machine */
#define AXP2101_ADC_VBAT_H_MASK 0x3F        /* 0x34 low 6 bits carry ADC[13:8]   */

/* Charger PROGRAM targets (nocsif_power_charge_program) — the verified-healthy config asserted at
 * boot + on plug-in. 0x18 CHARGE_GAUGE_WDT: b0 watchdog, b1 cell-charge enable, b2 button-charge
 * (XPowersLib enableCellbatteryCharge = b1). BATFET enable is 0x12 b3 (XPowersLib enableBATFET).
 * The code values match the bench baseline readback (0x16=04, 0x62=0B, 0x64=03). */
#define AXP2101_REG_CHG_GAUGE_WDT 0x18
#define AXP2101_CELL_CHG_EN_BIT   (1u << 1)   /* 0x18 b1 = cell battery charge enable */
#define AXP2101_BATFET_EN_BIT     (1u << 3)   /* 0x12 b3 = BATFET (battery <-> system) enable */
#define AXP2101_IINLIM_1500MA     0x04        /* 0x16 b2:0 code 4  = 1500 mA input ceiling */
#define AXP2101_ICC_500MA         0x0B        /* 0x62 b4:0 code 11 = 500 mA charge (thermal cap) */
#define AXP2101_CV_4V2            0x03        /* 0x64 b2:0 code 3  = 4.2 V CV target */

static const char *TAG = "axp2101";

static i2c_master_dev_handle_t s_dev;

/* Battery cache: written by nocsif_power_batt_tick, read by the cheap getters below. */
static int                s_batt_pct = -1;         /* percent, -1 if unknown */
static char               s_batt_str[8] = "--%";   /* pre-formatted getter output */
static nocsif_chg_state_t s_chg = NOCSIF_CHG_UNKNOWN;
static bool               s_vbus;                    /* USB power present */

/* Charger config + status cache (read-only decode) — primed at gauge_config, FSM/VBAT refreshed by
 * the battery tick, config regs refreshed by nocsif_power_charge_config_refresh(). */
static nocsif_charge_info_t s_chg_info = {
    .fsm = NOCSIF_CHGF_UNKNOWN, .vbat_mv = -1,
    .input_ilim_ma = -1, .charge_ilim_ma = -1, .cv_mv = -1,
};

static esp_err_t reg_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, AXP2101_TIMEOUT_MS);
}

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof buf, AXP2101_TIMEOUT_MS);
}

/* Read a register, apply (cur & ~mask) | bits, and write it back only if it changed. */
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
        /* Set the voltage before enabling, so the rail never briefly runs at a stale value. */
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
        /* Set the voltage before enabling, so the rail never briefly runs at a stale value. */
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
        /* Set the voltage before enabling, so the rail never briefly runs at a stale value. */
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
        /* Log a readback so the register mapping can be sanity-checked on the device. */
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
        /* Set the voltage before enabling, so the rail never briefly runs at a stale value. */
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
        /* Log a readback so the register mapping can be sanity-checked on the device. */
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
        /* Set the voltage before enabling, so the rail never briefly runs at a stale value.
         * Called frequently, since the amp is powered only while a sound is actually playing. */
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
        /* Set the voltage before enabling, so the rail never briefly runs at a stale value. */
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
        /* Log a readback so the register mapping can be sanity-checked on the device. */
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
        /* Set the voltage before enabling, so the rail never briefly runs at a stale value. */
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
        /* Log a readback so the register mapping can be sanity-checked on the device. */
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

    /* 1. Disable the hardware auto-off-on-hold so firmware decides what a long press does;
     *    the long-press IRQ still fires either way. */
    if ((err = reg_update(AXP2101_REG_PWROFF_EN, AXP2101_PWROFF_LONG_BIT, 0)) != ESP_OK) {
        ESP_LOGE(TAG, "clear PWROFF_EN b1 failed: %s", esp_err_to_name(err));
        return err;
    }
    /* 2. Set the long-press threshold to 1.5s, leaving the other level bits as-is. */
    if ((err = reg_update(AXP2101_REG_ONOFF_LEVEL, AXP2101_IRQLEVEL_MASK,
                          AXP2101_IRQLEVEL_1S5)) != ESP_OK) {
        ESP_LOGE(TAG, "set IRQLEVEL failed: %s", esp_err_to_name(err));
        return err;
    }
    /* 3. Clear out any stale latched IRQ bits before we start polling. */
    reg_write(AXP2101_REG_INTSTS1, 0xFF);
    reg_write(AXP2101_REG_INTSTS2, 0xFF);
    reg_write(AXP2101_REG_INTSTS3, 0xFF);
    /* 4. Enable the button's IRQ sources (not strictly required for polling, but harmless
     *    and leaves the option open to use the interrupt line later). */
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
    /* Log before writing, since power drops almost immediately after the PMU latches the
     * bit — nothing after this line is guaranteed to actually run. */
    ESP_LOGW(TAG, "AXP2101 soft power-off now (0x10 b0)");
    /* Set only the power-off bit, leaving the rest of the register untouched. The bit
     * auto-clears and can't be read back, so there's no way to confirm it after the fact. */
    esp_err_t err = reg_update(AXP2101_REG_COMMON_CFG, AXP2101_SOFT_PWROFF_BIT,
                               AXP2101_SOFT_PWROFF_BIT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "soft power-off write failed: %s", esp_err_to_name(err));
    }
    return err;   /* on success, power is usually gone before this line is reached */
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
        reg_write(AXP2101_REG_INTSTS2, pk);   /* clear only the button-event bits we just read */
        if (events) {
            *events = pk;
        }
    }
    return ESP_OK;
}

/* ---- battery fuel gauge + charge state -------------------------------------- */
static const char *chg_name(nocsif_chg_state_t s)
{
    switch (s) {
    case NOCSIF_CHG_CHARGING:    return "charging";
    case NOCSIF_CHG_DISCHARGING: return "discharging";
    case NOCSIF_CHG_STANDBY:     return "standby";
    default:                     return "unknown";
    }
}

/* ---- charger config/status decode (read-only) — see power.h ---------------- */
static nocsif_chg_fsm_t fsm_decode(uint8_t status2)
{
    switch (status2 & AXP2101_CHG_FSM_MASK) {
    case 0:  return NOCSIF_CHGF_TRICKLE;
    case 1:  return NOCSIF_CHGF_PRECHARGE;
    case 2:  return NOCSIF_CHGF_CC;
    case 3:  return NOCSIF_CHGF_CV;
    case 4:  return NOCSIF_CHGF_DONE;
    case 5:  return NOCSIF_CHGF_STOP;
    default: return NOCSIF_CHGF_UNKNOWN;   /* 6/7 reserved */
    }
}

/* 0x16 b2:0 → VBUS input current limit in mA. */
static int iinlim_ma(uint8_t raw)
{
    static const int tbl[6] = { 100, 500, 900, 1000, 1500, 2000 };
    uint8_t c = raw & AXP2101_IINLIM_MASK;
    return (c < 6) ? tbl[c] : -1;
}

/* 0x62 b4:0 → constant charge current in mA. Codes 0..8 are 0..200 mA (25 mA steps); codes 9..16
 * are 300..1000 mA (100 mA steps); anything above 16 is out of range. */
static int icc_ma(uint8_t raw)
{
    uint8_t c = raw & AXP2101_ICC_MASK;
    if (c <= 8)  return c * 25;
    if (c <= 16) return 200 + (c - 8) * 100;
    return -1;
}

/* 0x64 b2:0 → charge target / CV voltage in mV. */
static int cv_mv(uint8_t raw)
{
    switch (raw & AXP2101_CV_MASK) {
    case 1:  return 4000;
    case 2:  return 4100;
    case 3:  return 4200;
    case 4:  return 4350;
    case 5:  return 4400;
    default: return -1;   /* 0/6/7 reserved */
    }
}

esp_err_t nocsif_power_gauge_config(void)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Explicitly set the gauge, battery-detect, and voltage-ADC enable bits rather than
     * trusting their default state. Each write touches only its own bit, leaving neighbouring
     * config bits untouched; failures here are non-fatal since these default on anyway. */
    reg_update(AXP2101_REG_MODULE_EN,   AXP2101_GAUGE_EN_BIT, AXP2101_GAUGE_EN_BIT);
    reg_update(AXP2101_REG_BAT_DET,     AXP2101_BAT_DET_EN,   AXP2101_BAT_DET_EN);
    reg_update(AXP2101_REG_ADC_CH_CTRL, AXP2101_ADC_VBAT_EN,  AXP2101_ADC_VBAT_EN);

    /* Do one read now so the first UI render already has a real percentage. */
    nocsif_power_batt_tick();
    ESP_LOGI(TAG, "battery gauge: %s %d%% (vbus=%d)", chg_name(s_chg), s_batt_pct, s_vbus);

    /* Charging diagnostics (read-only): cache the charger config regs (limits / CV / BATFET) and
     * log the full decoded + raw block so boot ground-truth lands in the flash logbook. The
     * firmware does not program the charger — this is purely so a stuck-charge report is debuggable
     * and so a future re-arm/program path has a verified baseline. */
    nocsif_power_charge_config_refresh();
    nocsif_power_charge_dump();                 /* as-found (diagnostics) */
    nocsif_power_charge_program();              /* assert known-good config + (re-)enable charging */
    return ESP_OK;
}

void nocsif_power_batt_tick(void)
{
    if (s_dev == NULL) {
        return;
    }
    uint8_t s1 = 0;
    /* Read presence/VBUS first; bail out on error and keep the previous cache rather than
     * momentarily showing "--%". */
    if (reg_read(AXP2101_REG_STATUS1, &s1) != ESP_OK) {
        return;
    }
    bool present = (s1 & AXP2101_STATUS1_BATT) != 0;
    s_vbus = (s1 & AXP2101_STATUS1_VBUS) != 0;

    /* On a read error, just leave the charge state at its previous value. */
    nocsif_chg_state_t prev = s_chg;
    uint8_t s2 = 0;
    if (reg_read(AXP2101_REG_STATUS2, &s2) == ESP_OK) {
        switch ((s2 >> AXP2101_CHG_DIR_SHIFT) & AXP2101_CHG_DIR_MASK) {
        case 1:  s_chg = NOCSIF_CHG_CHARGING;    break;
        case 2:  s_chg = NOCSIF_CHG_DISCHARGING; break;
        case 0:  s_chg = NOCSIF_CHG_STANDBY;     break;
        default: s_chg = NOCSIF_CHG_UNKNOWN;     break;   /* reserved value */
        }
        /* Same read carries the charger state machine in b2:0 (charging/CV/done/stopped). */
        s_chg_info.fsm         = fsm_decode(s2);
        s_chg_info.raw_status2 = s2;
    }

    /* Battery voltage (VBAT ADC 0x34/0x35) — a cross-check for the SOC gauge (a frozen gauge shows
     * a plausible voltage with a stuck %). Needs the VBAT ADC channel (0x30 b0, set in gauge_config).
     * Keep the last-good value on a transient I2C error or an out-of-range read. */
    uint8_t vh = 0, vl = 0;
    if (reg_read(AXP2101_REG_ADC_VBAT_H, &vh) == ESP_OK &&
        reg_read(AXP2101_REG_ADC_VBAT_L, &vl) == ESP_OK) {
        int mv = ((int)(vh & AXP2101_ADC_VBAT_H_MASK) << 8) | vl;
        if (mv > 2000 && mv < 5000) s_chg_info.vbat_mv = mv;   /* plausible Li-ion window */
    }

    /* The percent reading only makes sense with a battery present; treat anything above 100
     * as the gauge not being settled yet, and keep the last good value on a read error. */
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
                s_batt_pct = -1;                 /* gauge not settled yet */
                strcpy(s_batt_str, "--%");
            }
        }
        /* else: I2C read failed, so just keep the existing cached value */
    }

    /* Log only on a charge-state change (plug/unplug), never per tick. Enriched with the charger
     * FSM + battery voltage so a "plugged but not charging" state is visible in the logbook. */
    if (s_chg != prev) {
        ESP_LOGI(TAG, "battery %s: %d%% %dmV fsm=%s (vbus=%d)", chg_name(s_chg), s_batt_pct,
                 s_chg_info.vbat_mv, nocsif_power_charge_fsm_str(), s_vbus);
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
        s_vbus = *present;   /* update the cache too, since we already have the answer */
    }
    return err;
}

/* ---- charger config + status (read-only diagnostics) — see power.h --------- */
const char *nocsif_power_charge_fsm_str(void)
{
    switch (s_chg_info.fsm) {
    case NOCSIF_CHGF_TRICKLE:   return "trickle";
    case NOCSIF_CHGF_PRECHARGE: return "pre-charge";
    case NOCSIF_CHGF_CC:        return "charging (CC)";
    case NOCSIF_CHGF_CV:        return "charging (CV)";
    case NOCSIF_CHGF_DONE:      return "full";
    case NOCSIF_CHGF_STOP:      return "not charging";
    default:                    return "unknown";
    }
}

nocsif_chg_fsm_t nocsif_power_charge_fsm(void) { return s_chg_info.fsm; }
int              nocsif_power_vbat_mv(void)    { return s_chg_info.vbat_mv; }

bool nocsif_power_charge_info(nocsif_charge_info_t *out)
{
    if (out == NULL) {
        return false;
    }
    if (s_dev == NULL) {
        memset(out, 0, sizeof *out);
        return false;
    }
    *out = s_chg_info;
    return true;
}

void nocsif_power_charge_config_refresh(void)
{
    if (s_dev == NULL) {
        return;
    }
    uint8_t b12 = 0, b18 = 0, b15 = 0, b16 = 0, b62 = 0, b64 = 0;
    reg_read(AXP2101_REG_BATFET, &b12);
    reg_read(AXP2101_REG_CHG_GAUGE_WDT, &b18);
    reg_read(AXP2101_REG_VINLIM, &b15);
    reg_read(AXP2101_REG_IINLIM, &b16);
    reg_read(AXP2101_REG_ICC_CHG, &b62);
    reg_read(AXP2101_REG_CV_CHG, &b64);
    s_chg_info.raw_batfet     = b12;
    s_chg_info.raw_chgwdt     = b18;
    s_chg_info.raw_vinlim     = b15;
    s_chg_info.raw_iinlim     = b16;
    s_chg_info.raw_icc        = b62;
    s_chg_info.raw_cv         = b64;
    s_chg_info.input_ilim_ma  = iinlim_ma(b16);
    s_chg_info.charge_ilim_ma = icc_ma(b62);
    s_chg_info.cv_mv          = cv_mv(b64);
}

void nocsif_power_charge_dump(void)
{
    if (s_dev == NULL) {
        ESP_LOGW(TAG, "charge dump: PMU not attached");
        return;
    }
    nocsif_power_charge_config_refresh();   /* make the raw/decoded config current */
    ESP_LOGW(TAG, "charge: fsm=%s vbat=%dmV soc=%d%% dir=%s vbus=%d",
             nocsif_power_charge_fsm_str(), s_chg_info.vbat_mv, s_batt_pct,
             chg_name(s_chg), (int)s_vbus);
    ESP_LOGW(TAG, "charge cfg: chg-en=%d batfet=%d in-ilim=%dmA chg-ilim=%dmA cv=%dmV  "
                  "raw[0x12=%02X 0x18=%02X 0x15=%02X 0x16=%02X 0x62=%02X 0x64=%02X 0x01=%02X]",
             (s_chg_info.raw_chgwdt & AXP2101_CELL_CHG_EN_BIT) ? 1 : 0,
             (s_chg_info.raw_batfet & AXP2101_BATFET_EN_BIT) ? 1 : 0,
             s_chg_info.input_ilim_ma, s_chg_info.charge_ilim_ma, s_chg_info.cv_mv,
             s_chg_info.raw_batfet, s_chg_info.raw_chgwdt, s_chg_info.raw_vinlim,
             s_chg_info.raw_iinlim, s_chg_info.raw_icc, s_chg_info.raw_cv, s_chg_info.raw_status2);
}

void nocsif_power_charge_program(void)
{
    if (s_dev == NULL) {
        return;
    }
    /* BATFET on — keep the battery connected to the system (0x12 b3). */
    reg_update(AXP2101_REG_BATFET, AXP2101_BATFET_EN_BIT, AXP2101_BATFET_EN_BIT);
    /* Cell-charge enable (0x18 b1) — the bit an ESP32 reset cannot restore on its own. */
    reg_update(AXP2101_REG_CHG_GAUGE_WDT, AXP2101_CELL_CHG_EN_BIT, AXP2101_CELL_CHG_EN_BIT);
    /* Input current ceiling 1500 mA (0x16 b2:0): room for system load + the 500 mA charge. VINDPM
     * (0x15) auto-throttles a weak source, so this ceiling is safe on a 500 mA PC port too. */
    reg_update(AXP2101_REG_IINLIM, AXP2101_IINLIM_MASK, AXP2101_IINLIM_1500MA);
    /* Constant charge current 500 mA (0x62 b4:0) — the HARDWARE.md PMU-thermal cap. */
    reg_update(AXP2101_REG_ICC_CHG, AXP2101_ICC_MASK, AXP2101_ICC_500MA);
    /* CV target 4.2 V (0x64 b2:0) — standard Li-ion full voltage (never 4.35/4.4). */
    reg_update(AXP2101_REG_CV_CHG, AXP2101_CV_MASK, AXP2101_CV_4V2);

    /* Readback so the applied config is on-device-verifiable (same discipline as the rails). */
    uint8_t b12 = 0, b18 = 0, b16 = 0, b62 = 0, b64 = 0;
    reg_read(AXP2101_REG_BATFET, &b12);
    reg_read(AXP2101_REG_CHG_GAUGE_WDT, &b18);
    reg_read(AXP2101_REG_IINLIM, &b16);
    reg_read(AXP2101_REG_ICC_CHG, &b62);
    reg_read(AXP2101_REG_CV_CHG, &b64);
    ESP_LOGI(TAG, "charge program: batfet=%d chg-en=%d in-ilim=%dmA chg-ilim=%dmA cv=%dmV  "
                  "raw[0x12=%02X 0x18=%02X 0x16=%02X 0x62=%02X 0x64=%02X]",
             (b12 & AXP2101_BATFET_EN_BIT) ? 1 : 0, (b18 & AXP2101_CELL_CHG_EN_BIT) ? 1 : 0,
             iinlim_ma(b16), icc_ma(b62), cv_mv(b64), b12, b18, b16, b62, b64);

    /* Refresh the cached snapshot so getters/UI reflect the programmed values. */
    nocsif_power_charge_config_refresh();
}
