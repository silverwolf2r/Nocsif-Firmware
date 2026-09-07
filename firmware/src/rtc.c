/*
 * NocSif — PCF85063A real-time clock (UI-shell P4.2). See rtc.h.
 *
 * Register-direct, no vendor library (matches power.c's style). Register facts verified
 * against the NXP PCF85063A datasheet Rev.7 (30 March 2018) by a register-verification
 * workflow that reconciled three independent datasheet extractions (+ SensorLib/LilyGoLib
 * cross-check), 2026-08-10:
 *   0x00 Control_1 : b5 STOP (0=run, 1=stop) · b1 12_24 (0=24-hour, our mode) · POR = 0x00.
 *   0x04 Seconds   : b7 = OS oscillator-stop / clock-integrity flag (1 => time unreliable;
 *                    POR default 1, stays set until a seconds write clears it) · b6:0 BCD.
 *   0x05 Minutes   : b6:0 BCD.        0x06 Hours   : b5:0 BCD (24-hour mode).
 *   0x07 Days      : b5:0 BCD (1-31). 0x08 Weekdays: b2:0 (0=Sun..6=Sat — matches tm_wday).
 *   0x09 Months    : b4:0 BCD (1-12). 0x0A Years   : BCD 0-99, full year = 2000 + value.
 * Burst read is the datasheet's own recommended method: set the pointer to 0x04, then read 7
 * bytes with auto-increment (0x04..0x0A). The time counters are frozen for the duration of the
 * access and one pending tick is applied after, so the 7 bytes are internally consistent as
 * long as the access completes in < 1 s — a single i2c_master_transmit_receive satisfies this.
 * Safe time-set (datasheet 8.2.1.2 + Table 7): STOP=1 -> write 0x04..0x0A in one auto-increment
 * write (Seconds b7=0 also clears OS) -> STOP=0.
 *
 * Seeding: on a fresh unit (or after the VRTC backup rail was lost) the OS flag reads 1 at boot
 * and the time is meaningless, so we seed the clock once from the FIRMWARE BUILD TIMESTAMP
 * (__DATE__/__TIME__) and clear OS. A unit whose backup kept time (OS clear) is left running and
 * just keeps ticking. The build-time seed is a placeholder that drifts from true wall-clock; real
 * time sync (GNSS/NTP) is a later milestone (M5/M8) and is intentionally not pulled in here.
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
#define REG_SECONDS         0x04        /* burst-read base: Seconds..Years = 0x04..0x0A */
#define CTRL1_STOP          (1u << 5)   /* 0x00 b5: 1 = clock stopped */
#define CTRL1_12_24         (1u << 1)   /* 0x00 b1: 0 = 24-hour mode */
#define SEC_OS              (1u << 7)   /* 0x04 b7: 1 = oscillator stopped / time unreliable */

/* Middot for the date line (kept local so this hardware driver needn't include the UI theme /
 * LVGL; identical bytes to ui_theme.h's NOCSIF_DOT). */
#define RTC_DOT             "\xC2\xB7"

static const char *TAG = "rtc";

static i2c_master_dev_handle_t s_dev;
static bool      s_valid;                    /* last read produced a trustworthy time */
static struct tm s_tm;                       /* last decoded time (valid iff s_valid) */
static char      s_clock[8]  = "--:--";      /* cached "HH:MM" (getter output)        */
static char      s_date[32]  = "-- " RTC_DOT " --";  /* cached date line              */

/* ---- BCD + helpers --------------------------------------------------------- */
static inline uint8_t bcd2dec(uint8_t b) { return (uint8_t)((b >> 4) * 10 + (b & 0x0F)); }
static inline uint8_t dec2bcd(uint8_t d) { return (uint8_t)(((d / 10) << 4) | (d % 10)); }

/* Day-of-week 0=Sunday (Sakamoto) — used only when seeding, so the seeded weekday register is
 * correct and the RTC's own daily weekday counter then stays consistent. m is 1-12. */
static int day_of_week(int y, int m, int d)
{
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

/* Fields decode into ranges strftime can index safely. The OS flag already guarantees chip
 * integrity, but an I2C glitch could hand back a plausible-OS-clear-but-garbage frame; gating
 * validity on this keeps a wild tm_wday/tm_mon out of strftime's name tables. */
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

/* read-modify-write: (cur & ~mask) | bits (mirrors power.c). */
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
/* One atomic burst read of 0x04..0x0A -> struct tm (24-hour). *reliable = OS flag clear. */
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
    out->tm_hour = bcd2dec(r[2] & 0x3F);          /* 24-hour mode (12_24 = 0) */
    out->tm_mday = bcd2dec(r[3] & 0x3F);
    out->tm_wday = r[4] & 0x07;                    /* 0=Sun, matches struct tm  */
    out->tm_mon  = (int)bcd2dec(r[5] & 0x1F) - 1;  /* tm_mon is 0-11            */
    out->tm_year = 2000 + (int)bcd2dec(r[6]) - 1900;
    out->tm_isdst = -1;
    /* Reliable = the chip's own integrity flag (OS clear) AND a range-sane decode. */
    *reliable = ((r[0] & SEC_OS) == 0) && tm_in_range(out);
    return ESP_OK;
}

/* Safe time-set: STOP -> write 0x04..0x0A in one auto-increment write (Seconds b7=0 clears OS)
 * -> release STOP. Writing CTRL1=CTRL1_STOP also fixes 24-hour mode (12_24=0) and 7 pF caps. */
static esp_err_t set_time(const struct tm *t)
{
    esp_err_t err;
    if ((err = reg_write(REG_CTRL1, CTRL1_STOP)) != ESP_OK) {       /* 0x20: STOP, 24h */
        return err;
    }
    uint8_t buf[8];
    buf[0] = REG_SECONDS;                                          /* auto-increment base */
    buf[1] = (uint8_t)(dec2bcd((uint8_t)t->tm_sec) & 0x7F);        /* OS cleared (b7=0)   */
    buf[2] = (uint8_t)(dec2bcd((uint8_t)t->tm_min) & 0x7F);
    buf[3] = (uint8_t)(dec2bcd((uint8_t)t->tm_hour) & 0x3F);       /* 24-hour             */
    buf[4] = (uint8_t)(dec2bcd((uint8_t)t->tm_mday) & 0x3F);
    buf[5] = (uint8_t)(t->tm_wday & 0x07);
    buf[6] = (uint8_t)(dec2bcd((uint8_t)(t->tm_mon + 1)) & 0x1F);
    buf[7] = (uint8_t)(dec2bcd((uint8_t)((t->tm_year + 1900) - 2000)) & 0xFF);
    if ((err = i2c_master_transmit(s_dev, buf, sizeof buf, PCF_TIMEOUT_MS)) != ESP_OK) {
        reg_write(REG_CTRL1, 0x00);                               /* best-effort re-run  */
        return err;
    }
    return reg_write(REG_CTRL1, 0x00);                            /* release STOP, run   */
}

/* Parse the compile-time build timestamp (__DATE__ = "Mmm dd yyyy", day space-padded;
 * __TIME__ = "HH:MM:SS") into a struct tm, weekday computed. false if it can't be parsed. */
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
        strftime(wd, sizeof wd, "%a", &s_tm);                    /* Mon */
        strftime(mo, sizeof mo, "%b", &s_tm);                    /* Aug */
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
        /* OS flag set (fresh unit / backup lost) or read error -> seed from the build time. Track
         * WHICH step fails so a bring-up log names the real cause (not the pre-seed read's err). */
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
    w.tm_wday = day_of_week(w.tm_year + 1900, w.tm_mon + 1, w.tm_mday);  /* keep the RTC weekday sane */
    if (!tm_in_range(&w)) return ESP_ERR_INVALID_ARG;

    esp_err_t err = set_time(&w);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nocsif_rtc_set: write failed: %s", esp_err_to_name(err));
        return err;
    }
    struct tm rt;
    bool reliable = false;
    if (read_time(&rt, &reliable) == ESP_OK && reliable) {   /* refresh cache from the chip */
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
        s_valid = reliable;            /* OS could re-assert if power/backup was lost */
        if (reliable) {
            s_tm = t;
        }
    }
    /* On a transient I2C error keep the last good time (s_valid unchanged). */
    refresh_strings();
}
