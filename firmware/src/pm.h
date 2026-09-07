/*
 * NocSif — CPU power management (M11 power slice P3).
 *
 * Thin wrapper over the ESP-IDF power-management framework (esp_pm). The one lever exposed is dynamic
 * frequency scaling (DFS): when enabled the CPU is allowed to down-clock to 80 MHz while idle and ramps
 * back to 240 MHz on demand — a transparent, system-wide battery saving. Peripheral drivers (spi_master
 * for the QSPI display, i2c for touch/RTC/PMU/IMU) hold PM locks during their transfers, so DFS does not
 * disturb display or sensor timing. Light sleep is intentionally NOT enabled here (a later, riskier tier).
 *
 * Requires CONFIG_PM_ENABLE=y (sdkconfig). With PM compiled in but DFS off, the CPU stays pinned at
 * 240 MHz, so this is a no-op until the "dfs_en" setting is turned on (Settings > Power > Power saver).
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Apply the DFS policy: dfs_on lets the CPU scale 240 MHz (max) down to 80 MHz (min) when idle; off
 * pins it at 240 (no scaling). Safe to call at runtime (the UI toggle) and at boot. Returns the
 * esp_pm_configure result — ESP_ERR_NOT_SUPPORTED (logged) if CONFIG_PM_ENABLE is off. */
esp_err_t nocsif_pm_set_dfs(bool dfs_on);

/* Read the persisted "dfs_en" setting (default off) and apply it. Call once at boot after settings init. */
void nocsif_pm_init(void);

/* Cached DFS-enabled flag (what was last applied). No hardware. */
bool nocsif_pm_dfs_enabled(void);

#ifdef __cplusplus
}
#endif
