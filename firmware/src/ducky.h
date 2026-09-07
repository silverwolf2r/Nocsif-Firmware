/*
 * NocSif — DuckyScript player (M4-P4).
 *
 * Reads a DuckyScript text macro from the microSD and plays it as USB HID keyboard events.
 * Runs entirely on a dedicated worker task: the UI (or an auto-run) only *requests* a run,
 * which is non-blocking and safe from an LVGL callback.
 *
 * Flow of a run (nocsif_ducky_run_file):
 *   1. Require the USB composite up and the HID endpoint ready (else ERR_HID_DOWN, no keys).
 *   2. Deterministically claim the card (USB-MSC ownership -> MOUNT_APP) — NOT a host eject.
 *   3. Read the whole macro into RAM, then release the card back to the host and play from
 *      RAM (so a host re-scan of the drive mid-run cannot disturb the macro).
 *   4. Parse + execute; ALWAYS release every key on exit (success, parse error, or abort).
 *
 * Supported grammar (US base + runtime LOCALE): REM, STRING, STRINGLN, ENTER/RETURN,
 * DELAY, DEFAULTDELAY/DEFAULT_DELAY, DEFAULTCHARDELAY/DEFAULT_CHAR_DELAY, modifier combos
 * (GUI/WINDOWS, CTRL/CONTROL, ALT, SHIFT), named keys (ESC, TAB, SPACE, BACKSPACE, DELETE,
 * INSERT, HOME, END, PAGEUP, PAGEDOWN, UP/DOWN/LEFT/RIGHT, CAPSLOCK, PRINTSCREEN, MENU/APP),
 * F1..F12, REPEAT, LOCALE, and single printable characters.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Default macro path when a run is requested with NULL. */
#define NOCSIF_DUCKY_DEFAULT_PATH "/sd/macro.txt"

typedef enum {
    NOCSIF_DUCKY_IDLE = 0,   /* never run, or finished and reset by the next request */
    NOCSIF_DUCKY_RUNNING,    /* a macro is playing */
    NOCSIF_DUCKY_DONE,       /* last run completed */
    NOCSIF_DUCKY_ERR_HID_DOWN,  /* USB host / HID endpoint not ready */
    NOCSIF_DUCKY_ERR_SD_CLAIM,  /* could not claim the card for app access */
    NOCSIF_DUCKY_ERR_OPEN,      /* macro file missing / unreadable */
    NOCSIF_DUCKY_ERR_NOMEM,     /* could not buffer the macro */
} nocsif_ducky_state_t;

/* Create the idle worker task. Idempotent; call once at boot after nocsif_usb_gadget_init(). */
esp_err_t nocsif_ducky_init(void);

/* Self-test probe (RAM-BUDGET remake #7): true once the worker has scheduled and confirmed its task
 * stack lives in PSRAM. Used only by the compile-gated NOCSIF_PSRAM_STACK_SELFTEST hook. */
bool nocsif_ducky_stack_is_psram(void);

/* The transport a run plays over — USB HID (the M4 default) or BLE HID-over-GATT (M7). The player
 * sets the matching hid_kbd sink for the run and waits on that transport's readiness. */
typedef enum {
    NOCSIF_DUCKY_SINK_USB = 0,   /* USB HID: needs the composite up + host + HID endpoint ready   */
    NOCSIF_DUCKY_SINK_BLE,       /* BLE HID: needs a bonded, subscribed keyboard host             */
} nocsif_ducky_sink_t;

/* Request a macro run. Non-blocking and LVGL-callback-safe (only signals the worker). `path`
 * is copied; NULL uses NOCSIF_DUCKY_DEFAULT_PATH. Ignored if a run is already in progress. */
void nocsif_ducky_request_run(const char *path);                              /* USB (M4)          */

/* Request a macro run over a specific transport (USB or BLE). Same semantics as the above. Over BLE
 * the card is read under the /sd lock only (no USB-MSC claim — there is no MSC host in play). */
void nocsif_ducky_request_run_ex(const char *path, nocsif_ducky_sink_t sink);

/* Type a literal string over a transport (no file). Copied; capped to an internal buffer. Used by the
 * BLE Keyboard "Type test string" affordance. Ignored if a run is already in progress. */
void nocsif_ducky_request_type(const char *text, nocsif_ducky_sink_t sink);

/* Current player state, for the UI to poll. */
nocsif_ducky_state_t nocsif_ducky_state(void);

#ifdef __cplusplus
}
#endif
