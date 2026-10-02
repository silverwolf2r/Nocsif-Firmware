/*
 * NocSif — public API for the AXP2101 power-management IC.
 *
 * The AXP2101 gates every switchable power rail on the watch (display, SD, NFC, sensors,
 * speaker, LoRa, GNSS), plus the power button, battery gauge, and software power-off. This
 * driver talks to it directly over I2C at the register level, since its POR state can't be
 * trusted and every rail's voltage and enable bit are programmed explicitly at each use.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register the AXP2101 on the shared I2C bus. Requires the I2C bus already initialised.
 * Idempotent. */
esp_err_t nocsif_power_init(void);

/* Turn the display's 3.3V rail on or off. */
esp_err_t nocsif_power_display_rail(bool on);

/* Turn the microSD card's 3.3V rail on or off. */
esp_err_t nocsif_power_sd_rail(bool on);

/* Turn the NFC chip's 3.3V rail on or off. */
esp_err_t nocsif_power_nfc_rail(bool on);

/* Turn the IMU/sensor's 3.3V rail on or off. */
esp_err_t nocsif_power_sensor_rail(bool on);

/* Turn the speaker amplifier's 3.3V rail on or off. Toggled frequently by the audio
 * worker, which powers it only while a sound is actually playing. */
esp_err_t nocsif_power_speaker_rail(bool on);

/* Turn the LoRa radio's 3.3V rail on or off. */
esp_err_t nocsif_power_lora_rail(bool on);

/* Turn the GNSS module's 3.3V rail on or off. */
esp_err_t nocsif_power_gnss_rail(bool on);

/* ---- Power button (PWRKEY) -------------------------------------------------------- *
 * The PMU detects press/release/long/short button events in hardware and latches them
 * in an IRQ status register that firmware reads over I2C, rather than wiring up a GPIO
 * interrupt line. */

/* Event bits returned by nocsif_power_pwrkey_poll(). */
#define NOCSIF_PWRKEY_RELEASE  (1u << 0)
#define NOCSIF_PWRKEY_PRESS    (1u << 1)
#define NOCSIF_PWRKEY_LONG     (1u << 2)   /* held past the long-press threshold */
#define NOCSIF_PWRKEY_SHORT    (1u << 3)

/* Set up the button so firmware, not the PMU, decides what a long press does: disables
 * the PMU's own hardware auto-power-off on hold, sets the long-press threshold, and
 * enables the button's IRQ latch. Requires nocsif_power_init() first. */
esp_err_t nocsif_power_pwrkey_config(void);

/* Read and clear any button events latched since the last poll, into *events (0 if none). */
esp_err_t nocsif_power_pwrkey_poll(uint8_t *events);

/* Command a deliberate software power-off — turns off every rail except the RTC backup,
 * so the clock keeps time. Doesn't return on success, since power drops almost
 * immediately; the caller must flush anything important first. While USB power is
 * present the PMU will immediately re-power the watch (expected — true off needs the
 * cable unplugged). Requires nocsif_power_init(). */
esp_err_t nocsif_power_off(void);

/* ---- Battery fuel gauge + charge state --------------------------------------------- *
 * The AXP2101 has an on-chip gauge reporting state-of-charge directly as 0-100%, plus
 * status registers for charge direction and USB presence.
 *
 * As with rtc.c: the getters below (batt_pct/_str/_charging/_vbus_present) just read a
 * cache and touch no hardware, so they're safe to call from the LVGL task. Only
 * nocsif_power_batt_tick() does the actual I2C work; call it from one slow periodic
 * timer since the battery changes slowly. */

/* Battery charge direction. */
typedef enum {
    NOCSIF_CHG_UNKNOWN = 0,   /* couldn't be read, or no battery present */
    NOCSIF_CHG_STANDBY,       /* no current flowing */
    NOCSIF_CHG_CHARGING,
    NOCSIF_CHG_DISCHARGING,
} nocsif_chg_state_t;

/* Make sure the fuel gauge, battery-detect, and battery-voltage ADC are all enabled,
 * and do an initial cache read. Requires nocsif_power_init() first. Idempotent. */
esp_err_t nocsif_power_gauge_config(void);

/* Read the battery state over I2C and refresh the cached percent, string, charge state,
 * and VBUS flag. Call periodically (every 30-60s is plenty) from one place; keeps the
 * last known-good values on a transient I2C error. Safe to call before init (no-op). */
void nocsif_power_batt_tick(void);

/* Cached state-of-charge percent (0-100), or -1 if unknown. No I2C access. */
int nocsif_power_batt_pct(void);

/* Cached "NN%" string, or "--%" if unknown. No I2C access; stable between ticks. */
const char *nocsif_power_batt_str(void);

/* Cached charge state. No I2C access. */
nocsif_chg_state_t nocsif_power_charging(void);

/* Cached USB-power-present flag. No I2C access. */
bool nocsif_power_vbus_present(void);

/* Do a fresh (uncached) I2C read of USB-power presence into *present, for a caller that
 * needs to notice a plug/unplug promptly rather than waiting for the next battery tick.
 * Leaves *present untouched and returns an error code on I2C failure. */
esp_err_t nocsif_power_vbus_read(bool *present);

/* ---- AXP2101 charger config + status (battery/charging diagnostics) --------- *
 * A READ-ONLY decode of the charger block the firmware otherwise never touches: the charge
 * state-machine (STATUS2 0x01 b2:0), the VBUS input current limit (0x16), the constant charge
 * current limit (0x62), the charge target / CV voltage (0x64), and the battery voltage (VBAT ADC
 * 0x34/0x35). Register addresses + encodings are from the AXP2101 datasheet / XPowersLib (pinned
 * 2026-10-01); the raw bytes are kept alongside the decode so an on-device nocsif_power_charge_dump()
 * readback can confirm the mapping before any WRITE path trusts it. Cached like the fuel gauge: the
 * config regs change only when something writes them (read once at gauge_config); the FSM + VBAT
 * refresh on nocsif_power_batt_tick(). All getters are cached (no I2C) — safe on the LVGL task. */

/* Charger state machine, decoded from STATUS2 (0x01) b2:0. */
typedef enum {
    NOCSIF_CHGF_UNKNOWN = 0,  /* reserved code (6/7) / not read yet   */
    NOCSIF_CHGF_TRICKLE,      /* 0 — trickle (deeply depleted cell)   */
    NOCSIF_CHGF_PRECHARGE,    /* 1 — pre-charge                       */
    NOCSIF_CHGF_CC,           /* 2 — constant current                 */
    NOCSIF_CHGF_CV,           /* 3 — constant voltage                 */
    NOCSIF_CHGF_DONE,         /* 4 — charge complete (full)           */
    NOCSIF_CHGF_STOP,         /* 5 — not charging                     */
} nocsif_chg_fsm_t;

/* A cached snapshot of the charger block. Decoded fields are -1 / UNKNOWN until read or when the
 * gauge/ADC is unsettled; raw_* are the untouched register bytes, for ground-truth verification. */
typedef struct {
    nocsif_chg_fsm_t fsm;         /* STATUS2 0x01 b2:0 decoded                         */
    int  vbat_mv;                 /* battery voltage (mV), VBAT ADC 0x34/0x35; -1 unk  */
    int  input_ilim_ma;           /* VBUS input current limit (mA), 0x16; -1 unknown   */
    int  charge_ilim_ma;          /* constant charge current limit (mA), 0x62; -1 unk  */
    int  cv_mv;                   /* charge target / CV voltage (mV), 0x64; -1 unknown */
    uint8_t raw_batfet;           /* 0x12 BATFET_CTRL                                  */
    uint8_t raw_chgwdt;           /* 0x18 CHARGE_GAUGE_WDT (b1 = cell-charge enable)   */
    uint8_t raw_vinlim;           /* 0x15 INPUT_VOL_LIMIT_CTRL                         */
    uint8_t raw_iinlim;           /* 0x16 INPUT_CUR_LIMIT_CTRL                         */
    uint8_t raw_icc;              /* 0x62 ICC_CHG_SET                                  */
    uint8_t raw_cv;               /* 0x64 CV_CHG_VOL_SET                               */
    uint8_t raw_status2;          /* 0x01 STATUS2                                      */
} nocsif_charge_info_t;

/* Copy the cached charger snapshot into *out. Returns false (and zeroes *out) if the PMU isn't
 * attached. No I2C — safe on the LVGL task. */
bool nocsif_power_charge_info(nocsif_charge_info_t *out);

/* Cached charger FSM + a short human string ("charging (CC)" / "full" / "not charging" / ...). */
nocsif_chg_fsm_t nocsif_power_charge_fsm(void);
const char *nocsif_power_charge_fsm_str(void);

/* Cached battery voltage in mV (VBAT ADC), or -1 when unknown. No I2C. */
int nocsif_power_vbat_mv(void);

/* Re-read the charger CONFIG registers (limits / CV / BATFET) over I2C and refresh the cache +
 * decoded fields. They change only when something writes them, so this runs once at gauge_config;
 * call it again after any future charger write. Safe before init (no-op). */
void nocsif_power_charge_config_refresh(void);

/* Log the full decoded + raw charger block at WARN level (so the flash logbook captures it for
 * untethered reading in System > Diagnostics). Called at boot after gauge_config; also exposed for
 * a UI "refresh" action / the desktop bridge. Does I2C — call off the LVGL task. */
void nocsif_power_charge_dump(void);

/* Assert the known-good charger configuration over I2C and (re-)enable charging: BATFET on
 * (0x12 b3), cell-charge enable on (0x18 b1), VBUS input current limit 1500 mA (0x16), constant
 * charge current 500 mA (0x62 — the HARDWARE.md PMU-thermal cap), CV target 4.2 V (0x64). Each is
 * a read-modify-write of only its field (neighbours preserved) with a readback log, so it is a
 * no-op on an already-healthy PMU and a recovery on a drifted one.
 *
 * WHY: the firmware otherwise leaves the charger entirely at the PMU's power-on defaults, and the
 * AXP2101 keeps its register + fault state across an ESP32 reset (it is a separate always-powered
 * PMIC). So a charger that latched off — or a config that drifted (e.g. charge-enable cleared, or
 * the input limit stuck at 100 mA) — could never recover across a reboot, only a reflash/power-cycle.
 * Asserting the config here is what makes a reboot AND a USB plug-in restore charging. The 1500 mA
 * input ceiling leaves room for system load + the 500 mA charge; VINDPM (0x15) throttles a weak
 * source automatically, so it is safe on a PC port too.
 *
 * Called from gauge_config at boot (even in safe mode — charging must work while crash-looping at
 * low battery) and on a VBUS rising edge (buttons.c). Does I2C — call off the LVGL task. */
void nocsif_power_charge_program(void);

#ifdef __cplusplus
}
#endif
