/*
 * NocSif — Connectivity Governor: the WiFi power policy tick (P1) + cyclic GPS and places (P2–P4).
 * See governor.h for the rules.
 *
 * Inputs are cached getters (wifi.c / weather.c / gnss.c / imu.c); outputs are non-blocking posts to
 * the WiFi worker (nocsif_wifi_request_enable -> esp_wifi_start/stop with the driver's memory retained;
 * nocsif_wifi_request_geo_stamp -> the worker persists), the thread-safe esp_wifi_set_ps, and the GNSS
 * hold flag. Nothing here allocates, blocks, or touches NVS on the tick (settings are cached; setters
 * persist). The self-test build (-DNOCSIF_GOV_SELFTEST=1) shrinks the timers so a COM7 capture shows
 * park -> retry -> re-link and a GPS cycle inside two minutes.
 */
#include "governor.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "wifi.h"
#include "weather.h"
#include "ota.h"              /* §4.10: a GitHub check / download holds the STA */
#include "gnss.h"
#include "imu.h"
#include "settings.h"
#include "reliability.h"

static const char *TAG = "gov";

#define GOV_TICK_US   (1000 * 1000)
#define K_AUTO_OFF    "gov_wifi_ao"    /* 1 = park an idle, unlinked STA (default on)            */
#define K_IDLE_MIN    "gov_wifi_idle"  /* minutes unlinked + idle before parking (default 5)     */
#define K_RETRY_MIN   "gov_wifi_rt"    /* minutes between wake-and-look retries (0 = never; 15)  */
#define K_PS          "gov_wifi_ps"    /* 1 = modem-sleep MAX when linked + idle (default on)    */
#define K_CC_WIFI     "cc.wifi"        /* the CC tile's persisted boot intent (ui.c CC_K_WIFI)   */
#define K_GEO         "gov_geo"        /* 1 = auto-connect by location (default on)              */
#define K_GPS_MIN     "gov_gps_min"    /* GPS check period, minutes: 2/5/10/30 (default 5)       */
#define K_RADIUS      "gov_radius"     /* place radius, metres: 100/200/500 (default 200)        */

#ifndef NOCSIF_GOV_SELFTEST
#define NOCSIF_GOV_SELFTEST 0
#endif
#if NOCSIF_GOV_SELFTEST
#define GOV_RETRY_LOOK_S  20u               /* self-test: park after 20 s, retry every 40 s, look 20 s */
#define GOV_IDLE_S(m)     20u
#define GOV_RETRY_S(m)    40u
#define GOV_GPS_S(m)      60u               /* self-test: a GPS cycle every 60 s                       */
#else
#define GOV_RETRY_LOOK_S  45u               /* a retry wake looks for the saved network this long */
#define GOV_IDLE_S(m)     ((m) * 60u)
#define GOV_RETRY_S(m)    ((m) * 60u)
#define GOV_GPS_S(m)      ((m) * 60u)
#endif
#define GOV_AWAY_IDLE_S   60u               /* outside every known place: park an unlinked STA after this */
#define GOV_FIX_TIMEOUT_S 60u               /* a cycle gives up after this without a valid fix          */
#define GOV_FIX_FRESH_MS  5000u             /* a fix this recent counts for the fence check              */
#define GOV_STILL_GATE_MS (5u * 60u * 1000u) /* skip cycles while the watch has been still this long   */
#define GOV_HYST          1.3f              /* exit a place at 1.3x its radius (no edge flapping)        */
#define GOV_STAMP_MIN_S   60u               /* offer a geo-stamp to the worker at most this often        */

/* ---- P1 WiFi state ------------------------------------------------------------------------ */
static bool     s_inited;
static bool     s_user_on;                  /* intent: the CC tile / persisted cc.wifi                */
static bool     s_parked;                   /* the Governor stopped the STA (intent still on)          */
static bool     s_retry_wake;               /* the current up-time is a parked-retry look, not a user wake */
static bool     s_auto_off = true;
static bool     s_ps       = true;
static uint32_t s_idle_min = 5;
static uint32_t s_retry_min = 15;
static uint32_t s_idle_s;                   /* consecutive seconds unlinked + idle while up            */
static uint32_t s_park_s;                   /* seconds parked (retry countdown)                        */
static int      s_ps_cur = -1;              /* last applied wifi_ps_type_t (-1 = unknown)              */
static volatile nocsif_gov_wifi_state_t s_state;
static char     s_status[2][56];            /* double-buffered status line (tick writes, LVGL reads)   */
static volatile int s_status_i;
static esp_timer_handle_t s_tick;

/* ---- P2/P3 location state ------------------------------------------------------------------ */
typedef enum { GPS_IDLE = 0, GPS_WAIT_FIX } gps_state_t;
static bool     s_geo = true;
static uint32_t s_gps_min  = 5;
static uint32_t s_radius_m = 200;
static gps_state_t s_gps;
static bool     s_gps_hold;                 /* we hold the receiver right now                          */
static uint32_t s_gps_idle_s;               /* seconds since the last cycle ended                      */
static uint32_t s_gps_wait_s;               /* seconds waiting for a fix in this cycle                 */
static bool     s_gps_still_skip;           /* the last due cycle was skipped for stillness            */
static bool     s_had_fix;                  /* at least one fix has been evaluated since boot          */
static int      s_place = -1;               /* saved-network slot we are inside, or -1                 */
static bool     s_place_known;              /* at least one saved network has a learned location       */
static uint32_t s_stamp_age_s = GOV_STAMP_MIN_S;
static char     s_loc[2][64];
static volatile int s_loc_i;

/* ---- helpers ------------------------------------------------------------------------------- */
static void publish_status(const char *s)
{
    int n = (s_status_i + 1) & 1;
    strncpy(s_status[n], s, sizeof s_status[n] - 1);
    s_status[n][sizeof s_status[n] - 1] = '\0';
    s_status_i = n;
}

static void publish_loc(const char *s)
{
    int n = (s_loc_i + 1) & 1;
    strncpy(s_loc[n], s, sizeof s_loc[n] - 1);
    s_loc[n][sizeof s_loc[n] - 1] = '\0';
    s_loc_i = n;
}

static void set_state(nocsif_gov_wifi_state_t st, const char *why)
{
    if (st != s_state) {
        ESP_LOGI(TAG, "wifi: %s", why);
        s_state = st;
    }
}

/* Which holder keeps the radio busy (NULL = none). Order = the most specific first. */
static const char *busy_holder(void)
{
    if (nocsif_wifi_companion_active())                         return "companion";
    if (nocsif_wifi_portal_active())                            return "portal";
    if (nocsif_wifi_ap_active())                                return "access point";
    if (nocsif_wifi_pcap_active())                              return "record";
    if (nocsif_wifi_monitor_active())                           return "capture";
    if (nocsif_weather_state() == NOCSIF_WX_FETCHING)           return "weather";
    if (nocsif_ota_web_busy())                                  return "update";   /* §4.10 GitHub pull */
    if (nocsif_wifi_join_state() == NOCSIF_WIFI_JOIN_JOINING)   return "joining";
    if (nocsif_wifi_scanning())                                 return "scan";
    return NULL;
}

static void set_ps(wifi_ps_type_t ps)
{
    if ((int)ps == s_ps_cur) {
        return;
    }
    if (esp_wifi_set_ps(ps) == ESP_OK) {
        s_ps_cur = (int)ps;
        ESP_LOGI(TAG, "wifi modem-sleep -> %s", ps == WIFI_PS_MAX_MODEM ? "MAX (linked, idle)" : "MIN (active)");
    }
}

/* Equirectangular metres between two points — plenty at fence scale. */
static float dist_m(double la1, double lo1, double la2, double lo2)
{
    const double d2r = 3.14159265358979 / 180.0;
    double mlat = (la1 + la2) * 0.5 * d2r;
    double dy = (la2 - la1) * 111320.0;
    double dx = (lo2 - lo1) * 111320.0 * cos(mlat);
    return (float)sqrt(dx * dx + dy * dy);
}

static void place_name(int idx, char *out, size_t n)
{
    if (idx < 0 || !nocsif_wifi_place_get(idx, NULL, NULL, out, n) || out[0] == '\0') {
        snprintf(out, n, "?");
    }
}

/* ---- P2/P3: fence evaluation on a fresh fix (places are BSSID-keyed, see wifi.h) ------------ */
static void evaluate_fences(const nocsif_gnss_fix_t *fx)
{
    const int n = nocsif_wifi_place_count();
    int   inside = -1;
    float best   = 1e9f;
    for (int i = 0; i < n; i++) {
        int32_t la, lo;
        if (!nocsif_wifi_place_get(i, &la, &lo, NULL, 0)) continue;
        float d = dist_m(fx->lat_deg, fx->lon_deg, la / 1e6, lo / 1e6);
        float r = (i == s_place) ? (float)s_radius_m * GOV_HYST : (float)s_radius_m;   /* hysteresis */
        if (d <= r && d < best) { best = d; inside = i; }
    }
    s_place_known = (n > 0);
    s_had_fix = true;

    if (inside != s_place) {
        char nm[33];
        if (inside >= 0) {
            place_name(inside, nm, sizeof nm);
            ESP_LOGI(TAG, "place: ENTER \"%s\" (%.0f m)%s", nm, (double)best, s_parked ? " -> waking WiFi" : "");
            s_place = inside;
            if (s_parked) {
                nocsif_gov_wifi_wake();
            }
            nocsif_weather_request_refresh(false);        /* P4: opportunistic — WiFi is coming up anyway */
        } else {
            place_name(s_place, nm, sizeof nm);
            ESP_LOGI(TAG, "place: EXIT \"%s\"", nm);
            s_place = -1;
        }
    }

    /* P3 learn: a fresh fix while linked stamps the connected AP as a place — offered at most once a
     * minute, and only when it would change something (an unknown BSSID, or one that moved beyond the
     * radius). nocsif_wifi_connected_place asks the WiFi task for the BSSID (thread-safe, ~ms). */
    if (nocsif_wifi_connected() && s_stamp_age_s >= GOV_STAMP_MIN_S) {
        int ci = nocsif_wifi_connected_place();
        int32_t la, lo;
        bool have = (ci >= 0) && nocsif_wifi_place_get(ci, &la, &lo, NULL, 0);
        if (!have || dist_m(fx->lat_deg, fx->lon_deg, la / 1e6, lo / 1e6) > (float)s_radius_m) {
            nocsif_wifi_request_geo_stamp((int32_t)(fx->lat_deg * 1e6), (int32_t)(fx->lon_deg * 1e6));
            s_stamp_age_s = 0;
        }
    }
}

static void gps_release(void)
{
    if (s_gps_hold) {
        nocsif_gnss_set_hold(false);
        s_gps_hold = false;
    }
    s_gps = GPS_IDLE;
    s_gps_idle_s = 0;
}

static void gps_tick(void)
{
    char line[64];
    s_stamp_age_s++;
    if (!s_geo || nocsif_reliability_safe_mode()) {
        gps_release();
        publish_loc("off");
        return;
    }
    nocsif_gnss_fix_t fx;
    const bool have  = nocsif_gnss_fix_snapshot(&fx);
    const bool fresh = have && fx.valid && fx.fix_age_ms <= GOV_FIX_FRESH_MS;
    if (fresh) {
        evaluate_fences(&fx);                            /* any holder's fixes count — free position */
    }
    const bool live_other = nocsif_gnss_live() && !s_gps_hold;   /* a screen / GPX / wardrive has it live */
    char nm[33];

    switch (s_gps) {
    case GPS_IDLE:
        s_gps_idle_s++;
        if (live_other) {
            s_gps_idle_s = 0;                            /* their fixes serve us; no cycle needed */
            s_gps_still_skip = false;
            break;
        }
        if (s_gps_idle_s >= GOV_GPS_S(s_gps_min)) {
            if (s_had_fix && nocsif_imu_still_ms() >= GOV_STILL_GATE_MS) {
                if (!s_gps_still_skip) ESP_LOGI(TAG, "gps: cycle skipped — watch still for %us (position unchanged)",
                                                (unsigned)(nocsif_imu_still_ms() / 1000u));
                s_gps_still_skip = true;
                s_gps_idle_s = 0;                        /* re-check next period */
                break;
            }
            s_gps_still_skip = false;
            s_gps_idle_s = 0;
            s_gps_wait_s = 0;
            nocsif_gnss_init();                          /* idempotent (lazy worker) */
            nocsif_gnss_set_hold(true);
            s_gps_hold = true;
            s_gps = GPS_WAIT_FIX;
            ESP_LOGI(TAG, "gps: waking for a fix (period %us, timeout %us)", (unsigned)GOV_GPS_S(s_gps_min), (unsigned)GOV_FIX_TIMEOUT_S);
        }
        break;
    case GPS_WAIT_FIX:
        s_gps_wait_s++;
        if (fresh) {
            ESP_LOGI(TAG, "gps: fix in %us (sats %u, hdop %.1f) -> fences evaluated", (unsigned)s_gps_wait_s,
                     (unsigned)fx.sats_used, (double)fx.hdop);
            gps_release();
        } else if (s_gps_wait_s >= GOV_FIX_TIMEOUT_S) {
            ESP_LOGI(TAG, "gps: no fix in %us (indoors?) — sleeping until the next period", (unsigned)GOV_FIX_TIMEOUT_S);
            gps_release();
        }
        break;
    }

    /* Status line. */
    if (s_gps == GPS_WAIT_FIX) {
        snprintf(line, sizeof line, "gps fixing" "\xE2\x80\xA6" " (%us)", (unsigned)s_gps_wait_s);
    } else {
        char where[40];
        if (s_place >= 0) {
            place_name(s_place, nm, sizeof nm);
            snprintf(where, sizeof where, "inside %.24s", nm);
        } else if (s_place_known) {
            strcpy(where, s_had_fix ? "outside known places" : "places known " "\xC2\xB7" " no fix yet");
        } else {
            strcpy(where, "no places learned yet");
        }
        if (live_other) {
            snprintf(line, sizeof line, "%s " "\xC2\xB7" " gps live", where);
        } else if (s_gps_still_skip) {
            snprintf(line, sizeof line, "%s " "\xC2\xB7" " still, gps paused", where);
        } else {
            uint32_t left = GOV_GPS_S(s_gps_min) - s_gps_idle_s;
            snprintf(line, sizeof line, "%s " "\xC2\xB7" " next %u:%02u", where, (unsigned)(left / 60), (unsigned)(left % 60));
        }
    }
    publish_loc(line);
}

/* ---- P1 WiFi tick (+ the P3 policy hooks) --------------------------------------------------- */
static void wifi_tick(void)
{
    char line[56];

    if (!s_user_on || nocsif_reliability_safe_mode()) {
        s_parked = false;
        set_state(NOCSIF_GOV_WIFI_OFF, "off (intent off)");
        publish_status("off");
        return;
    }
    const bool  sta_on = nocsif_wifi_enabled();
    const bool  linked = nocsif_wifi_connected();
    const char *holder = busy_holder();
    const bool  away   = s_geo && s_place_known && s_place < 0 && s_had_fix;   /* outside every known place */

    if (s_parked) {
        if (sta_on) {                        /* someone woke it (tile, weather force, a retry, a place) */
            s_parked = false;
            s_idle_s = 0;
            set_state(NOCSIF_GOV_WIFI_WAKING, s_retry_wake ? "retry: looking for the saved network" : "woken");
        } else {
            s_park_s++;
            const uint32_t rt = s_retry_min ? GOV_RETRY_S(s_retry_min) : 0;
            if (rt && s_park_s >= rt) {
                s_park_s = 0;
                s_retry_wake = true;
                nocsif_wifi_request_enable(true);
                ESP_LOGI(TAG, "wifi: parked %us — waking to look for the saved network (%us)",
                         (unsigned)rt, (unsigned)GOV_RETRY_LOOK_S);
            }
            if (rt) {
                uint32_t left = rt - s_park_s;
                snprintf(line, sizeof line, "parked%s " "\xC2\xB7" " retry in %u:%02u", away ? " (away)" : "",
                         (unsigned)(left / 60), (unsigned)(left % 60));
            } else {
                snprintf(line, sizeof line, "parked%s " "\xC2\xB7" " tap Wi-Fi to wake", away ? " (away)" : "");
            }
            set_state(NOCSIF_GOV_WIFI_PARKED, "parked");
            publish_status(line);
            return;
        }
    }

    if (!sta_on) {                           /* intent on, radio down: airplane, or the worker is bringing it up */
        set_state(NOCSIF_GOV_WIFI_WAKING, "radio down (intent on)");
        publish_status("radio off");
        return;
    }

    if (linked) {
        s_retry_wake = false;
        s_idle_s = 0;
        set_ps((holder == NULL && s_ps) ? WIFI_PS_MAX_MODEM : WIFI_PS_MIN_MODEM);
        if (holder) {
            snprintf(line, sizeof line, "linked " "\xC2\xB7" " busy: %s", holder);
            set_state(NOCSIF_GOV_WIFI_BUSY, "linked, busy");
        } else {
            strcpy(line, s_ps ? "linked " "\xC2\xB7" " idle (modem sleep)" : "linked " "\xC2\xB7" " idle");
            set_state(NOCSIF_GOV_WIFI_LINKED_IDLE, "linked, idle (modem sleep)");
        }
        publish_status(line);
        return;
    }

    /* Up, not linked. */
    set_ps(WIFI_PS_MIN_MODEM);
    if (holder) {
        s_idle_s = 0;
        snprintf(line, sizeof line, "busy: %s", holder);
        set_state(NOCSIF_GOV_WIFI_BUSY, "busy, unlinked");
        publish_status(line);
        return;
    }
    s_idle_s++;
    /* P3: outside every known place there is nothing to find — park quickly; a place-enter wakes it. */
    const uint32_t limit = s_retry_wake ? GOV_RETRY_LOOK_S : (away ? GOV_AWAY_IDLE_S : GOV_IDLE_S(s_idle_min));
    if (s_auto_off && s_idle_s >= limit) {
        nocsif_wifi_request_enable(false);   /* esp_wifi_stop — driver memory retained, nothing re-fragments */
        s_parked = true;
        s_park_s = 0;
        s_idle_s = 0;
        ESP_LOGI(TAG, "wifi: PARKED — no link for %us and nothing holds it%s%s", (unsigned)limit,
                 s_retry_wake ? " (retry look ended)" : "", away ? " (outside known places)" : "");
        s_retry_wake = false;
        set_state(NOCSIF_GOV_WIFI_PARKED, "parked");
        publish_status("parked");
        return;
    }
    if (s_auto_off) {
        uint32_t left = limit - s_idle_s;
        snprintf(line, sizeof line, "searching%s " "\xC2\xB7" " parks in %u:%02u", away ? " (away)" : "",
                 (unsigned)(left / 60), (unsigned)(left % 60));
    } else {
        strcpy(line, "searching");
    }
    set_state(NOCSIF_GOV_WIFI_SEARCHING, "searching (unlinked, idle)");
    publish_status(line);
}

static void tick_cb(void *arg)
{
    (void)arg;
    gps_tick();       /* P2/P3 first: a place-enter this second can wake WiFi below */
    wifi_tick();
}

/* ---- public API ------------------------------------------------------------------------------ */

esp_err_t nocsif_gov_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    s_auto_off  = nocsif_settings_get_i32(K_AUTO_OFF, 1) != 0;
    s_ps        = nocsif_settings_get_i32(K_PS, 1) != 0;
    s_idle_min  = (uint32_t)nocsif_settings_get_i32(K_IDLE_MIN, 5);
    s_retry_min = (uint32_t)nocsif_settings_get_i32(K_RETRY_MIN, 15);
    s_user_on   = nocsif_settings_get_i32(K_CC_WIFI, 1) != 0;
    s_geo       = nocsif_settings_get_i32(K_GEO, 1) != 0;
    s_gps_min   = (uint32_t)nocsif_settings_get_i32(K_GPS_MIN, 5);
    s_radius_m  = (uint32_t)nocsif_settings_get_i32(K_RADIUS, 200);
    if (s_idle_min < 1)  s_idle_min = 1;
    if (s_gps_min < 1)   s_gps_min = 1;
    if (s_radius_m < 50) s_radius_m = 50;
    publish_status("off");
    publish_loc("off");
    /* Seed "places known" so the away-park rule and the status line are right before the first fix. */
    s_place_known = nocsif_wifi_place_count() > 0;
    if (nocsif_reliability_safe_mode()) {
        ESP_LOGW(TAG, "safe mode — policy tick not started");
        s_inited = true;
        return ESP_OK;
    }
    const esp_timer_create_args_t args = {
        .callback        = tick_cb,
        .name            = "gov",
        .dispatch_method = ESP_TIMER_TASK,
    };
    esp_err_t e = esp_timer_create(&args, &s_tick);
    if (e == ESP_OK) {
        e = esp_timer_start_periodic(s_tick, GOV_TICK_US);
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "tick create/start -> %s", esp_err_to_name(e));
        return e;
    }
    s_inited = true;
    ESP_LOGI(TAG, "WiFi policy up: auto-off %s (idle %u min, retry %s), modem-sleep %s, intent %s; "
                  "location %s (gps every %u min, radius %u m, %s)%s",
             s_auto_off ? "on" : "off", (unsigned)s_idle_min, s_retry_min ? "on" : "never",
             s_ps ? "on" : "off", s_user_on ? "on" : "off",
             s_geo ? "on" : "off", (unsigned)s_gps_min, (unsigned)s_radius_m,
             s_place_known ? "places learned" : "no places learned yet",
             NOCSIF_GOV_SELFTEST ? " [SELFTEST timers: park 20 s, retry 40 s, look 20 s, gps 60 s]" : "");
    return ESP_OK;
}

void nocsif_gov_wifi_user(bool on)
{
    s_user_on = on;
    s_parked  = false;
    s_retry_wake = false;
    s_idle_s  = 0;
    s_park_s  = 0;
}

void nocsif_gov_wifi_wake(void)
{
    if (!s_parked) {
        return;
    }
    s_parked = false;
    s_retry_wake = false;
    s_idle_s = 0;
    nocsif_wifi_request_enable(true);
    ESP_LOGI(TAG, "wifi: woken from parked");
}

bool nocsif_gov_wifi_parked(void)                 { return s_parked; }
nocsif_gov_wifi_state_t nocsif_gov_wifi_state(void) { return s_state; }
const char *nocsif_gov_wifi_status_str(void)      { return s_status[s_status_i]; }
const char *nocsif_gov_loc_status_str(void)       { return s_loc[s_loc_i]; }
int  nocsif_gov_place(void)                       { return s_place; }

bool     nocsif_gov_auto_off(void)    { return s_auto_off; }
uint32_t nocsif_gov_idle_min(void)    { return s_idle_min; }
uint32_t nocsif_gov_retry_min(void)   { return s_retry_min; }
bool     nocsif_gov_modem_sleep(void) { return s_ps; }
bool     nocsif_gov_geo(void)         { return s_geo; }
uint32_t nocsif_gov_gps_min(void)     { return s_gps_min; }
uint32_t nocsif_gov_radius_m(void)    { return s_radius_m; }

void nocsif_gov_set_auto_off(bool on)
{
    s_auto_off = on;
    s_idle_s = 0;
    nocsif_settings_set_i32(K_AUTO_OFF, on ? 1 : 0);
    if (!on && s_parked) {
        nocsif_gov_wifi_wake();               /* turning auto-off off un-parks immediately */
    }
}
void nocsif_gov_set_idle_min(uint32_t m)
{
    s_idle_min = m ? m : 1;
    s_idle_s = 0;
    nocsif_settings_set_i32(K_IDLE_MIN, (int32_t)s_idle_min);
}
void nocsif_gov_set_retry_min(uint32_t m)
{
    s_retry_min = m;
    s_park_s = 0;
    nocsif_settings_set_i32(K_RETRY_MIN, (int32_t)m);
}
void nocsif_gov_set_modem_sleep(bool on)
{
    s_ps = on;
    nocsif_settings_set_i32(K_PS, on ? 1 : 0);
    s_ps_cur = -1;                            /* re-apply on the next tick */
}
void nocsif_gov_set_geo(bool on)
{
    s_geo = on;
    nocsif_settings_set_i32(K_GEO, on ? 1 : 0);
    if (!on) {
        gps_release();
        s_place = -1;
    }
    s_gps_idle_s = 0;
}
void nocsif_gov_set_gps_min(uint32_t m)
{
    s_gps_min = m ? m : 1;
    s_gps_idle_s = 0;
    nocsif_settings_set_i32(K_GPS_MIN, (int32_t)s_gps_min);
}
void nocsif_gov_set_radius_m(uint32_t m)
{
    s_radius_m = m < 50 ? 50 : m;
    nocsif_settings_set_i32(K_RADIUS, (int32_t)s_radius_m);
}
