/*
 * NocSif — PCF85063A RTC driver implementation. See rtc.h.
 *
 * Talks to the chip directly at the register level (no vendor library). Time is kept as
 * BCD across seven consecutive registers (seconds through years) and read with a single
 * burst transaction, which the chip freezes for the duration so the fields stay
 * consistent with each other. Setting the time stops the clock, writes all seven
 * registers in one go, then restarts it — the datasheet's documented safe sequence,
 * which also clears the integrity flag.
 *
 * If the integrity flag is set at boot (fresh unit, or the backup battery was ever
 * lost), the time is meaningless, so it's seeded once from the firmware's own build
 * timestamp just so the UI shows something plausible until a real time source exists.
 */
#include "rtc.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "i2c_scan.h"          /* nocsif_i2c_bus() */
#include "driver/i2c_master.h"
#include "esp_log.h"

#define PCF_ADDR            0x51
#define PCF_SCL_HZ          400000
#define PCF_TIMEOUT_MS      100

#define REG_CTRL1           0x00
#define REG_SECONDS         0x04        /* first of the 7 consecutive time registers */
#define CTRL1_STOP          (1u << 5)   /* 1 = clock stopped */
#define CTRL1_12_24         (1u << 1)   /* 0 = 24-hour mode */
#define SEC_OS              (1u << 7)   /* 1 = oscillator has stopped, time is unreliable */

/* Middot glyph for the date line, duplicated here so this hardware driver doesn't need
 * to pull in the UI theme header. */
#define RTC_DOT             "\xC2\xB7"

static const char *TAG = "rtc";

static i2c_master_dev_handle_t s_dev;
static bool      s_valid;                    /* whether s_tm can be trusted */
static struct tm s_tm;                       /* last decoded time */
static char      s_clock[8]  = "--:--";      /* cached "HH:MM" string */
static char      s_date[32]  = "-- " RTC_DOT " --";  /* cached date line */

/* ---- BCD + helpers --------------------------------------------------------- */
static inline uint8_t bcd2dec(uint8_t b) { return (uint8_t)((b >> 4) * 10 + (b & 0x0F)); }
static inline uint8_t dec2bcd(uint8_t d) { return (uint8_t)(((d / 10) << 4) | (d % 10)); }

/* Sakamoto's algorithm for day-of-week (0=Sunday), used when writing a new date so the
 * chip's weekday register stays consistent with the actual date. m is 1-12. */
static int day_of_week(int y, int m, int d)
{
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

/* Sanity-check that decoded fields are in range before trusting them — a garbled I2C
 * transfer could otherwise pass the integrity flag check yet hand back nonsense that
 * would crash strftime's name-table lookups. */
static bool tm_in_range(const struct tm *t)
{
    return t->tm_sec  >= 0 && t->tm_sec  <= 59 &&
           t->tm_min  >= 0 && t->tm_min  <= 59 &&
           t->tm_hour >= 0 && t->tm_hour <= 23 &&
           t->tm_mday >= 1 && t->tm_mday <= 31 &&
           t->tm_mon  >= 0 && t->tm_mon  <= 11 &&
           t->tm_wday >= 0 && t->tm_wday <= 6;
}

static esp_err_t reg_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, PCF_TIMEOUT_MS);
}

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof buf, PCF_TIMEOUT_MS);
}

/* Read a register, apply (cur & ~mask) | bits, and write it back only if it changed. */
static esp_err_t reg_update(uint8_t reg, uint8_t mask, uint8_t bits)
{
    uint8_t cur;
    esp_err_t err = reg_read(reg, &cur);
    if (err != ESP_OK) return err;
    uint8_t next = (uint8_t)((cur & ~mask) | bits);
    if (next == cur) return ESP_OK;
    return reg_write(reg, next);
}

/* ---- time read / decode ---------------------------------------------------- */
/* Burst-read all 7 time registers in one transaction and decode into *out.
 * *reliable reports whether the integrity flag was clear. */
static esp_err_t read_time(struct tm *out, bool *reliable)
{
    uint8_t ptr = REG_SECONDS;
    uint8_t r[7];
    esp_err_t err = i2c_master_transmit_receive(s_dev, &ptr, 1, r, sizeof r, PCF_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    memset(out, 0, sizeof *out);
    out->tm_sec  = bcd2dec(r[0] & 0x7F);
    out->tm_min  = bcd2dec(r[1] & 0x7F);
    out->tm_hour = bcd2dec(r[2] & 0x3F);          /* only meaningful in 24-hour mode */
    out->tm_mday = bcd2dec(r[3] & 0x3F);
    out->tm_wday = r[4] & 0x07;                    /* already 0=Sunday, matches struct tm */
    out->tm_mon  = (int)bcd2dec(r[5] & 0x1F) - 1;  /* struct tm months are 0-11 */
    out->tm_year = 2000 + (int)bcd2dec(r[6]) - 1900;
    out->tm_isdst = -1;
    /* Trust the read only if both the chip's own integrity flag is clear and the
     * decoded fields are actually in range. */
    *reliable = ((r[0] & SEC_OS) == 0) && tm_in_range(out);
    return ESP_OK;
}

/* Write a new time using the chip's documented safe sequence: stop the clock, write all
 * seven time registers in one transaction, then restart it. */
static esp_err_t set_time(const struct tm *t)
{
    esp_err_t err;
    if ((err = reg_write(REG_CTRL1, CTRL1_STOP)) != ESP_OK) {
        return err;
    }
    uint8_t buf[8];
    buf[0] = REG_SECONDS;                                          /* auto-increment base */
    buf[1] = (uint8_t)(dec2bcd((uint8_t)t->tm_sec) & 0x7F);        /* also clears the OS flag */
    buf[2] = (uint8_t)(dec2bcd((uint8_t)t->tm_min) & 0x7F);
    buf[3] = (uint8_t)(dec2bcd((uint8_t)t->tm_hour) & 0x3F);
    buf[4] = (uint8_t)(dec2bcd((uint8_t)t->tm_mday) & 0x3F);
    buf[5] = (uint8_t)(t->tm_wday & 0x07);
    buf[6] = (uint8_t)(dec2bcd((uint8_t)(t->tm_mon + 1)) & 0x1F);
    buf[7] = (uint8_t)(dec2bcd((uint8_t)((t->tm_year + 1900) - 2000)) & 0xFF);
    if ((err = i2c_master_transmit(s_dev, buf, sizeof buf, PCF_TIMEOUT_MS)) != ESP_OK) {
        reg_write(REG_CTRL1, 0x00);                               /* try to restart it anyway */
        return err;
    }
    return reg_write(REG_CTRL1, 0x00);                            /* release the stop, resume running */
}

/* Parse the compiler's build-date/time macros into a struct tm, computing the weekday.
 * Returns false if either macro can't be parsed. */
static bool build_time(struct tm *out)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mmm[4] = { 0 };
    int dd = 0, yyyy = 0, hh = 0, mi = 0, ss = 0;
    if (sscanf(__DATE__, "%3s %d %d", mmm, &dd, &yyyy) != 3) return false;
    if (sscanf(__TIME__, "%d:%d:%d", &hh, &mi, &ss) != 3)    return false;
    const char *p = strstr(months, mmm);
    if (p == NULL) return false;
    int mon0 = (int)((p - months) / 3);                          /* 0-11 */
    memset(out, 0, sizeof *out);
    out->tm_year  = yyyy - 1900;
    out->tm_mon   = mon0;
    out->tm_mday  = dd;
    out->tm_hour  = hh;
    out->tm_min   = mi;
    out->tm_sec   = ss;
    out->tm_wday  = day_of_week(yyyy, mon0 + 1, dd);
    out->tm_isdst = -1;
    return true;
}

/* ---- cached display strings ------------------------------------------------ */
static void refresh_strings(void)
{
    if (s_valid) {
        char wd[8] = { 0 }, mo[8] = { 0 };
        strftime(s_clock, sizeof s_clock, "%H:%M", &s_tm);
        strftime(wd, sizeof wd, "%a", &s_tm);                    /* abbreviated weekday */
        strftime(mo, sizeof mo, "%b", &s_tm);                    /* abbreviated month */
        snprintf(s_date, sizeof s_date, "%s " RTC_DOT " %s %d %d",
                 wd, mo, s_tm.tm_mday, s_tm.tm_year + 1900);
    } else {
        strcpy(s_clock, "--:--");
        strcpy(s_date, "-- " RTC_DOT " --");
    }
}

/* ---- public API ------------------------------------------------------------ */
esp_err_t nocsif_rtc_init(void)
{
    if (s_dev != NULL) {
        return ESP_OK;
    }
    i2c_master_bus_handle_t bus = nocsif_i2c_bus();
    if (bus == NULL) {
        ESP_LOGE(TAG, "shared I2C bus not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = PCF_ADDR,
        .scl_speed_hz    = PCF_SCL_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (err != ESP_OK) {
        s_dev = NULL;
        ESP_LOGE(TAG, "add_device(0x%02X) failed: %s", PCF_ADDR, esp_err_to_name(err));
        return err;
    }

    /* Ensure 24-hour mode and that the clock is running (clear STOP + 12_24; leave other bits,
     * e.g. CAP_SEL load-cap select, as programmed). */
    reg_update(REG_CTRL1, (uint8_t)(CTRL1_STOP | CTRL1_12_24), 0);

    struct tm t;
    bool reliable = false;
    err = read_time(&t, &reliable);
    if (err == ESP_OK && reliable) {
        s_tm = t;
        s_valid = true;
        ESP_LOGI(TAG, "PCF85063A @ 0x%02X: time valid (OS clear) %04d-%02d-%02d %02d:%02d:%02d",
                 PCF_ADDR, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                 t.tm_hour, t.tm_min, t.tm_sec);
    } else {
        /* Time isn't trustworthy — seed from the build timestamp instead, logging exactly
         * which step failed if this doesn't work. */
        struct tm seed;
        esp_err_t serr = ESP_OK, rerr = ESP_OK;
        if (!build_time(&seed)) {
            s_valid = false;
            ESP_LOGE(TAG, "PCF85063A @ 0x%02X: time unreliable (OS set) and build-timestamp parse "
                          "failed — clock shows --:--", PCF_ADDR);
        } else if ((serr = set_time(&seed)) != ESP_OK) {
            s_valid = false;
            ESP_LOGE(TAG, "PCF85063A @ 0x%02X: time unreliable (OS set) and seed write failed: %s "
                          "— clock shows --:--", PCF_ADDR, esp_err_to_name(serr));
        } else if ((rerr = read_time(&t, &reliable)) != ESP_OK || !reliable) {
            s_valid = false;
            ESP_LOGE(TAG, "PCF85063A @ 0x%02X: seeded but re-read %s (read=%s, reliable=%d) — "
                          "clock shows --:--", PCF_ADDR,
                     (rerr != ESP_OK) ? "failed" : "still not integrity-clean",
                     esp_err_to_name(rerr), reliable);
        } else {
            s_tm = t;
            s_valid = true;
            ESP_LOGW(TAG, "PCF85063A @ 0x%02X: time was unreliable (OS set) — seeded from build "
                          "%04d-%02d-%02d %02d:%02d:%02d",
                     PCF_ADDR, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                     t.tm_hour, t.tm_min, t.tm_sec);
        }
    }
    refresh_strings();
    return ESP_OK;
}

bool nocsif_rtc_get(struct tm *out)
{
    if (!s_valid || out == NULL) {
        return false;
    }
    *out = s_tm;
    return true;
}

esp_err_t nocsif_rtc_set(const struct tm *t)
{
    if (s_dev == NULL) return ESP_ERR_INVALID_STATE;
    if (t == NULL)     return ESP_ERR_INVALID_ARG;

    struct tm w = *t;
    w.tm_wday = day_of_week(w.tm_year + 1900, w.tm_mon + 1, w.tm_mday);  /* don't trust the caller's weekday */
    if (!tm_in_range(&w)) return ESP_ERR_INVALID_ARG;

    esp_err_t err = set_time(&w);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nocsif_rtc_set: write failed: %s", esp_err_to_name(err));
        return err;
    }
    struct tm rt;
    bool reliable = false;
    if (read_time(&rt, &reliable) == ESP_OK && reliable) {   /* read back to refresh the cache */
        s_tm = rt;
        s_valid = true;
    }
    refresh_strings();
    ESP_LOGI(TAG, "clock set: %04d-%02d-%02d %02d:%02d:%02d",
             w.tm_year + 1900, w.tm_mon + 1, w.tm_mday, w.tm_hour, w.tm_min, w.tm_sec);
    return ESP_OK;
}

bool nocsif_rtc_time_valid(void)
{
    return s_valid;
}

const char *nocsif_rtc_clock_str(void)
{
    return s_clock;
}

const char *nocsif_rtc_date_str(void)
{
    return s_date;
}

void nocsif_rtc_tick(void)
{
    if (s_dev == NULL) {
        return;
    }
    struct tm t;
    bool reliable = false;
    if (read_time(&t, &reliable) == ESP_OK) {
        s_valid = reliable;            /* the integrity flag can reassert if power was lost */
        if (reliable) {
            s_tm = t;
        }
    }
    /* On a read error, leave s_valid and s_tm at their previous values. */
    refresh_strings();
}
