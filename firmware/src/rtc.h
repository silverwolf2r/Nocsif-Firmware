/*
 * NocSif — PCF85063A real-time clock (UI-shell P4.2)
 *
 * The PCF85063A (I2C 0x51) on the shared bus keeps wall-clock time across resets on its
 * VRTC backup rail (LDO1, can't be turned off). This driver attaches to the bus like
 * power.c (register-direct, no vendor library), burst-reads the BCD time registers into a
 * C struct tm, and exposes cheap CACHED strings for the UI to read at build/tick time.
 *
 * Integrity: the Seconds register's top bit is the OS (oscillator-stop) flag — when set,
 * the oscillator stopped since the flag was last cleared, so the time is unreliable. On a
 * fresh unit (or after the backup rail was lost) that flag is set at first boot; we seed the
 * clock from a build-time value so the header shows a plausible, advancing time. Real time
 * sync (GNSS / NTP) is a later milestone (M5/M8) and is intentionally NOT pulled in here.
 * When the time is unreliable, the cached clock string is "--:--".
 *
 * THREADING: nocsif_rtc_clock_str()/date_str() return CACHED strings and touch no hardware,
 * so they are safe to call from the LVGL task (e.g. build_header). nocsif_rtc_tick() performs
 * the I2C read and refreshes the cache; drive it from ONE periodic caller (the header tick
 * LVGL timer in ui.c). The I2C transaction is short (~8 bytes) — the touch indev already does
 * blocking I2C on the LVGL task — but the cache indirection lets the poll move to a worker
 * task later without changing callers.
 */
#pragma once

#include <time.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Attach the PCF85063A (0x51) to the shared I2C bus, ensure 24-hour mode and that the clock
 * is running, read the current time, and — if the time is unreliable (OS flag set) — seed it
 * from the firmware build timestamp and clear the flag. Primes the cached strings so the very
 * first header render shows a real time, not "--:--". Requires nocsif_i2c_init() first.
 * Idempotent. Returns ESP_OK once attached (seeding/validity are reported via the getters). */
esp_err_t nocsif_rtc_init(void);

/* Copy the most-recently-read time into *out (struct tm, 24-hour). Returns false if the time
 * is unreliable or has never been read. */
bool nocsif_rtc_get(struct tm *out);

/* Set the clock (M8-P3 GNSS sync / any trusted source). *t is 24-hour local wall-clock; tm_wday is
 * recomputed from the date so the caller need not supply it. Performs the datasheet-safe
 * STOP→write→run sequence (also clears the OS integrity flag), re-reads to refresh the cache +
 * validity, so the header shows the new time at once. Short blocking I2C — safe on the LVGL task
 * (like nocsif_rtc_tick). Returns ESP_OK, ESP_ERR_INVALID_STATE (not attached), ESP_ERR_INVALID_ARG
 * (null / out-of-range fields), or an I2C error. */
esp_err_t nocsif_rtc_set(const struct tm *t);

/* True when the last read produced a trustworthy time (OS flag clear and the read succeeded). */
bool nocsif_rtc_time_valid(void);

/* Cached "HH:MM" (24-hour), or "--:--" when the time is unreliable. No I2C — safe on the LVGL
 * task. Returns a pointer to a static buffer owned by the module (stable between ticks). */
const char *nocsif_rtc_clock_str(void);

/* Cached date line, e.g. "Mon " NOCSIF_DOT " Aug 10 2026", or "-- " NOCSIF_DOT " --" when the
 * time is unreliable. No I2C — safe on the LVGL task. Static module-owned buffer. */
const char *nocsif_rtc_date_str(void);

/* Read the clock over I2C and refresh the cached strings. Call from the single header-tick
 * timer (once every ~20 s; the labels are guarded update-on-change, so this only actually
 * repaints on a minute/day rollover). Safe to call before nocsif_rtc_init() (no-op). */
void nocsif_rtc_tick(void);

#ifdef __cplusplus
}
#endif
