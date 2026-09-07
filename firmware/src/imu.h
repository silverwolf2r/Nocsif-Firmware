/*
 * NocSif — BHI260AP inertial sensor hub (M11 watch-core, slice A1).
 *
 * The BHI260AP (I2C 0x28, rail ALDO4, IRQ GPIO8) is a Bosch programmable smart-sensor
 * hub, not a bare accelerometer: it runs firmware that exposes "virtual sensors"
 * (accelerometer, orientation, wrist-wear-wakeup, step-counter, tilt, any/no-motion).
 * On this board it boots from a HOST-UPLOADED RAM image every power-on, so bring-up
 * pushes the ~117 KB firmware over I2C (bhy2_upload_firmware_to_ram + bhy2_boot_from_ram),
 * then configures a virtual sensor and drains the sensor-hub FIFO.
 *
 * A1 scope: bring the hub up and stream the accelerometer (proof of life). Wrist-raise
 * auto-wake (B1), motion gestures (B2) and steps (D1) build on this foundation later.
 *
 * THREADING (mirrors wifi.c / rtc.c): a dedicated worker task owns the sensor and does the
 * ~2-3 s firmware upload + boot + FIFO polling OFF the LVGL task. The getters below return
 * CACHED values under a short spinlock and touch no hardware, so they are safe to call from
 * the LVGL task (a diagnostics label, a future watchface tick). Bring-up is lazy and gated on
 * reliability safe mode (skipped after a crash-loop). The driver is independent of the BLE/WiFi
 * single radio (I2C only), so it composes freely with them.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Coarse bring-up state, for the status string / a diagnostics row. */
typedef enum {
    NOCSIF_IMU_OFF = 0,   /* not started, or skipped in safe mode         */
    NOCSIF_IMU_BOOTING,   /* worker up: uploading firmware / configuring  */
    NOCSIF_IMU_ONLINE,    /* firmware booted, accelerometer streaming     */
    NOCSIF_IMU_FAILED,    /* not found on I2C, or boot/verify failed       */
} nocsif_imu_state_t;

/* Start the BHI260AP worker (powers ALDO4, probes 0x28, uploads+boots the RAM firmware,
 * enables the accelerometer). Non-blocking: returns immediately while the worker task does
 * the slow upload. No-op returning ESP_OK in reliability safe mode (stays OFF). Idempotent —
 * a second call while running is ignored. Requires nocsif_i2c_init() + nocsif_power_init(). */
esp_err_t nocsif_imu_init(void);

/* Current bring-up state (cached; no I2C). */
nocsif_imu_state_t nocsif_imu_state(void);

/* True once the accelerometer is streaming (state == ONLINE). */
bool nocsif_imu_online(void);

/* Latest accelerometer vector in g (1 g = gravity), remapped to the watch frame
 * (X = right, Y = up/12-o'clock, Z = out of the screen). Cached; no I2C — LVGL-safe.
 * Returns false and leaves *x/*y/*z untouched until the first sample has arrived. */
bool nocsif_imu_accel_g(float *x, float *y, float *z);

/* Cached one-line status string for a diagnostics row, e.g. "off", "booting…",
 * "not found", or "x+0.01 y-0.02 z+0.99 g". Static buffer, stable between calls; LVGL-safe. */
const char *nocsif_imu_status_str(void);

/* ---- shake-to-wake (M11 B1) ----------------------------------------------- *
 * Detection is accel-based (host-side, on the IMU worker): a deliberate wrist twist back-and-forth
 * is a sustained burst of large sample-to-sample accel changes, integrated by a leaky accumulator so
 * a single bump or slow motion doesn't trigger. The setter only records intent; the UI polls
 * _take_wrist_raise() from an LVGL timer and lights the panel when a shake landed while asleep. All
 * three are cache-only (no I2C) and safe to call from the LVGL task. */

/* Enable/disable shake detection. Recorded now, honored by the worker on its next poll, so it is
 * safe to call before the hub finishes booting. */
void nocsif_imu_set_wrist_wake(bool enable);

/* True once shake detection is available (the IMU is online and streaming; else the Settings row
 * shows "n/a"). Cached; no I2C. */
bool nocsif_imu_wrist_wake_available(void);

/* Atomically read-and-clear the shake-detection latch: returns how many shakes landed since the last
 * call (0 = none). A caller that only needs "any shake" tests > 0; the count lets the sleep path
 * require several fires (sustained shaking). Only fires while detection is enabled. LVGL-safe. */
unsigned nocsif_imu_take_wrist_raise(void);

/* ---- stillness (M11 F1 power) --------------------------------------------- *
 * How long the accelerometer has read near-constant (no meaningful motion), in ms — 0 while moving.
 * Always tracked once online (independent of shake). The UI's "sleep when still" power option uses this
 * (with a flat-orientation check) to blank the panel early when the watch is set down. Cached; LVGL-safe. */
uint32_t nocsif_imu_still_ms(void);

/* ---- relative heading (M11 F2 Signal-Hunt) -------------------------------- *
 * A RELATIVE yaw angle from the BHI260AP's game-rotation-vector fusion (GAMERV, accel+gyro, NO
 * magnetometer on this board — so it is a clock-relative bearing that slowly drifts, NOT a compass
 * heading). Signal Hunt's rotation-sweep bearing uses this: zero it at the sweep start, then read the
 * change. ON-DEMAND to save gyro power — enable it while hunting, disable on exit; off it costs nothing.
 * All three are cache-only (no I2C) and LVGL-safe; the worker owns the hub-sensor enable/disable. */

/* Enable/disable the heading (GAMERV) virtual sensor. Recorded now, honored by the worker on its next
 * poll (it does the I2C), so it is safe to call before the hub finishes booting. Disabling drops the
 * gyro-backed sensor so idle draw returns to accel-only. */
void nocsif_imu_set_heading(bool enable);

/* Latest relative heading in degrees, normalized to [0,360). Cached; no I2C. Returns false (and leaves
 * *deg untouched) until heading is enabled AND the fusion has produced its first sample (it settles over
 * ~1-2 s after enable). */
bool nocsif_imu_heading_deg(float *deg);

#ifdef __cplusplus
}
#endif
