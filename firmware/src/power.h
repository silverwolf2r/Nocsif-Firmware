/*
 * NocSif — AXP2101 PMU power rails (M1)
 *
 * The AXP2101 (0x34) gates the T-Watch Ultra rails. M1 needs ALDO2 = 3.3V, the
 * display rail (docs/HARDWARE.md; LilyGoWatchUltra.cpp:396,442-443). Registers
 * are written directly (no XPowersLib): ALDO2 voltage = reg 0x93 (code
 * (mV-500)/100 in bits[4:0]), enable = reg 0x90 bit1. POR rail state is OTP-
 * programmable and not to be trusted, so we program voltage+enable every boot.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Attach the AXP2101 (0x34) to the shared I2C bus. Requires nocsif_i2c_init()
 * first. Idempotent. */
esp_err_t nocsif_power_init(void);

/* Enable/disable ALDO2 (the display rail) at 3.3V. Sets the voltage before the
 * enable bit; read-modify-write so other rails are undisturbed. */
esp_err_t nocsif_power_display_rail(bool on);

/* Enable/disable ALDO1 (the microSD rail) at 3.3V (M4-P1). Same discipline as the
 * display rail: voltage (reg 0x92) before enable (reg 0x90 bit0); read-modify-write. */
esp_err_t nocsif_power_sd_rail(bool on);

/* Enable/disable DLDO1 (the ST25R3916 NFC rail) at 3.3V (M6). Same discipline as the
 * display/SD rails: voltage (reg 0x99) before enable (reg 0x90 bit7); read-modify-write.
 * Logs a 0x90/0x99 readback on enable so the DLDO1 register mapping is on-device-verifiable. */
esp_err_t nocsif_power_nfc_rail(bool on);

/* Enable/disable ALDO4 (the BHI260AP sensor/IMU rail) at 3.3V (M11). Same discipline as the
 * display/SD/NFC rails: voltage (reg 0x95) before enable (reg 0x90 bit3); read-modify-write.
 * Logs a 0x90/0x95 readback on enable so the ALDO4 register mapping is on-device-verifiable. */
esp_err_t nocsif_power_sensor_rail(bool on);

/* Enable/disable BLDO2 (the MAX98357A speaker-amp rail) at 3.3V (M11-E1). Same discipline as the
 * other rails: voltage (reg 0x97) before enable (reg 0x90 bit5); read-modify-write. The audio worker
 * (audio.c) powers this only while a tone/cue plays and drops it in between to save power, so unlike
 * the always-on rails this toggles frequently — kept quiet (no per-call readback log). */
esp_err_t nocsif_power_speaker_rail(bool on);

/* Enable/disable ALDO3 (the SX1262 LoRa rail) at 3.3V (M9). Same discipline as the
 * display/SD/NFC/sensor rails: voltage (reg 0x94) before enable (reg 0x90 bit2); read-modify-write.
 * Logs a 0x90/0x94 readback on enable so the ALDO3 register mapping is on-device-verifiable (0x94 is
 * the datasheet-sequential ALDO slot: 0x92 ALDO1, 0x93 ALDO2, 0x94 ALDO3, 0x95 ALDO4). */
esp_err_t nocsif_power_lora_rail(bool on);

/* Enable/disable BLDO1 (the u-blox GNSS rail) at 3.3V (M8). Same discipline as the other rails:
 * voltage (reg 0x96) before enable (reg 0x90 bit4); read-modify-write. Logs a 0x90/0x96 readback
 * on enable so the BLDO1 register mapping is on-device-verifiable. LilyGoLib powers the GPS at
 * BLDO1 3.3V (LilyGoWatchUltra.cpp powerControl(POWER_GPS)). */
esp_err_t nocsif_power_gnss_rail(bool on);

/* ---- AXP2101 PWRKEY (PWR button) — UI-shell P4.1 --------------------------- *
 * The PMU classifies the power key in hardware and latches the result in its IRQ
 * status register (0x49); we read/decode it over I2C, so no IRQ GPIO line is needed
 * (the native PMU_INT = GPIO7 is only a low-power notify, added later if wanted).
 * Registers verified against the AXP2101 datasheet + XPowersLib (2026-08-10). */

/* PWRKEY event bits as returned by nocsif_power_pwrkey_poll (0x49 bits b3:b0). */
#define NOCSIF_PWRKEY_RELEASE  (1u << 0)   /* b0 positive edge (button up)   */
#define NOCSIF_PWRKEY_PRESS    (1u << 1)   /* b1 negative edge (button down) */
#define NOCSIF_PWRKEY_LONG     (1u << 2)   /* b2 long press (>= IRQLEVEL ~1.5 s) */
#define NOCSIF_PWRKEY_SHORT    (1u << 3)   /* b3 short press                 */

/* Configure the PWRKEY so FIRMWARE owns the button: clears the long-hold
 * hardware-power-off source (reg 0x22 b1), sets the long-press IRQ threshold
 * (reg 0x27 IRQLEVEL ~1.5 s), drains stale IRQ latches, and enables the PWRKEY IRQ
 * sources. Logs the before/after of 0x22/0x27. Requires nocsif_power_init() first. */
esp_err_t nocsif_power_pwrkey_config(void);

/* Poll the PWRKEY IRQ status (reg 0x49): returns the latched PWRKEY event bits
 * (NOCSIF_PWRKEY_*) in *events (0 = nothing since last poll) and write-1-clears just
 * those bits (battery/VBUS latches in b7:b4 are preserved). */
esp_err_t nocsif_power_pwrkey_poll(uint8_t *events);

/* Command a DELIBERATE software power-off (UI-shell P4.4 — the power menu's "Power off").
 * Sets AXP2101 REG 0x10 (COMMON_CONFIG) b0 "Soft PWROFF" via read-modify-write (so b5
 * internal-off-discharge and the other config bits are preserved) — the same operation as
 * XPowersLib shutdown(). Turns off every rail except VRTC; the RTC keeps time. Because P4.1
 * cleared the hardware long-hold auto-off (0x22 b1) so firmware owns the button, power-off
 * must be commanded here; this is INDEPENDENT of 0x22 (different register). The bit is
 * auto-clearing and the rails drop almost immediately, so the caller must flush anything
 * important FIRST and this does not return on success. ⚠ While USB VBUS is present the PMU's
 * wake sources re-power the watch at once (it will NOT stay off — expected, per LilyGo; true
 * off needs USB unplugged). Register verified against the AXP2101 datasheet V1.0 §6.5.4.3 /
 * §6.13.2.7 + XPowersLib (2026-08-10). Requires nocsif_power_init(). */
esp_err_t nocsif_power_off(void);

/* ---- AXP2101 battery fuel gauge + charge state — UI-shell P4.3 ------------- *
 * The AXP2101 carries an on-chip E-Gauge that reports battery state-of-charge as a
 * direct 0..100 percent (reg 0xA4); the charge state (current direction + charger
 * FSM) is in STATUS2 (0x01), and battery-present + USB-input in STATUS1 (0x00). This
 * is register-direct like the rails/PWRKEY (no XPowersLib) and READ-ONLY except three
 * POR-default-on enables verified/set once at init. Registers verified against the
 * AXP2101 datasheet V1.0 (6.11 E-Gauge + 6.13.2 register tables), XPowersLib, and
 * LilyGoLib usage (2026-08-10).
 *
 * THREADING mirrors rtc.{c,h}: nocsif_power_batt_pct/_str/_charging/_vbus_present return
 * CACHED values and touch no hardware, so they are safe on the LVGL task (e.g. build_header,
 * the live-label hook). nocsif_power_batt_tick() performs the I2C reads + refreshes the cache;
 * drive it from ONE periodic caller (the header-tick LVGL timer in ui.c) at a SLOW cadence —
 * the battery moves slowly. */

/* Charge state, decoded from STATUS2 (0x01) b6:5 battery-current direction. */
typedef enum {
    NOCSIF_CHG_UNKNOWN = 0,   /* couldn't read / no battery / reserved code */
    NOCSIF_CHG_STANDBY,       /* b6:5 = 00 — idle, no battery current       */
    NOCSIF_CHG_CHARGING,      /* b6:5 = 01                                  */
    NOCSIF_CHG_DISCHARGING,   /* b6:5 = 10                                  */
} nocsif_chg_state_t;

/* Ensure the fuel gauge (reg 0x18 b3), battery-detection (0x68 b0), and battery-voltage
 * ADC (0x30 b0) are enabled, and prime the battery cache. All three are POR default-on, but
 * OTP state is not trusted (same discipline as the rails) and this mirrors LilyGo's boot; the
 * read-modify-write touches only the one bit each, so no-ops when already set. Requires
 * nocsif_power_init() first. Idempotent. */
esp_err_t nocsif_power_gauge_config(void);

/* Read the PMU battery state over I2C (STATUS1 0x00, STATUS2 0x01, percent 0xA4) and refresh
 * the cached percent / "NN%" string / charge state / VBUS flag. Call from the single header-tick
 * timer (ui.c) at a slow ~30-60 s cadence. Keeps the last good cache on a transient I2C error.
 * Logs a line only when the charge state changes (so plug/unplug is visible, not per-tick spam).
 * Safe before nocsif_power_init() (no-op). */
void nocsif_power_batt_tick(void);

/* Cached battery state-of-charge percent (0..100), or -1 when unknown (no battery / gauge
 * unsettled / not yet read). No I2C — safe on the LVGL task. */
int nocsif_power_batt_pct(void);

/* Cached "NN%" string, or "--%" when the percent is unknown. No I2C — safe on the LVGL task;
 * this is the getter for the P4.2 live-label hook. Static module-owned buffer, stable between
 * ticks. */
const char *nocsif_power_batt_str(void);

/* Cached charge state (charging / discharging / standby / unknown). No I2C. */
nocsif_chg_state_t nocsif_power_charging(void);

/* Cached USB VBUS-present flag (STATUS1 0x00 b5). No I2C. */
bool nocsif_power_vbus_present(void);

/* Fresh (non-cached) VBUS-present read: does ONE I2C read of STATUS1 (0x00) b5 and returns it in
 * *present. Unlike the cached getter above (refreshed only on the slow battery tick), this is for a
 * caller that needs a prompt plug/unplug edge — e.g. the buttons worker polling for a USB-plug cue.
 * Runs on a normal task (I2C); returns the read error and leaves *present untouched on failure. */
esp_err_t nocsif_power_vbus_read(bool *present);

#ifdef __cplusplus
}
#endif
