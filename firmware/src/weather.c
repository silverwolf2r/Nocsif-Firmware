/*
 * NocSif — Weather (Open-Meteo over WiFi) worker. See weather.h for the design notes.
 *
 * The worker parks on a task notification with a periodic timeout: a manual request (or a
 * location / init event) wakes it immediately; the timeout drives opportunistic auto-refresh
 * (fetch only when a station link already exists and the cache is stale). A fetch performs one
 * HTTP GET to Open-Meteo, parses the small JSON with cJSON, and publishes a spinlock-guarded
 * record for the LVGL getters.
 */
#include "weather.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "cJSON.h"

#include "wifi.h"
#include "settings.h"
#include "reliability.h"

static const char *TAG = "nocsif_wx";

/* ---- tunables ---------------------------------------------------------------------- */
#define WX_TASK_STACK   8192          /* HTTP fetch + JSON parse headroom                   */
#define WX_TASK_PRIO    3
#define WX_TICK_MS      60000         /* auto-refresh poll cadence                          */
#define WX_STALE_S      900           /* consider the cache stale after 15 min              */
#define WX_WIFI_WAIT_MS 12000         /* how long a forced refresh waits for a station link */
#define WX_HTTP_CAP     4096          /* response buffer (the query returns ~1.5 KB)        */
#define WX_MOVE_UD      3000          /* auto-follow: ~0.003° ≈ 300 m min move to re-store  */
#define WX_NOTE_MIN_US  30000000LL    /* auto-follow: ≤ one NVS write per 30 s while moving  */
#define GF_RADIUS_M     150.0f        /* geofence: "home area" radius                        */
#define GF_FETCH_MIN_US 300000000LL   /* geofence: ≤ one enter-triggered fetch per 5 min     */
#define DEG2RAD         0.017453292519943295

/* NVS keys (namespace "nocsif"; ≤15 chars). Lat/lon stored as signed micro-degrees. */
#define K_WX_LAT     "wx_lat"
#define K_WX_LON     "wx_lon"
#define K_WX_HASLOC  "wx_hasloc"
#define K_WX_UNITS   "wx_units"       /* 0 = °F/mph (default), 1 = °C/km·h                  */
#define K_WX_GFLAT   "wx_gflat"       /* geofence anchor (micro-degrees)                    */
#define K_WX_GFLON   "wx_gflon"
#define K_WX_GFSET   "wx_gfset"       /* 1 once a home anchor has been learned              */

/* ---- module state ------------------------------------------------------------------ */
static portMUX_TYPE       s_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t       s_task;
static nocsif_weather_t   s_wx;                 /* published record (guarded)               */
static bool               s_have;              /* a record has been published              */
static int64_t            s_fetch_us;          /* esp_timer at the last successful fetch    */

/* RAM-cached config (loaded at init; written through to NVS on change). */
static int32_t            s_lat_ud, s_lon_ud;  /* micro-degrees */
static bool               s_has_loc;
static bool               s_metric;

/* Geofence: an auto-learned "last-connected" anchor (persisted) + live inside/outside state (RAM). */
static int32_t            s_gf_lat_ud, s_gf_lon_ud;
static bool               s_gf_set;
static bool               s_gf_inside;
static int64_t            s_gf_fetch_us;       /* last geofence-enter fetch (rate-limit) */

/* Pending request flags (UI thread → worker). */
static volatile bool      s_req;
static volatile bool      s_req_force;

/* Auto-follow: the GNSS worker stashes a fresh position here (cheap) and wakes the weather task,
 * which does the throttled NVS persist off the GPS hot path. */
static volatile bool      s_pending_fix;
static volatile int32_t   s_pending_lat_ud, s_pending_lon_ud;
static int64_t            s_note_us;   /* last adopt time (rate-limit, weather task only) */

/* Peek-chip temperature string (module-owned, stable between updates). */
static char               s_peek[8] = "--\xC2\xB0";

/* ---- small helpers ----------------------------------------------------------------- */

static void set_state(nocsif_weather_state_t st)
{
    taskENTER_CRITICAL(&s_lock);
    s_wx.state = st;
    taskEXIT_CRITICAL(&s_lock);
}

/* Sakamoto's algorithm — weekday of a proleptic-Gregorian date (m in 1..12). */
static const char *wday3(int y, int m, int d)
{
    static const char *n[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const int   t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 1 || m > 12) return "";
    if (m < 3) y -= 1;
    int w = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
    if (w < 0) w += 7;
    return n[w];
}

const char *nocsif_weather_code_text(int c)
{
    switch (c) {
        case 0:                     return "Clear";
        case 1:                     return "Mainly clear";
        case 2:                     return "Partly cloudy";
        case 3:                     return "Overcast";
        case 45: case 48:           return "Fog";
        case 51: case 53: case 55:  return "Drizzle";
        case 56: case 57:           return "Freezing drizzle";
        case 61: case 63: case 65:  return "Rain";
        case 66: case 67:           return "Freezing rain";
        case 71: case 73: case 75:  return "Snow";
        case 77:                    return "Snow grains";
        case 80: case 81: case 82:  return "Showers";
        case 85: case 86:           return "Snow showers";
        case 95:                    return "Thunderstorm";
        case 96: case 99:           return "Thunderstorm, hail";
        default:                    return "\xE2\x80\x93";   /* en-dash (in-font; em-dash is not) */
    }
}

/* Format the peek chip from the current temperature (rounded, unit-agnostic glyph). */
static void format_peek(float temp)
{
    int t = (int)lroundf(temp);
    snprintf(s_peek, sizeof s_peek, "%d\xC2\xB0", t);
}

/* Unit conversions (Open-Meteo already returns in the requested unit; a toggle converts the
 * cached record in place so the UI updates instantly with no re-fetch). */
static float c_to_f(float c) { return c * 9.0f / 5.0f + 32.0f; }
static float f_to_c(float f) { return (f - 32.0f) * 5.0f / 9.0f; }
static float mph_to_kmh(float m) { return m * 1.609344f; }
static float kmh_to_mph(float k) { return k / 1.609344f; }

/* ---- NVS config -------------------------------------------------------------------- */
static void load_config(void)
{
    s_lat_ud  = nocsif_settings_get_i32(K_WX_LAT, 0);
    s_lon_ud  = nocsif_settings_get_i32(K_WX_LON, 0);
    s_has_loc = nocsif_settings_get_i32(K_WX_HASLOC, 0) != 0;
    s_metric  = nocsif_settings_get_i32(K_WX_UNITS, 0) != 0;
    s_gf_lat_ud = nocsif_settings_get_i32(K_WX_GFLAT, 0);
    s_gf_lon_ud = nocsif_settings_get_i32(K_WX_GFLON, 0);
    s_gf_set    = nocsif_settings_get_i32(K_WX_GFSET, 0) != 0;
}

/* Great-circle-ish distance between two micro-degree points (equirectangular approx; fine at the
 * ~150 m geofence scale). */
static float gf_dist_m(int32_t alat_ud, int32_t alon_ud, int32_t blat_ud, int32_t blon_ud)
{
    double dlat = (alat_ud - blat_ud) / 1e6;
    double dlon = (alon_ud - blon_ud) / 1e6;
    double mlat = ((alat_ud + blat_ud) / 2.0) / 1e6;
    double x = dlat * 111320.0;
    double y = dlon * 111320.0 * cos(mlat * DEG2RAD);
    return (float)sqrt(x * x + y * y);
}

/* ---- HTTP fetch + parse ------------------------------------------------------------ */

/* One blocking HTTP GET into `buf` (NUL-terminated). Returns ESP_OK + *out_len on a 200.
 * Plain HTTP (not TLS): Open-Meteo serves the API over port 80, and weather is public data — this
 * avoids the mbedTLS handshake, which on this board (WiFi up) either can't allocate its ~20 KB of
 * record buffers or stalls mid-handshake. No cert bundle, no TLS memory, no handshake round-trips. */
static esp_err_t http_get(const char *url, char *buf, int cap, int *out_len)
{
    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_GET,
        .timeout_ms        = 10000,
        .buffer_size       = 1024,
        .buffer_size_tx    = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return ESP_FAIL;

    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "http open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(c);
        return err;
    }
    esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);

    int total = 0, n;
    while (total < cap - 1 &&
           (n = esp_http_client_read(c, buf + total, cap - 1 - total)) > 0) {
        total += n;
    }
    buf[total] = '\0';
    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    if (status != 200) {
        ESP_LOGW(TAG, "http status %d (%d bytes)", status, total);
        return ESP_FAIL;
    }
    if (total <= 0) return ESP_FAIL;
    *out_len = total;
    return ESP_OK;
}

/* Safely read a numeric JSON member (returns dflt if absent / not a number). */
static double jnum(const cJSON *obj, const char *key, double dflt)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(it) ? it->valuedouble : dflt;
}

/* Parse an Open-Meteo forecast document into *w (units already match the query). */
static bool parse_forecast(const char *json, bool metric, nocsif_weather_t *w)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) { ESP_LOGW(TAG, "json parse failed"); return false; }

    bool ok = false;
    const cJSON *cur = cJSON_GetObjectItemCaseSensitive(root, "current");
    if (cJSON_IsObject(cur)) {
        memset(w, 0, sizeof *w);
        w->metric   = metric;
        w->temp     = (float)jnum(cur, "temperature_2m", 0);
        w->feels    = (float)jnum(cur, "apparent_temperature", jnum(cur, "temperature_2m", 0));
        w->humidity = (int)jnum(cur, "relative_humidity_2m", 0);
        w->wind     = (float)jnum(cur, "wind_speed_10m", 0);
        w->code     = (int)jnum(cur, "weather_code", -1);
        w->is_day   = jnum(cur, "is_day", 1) != 0;

        /* Daily arrays (weather_code / temperature_2m_max / _min / time), first 3 days. */
        const cJSON *daily = cJSON_GetObjectItemCaseSensitive(root, "daily");
        const cJSON *dc  = cJSON_GetObjectItemCaseSensitive(daily, "weather_code");
        const cJSON *dhi = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_max");
        const cJSON *dlo = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_min");
        const cJSON *dt  = cJSON_GetObjectItemCaseSensitive(daily, "time");
        for (int i = 0; i < 3; i++) {
            const cJSON *ec = cJSON_IsArray(dc)  ? cJSON_GetArrayItem(dc,  i) : NULL;
            const cJSON *eh = cJSON_IsArray(dhi) ? cJSON_GetArrayItem(dhi, i) : NULL;
            const cJSON *el = cJSON_IsArray(dlo) ? cJSON_GetArrayItem(dlo, i) : NULL;
            const cJSON *et = cJSON_IsArray(dt)  ? cJSON_GetArrayItem(dt,  i) : NULL;
            w->d_code[i] = cJSON_IsNumber(ec) ? ec->valueint : -1;
            w->d_hi[i]   = cJSON_IsNumber(eh) ? (float)eh->valuedouble : 0;
            w->d_lo[i]   = cJSON_IsNumber(el) ? (float)el->valuedouble : 0;
            w->d_day[i][0] = '\0';
            if (cJSON_IsString(et) && et->valuestring) {
                int y, m, d;
                if (sscanf(et->valuestring, "%d-%d-%d", &y, &m, &d) == 3) {
                    const char *wd = (i == 0) ? "Today" : wday3(y, m, d);
                    strlcpy(w->d_day[i], wd, sizeof w->d_day[i]);
                }
            }
        }
        w->valid = true;
        ok = true;
    }
    cJSON_Delete(root);
    return ok;
}

/* Publish a freshly-parsed record (called on the worker). */
static void publish(const nocsif_weather_t *w)
{
    taskENTER_CRITICAL(&s_lock);
    s_wx = *w;
    s_wx.state = NOCSIF_WX_OK;
    s_have = true;
    taskEXIT_CRITICAL(&s_lock);
    s_fetch_us = esp_timer_get_time();
    format_peek(w->temp);
}

/* The actual fetch: gate on location + WiFi, GET, parse, publish. */
static void do_fetch(bool force)
{
    if (!s_has_loc)                     { set_state(NOCSIF_WX_NOLOC);  return; }
    if (nocsif_reliability_safe_mode()) { set_state(NOCSIF_WX_NOWIFI); return; }

    if (!nocsif_wifi_connected()) {
        if (!force) { set_state(NOCSIF_WX_NOWIFI); return; }
        nocsif_wifi_request_enable(true);        /* explicit tap: bring the radio up + wait */
        int waited = 0;
        while (!nocsif_wifi_connected() && waited < WX_WIFI_WAIT_MS) {
            vTaskDelay(pdMS_TO_TICKS(250));
            waited += 250;
        }
        if (!nocsif_wifi_connected()) { set_state(NOCSIF_WX_NOWIFI); return; }
    }

    set_state(NOCSIF_WX_FETCHING);

    double lat = s_lat_ud / 1e6, lon = s_lon_ud / 1e6;
    bool metric = s_metric;
    char url[320];
    snprintf(url, sizeof url,
             "http://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,relative_humidity_2m,apparent_temperature,is_day,"
             "weather_code,wind_speed_10m"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min"
             "&timezone=auto&forecast_days=3&temperature_unit=%s&wind_speed_unit=%s",
             lat, lon, metric ? "celsius" : "fahrenheit", metric ? "kmh" : "mph");

    char *buf = malloc(WX_HTTP_CAP);
    if (!buf) { set_state(NOCSIF_WX_ERR); return; }

    int len = 0;
    esp_err_t err = http_get(url, buf, WX_HTTP_CAP, &len);
    if (err == ESP_OK) {
        nocsif_weather_t w;
        if (parse_forecast(buf, metric, &w)) {
            publish(&w);
            /* Learn/refresh the last-connected anchor: WiFi just connected here. Re-persist only when
             * it moves out of the current radius (a genuinely new WiFi spot). */
            if (!s_gf_set || gf_dist_m(s_lat_ud, s_lon_ud, s_gf_lat_ud, s_gf_lon_ud) > GF_RADIUS_M) {
                s_gf_lat_ud = s_lat_ud;
                s_gf_lon_ud = s_lon_ud;
                s_gf_set = true;
                nocsif_settings_set_i32(K_WX_GFLAT, s_gf_lat_ud);
                nocsif_settings_set_i32(K_WX_GFLON, s_gf_lon_ud);
                nocsif_settings_set_i32(K_WX_GFSET, 1);
                ESP_LOGI(TAG, "geofence: last-connected anchor @ %.5f, %.5f",
                         s_gf_lat_ud / 1e6, s_gf_lon_ud / 1e6);
            }
            s_gf_inside = true;   /* we just fetched here — we're at the anchor */
            ESP_LOGI(TAG, "fetch ok: %.0f\xc2\xb0 code %d (stack hwm %u)",
                     (double)w.temp, w.code, (unsigned)uxTaskGetStackHighWaterMark(NULL));
        } else {
            set_state(NOCSIF_WX_ERR);
        }
    } else {
        set_state(NOCSIF_WX_ERR);
    }
    free(buf);
}

/* ---- worker task ------------------------------------------------------------------- */
static bool cache_stale(void)
{
    if (!s_have) return true;
    int64_t age = (esp_timer_get_time() - s_fetch_us) / 1000000;
    return age >= WX_STALE_S;
}

/* Handle a GNSS-stashed position. Runs on the WEATHER task, so NVS/flash never touches the GPS hot
 * path. Two jobs: (1) geofence enter → one forced fetch (evaluated on EVERY fix, precise);
 * (2) auto-follow — persist the fetch location on a meaningful move (rate-limited). */
static void adopt_fix(int32_t lat_ud, int32_t lon_ud)
{
    /* (1) geofence — re-entering the last-connected area wakes WiFi for one forced refresh. */
    if (s_gf_set) {
        bool inside = gf_dist_m(lat_ud, lon_ud, s_gf_lat_ud, s_gf_lon_ud) <= GF_RADIUS_M;
        if (inside && !s_gf_inside) {
            int64_t now2 = esp_timer_get_time();
            if (!s_gf_fetch_us || (now2 - s_gf_fetch_us) >= GF_FETCH_MIN_US) {
                s_gf_fetch_us = now2;
                s_req = true;
                s_req_force = true;                 /* wake WiFi + fetch on arrival */
                ESP_LOGI(TAG, "geofence: re-entered last-connected area -> forced refresh");
            }
        }
        s_gf_inside = inside;
    }

    /* (2) auto-follow — persist the weather fetch location only on a meaningful move (rate-limited). */
    bool first = !s_has_loc;
    bool moved = first ||
                 labs((long)(lat_ud - s_lat_ud)) > WX_MOVE_UD ||
                 labs((long)(lon_ud - s_lon_ud)) > WX_MOVE_UD;
    if (!moved) return;
    int64_t now = esp_timer_get_time();
    if (!first && s_note_us && (now - s_note_us) < WX_NOTE_MIN_US) return;
    s_note_us = now;

    s_lat_ud = lat_ud;
    s_lon_ud = lon_ud;
    bool was = s_has_loc;
    s_has_loc = true;
    nocsif_settings_set_i32(K_WX_LAT, s_lat_ud);
    nocsif_settings_set_i32(K_WX_LON, s_lon_ud);
    if (!was) nocsif_settings_set_i32(K_WX_HASLOC, 1);
    ESP_LOGI(TAG, "auto-follow: location <- %.5f, %.5f", lat_ud / 1e6, lon_ud / 1e6);
    /* the fetch is kicked by the auto-refresh branch below (once we return to the loop) */
}

static void wx_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(WX_TICK_MS));

        if (s_pending_fix) {
            s_pending_fix = false;
            adopt_fix(s_pending_lat_ud, s_pending_lon_ud);
        }

        if (s_req) {
            s_req = false;
            do_fetch(s_req_force);
        } else if (s_has_loc && nocsif_wifi_connected() && cache_stale()) {
            do_fetch(false);   /* opportunistic — never forces the radio up */
        }
    }
}

/* ---- public API -------------------------------------------------------------------- */
esp_err_t nocsif_weather_init(void)
{
    if (s_task) return ESP_OK;                    /* idempotent */
    if (nocsif_reliability_safe_mode()) {
        ESP_LOGW(TAG, "safe mode — weather worker disabled");
        return ESP_OK;
    }
    load_config();
    s_wx.state = s_has_loc ? NOCSIF_WX_IDLE : NOCSIF_WX_NOLOC;
    if (xTaskCreate(wx_task, "nocsif_wx", WX_TASK_STACK, NULL, WX_TASK_PRIO, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        s_task = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool nocsif_weather_available(void)
{
    return s_task != NULL;
}

bool nocsif_weather_has_location(void)
{
    return s_has_loc;
}

bool nocsif_weather_get_location(double *lat, double *lon)
{
    if (!s_has_loc) return false;
    if (lat) *lat = s_lat_ud / 1e6;
    if (lon) *lon = s_lon_ud / 1e6;
    return true;
}

void nocsif_weather_set_location(double lat, double lon)
{
    s_lat_ud  = (int32_t)lround(lat * 1e6);
    s_lon_ud  = (int32_t)lround(lon * 1e6);
    s_has_loc = true;
    nocsif_settings_set_i32(K_WX_LAT, s_lat_ud);
    nocsif_settings_set_i32(K_WX_LON, s_lon_ud);
    nocsif_settings_set_i32(K_WX_HASLOC, 1);
    nocsif_weather_request_refresh(true);         /* the user just set it — fetch now */
}

void nocsif_weather_note_fix(double lat, double lon)
{
    /* CHEAP by design — this runs on the GNSS worker's hot path: no NVS, no blocking. Just stash
     * the latest position and wake the weather task (which does the geofence eval + throttled
     * persist). Skip exact repeats so a stationary receiver doesn't wake the task every second;
     * every real change is forwarded so the geofence sees precise crossings. */
    int32_t lat_ud = (int32_t)lround(lat * 1e6);
    int32_t lon_ud = (int32_t)lround(lon * 1e6);
    if (lat_ud == s_pending_lat_ud && lon_ud == s_pending_lon_ud) return;
    s_pending_lat_ud = lat_ud;
    s_pending_lon_ud = lon_ud;
    s_pending_fix = true;
    if (s_task) xTaskNotifyGive(s_task);
}

bool nocsif_weather_metric(void)
{
    return s_metric;
}

void nocsif_weather_set_metric(bool metric)
{
    if (metric == s_metric) return;
    s_metric = metric;
    nocsif_settings_set_i32(K_WX_UNITS, metric ? 1 : 0);

    /* Convert the cached record in place (no re-fetch) so the UI updates instantly. */
    taskENTER_CRITICAL(&s_lock);
    if (s_have) {
        s_wx.temp  = metric ? f_to_c(s_wx.temp)  : c_to_f(s_wx.temp);
        s_wx.feels = metric ? f_to_c(s_wx.feels) : c_to_f(s_wx.feels);
        s_wx.wind  = metric ? mph_to_kmh(s_wx.wind) : kmh_to_mph(s_wx.wind);
        for (int i = 0; i < 3; i++) {
            s_wx.d_hi[i] = metric ? f_to_c(s_wx.d_hi[i]) : c_to_f(s_wx.d_hi[i]);
            s_wx.d_lo[i] = metric ? f_to_c(s_wx.d_lo[i]) : c_to_f(s_wx.d_lo[i]);
        }
        s_wx.metric = metric;
    }
    float t = s_wx.temp;
    bool have = s_have;
    taskEXIT_CRITICAL(&s_lock);
    if (have) format_peek(t);
}

void nocsif_weather_request_refresh(bool force_wifi)
{
    s_req_force = force_wifi;
    s_req = true;
    if (s_task) xTaskNotifyGive(s_task);
}

bool nocsif_weather_snapshot(nocsif_weather_t *out)
{
    if (!out) return false;
    taskENTER_CRITICAL(&s_lock);
    bool have = s_have || s_wx.state != NOCSIF_WX_IDLE;
    *out = s_wx;
    taskEXIT_CRITICAL(&s_lock);
    if (s_have)
        out->age_s = (uint32_t)((esp_timer_get_time() - s_fetch_us) / 1000000);
    else
        out->age_s = 0;
    return have;
}

nocsif_weather_state_t nocsif_weather_state(void)
{
    taskENTER_CRITICAL(&s_lock);
    nocsif_weather_state_t st = s_wx.state;
    taskEXIT_CRITICAL(&s_lock);
    return st;
}

const char *nocsif_weather_temp_str(void)
{
    return s_peek;
}
