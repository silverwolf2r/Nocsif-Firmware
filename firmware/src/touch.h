/*
 * NocSif — CST9217 capacitive touch controller (M2)
 *
 * Hynitron CST9217 @ I2C 0x1A on the shared bus (SDA=3/SCL=2), the same family
 * as the CST92xx driven by LilyGo's SensorLib TouchDrvCST92xx. Panel is the
 * 410x502 CO5300 AMOLED (docs/HARDWARE.md).
 *
 * PRECONDITION: the touch reset line (XL9555 IO10, nocsif_xl9555_touch_reset)
 * must be released before init — it already default-releases at cold boot, but
 * a clean low->high pulse is the reliable path (see nocsif_touch_reset_pulse).
 *
 * Bring-up strategy is POLLING over I2C: the CST9217 also has an active-low INT
 * on a dedicated GPIO (TP_INT), but polling the report register needs no ISR and
 * is enough to prove coordinates. INT-driven reads are a later optimisation.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Max simultaneous touch points the CST9217 reports (family supports up to 5;
 * a watch panel realistically sees 1-2). */
#define NOCSIF_TOUCH_MAX_POINTS 5

typedef struct {
    uint16_t x;       /* panel X (raw controller coordinate) */
    uint16_t y;       /* panel Y (raw controller coordinate) */
    uint8_t  id;      /* finger/track id reported by the controller */
} nocsif_touch_point_t;

/* Attach the CST9217 (0x1A) to the shared I2C bus and confirm it responds
 * (chip-id read). Requires nocsif_i2c_init() first and the touch reset released.
 * Idempotent. Returns ESP_OK once the controller is ready to poll. */
esp_err_t nocsif_touch_init(void);

/* Poll the controller once. Writes up to `max` active points into `pts` and the
 * number actually touching into `*count` (0 = no touch). Returns ESP_OK on a
 * clean read (including the no-touch case), or an I2C error. */
esp_err_t nocsif_touch_read(nocsif_touch_point_t *pts, int max, int *count);

/* True if the most recent nocsif_touch_read() frame carried the controller's built-in
 * COVER-SCREEN gesture — a full-hand cover, which the CST9217 flags directly (it has no
 * coordinate, so it is NOT a touch point and does not appear in *count). Reflects the last
 * read only; call it right after nocsif_touch_read from the same task. Used for palm-to-sleep. */
bool nocsif_touch_cover(void);

#ifdef __cplusplus
}
#endif
