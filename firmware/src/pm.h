/*
 * NocSif — CPU dynamic frequency scaling toggle.
 *
 * Wraps ESP-IDF's esp_pm framework to let the CPU drop to 80 MHz while idle and ramp back to
 * 240 MHz on demand, saving power system-wide. Peripheral drivers hold PM locks during their
 * own transfers, so this doesn't disturb display or sensor timing. Light sleep is not enabled
 * here. Requires CONFIG_PM_ENABLE in sdkconfig; with DFS left off the CPU just stays at 240 MHz.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Turn DFS on (CPU may scale 240->80 MHz when idle) or off (pinned at 240 MHz). Callable at
 * boot or at runtime from a UI toggle. Returns ESP_ERR_NOT_SUPPORTED if PM isn't compiled in. */
esp_err_t nocsif_pm_set_dfs(bool dfs_on);

/* Load the persisted DFS preference and apply it. Call once at boot, after settings init. */
void nocsif_pm_init(void);

/* Whether DFS is currently applied, from cache — no hardware access. */
bool nocsif_pm_dfs_enabled(void);

#ifdef __cplusplus
}
#endif
