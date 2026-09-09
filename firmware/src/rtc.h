/*
 * NocSif — public API for the PCF85063A real-time clock chip.
 *
 * The chip runs off a battery-backed rail that survives resets, so it keeps wall-clock
 * time across reboots. Its integrity flag tells us whether that time is actually
 * trustworthy; if not (a brand new unit, or the backup rail was ever lost), the clock is
 * seeded from the firmware build timestamp so the UI at least shows something plausible
 * until a real time source (GNSS/NTP) is wired up later.
 *
 * The string getters below just return a cache and never touch I2C, so they're safe to
 * call from the LVGL task. nocsif_rtc_tick() is the only function that actually reads the
 * chip; call it from one periodic timer.
 */
#pragma once

#include <time.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register the chip on the shared I2C bus, force 24-hour mode and a running clock, read
 * the current time, and seed it from the build timestamp if it isn't trustworthy yet.
 * Requires the I2C bus already initialised. Idempotent. */
esp_err_t nocsif_rtc_init(void);

/* Copy the most recently read time into *out. Returns false if the time isn't trustworthy
 * or hasn't been read yet. */
bool nocsif_rtc_get(struct tm *out);

/* Set the clock to *t (24-hour local time; the weekday field is recomputed, so the caller
 * doesn't need to supply it). Refreshes the cache immediately after. Short blocking I2C —
 * safe to call from the LVGL task. */
esp_err_t nocsif_rtc_set(const struct tm *t);

/* True if the last read produced a time that can be trusted. */
bool nocsif_rtc_time_valid(void);

/* Cached "HH:MM" string, or "--:--" if the time isn't trustworthy. No I2C access. */
const char *nocsif_rtc_clock_str(void);

/* Cached date line (e.g. weekday, month, day, year), or a placeholder if the time isn't
 * trustworthy. No I2C access. */
const char *nocsif_rtc_date_str(void);

/* Read the clock over I2C and refresh the cached strings. Call periodically from one
 * place. Safe to call before nocsif_rtc_init() (no-op). */
void nocsif_rtc_tick(void);

#ifdef __cplusplus
}
#endif
