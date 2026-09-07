/*
 * NocSif — CPU power management (M11 power slice P3). See pm.h.
 */
#include "pm.h"

#include "settings.h"

#include "esp_pm.h"
#include "esp_log.h"

static const char *TAG = "nocsif_pm";

#define PM_MAX_MHZ 240
#define PM_MIN_MHZ 80    /* DFS floor while idle (kept off XTAL/40 for safe peripheral timing) */

static bool s_dfs_on;

esp_err_t nocsif_pm_set_dfs(bool dfs_on)
{
    esp_pm_config_t cfg = {
        .max_freq_mhz       = PM_MAX_MHZ,
        .min_freq_mhz       = dfs_on ? PM_MIN_MHZ : PM_MAX_MHZ,
        .light_sleep_enable = false,   /* P3 is DFS only; automatic light sleep is a later tier */
    };
    esp_err_t e = esp_pm_configure(&cfg);
    if (e == ESP_OK) {
        s_dfs_on = dfs_on;
        ESP_LOGI(TAG, "DFS %s (CPU %d%s MHz)", dfs_on ? "ON" : "off", PM_MAX_MHZ,
                 dfs_on ? "..80 idle" : " fixed");
    } else {
        ESP_LOGW(TAG, "esp_pm_configure -> %s (is CONFIG_PM_ENABLE set?)", esp_err_to_name(e));
    }
    return e;
}

void nocsif_pm_init(void)
{
    nocsif_pm_set_dfs(nocsif_settings_get_i32("dfs_en", 0) != 0);
}

bool nocsif_pm_dfs_enabled(void)
{
    return s_dfs_on;
}
