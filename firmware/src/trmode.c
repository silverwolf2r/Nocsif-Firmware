/*
 * trmode.c — Travel Router mode (see trmode.h, docs/design/travel-router-mode.md).
 */
#include "trmode.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"

#include "settings.h"

static const char *TAG = "trmode";

#define K_TRMODE "tr_mode"

static bool s_active;

void nocsif_trmode_boot_init(bool safe_mode)
{
    const bool flag = nocsif_settings_get_i32(K_TRMODE, 0) != 0;
    s_active = flag && !safe_mode;
    if (flag && safe_mode) {
        ESP_LOGW(TAG, "Travel Router mode is saved, but safe mode boots a normal watch");
    } else if (s_active) {
        ESP_LOGW(TAG, "Travel Router mode: Wi-Fi gets the radio memory; paused: %s", nocsif_trmode_paused_text());
    }
}

bool nocsif_trmode_active(void) { return s_active; }

/* App ids (and their children "id.*") paused in the mode. "usb.download" is Wi-Fi -> SD and stays. */
static const char *const k_paused[] = {
    "ble", "phone", "notif", "hunt", "controllers", "ctrl",   /* Bluetooth + Signal Hunt + controllers */
    "usb",                                                  /* File Share, serial, HID keyboard, macros */
    "lora", "nfc", "gnss", "autom",                         /* Sub-GHz, NFC, Location, GeoFence         */
    "system.mic", "voice", "tuner", "pianotuner",           /* microphone                               */
    "audio.player",                                         /* SD playback                              */
    "system.companion",                                     /* Web control / phone mirror (own project) */
};

static bool id_has_prefix(const char *id, const char *p)
{
    const size_t n = strlen(p);
    return strncmp(id, p, n) == 0 && (id[n] == '\0' || id[n] == '.');
}

bool nocsif_trmode_app_paused(const char *app_id)
{
    if (!s_active || app_id == NULL) return false;
    if (strcmp(app_id, "usb.download") == 0) return false;
    for (size_t i = 0; i < sizeof k_paused / sizeof k_paused[0]; i++) {
        if (id_has_prefix(app_id, k_paused[i])) return true;
    }
    return false;
}

const char *nocsif_trmode_paused_text(void)
{
    return "Bluetooth, File Share & USB, Web control, Sub-GHz, NFC, Location, microphone, Audio Player, "
           "Signal Hunt and background alerts";
}

void nocsif_trmode_set_and_restart(bool on)
{
    nocsif_settings_set_i32(K_TRMODE, on ? 1 : 0);
    ESP_LOGW(TAG, "%s Travel Router mode — restarting", on ? "entering" : "leaving");
    vTaskDelay(pdMS_TO_TICKS(150));                 /* let the log line out */
    esp_restart();
}
