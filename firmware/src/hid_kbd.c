/*
 * NocSif — HID keyboard typing engine + keymap/LOCALE tables (M4-P4). See hid_kbd.h.
 *
 * Keymap model (data-driven): the US layout is the base table, taken
 * straight from TinyUSB's HID_ASCII_TO_KEYCODE ([128][2] = {shift_needed,
 * keycode}). A non-US layout is the US base plus a sparse diff list of the
 * ASCII characters whose key/modifier differ; lookup checks the active
 * layout's diff first, then falls back to US. Adding a layout is just
 * another diff table. Non-ASCII characters (accented letters, £, €, §) are
 * outside the ASCII table and are simply skipped — a UTF-8 STRING would need
 * multibyte handling (later).
 */
#include "hid_kbd.h"
#include "nocsif_usb_desc.h"   /* nocsif_usb_hid_report_desc */
#include "ble.h"               /* nocsif_ble_hid_ready / _send_report — the BLE transport sink */

#include <string.h>
#include <strings.h>           /* strcasecmp */
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tinyusb.h"           /* tud_hid_* + HID_KEY_* + KEYBOARD_MODIFIER_* + HID_ASCII_TO_KEYCODE */
#include "esp_log.h"

static const char *TAG = "hid_kbd";

/* Max time to wait for the HID endpoint to be ready before a single report is dropped. */
#define KBD_READY_BUDGET_MS 1000

/* US base table: shift flag + keycode per ASCII code. */
static const uint8_t s_us_map[128][2] = { HID_ASCII_TO_KEYCODE };

/* A layout diff entry: for `ascii`, use this modifier + keycode instead of
 * the US base. keycode 0 means "this character is unmapped in this layout"
 * (skip, do not fall back). */
typedef struct {
    uint8_t ascii;
    uint8_t modifier;
    uint8_t keycode;
} kbd_diff_t;

#define S KEYBOARD_MODIFIER_LEFTSHIFT
#define A KEYBOARD_MODIFIER_RIGHTALT   /* AltGr */

/* GB/UK: @ and " swap; #, ~, \, | move onto the two ISO keys. £ and ¬ are non-ASCII. */
static const kbd_diff_t s_gb_diff[] = {
    {'"',  S, HID_KEY_2},                             /* " = Shift+2 */
    {'@',  S, HID_KEY_APOSTROPHE},                    /* @ = Shift+' */
    {'#',  0, HID_KEY_EUROPE_1},                      /* # = ISO key 1 (0x32) */
    {'~',  S, HID_KEY_EUROPE_1},                      /* ~ = Shift+ISO1 */
    {'\\', 0, HID_KEY_EUROPE_2},                      /* \ = ISO key 2 (0x64) */
    {'|',  S, HID_KEY_EUROPE_2},                      /* | = Shift+ISO2 */
};

/* DE/QWERTZ: z<->y swap; shifted-number symbols shift left one; +/* on the
 * ]-key; ß/? on the -key; ISO # ' and < >; the AltGr block for @ [ ] { } \ | ~;
 * ^ and ` are dead keys -> unmapped for v1. Non-ASCII ä ö ü ß € § are outside
 * the ASCII table. Validate on-device. */
static const kbd_diff_t s_de_diff[] = {
    {'y', 0, HID_KEY_Z}, {'Y', S, HID_KEY_Z},
    {'z', 0, HID_KEY_Y}, {'Z', S, HID_KEY_Y},
    {'"', S, HID_KEY_2},
    {'&', S, HID_KEY_6},
    {'/', S, HID_KEY_7},
    {'(', S, HID_KEY_8},
    {')', S, HID_KEY_9},
    {'=', S, HID_KEY_0},
    {'?', S, HID_KEY_MINUS},
    {'+', 0, HID_KEY_BRACKET_RIGHT},
    {'*', S, HID_KEY_BRACKET_RIGHT},
    {'#', 0, HID_KEY_BACKSLASH},
    {'\'', S, HID_KEY_BACKSLASH},
    {'-', 0, HID_KEY_SLASH},
    {'_', S, HID_KEY_SLASH},
    {';', S, HID_KEY_COMMA},
    {':', S, HID_KEY_PERIOD},
    {'<', 0, HID_KEY_EUROPE_2},
    {'>', S, HID_KEY_EUROPE_2},
    {'@', A, HID_KEY_Q},
    {'[', A, HID_KEY_8},
    {']', A, HID_KEY_9},
    {'{', A, HID_KEY_7},
    {'}', A, HID_KEY_0},
    {'\\', A, HID_KEY_MINUS},
    {'|', A, HID_KEY_EUROPE_2},
    {'~', A, HID_KEY_BRACKET_RIGHT},
    {'^', 0, 0},   /* dead key — unmapped for v1 */
    {'`', 0, 0},   /* dead key — unmapped for v1 */
};

#undef S
#undef A

typedef struct {
    const char       *name;
    const kbd_diff_t *diff;
    size_t            ndiff;
} kbd_locale_t;

static const kbd_locale_t s_locales[] = {
    {"US", NULL,       0},
    {"GB", s_gb_diff,  sizeof(s_gb_diff) / sizeof(s_gb_diff[0])},
    {"DE", s_de_diff,  sizeof(s_de_diff) / sizeof(s_de_diff[0])},
};

static const kbd_locale_t *s_active = &s_locales[0];   /* US default */

/* Host keyboard-LED bitmap, captured from SET_REPORT. Bit
 * KEYBOARD_LED_CAPSLOCK decides whether an unshifted letter key produces
 * upper or lower case on the host. */
static volatile uint8_t s_led;

/* ------------------------------------------------------------------ */
/* HID-class callbacks (app-owned; esp_tinyusb does not provide these) */
/* ------------------------------------------------------------------ */

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return nocsif_usb_hid_report_desc(NULL);
}

/* A host may GET_REPORT(input) during HID init. Returns the current keyboard
 * report — which is "no keys pressed" (all zeros), since keys are driven
 * with immediate press/release rather than holding state. Returning 0 here
 * would be wrong: the HID class driver asserts the returned length is > 0
 * for the input case. */
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen)
{
    (void)instance; (void)report_id;
    if (report_type == HID_REPORT_TYPE_INPUT && reqlen >= 8) {
        memset(buffer, 0, 8);   /* 8-byte boot-keyboard report: no keys down */
        return 8;
    }
    return 0;   /* output/feature: nothing to source (benign stall) */
}

/* The host issues SET_REPORT (OUTPUT) to drive the keyboard LEDs (Caps/Num/
 * Scroll lock). Captures byte 0 so nocsif_hid_kbd_char can compensate for
 * CapsLock. Tolerates any size. */
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance; (void)report_id;
    if (report_type == HID_REPORT_TYPE_OUTPUT && bufsize >= 1) {
        s_led = buffer[0];
    }
}

/* ------------------------------------------------------------------ */
/* Keymap                                                              */
/* ------------------------------------------------------------------ */

bool nocsif_hid_kbd_set_locale(const char *name)
{
    if (name == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(s_locales) / sizeof(s_locales[0]); i++) {
        if (strcasecmp(name, s_locales[i].name) == 0 ||
            (strcasecmp(name, "UK") == 0 && strcmp(s_locales[i].name, "GB") == 0)) {
            s_active = &s_locales[i];
            ESP_LOGI(TAG, "keyboard layout -> %s", s_active->name);
            return true;
        }
    }
    return false;
}

const char *nocsif_hid_kbd_locale(void)
{
    return s_active->name;
}

bool nocsif_hid_kbd_lookup(char ch, uint8_t *modifier, uint8_t *keycode)
{
    uint8_t c = (uint8_t)ch;
    if (c >= 128) {
        if (keycode) *keycode = 0;
        return false;   /* non-ASCII: outside the table */
    }
    /* Active layout's diff wins over the US base (even a keycode-0 "unmapped" override). */
    for (size_t i = 0; i < s_active->ndiff; i++) {
        if (s_active->diff[i].ascii == c) {
            if (modifier) *modifier = s_active->diff[i].modifier;
            if (keycode)  *keycode  = s_active->diff[i].keycode;
            return s_active->diff[i].keycode != 0;
        }
    }
    uint8_t kc = s_us_map[c][1];
    if (modifier) *modifier = s_us_map[c][0] ? KEYBOARD_MODIFIER_LEFTSHIFT : 0;
    if (keycode)  *keycode  = kc;
    return kc != 0;
}

/* ------------------------------------------------------------------ */
/* Transport sink (USB / BLE)                                          */
/* ------------------------------------------------------------------ */

static nocsif_hid_sink_t s_sink = NOCSIF_HID_SINK_USB;

void nocsif_hid_kbd_set_sink(nocsif_hid_sink_t sink) { s_sink = sink; }
nocsif_hid_sink_t nocsif_hid_kbd_sink(void)          { return s_sink; }

/* Returns whether the active transport is ready to accept a report right now. */
static bool sink_ready(void)
{
    return (s_sink == NOCSIF_HID_SINK_BLE) ? nocsif_ble_hid_ready() : tud_hid_ready();
}

/* Sends one 8-byte boot-keyboard report through the active transport. kc6 may be NULL (all keys up). */
static void sink_send(uint8_t modifier, const uint8_t kc6[6])
{
    if (s_sink == NOCSIF_HID_SINK_BLE) {
        uint8_t rep[8] = {
            modifier, 0,
            kc6 ? kc6[0] : 0, kc6 ? kc6[1] : 0, kc6 ? kc6[2] : 0,
            kc6 ? kc6[3] : 0, kc6 ? kc6[4] : 0, kc6 ? kc6[5] : 0,
        };
        nocsif_ble_hid_send_report(rep);
    } else {
        tud_hid_keyboard_report(0, modifier, kc6);
    }
}

/* ------------------------------------------------------------------ */
/* Report emission                                                     */
/* ------------------------------------------------------------------ */

/* Blocks until the active transport is ready, or KBD_READY_BUDGET_MS elapses. */
static bool wait_ready(void)
{
    uint32_t waited = 0;
    while (!sink_ready()) {
        if (waited >= KBD_READY_BUDGET_MS) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
        waited += 1;
    }
    return true;
}

void nocsif_hid_kbd_release_all(void)
{
    if (sink_ready()) {
        sink_send(0, NULL);   /* all-zero: release every key + modifier */
    }
}

bool nocsif_hid_kbd_key(uint8_t modifier, uint8_t keycode, uint32_t hold_ms)
{
    if (!wait_ready()) {
        ESP_LOGW(TAG, "HID not ready — key dropped");
        return false;
    }
    uint8_t kc[6] = { keycode, 0, 0, 0, 0, 0 };
    sink_send(modifier, kc);                    /* press (keycode 0 => modifier-only tap) */
    vTaskDelay(pdMS_TO_TICKS(hold_ms ? hold_ms : 2));

    if (!wait_ready()) {
        nocsif_hid_kbd_release_all();
        return false;
    }
    sink_send(0, NULL);                         /* release (mandatory or the key repeats) */
    return true;
}

bool nocsif_hid_kbd_char(char ch, uint32_t hold_ms)
{
    uint8_t mod, kc;
    if (!nocsif_hid_kbd_lookup(ch, &mod, &kc)) {
        ESP_LOGW(TAG, "char 0x%02x unmapped in %s — skipped", (uint8_t)ch, s_active->name);
        return false;
    }
    /* CapsLock inverts case for the letter keys (a physical key + host CapsLock
     * decide case). HID_KEY_A..HID_KEY_Z are the contiguous 0x04..0x1D block.
     * Exclude AltGr-produced glyphs: a layout can map a letter *keycode* to a
     * non-letter symbol on the AltGr layer (e.g. DE '@' = AltGr+Q), where
     * CapsLock has no effect — flipping Shift there would type the wrong
     * (level-4) glyph or nothing. */
    if ((s_led & KEYBOARD_LED_CAPSLOCK) && kc >= HID_KEY_A && kc <= HID_KEY_Z &&
        !(mod & KEYBOARD_MODIFIER_RIGHTALT)) {
        mod ^= KEYBOARD_MODIFIER_LEFTSHIFT;
    }
    return nocsif_hid_kbd_key(mod, kc, hold_ms);
}
