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

static const char *TAG = "axp2101";

static i2c_master_dev_handle_t s_dev;

/* Battery cache: written by nocsif_power_batt_tick, read by the cheap getters below. */
static int                s_batt_pct = -1;         /* percent, -1 if unknown */
static char               s_batt_str[8] = "--%";   /* pre-formatted getter output */
static nocsif_chg_state_t s_chg = NOCSIF_CHG_UNKNOWN;
static bool               s_vbus;                    /* USB power present */

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

    /* Only log when the charge state actually changes, not on every tick. */
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
        s_vbus = *present;   /* update the cache too, since we already have the answer */
    }
    return err;
}
