/*
 * NocSif — shared I2C bus + scan (M0.1). See i2c_scan.h.
 *
 * IDF is 5.5.x, so this uses the new bus/device driver (driver/i2c_master.h),
 * not the deprecated legacy driver/i2c.h. Probing is done with
 * i2c_master_probe(), which drives a bare address frame and reports the ACK.
 */
#include "i2c_scan.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "driver/i2c_master.h"
#include "esp_log.h"

#define NOCSIF_I2C_PORT      I2C_NUM_0
#define NOCSIF_I2C_SDA_GPIO  GPIO_NUM_3   /* docs/HARDWARE.md: SDA = GPIO3 */
#define NOCSIF_I2C_SCL_GPIO  GPIO_NUM_2   /* docs/HARDWARE.md: SCL = GPIO2 */
#define NOCSIF_I2C_PROBE_MS  50           /* per-address timeout; a NACK returns fast */
#define NOCSIF_I2C_SCAN_LO   0x08         /* skip the reserved 0x00-0x07 ... */
#define NOCSIF_I2C_SCAN_HI   0x77         /* ... and 0x78-0x7F address blocks */

static const char *TAG = "i2c";

/* NULL until nocsif_i2c_init() succeeds; then shared by all I2C users. */
static i2c_master_bus_handle_t s_bus;

/* Expected devices on the T-Watch Ultra I2C bus (docs/HARDWARE.md). Several
 * do NOT ACK from a cold boot, and that is not a fault:
 *   - CST9217 is held in reset until XL9555 GPIO10 is driven,
 *   - the sensor sits on AXP2101 ALDO4 which may default off,
 *   - the haptic driver is gated by XL9555 GPIO6.
 * The AXP2101 (powers the SoC), XL9555 and RTC should always answer. */
typedef struct {
    uint8_t     addr;
    const char *name;
    const char *note;
    bool        always;   /* must ACK on a healthy bus; absence => real fault, not "maybe off" */
} nocsif_i2c_dev_t;

static const nocsif_i2c_dev_t k_known[] = {
    { 0x1A, "CST9217 touch",   "held in reset via XL9555 GPIO10",                    false },
    { 0x20, "XL9555 expander", "gates display power (GPIO7) / touch reset (GPIO10)", true  },
    { 0x28, "BHI260AP sensor", "power via AXP2101 ALDO4 (may be off)",               false },
    { 0x34, "AXP2101 PMU",     "always present (powers the SoC)",                    true  },
    { 0x51, "PCF85063A RTC",   "always present (VRTC/backup battery)",               true  },
    { 0x5A, "DRV2605 haptic",  "enable via XL9555 GPIO6",                            false },
};
#define K_KNOWN_COUNT (sizeof(k_known) / sizeof(k_known[0]))

static const nocsif_i2c_dev_t *known_lookup(uint8_t addr)
{
    for (size_t i = 0; i < K_KNOWN_COUNT; i++) {
        if (k_known[i].addr == addr) {
            return &k_known[i];
        }
    }
    return NULL;
}

esp_err_t nocsif_i2c_init(void)
{
    if (s_bus != NULL) {
        return ESP_OK;
    }
    const i2c_master_bus_config_t cfg = {
        .i2c_port = NOCSIF_I2C_PORT,
        .sda_io_num = NOCSIF_I2C_SDA_GPIO,
        .scl_io_num = NOCSIF_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,  /* board has externals too; harmless in parallel */
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
    if (err != ESP_OK) {
        s_bus = NULL;
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "I2C master bus up: SDA=%d SCL=%d port=%d (internal pullups on)",
             NOCSIF_I2C_SDA_GPIO, NOCSIF_I2C_SCL_GPIO, NOCSIF_I2C_PORT);
    return ESP_OK;
}

i2c_master_bus_handle_t nocsif_i2c_bus(void)
{
    return s_bus;
}

/* Sweeps the address range into present[128]; returns the count that ACKed.
 * The i2c_master driver logs a warning on every NACK, which would bury the
 * report, so its "i2c.master" tag is muted for the duration of the sweep
 * (our own timeout warnings use the "i2c" tag and are unaffected). */
static int scan_bus(bool present[128])
{
    for (int i = 0; i < 128; i++) {
        present[i] = false;
    }

    int found = 0;
    esp_log_level_t prev = esp_log_level_get("i2c.master");
    esp_log_level_set("i2c.master", ESP_LOG_NONE);

    for (uint8_t addr = NOCSIF_I2C_SCAN_LO; addr <= NOCSIF_I2C_SCAN_HI; addr++) {
        esp_err_t err = i2c_master_probe(s_bus, addr, NOCSIF_I2C_PROBE_MS);
        if (err == ESP_OK) {
            present[addr] = true;
            found++;
        } else if (err == ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "probe 0x%02X timed out (bus stuck? check pullups/power)", addr);
        }
        /* ESP_ERR_NOT_FOUND is the ordinary "nobody home" case. */
    }

    esp_log_level_set("i2c.master", prev);
    return found;
}

int nocsif_i2c_scan(void)
{
    if (s_bus == NULL) {
        ESP_LOGE(TAG, "scan requested but bus not initialized");
        return -1;
    }

    bool present[128];
    int found = scan_bus(present);

    /* i2cdetect-style grid. */
    printf("I2C scan  SDA=%d SCL=%d  (probed 0x%02X-0x%02X)\n",
           NOCSIF_I2C_SDA_GPIO, NOCSIF_I2C_SCL_GPIO, NOCSIF_I2C_SCAN_LO, NOCSIF_I2C_SCAN_HI);
    printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");
    for (int row = 0; row < 128; row += 16) {
        printf("%02x:", row);
        for (int col = 0; col < 16; col++) {
            int addr = row + col;
            if (addr < NOCSIF_I2C_SCAN_LO || addr > NOCSIF_I2C_SCAN_HI) {
                printf("   ");
            } else if (present[addr]) {
                printf(" %02x", addr);
            } else {
                printf(" --");
            }
        }
        printf("\n");
    }

    /* Per-device breakdown. */
    printf("found %d device(s):\n", found);
    for (int addr = NOCSIF_I2C_SCAN_LO; addr <= NOCSIF_I2C_SCAN_HI; addr++) {
        if (present[addr]) {
            const nocsif_i2c_dev_t *d = known_lookup((uint8_t)addr);
            printf("  0x%02X  %s\n", addr, d ? d->name : "unknown / unexpected device");
        }
    }

    /* Always-present parts (PMU, expander, RTC) must ACK on a healthy bus.
     * With internal pullups on, a pin-swap / wiring fault NACKs
     * (ESP_ERR_NOT_FOUND) rather than timing out, so the "bus stuck" warning
     * above never fires — absence of one of these is a real fault, not a
     * "maybe unpowered". */
    for (size_t i = 0; i < K_KNOWN_COUNT; i++) {
        if (k_known[i].always && !present[k_known[i].addr]) {
            ESP_LOGE(TAG, "FAULT 0x%02X %s expected to ALWAYS ACK but absent — check SDA/SCL pins, pullups, power",
                     k_known[i].addr, k_known[i].name);
        }
    }

    /* Genuinely gated parts: absence during bring-up is expected, with the
     * likely reason, so it isn't mistaken for a dead part. */
    bool header_done = false;
    for (size_t i = 0; i < K_KNOWN_COUNT; i++) {
        if (!k_known[i].always && !present[k_known[i].addr]) {
            if (!header_done) {
                printf("expected but absent (likely unpowered / in reset):\n");
                header_done = true;
            }
            printf("  0x%02X  %-15s (%s)\n", k_known[i].addr, k_known[i].name, k_known[i].note);
        }
    }

    return found;
}

int nocsif_i2c_scan_compact(void)
{
    if (s_bus == NULL) {
        return -1;
    }

    bool present[128];
    int found = scan_bus(present);

    char line[160];
    int n = snprintf(line, sizeof line, "rescan: %d device(s):", found);
    for (int addr = NOCSIF_I2C_SCAN_LO; addr <= NOCSIF_I2C_SCAN_HI && n > 0 && n < (int)sizeof line; addr++) {
        if (present[addr]) {
            n += snprintf(line + n, sizeof line - n, " 0x%02X", addr);
        }
    }
    ESP_LOGI(TAG, "%s", line);
    return found;
}
