/*
 * XL9555 I2C GPIO expander driver (M1).
 *
 * This chip is a PCA9555-compatible 16-bit I/O expander at address 0x20 that gates
 * several power/reset rails on the T-Watch Ultra:
 *   IO6  = haptic driver (DRV2605) enable
 *   IO7  = display power supply enable (the M1 display gate)
 *   IO10 = touch controller (CST9217) reset, active-low released (high = released;
 *          it is already released at cold boot)
 *   IO12 = microSD card-detect input
 * Pins 0..15 map to two 8-bit ports: IO0..IO7 = port 0, IO8..IO15 = port 1.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XL9555_IO_HAPTIC_EN   6
#define XL9555_IO_DISPLAY_EN  7
#define XL9555_IO_TOUCH_RST   10
#define XL9555_IO_SD_DETECT   12

/* Probe and attach the XL9555 at 0x20 on the shared I2C bus. Call nocsif_i2c_init()
 * first. Safe to call more than once. */
esp_err_t nocsif_xl9555_init(void);

/* Configure pin `io` (0..15) as a push-pull output and drive it to `level`.
 * Read-modify-writes the port registers so other pins keep their state. */
esp_err_t nocsif_xl9555_set_output(uint8_t io, bool level);

/* Named helpers for the fixed M1 pin assignments. */
esp_err_t nocsif_xl9555_display_power(bool on);   /* display power enable, IO7 */
esp_err_t nocsif_xl9555_touch_reset(bool released); /* touch reset, IO10 (high releases the CST9217) */
esp_err_t nocsif_xl9555_haptic_enable(bool on);   /* haptic enable, IO6 */

#ifdef __cplusplus
}
#endif
