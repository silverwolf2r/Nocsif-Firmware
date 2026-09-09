/*
 * NocSif — USB HID keyboard typing engine + remappable keymap/LOCALE layer (M4-P4).
 *
 * This module:
 *   - defines the three HID-class callbacks esp_tinyusb does not provide and
 *     the app must supply (tud_hid_descriptor_report_cb / tud_hid_get_report_cb
 *     / tud_hid_set_report_cb); these are non-weak in the TinyUSB HID class
 *     driver, so omitting them fails to link.
 *   - converts characters/keys into 8-byte boot-keyboard reports via a
 *     keymap, honouring the host CapsLock LED state (captured from
 *     SET_REPORT) so alphabetic case stays correct.
 *   - ships US (base), GB/UK and DE/QWERTZ layouts; further layouts are pure data tables.
 *
 * Every emit gates on the transport being ready and is followed by an
 * all-zero release report, so a key can never stick. The report_id is always
 * 0 (the report descriptor has no report ID).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Transport the emit functions send reports through: USB (TinyUSB, the M4
 * default) or BLE (HID-over-GATT notifications via ble.c). The keymap/layout
 * logic is identical on both — only the wire differs — so the whole
 * DuckyScript engine plays over either. The DuckyScript worker sets this per
 * run and restores USB afterwards; nothing else should change it. */
typedef enum {
    NOCSIF_HID_SINK_USB = 0,   /* TinyUSB HID (tud_hid_keyboard_report) — the default */
    NOCSIF_HID_SINK_BLE,       /* BLE HID-over-GATT (nocsif_ble_hid_send_report) */
} nocsif_hid_sink_t;

/* Selects/queries the transport the next emit uses. Not reentrant — the
 * single DuckyScript worker owns it for the duration of a run. */
void              nocsif_hid_kbd_set_sink(nocsif_hid_sink_t sink);
nocsif_hid_sink_t nocsif_hid_kbd_sink(void);

/* Selects the active keyboard layout by name ("US", "GB"/"UK", "DE"). Case-insensitive.
 * Returns false (and leaves the layout unchanged) for an unknown name. */
bool nocsif_hid_kbd_set_locale(const char *name);

/* Returns the name of the active layout (for logging). */
const char *nocsif_hid_kbd_locale(void);

/* Types one ASCII character through the active layout (press + release).
 * Characters the active layout cannot produce (keycode 0, e.g. non-ASCII or
 * a dead key) are skipped and reported false. hold_ms is the press-hold
 * before release. Gates on the transport being ready. */
bool nocsif_hid_kbd_char(char ch, uint32_t hold_ms);

/* Presses one keycode with a modifier mask (KEYBOARD_MODIFIER_*), holds,
 * then releases. Used for named keys (ENTER, F5, arrows, ...) and modifier
 * combos (GUI+r, CTRL+ALT+DELETE). keycode 0 with a non-zero modifier taps
 * the modifier(s) alone. */
bool nocsif_hid_kbd_key(uint8_t modifier, uint8_t keycode, uint32_t hold_ms);

/* Looks up the {modifier, keycode} the active layout uses for an ASCII
 * character. Returns false if unmapped. Used by the DuckyScript combo path
 * (e.g. GUI r -> the layout's 'r' keycode). *keycode is 0 when unmapped. */
bool nocsif_hid_kbd_lookup(char ch, uint8_t *modifier, uint8_t *keycode);

/* Sends an all-zero report (releases every key + modifier). Safe to call any
 * time; used as a belt-and-braces release on every exit path of a macro run. */
void nocsif_hid_kbd_release_all(void);

#ifdef __cplusplus
}
#endif
