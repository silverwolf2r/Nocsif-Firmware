/*
 * NocSif — Connectivity Governor, P1 (PLAN §4.6 / §4.17 C): radio power as ACTIVITY leases over drivers
 * that stay RESIDENT.
 *
 * THE ONE RULE: the Governor toggles activity and power states — it never
 * deinits a driver, because runtime teardown would defragment the
 * contiguous internal-DMA pool that the whole coexistence architecture
 * assumes is fixed at boot. So:
 *   - WiFi : start/stop through the STA worker (driver memory retained) plus
 *            modem-sleep when associated-but-idle. Deinit is never called.
 *   - BLE  : the controller is resident by design; scanning/adverts are
 *            already per-screen activity. P1 only reports.
 *   - GNSS : the rail is already per-screen / background-holder activity
 *            (Live Fix, GPX, Wardrive, geofence). P2 adds the fix-then-sleep
 *            duty cycle and the geofence engine.
 *
 * P1 WiFi policy (a 1 s tick over cached getters — no call-site sweep):
 * user intent (the CC tile / persisted "cc_wifi") is the input, the STA
 * power state is the output.
 *   - BUSY        a holder is active (monitor/capture, PCAP, AP, captive
 *                 portal, companion, a weather fetch, a join in progress)
 *                 -> radio up, modem-sleep MIN.
 *   - LINKED·IDLE associated, nothing holds it -> stay associated (the link
 *                 IS the presence signal) with modem-sleep MAX. Never parked
 *                 on an indoor GPS drop.
 *   - SEARCHING   up, not associated, nothing holds it -> counts idle
 *                 seconds; after `idle_min` it PARKS (esp_wifi_stop).
 *   - PARKED      every `retry_min` it wakes to look for the saved network,
 *                 then re-parks. A holder or a tile tap wakes it any time;
 *                 user intent stays ON. P2 replaces the retry timer with the
 *                 GPS geofence ("near home -> wake").
 * Airplane mode and the user's OFF are honoured as intent OFF (no parking, no retries).
 *
 * Threading: the tick runs on the esp_timer task; every action is a
 * non-blocking post to the WiFi worker or a thread-safe esp_wifi call; every
 * input is a cached getter (no NVS on the tick).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NOCSIF_GOV_WIFI_OFF = 0,     /* intent off (user / airplane / safe mode) */
    NOCSIF_GOV_WIFI_WAKING,      /* intent on, STA coming up (worker in flight) */
    NOCSIF_GOV_WIFI_BUSY,        /* a holder is active — radio up, modem-sleep MIN */
    NOCSIF_GOV_WIFI_LINKED_IDLE, /* associated, nothing holds it — modem-sleep MAX */
    NOCSIF_GOV_WIFI_SEARCHING,   /* up, unlinked, idle — counting down to park */
    NOCSIF_GOV_WIFI_PARKED,      /* STA stopped by the Governor; intent still on */
} nocsif_gov_wifi_state_t;

/* Starts the policy tick (reads persisted settings + the "cc_wifi" intent).
 * Idempotent; no-op in safe mode. Call once at boot after nocsif_wifi_init()
 * and nocsif_weather_init(). */
esp_err_t nocsif_gov_init(void);

/* Sets user intent for WiFi (called by the CC tile / airplane path). */
void nocsif_gov_wifi_user(bool on);

/* Wakes a PARKED STA now (a tile tap while parked; a feature that needs the link). No-op otherwise. */
void nocsif_gov_wifi_wake(void);

/* True while the Governor holds the STA stopped (intent on, radio parked). LVGL-safe. */
bool nocsif_gov_wifi_parked(void);

nocsif_gov_wifi_state_t nocsif_gov_wifi_state(void);

/* Short live status line for Settings > Connectivity (e.g. "linked · idle
 * (modem sleep)", "searching · parks in 3:20", "parked · retry in 12:00",
 * "busy · capture", "off"). Module-owned buffer; LVGL-safe. */
const char *nocsif_gov_wifi_status_str(void);

/* ---- settings (persisted; cached) ---------------------------------------------------------- */
bool     nocsif_gov_auto_off(void);            void nocsif_gov_set_auto_off(bool on);      /* park an idle unlinked STA */
uint32_t nocsif_gov_idle_min(void);            void nocsif_gov_set_idle_min(uint32_t m);   /* 1 / 5 / 15 / 30            */
uint32_t nocsif_gov_retry_min(void);           void nocsif_gov_set_retry_min(uint32_t m);  /* 0 (never) / 5 / 15 / 30    */
bool     nocsif_gov_modem_sleep(void);         void nocsif_gov_set_modem_sleep(bool on);   /* MAX modem-sleep when linked+idle */

/* ---- P2/P3/P4: location — cyclic GPS + places (fences around saved networks) -------------- *
 * P2: every `gps_min` the Governor holds the receiver until one fresh fix or
 *     a 60 s timeout, evaluates the fences, then releases. Skipped while the
 *     watch has been still for 5 min (position can't have changed); fixes
 *     any foreground session (Live Fix / GPX / Wardrive) produces are
 *     evaluated for free. Fences are circles of `radius_m` around each saved
 *     network's learned location, with 1.3x hysteresis on exit; no fix means
 *     the state is held (the indoor-hold rule).
 * P3: location is auto-learned — a fresh fix while linked stamps the
 *     connected profile (the WiFi worker persists it). Entering a place
 *     wakes a parked STA at once; outside every known place an unlinked STA
 *     parks after 60 s (GPS is the wake); the timer retry stays as the
 *     indoor fallback (a fix rarely lands indoors).
 * P4: weather gets an opportunistic refresh on place-enter; Settings >
 *     Connectivity shows a live "Location" line plus the three knobs below. */
bool     nocsif_gov_geo(void);                 void nocsif_gov_set_geo(bool on);           /* auto-connect by location   */
uint32_t nocsif_gov_gps_min(void);             void nocsif_gov_set_gps_min(uint32_t m);    /* 2 / 5 / 10 / 30 min        */
uint32_t nocsif_gov_radius_m(void);            void nocsif_gov_set_radius_m(uint32_t m);   /* 100 / 200 / 500 m          */
int      nocsif_gov_place(void);               /* wifi.h PLACE index (BSSID-keyed) we are inside, or -1        */
const char *nocsif_gov_loc_status_str(void);   /* "inside <ssid> · next check 4:10" etc. Module-owned; LVGL-safe */

#ifdef __cplusplus
}
#endif
