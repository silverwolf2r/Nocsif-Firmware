/*
 * NocSif — XL9555 I2C GPIO expander (M1)
 *
 * The XL9555 (0x20, PCA9555-compatible 16-bit expander) gates several rails on
 * the T-Watch Ultra (docs/HARDWARE.md, confirmed against the LilyGo hardware doc):
 *   IO6  = haptic (DRV2605) enable
 *   IO7  = display power-supply enable   <- the M1 display gate
 *   IO10 = touch (CST9217) reset  (high = released; already released at cold boot)
 *   IO12 = microSD insert-detect (input)
 * Pins are numbered 0..15: IO0..IO7 = port0, IO8..IO15 = port1.
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

/* Attach the XL9555 (0x20) to the shared I2C bus. Requires nocsif_i2c_init()
 * first. Idempotent. */
esp_err_t nocsif_xl9555_init(void);

/* Drive expander pin `io` (0..15) as a push-pull output at `level`. Read-modify-
 * write so other pins are left untouched. */
esp_err_t nocsif_xl9555_set_output(uint8_t io, bool level);

/* Convenience wrappers for the M1 display power sequence. */
esp_err_t nocsif_xl9555_display_power(bool on);   /* IO7 */
esp_err_t nocsif_xl9555_touch_reset(bool released); /* IO10, high = released */
esp_err_t nocsif_xl9555_haptic_enable(bool on);   /* IO6 */

#ifdef __cplusplus
}
#endif
