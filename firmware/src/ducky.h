/*
 * NocSif — DuckyScript player (M4-P4).
 *
 * Plays a DuckyScript macro from the microSD card as USB HID keyboard events.
 * Runs on a dedicated worker task, so requesting a run only signals the task
 * and is safe to call from an LVGL callback.
 *
 * Supported grammar: REM, STRING, STRINGLN, ENTER/RETURN, DELAY,
 * DEFAULTDELAY/DEFAULT_DELAY, DEFAULTCHARDELAY/DEFAULT_CHAR_DELAY, modifier
 * combos (GUI/WINDOWS, CTRL/CONTROL, ALT, SHIFT), named keys, F1..F12,
 * REPEAT, LOCALE, and single printable characters.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Default macro path used when a run is requested with NULL. */
#define NOCSIF_DUCKY_DEFAULT_PATH "/sd/macro.txt"

typedef enum {
    NOCSIF_DUCKY_IDLE = 0,   /* never run, or finished and reset by the next request */
    NOCSIF_DUCKY_RUNNING,    /* a macro is currently playing */
    NOCSIF_DUCKY_DONE,       /* last run completed */
    NOCSIF_DUCKY_ERR_HID_DOWN,  /* USB host / HID endpoint not ready */
    NOCSIF_DUCKY_ERR_SD_CLAIM,  /* could not claim the SD card for app access */
    NOCSIF_DUCKY_ERR_OPEN,      /* macro file missing or unreadable */
    NOCSIF_DUCKY_ERR_NOMEM,     /* could not allocate a buffer for the macro */
} nocsif_ducky_state_t;

/* Creates the idle worker task. Idempotent; call once at boot. */
esp_err_t nocsif_ducky_init(void);

/* Self-test: true once the worker task has confirmed its stack lives in PSRAM. */
bool nocsif_ducky_stack_is_psram(void);

/* Transport a run plays over: USB HID or BLE HID-over-GATT. */
typedef enum {
    NOCSIF_DUCKY_SINK_USB = 0,   /* USB HID: needs the composite up + host + HID endpoint ready */
    NOCSIF_DUCKY_SINK_BLE,       /* BLE HID: needs a bonded, subscribed keyboard host */
} nocsif_ducky_sink_t;

/* Requests a macro run over USB. Non-blocking; ignored if a run is already in
 * progress. `path` is copied; NULL uses NOCSIF_DUCKY_DEFAULT_PATH. */
void nocsif_ducky_request_run(const char *path);                              /* USB (M4)          */

/* Requests a macro run over the given transport. Same semantics as above. */
void nocsif_ducky_request_run_ex(const char *path, nocsif_ducky_sink_t sink);

/* Types a literal string over a transport (no file involved). Text is
 * copied and truncated to an internal buffer if too long. */
void nocsif_ducky_request_type(const char *text, nocsif_ducky_sink_t sink);

/* Returns the current player state, for the UI to poll. */
nocsif_ducky_state_t nocsif_ducky_state(void);

#ifdef __cplusplus
}
#endif
