/*
 * XL9555 I2C GPIO expander driver (M1).
 *
 * The XL9555 (0x20, PCA9555-compatible 16-bit expander) gates several rails on
 * the T-Watch Ultra. Mapping per LilyGo's own board code (vendor/LilyGoLib
 * LilyGoWatchUltra.cpp + the Arduino lilygo_twatch_ultra variant — the older hardware
 * doc had IO10/IO12 swapped, which made us drive the card-detect line as an output):
 *   IO6  = haptic (DRV2605) enable
 *   IO7  = display power-supply enable   <- the M1 display gate
 *   IO8  = touch (CST9217) reset  (high = released; already released at cold boot)
 *   IO10 = microSD card-detect (input; HIGH = slot empty)
 *   IO11 = LoRa antenna select (lora.cpp)
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
#define XL9555_IO_TOUCH_RST   8
#define XL9555_IO_SD_DETECT   10

/* Probe and attach the XL9555 at 0x20 on the shared I2C bus. Call nocsif_i2c_init()
 * first. Safe to call more than once. */
esp_err_t nocsif_xl9555_init(void);

/* Configure pin `io` (0..15) as a push-pull output and drive it to `level`.
 * Read-modify-writes the port registers so other pins keep their state. */
esp_err_t nocsif_xl9555_set_output(uint8_t io, bool level);

/* Make expander pin `io` (0..15) an input (the power-on default). The XL9555 keeps its
 * configuration across an ESP32 warm reset, so a pin an older build drove stays an output
 * until this is called. */
esp_err_t nocsif_xl9555_set_input(uint8_t io);

/* Read the live level of expander pin `io` (0..15) from the input port. */
esp_err_t nocsif_xl9555_get_level(uint8_t io, bool *level);

/* Convenience wrappers for the M1 display power sequence. */
esp_err_t nocsif_xl9555_display_power(bool on);   /* IO7 */
esp_err_t nocsif_xl9555_touch_reset(bool released); /* IO8, high = released */
esp_err_t nocsif_xl9555_haptic_enable(bool on);   /* IO6 */

#ifdef __cplusplus
}
#endif
