/*
 * NocSif — DFS toggle implementation. See pm.h.
 */
#include "pm.h"

#include "settings.h"
#include "trmode.h"     /* Travel Router mode pins the CPU at full speed */

#include "esp_pm.h"
#include "esp_log.h"

static const char *TAG = "nocsif_pm";

#define PM_MAX_MHZ 240
#define PM_MIN_MHZ 80    /* lowest clock DFS drops to while idle */

static bool s_dfs_on;

esp_err_t nocsif_pm_set_dfs(bool dfs_on)
{
    /* Travel Router mode: forwarding runs on the CPU, and Wi-Fi only holds the APB-max lock, so with DFS the
     * CPU forwarded at 80 MHz between bursts. The mode keeps 240 MHz; the saved preference is untouched and
     * applies again after exit. */
    if (dfs_on && nocsif_trmode_active()) {
        ESP_LOGI(TAG, "DFS held off: Travel Router mode keeps the CPU at %d MHz", PM_MAX_MHZ);
        dfs_on = false;
    }
    esp_pm_config_t cfg = {
        .max_freq_mhz       = PM_MAX_MHZ,
        .min_freq_mhz       = dfs_on ? PM_MIN_MHZ : PM_MAX_MHZ,
        .light_sleep_enable = false,   /* only DFS is handled here, not automatic light sleep */
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
