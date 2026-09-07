/*
 * NocSif — persistent settings store (UI-shell P4.4). See settings.h.
 *
 * Thin wrapper over a single NVS handle in the "nocsif" namespace. The two button-binding
 * values (FN launch target, PWR double-press shortcut) are also mirrored into a small RAM
 * cache so a button-action callback on the LVGL task can read them without a flash access.
 */
#include "settings.h"

#include <string.h>

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_random.h"
#include "mbedtls/sha256.h"
#include "mbedtls/platform_util.h"   /* mbedtls_platform_zeroize (not elided at -O2) */
#include "esp_log.h"

static const char *TAG = "settings";

#define NS              "nocsif"
#define KEY_FN_TARGET   "fn.target"     /* app id launched by the FN button       */
#define KEY_PWR_DOUBLE  "pwr.double"    /* app id launched by a PWR double-press   */
#define DEF_FN_TARGET   "wifi"          /* default FN target (an ENABLED screen)   */
#define BIND_MAX        32              /* longest app id we cache (leaf ids fit)  */

#define KEY_DEV_NAME    "dev.name"      /* friendly watch name (WiFi hostname source) */
#define DEF_DEV_NAME    "NocSif watch"  /* default identity                          */
#define DEVNAME_MAX     32              /* cache size (name shown as-is; hostname sanitized) */

#define KEY_PIN_SALT    "pin.salt"      /* random per-passcode salt (blob)         */
#define KEY_PIN_HASH    "pin.hash"      /* SHA-256(salt || pin) (blob)             */
#define PIN_SALT_LEN    16
#define PIN_HASH_LEN    32
#define PIN_INPUT_MAX   16              /* longest PIN we hash (keypad sends <= 8)  */

static nvs_handle_t s_nvs;
static bool         s_ready;

/* Button-binding RAM cache (see header — LVGL-task-safe reads). */
static char s_fn_target[BIND_MAX]  = DEF_FN_TARGET;
static char s_pwr_double[BIND_MAX] = "";

/* Device-name RAM cache (LVGL-task-safe read; primed at init). */
static char s_dev_name[DEVNAME_MAX] = DEF_DEV_NAME;

/* Passcode-present RAM cache (LVGL-task-safe read for the live tag). */
static bool s_has_pin;

esp_err_t nocsif_settings_get_str(const char *key, char *out, size_t len, const char *dflt)
{
    if (out == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ready) {
        size_t n = len;
        esp_err_t err = nvs_get_str(s_nvs, key, out, &n);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "get_str('%s') -> %s (using default)", key, esp_err_to_name(err));
        }
    }
    /* absent / not ready / error -> default */
    if (dflt) {
        strlcpy(out, dflt, len);
    } else {
        out[0] = '\0';
    }
    return ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nocsif_settings_set_str(const char *key, const char *val)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = nvs_set_str(s_nvs, key, val ? val : "");
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_str('%s') -> %s", key, esp_err_to_name(err));
    }
    return err;
}

int32_t nocsif_settings_get_i32(const char *key, int32_t dflt)
{
    if (!s_ready) {
        return dflt;
    }
    int32_t v = 0;
    esp_err_t err = nvs_get_i32(s_nvs, key, &v);
    if (err == ESP_OK) {
        return v;
    }
    if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "get_i32('%s') -> %s (using default)", key, esp_err_to_name(err));
    }
    return dflt;
}

esp_err_t nocsif_settings_set_i32(const char *key, int32_t val)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = nvs_set_i32(s_nvs, key, val);
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_i32('%s') -> %s", key, esp_err_to_name(err));
    }
    return err;
}

/* ---- button bindings ------------------------------------------------------- */

const char *nocsif_settings_fn_target(void)  { return s_fn_target;  }
const char *nocsif_settings_pwr_double(void) { return s_pwr_double; }

void nocsif_settings_set_fn_target(const char *id)
{
    strlcpy(s_fn_target, (id && id[0]) ? id : DEF_FN_TARGET, sizeof s_fn_target);
    nocsif_settings_set_str(KEY_FN_TARGET, s_fn_target);
    ESP_LOGI(TAG, "FN target = '%s'", s_fn_target);
}

void nocsif_settings_set_pwr_double(const char *id)
{
    strlcpy(s_pwr_double, (id && id[0]) ? id : "", sizeof s_pwr_double);
    nocsif_settings_set_str(KEY_PWR_DOUBLE, s_pwr_double);
    ESP_LOGI(TAG, "PWR double = '%s'", s_pwr_double[0] ? s_pwr_double : "(none)");
}

/* ---- device name ----------------------------------------------------------- */

const char *nocsif_settings_device_name(void) { return s_dev_name; }

void nocsif_settings_set_device_name(const char *name)
{
    strlcpy(s_dev_name, (name && name[0]) ? name : DEF_DEV_NAME, sizeof s_dev_name);
    nocsif_settings_set_str(KEY_DEV_NAME, s_dev_name);
    ESP_LOGI(TAG, "device name = '%s'", s_dev_name);
}

/* ---- passcode (hashed) ----------------------------------------------------- */

/* out[32] = SHA-256(salt[16] || pin). The scratch buffer is wiped before returning. */
static void pin_hash(const uint8_t salt[PIN_SALT_LEN], const char *pin, uint8_t out[PIN_HASH_LEN])
{
    uint8_t buf[PIN_SALT_LEN + PIN_INPUT_MAX];
    size_t plen = strnlen(pin, PIN_INPUT_MAX);
    memcpy(buf, salt, PIN_SALT_LEN);
    memcpy(buf + PIN_SALT_LEN, pin, plen);
    mbedtls_sha256(buf, PIN_SALT_LEN + plen, out, 0);   /* is224=0 -> SHA-256 */
    mbedtls_platform_zeroize(buf, sizeof buf);          /* wipe the salt||pin scratch */
}

esp_err_t nocsif_settings_set_pin(const char *pin)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pin == NULL || pin[0] == '\0') {
        return nocsif_settings_clear_pin();
    }
    uint8_t salt[PIN_SALT_LEN], hash[PIN_HASH_LEN];
    esp_fill_random(salt, sizeof salt);
    pin_hash(salt, pin, hash);
    esp_err_t err = nvs_set_blob(s_nvs, KEY_PIN_SALT, salt, sizeof salt);
    if (err == ESP_OK) err = nvs_set_blob(s_nvs, KEY_PIN_HASH, hash, sizeof hash);
    if (err == ESP_OK) err = nvs_commit(s_nvs);
    mbedtls_platform_zeroize(hash, sizeof hash);   /* don't leave the hash on the stack */
    if (err == ESP_OK) {
        s_has_pin = true;
        ESP_LOGI(TAG, "passcode set");
    } else {
        ESP_LOGE(TAG, "set_pin -> %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t nocsif_settings_clear_pin(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    /* erase_key returns NOT_FOUND if absent — fine, ignore. */
    esp_err_t e1 = nvs_erase_key(s_nvs, KEY_PIN_SALT);
    esp_err_t e2 = nvs_erase_key(s_nvs, KEY_PIN_HASH);
    (void)e1; (void)e2;
    esp_err_t err = nvs_commit(s_nvs);
    if (err == ESP_OK) {
        s_has_pin = false;   /* only drop the cache once the erase is durably committed */
        ESP_LOGI(TAG, "passcode cleared");
    } else {
        ESP_LOGE(TAG, "clear_pin commit -> %s (passcode still stored)", esp_err_to_name(err));
    }
    return err;
}

bool nocsif_settings_has_pin(void)
{
    return s_has_pin;
}

bool nocsif_settings_verify_pin(const char *pin)
{
    if (!s_ready || !s_has_pin || pin == NULL) {
        return false;
    }
    uint8_t salt[PIN_SALT_LEN], stored[PIN_HASH_LEN], calc[PIN_HASH_LEN];
    size_t sl = sizeof salt, hl = sizeof stored;
    if (nvs_get_blob(s_nvs, KEY_PIN_SALT, salt, &sl) != ESP_OK || sl != PIN_SALT_LEN) {
        return false;
    }
    if (nvs_get_blob(s_nvs, KEY_PIN_HASH, stored, &hl) != ESP_OK || hl != PIN_HASH_LEN) {
        return false;
    }
    pin_hash(salt, pin, calc);
    uint8_t diff = 0;
    for (int i = 0; i < PIN_HASH_LEN; i++) {
        diff |= (uint8_t)(calc[i] ^ stored[i]);   /* constant-time compare */
    }
    mbedtls_platform_zeroize(calc, sizeof calc);
    mbedtls_platform_zeroize(stored, sizeof stored);
    return diff == 0;
}

const char *nocsif_settings_pin_str(void)
{
    return s_has_pin ? "set" : "none";
}

esp_err_t nocsif_settings_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* The NVS partition is new or from an incompatible layout — wipe it and retry once
         * (nothing durable is stored there yet; the settings are all re-derivable defaults). */
        ESP_LOGW(TAG, "nvs_flash_init -> %s; erasing NVS partition and retrying", esp_err_to_name(err));
        if ((err = nvs_flash_erase()) == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s — settings will use defaults (RAM only)",
                 esp_err_to_name(err));
        return err;
    }
    if ((err = nvs_open(NS, NVS_READWRITE, &s_nvs)) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open('%s') failed: %s — settings will use defaults (RAM only)",
                 NS, esp_err_to_name(err));
        return err;
    }
    s_ready = true;

    /* Prime the button-binding cache from flash (falling back to the defaults). */
    nocsif_settings_get_str(KEY_FN_TARGET,  s_fn_target,  sizeof s_fn_target,  DEF_FN_TARGET);
    nocsif_settings_get_str(KEY_PWR_DOUBLE, s_pwr_double, sizeof s_pwr_double, "");
    nocsif_settings_get_str(KEY_DEV_NAME,   s_dev_name,   sizeof s_dev_name,   DEF_DEV_NAME);
    /* Prime the passcode-present flag (blob exists at the right length). nvs_get_blob with a
     * NULL out buffer returns the stored length, or NOT_FOUND. */
    size_t hl = 0;
    s_has_pin = (nvs_get_blob(s_nvs, KEY_PIN_HASH, NULL, &hl) == ESP_OK && hl == PIN_HASH_LEN);
    ESP_LOGI(TAG, "settings up (nvs '%s'): FN='%s' PWR-double='%s' passcode=%s",
             NS, s_fn_target, s_pwr_double[0] ? s_pwr_double : "(none)", s_has_pin ? "set" : "none");
    return ESP_OK;
}
