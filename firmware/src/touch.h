/*
 * CST9217 capacitive touch controller driver (M2).
 *
 * The Hynitron CST9217 sits on the shared I2C bus at 0x1A (SDA=3/SCL=2); it belongs to
 * the same CST92xx family LilyGo's SensorLib drives via TouchDrvCST92xx. The panel is
 * the 410x502 CO5300 AMOLED (docs/HARDWARE.md).
 *
 * PRECONDITION: the touch reset line (XL9555 IO10, nocsif_xl9555_touch_reset) must be
 * released before init. It is already released by default at cold boot, but issuing a
 * clean low->high pulse (nocsif_touch_reset_pulse) is the more reliable path.
 *
 * This driver polls the controller over I2C rather than using its active-low INT pin
 * (TP_INT): polling needs no ISR and is sufficient to read coordinates. Switching to
 * interrupt-driven reads is a possible future optimisation.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum simultaneous touch points the CST9217 can report (the family supports up
 * to 5; a watch-sized panel realistically only ever sees 1-2). */
#define NOCSIF_TOUCH_MAX_POINTS 5

typedef struct {
    uint16_t x;       /* raw panel X coordinate from the controller */
    uint16_t y;       /* raw panel Y coordinate from the controller */
    uint8_t  id;      /* finger/track id assigned by the controller */
} nocsif_touch_point_t;

/* Probe the CST9217 (0x1A) on the shared I2C bus and confirm it answers via a
 * chip-id read. Call nocsif_i2c_init() first and make sure touch reset has been
 * released. Safe to call more than once; returns ESP_OK once ready to poll. */
esp_err_t nocsif_touch_init(void);

/* Poll the controller for one report. Fills up to `max` points into `pts` and
 * sets `*count` to how many fingers are actually touching (0 = none). Returns
 * ESP_OK for any clean read, including no-touch, or an I2C error otherwise. */
esp_err_t nocsif_touch_read(nocsif_touch_point_t *pts, int max, int *count);

/* True if the last nocsif_touch_read() call saw the controller's built-in
 * cover-screen gesture (a full-hand cover). That gesture carries no coordinate
 * so it never shows up as a touch point in *count. Only reflects the most recent
 * read, so call it right after nocsif_touch_read on the same task. Feeds the
 * palm-to-sleep behaviour. */
bool nocsif_touch_cover(void);

#ifdef __cplusplus
}
#endif
