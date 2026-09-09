/*
 * NocSif — shared I2C bus + scan (M0.1)
 *
 * Brings up the T-Watch Ultra shared I2C master bus (SDA=GPIO3, SCL=GPIO2)
 * with the ESP-IDF 5.5 i2c_master driver and probes every 7-bit address,
 * annotating the known on-board devices (docs/HARDWARE.md). The bus handle
 * is created once and kept for reuse by the later PMU / expander / touch /
 * RTC / haptic drivers.
 */
#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Creates the shared I2C master bus on SDA=3 / SCL=2. Idempotent: safe to
 * call more than once; only the first call allocates the bus. */
esp_err_t nocsif_i2c_init(void);

/* Returns the shared I2C master bus handle, for other modules (PMU,
 * expander, touch, RTC, haptic) to add their devices to. NULL until
 * nocsif_i2c_init() succeeds. */
i2c_master_bus_handle_t nocsif_i2c_bus(void);

/* Probes 0x08..0x77 on the shared bus and prints an i2cdetect-style grid
 * plus a per-device breakdown (which expected T-Watch Ultra parts answered,
 * and which expected-but-absent ones are likely just unpowered or held in
 * reset). Returns the number of devices that ACKed, or -1 if the bus is not up. */
int nocsif_i2c_scan(void);

/* Same sweep, but emits a single compact log line instead of the full grid —
 * meant for periodic re-scans while bringing rails up. */
int nocsif_i2c_scan_compact(void);

#ifdef __cplusplus
}
#endif
