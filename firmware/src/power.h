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

#ifdef __cplusplus
}
#endif
