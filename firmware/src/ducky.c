/*
 * NocSif — DuckyScript player (M4-P4). See ducky.h.
 *
 * Parses one command per line (case-insensitive keyword), typing text through
 * the active keyboard layout in hid_kbd.c. The whole macro is read into RAM
 * up front and played from there. Every exit path releases all held keys.
 */
#include "ducky.h"
#include "hid_kbd.h"
#include "usb_gadget.h"
#include "sdcard.h"
#include "ble.h"          /* nocsif_ble_hid_ready() */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"   /* xTaskCreateWithCaps */
#include "esp_heap_caps.h"            /* MALLOC_CAP_SPIRAM */
#include "esp_memory_utils.h"         /* esp_ptr_external_ram */

#include "tinyusb.h"    /* HID_KEY_* + KEYBOARD_MODIFIER_* */
#include "esp_log.h"

static const char *TAG = "ducky";

#define DUCKY_MAX_FILE     (64 * 1024)   /* macro size cap */
#define DUCKY_KEY_HOLD_MS  6             /* press-hold duration before release */
#define DUCKY_DELAY_MAX_MS 60000         /* clamp for DELAY/DEFAULTDELAY values */
#define DUCKY_HID_WAIT_MS  1000          /* time to wait for HID-ready before erroring out */
#define DUCKY_LAST_MAX     1024          /* longest line REPEAT can replay */

static TaskHandle_t          s_task;
static volatile bool         s_stack_ext;        /* true once the worker stack is confirmed in PSRAM */
static volatile nocsif_ducky_state_t s_state = NOCSIF_DUCKY_IDLE;
static char                  s_req_path[128];
static char                  s_req_text[192];   /* buffer for an inline-type request */
static volatile nocsif_ducky_sink_t s_req_sink;  /* transport for the pending request */
static volatile bool         s_req_inline;       /* true = type s_req_text; false = run s_req_path */

/* Per-run executor state; single worker task, so no reentrancy concerns. */
static uint32_t s_default_delay_ms;
static uint32_t s_char_delay_ms;
static bool     s_started;     /* becomes true after the first command runs */
static bool     s_in_repeat;   /* guards against a replayed line itself triggering REPEAT */

typedef enum { LINE_NONEMIT = 0, LINE_EMIT, LINE_REPEAT } line_result_t;

/* ------------------------------------------------------------------ */
/* Small token helpers                                                 */
/* ------------------------------------------------------------------ */

/* Copies the first whitespace-delimited token at *p into out, uppercased and
 * NUL-terminated, up to n-1 chars. */
static void token_upper(const char *p, char *out, size_t n)
{
    size_t i = 0;
    while (*p && !isspace((unsigned char)*p) && i < n - 1) {
        out[i++] = (char)toupper((unsigned char)*p);
        p++;
    }
    out[i] = '\0';
}

/* Returns a pointer to the start of the next token, skipping the current
 * token and any following whitespace. */
static const char *next_token(const char *p)
{
    while (*p && !isspace((unsigned char)*p)) p++;
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

static const char *skip_ws(const char *p)
{
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

/* Returns the STRING/STRINGLN argument verbatim: everything after the
 * keyword and exactly one delimiter char, preserving whitespace. */
static const char *string_arg(const char *p)
{
    while (*p && !isspace((unsigned char)*p)) p++;   /* skip the keyword */
    if (*p) p++;                                      /* skip one delimiter char */
    return p;
}

/* Returns the modifier bitmask for a modifier keyword, or 0 if not one. */
static uint8_t modifier_of(const char *tok)
{
    if (!strcmp(tok, "GUI") || !strcmp(tok, "WINDOWS") || !strcmp(tok, "WIN"))
        return KEYBOARD_MODIFIER_LEFTGUI;
    if (!strcmp(tok, "CTRL") || !strcmp(tok, "CONTROL"))
        return KEYBOARD_MODIFIER_LEFTCTRL;
    if (!strcmp(tok, "ALT"))
        return KEYBOARD_MODIFIER_LEFTALT;
    if (!strcmp(tok, "SHIFT"))
        return KEYBOARD_MODIFIER_LEFTSHIFT;
    return 0;
}

/* Returns the HID keycode for a named key token, or 0 if not one. */
static uint8_t named_key_of(const char *tok)
{
    if (!strcmp(tok, "ENTER") || !strcmp(tok, "RETURN")) return HID_KEY_ENTER;
    if (!strcmp(tok, "ESC")   || !strcmp(tok, "ESCAPE")) return HID_KEY_ESCAPE;
    if (!strcmp(tok, "TAB"))         return HID_KEY_TAB;
    if (!strcmp(tok, "SPACE"))       return HID_KEY_SPACE;
    if (!strcmp(tok, "BACKSPACE"))   return HID_KEY_BACKSPACE;
    if (!strcmp(tok, "DELETE") || !strcmp(tok, "DEL")) return HID_KEY_DELETE;
    if (!strcmp(tok, "INSERT"))      return HID_KEY_INSERT;
    if (!strcmp(tok, "HOME"))        return HID_KEY_HOME;
    if (!strcmp(tok, "END"))         return HID_KEY_END;
    if (!strcmp(tok, "PAGEUP"))      return HID_KEY_PAGE_UP;
    if (!strcmp(tok, "PAGEDOWN"))    return HID_KEY_PAGE_DOWN;
    if (!strcmp(tok, "UP"))          return HID_KEY_ARROW_UP;
    if (!strcmp(tok, "DOWN"))        return HID_KEY_ARROW_DOWN;
    if (!strcmp(tok, "LEFT"))        return HID_KEY_ARROW_LEFT;
    if (!strcmp(tok, "RIGHT"))       return HID_KEY_ARROW_RIGHT;
    if (!strcmp(tok, "CAPSLOCK"))    return HID_KEY_CAPS_LOCK;
    if (!strcmp(tok, "PRINTSCREEN")) return HID_KEY_PRINT_SCREEN;
    if (!strcmp(tok, "MENU") || !strcmp(tok, "APP")) return HID_KEY_APPLICATION;
    if (tok[0] == 'F' && (tok[1] >= '1' && tok[1] <= '9')) {   /* F1..F12 */
        int n = atoi(tok + 1);
        if (n >= 1 && n <= 12) return (uint8_t)(HID_KEY_F1 + (n - 1));
    }
    return 0;
}

/* Parses a delay in ms, clamped to [0, DUCKY_DELAY_MAX_MS]. */
static uint32_t parse_delay(const char *arg)
{
    long v = strtol(arg, NULL, 10);
    if (v < 0) v = 0;
    if (v > DUCKY_DELAY_MAX_MS) v = DUCKY_DELAY_MAX_MS;
    return (uint32_t)v;
}

/* Waits the default inter-command delay before every command after the first. */
static void pre_command_delay(void)
{
    if (s_started) {
        if (s_default_delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(s_default_delay_ms));
        }
    } else {
        s_started = true;
    }
}

/* Types a literal string through the active layout, pausing char_delay between characters. */
static void type_string(const char *s)
{
    for (; *s; s++) {
        nocsif_hid_kbd_char(*s, DUCKY_KEY_HOLD_MS);
        if (s_char_delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(s_char_delay_ms));
        }
    }
}

/* ------------------------------------------------------------------ */
/* Line processing                                                     */
/* ------------------------------------------------------------------ */

/* Processes a modifier-combo/key line: ORs together the leading modifier
 * tokens, then treats the first non-modifier token as the final key (a
 * named key or a single printable char). A line of only modifiers taps them. */
static line_result_t process_combo(const char *p)
{
    uint8_t mod = 0;
    char tok[24];
    const char *cur = p;

    for (;;) {
        token_upper(cur, tok, sizeof(tok));
        uint8_t m = modifier_of(tok);
        if (m == 0) break;
        mod |= m;
        cur = next_token(cur);
        if (*cur == '\0') {                 /* line was only modifiers -> tap them */
            pre_command_delay();
            nocsif_hid_kbd_key(mod, 0, DUCKY_KEY_HOLD_MS);
            return LINE_EMIT;
        }
    }

    uint8_t named = named_key_of(tok);
    if (named != 0) {
        pre_command_delay();
        nocsif_hid_kbd_key(mod, named, DUCKY_KEY_HOLD_MS);
        return LINE_EMIT;
    }

    /* Single printable char (the raw, non-uppercased char at cur). */
    if (cur[0] != '\0' && (cur[1] == '\0' || isspace((unsigned char)cur[1]))) {
        uint8_t cmod, ckc;
        pre_command_delay();
        if (!nocsif_hid_kbd_lookup(cur[0], &cmod, &ckc) || ckc == 0) {
            ESP_LOGW(TAG, "combo key '%c' unmapped — skipped", cur[0]);
            return LINE_EMIT;
        }
        /* An explicit combo uses its own modifiers; a bare char uses its shifted mapping. */
        nocsif_hid_kbd_key(mod ? mod : cmod, ckc, DUCKY_KEY_HOLD_MS);
        return LINE_EMIT;
    }

    ESP_LOGW(TAG, "unknown command token '%s' — skipped", tok);
    return LINE_NONEMIT;
}

/* Processes one macro line. *count receives the REPEAT count when LINE_REPEAT is returned. */
static line_result_t process_line(const char *line, int *count)
{
    const char *p = skip_ws(line);
    if (*p == '\0') {
        return LINE_NONEMIT;
    }

    char kw[24];
    token_upper(p, kw, sizeof(kw));
    const char *arg = next_token(p);          /* start of the argument text (or NUL) */

    if (!strcmp(kw, "REM")) {
        return LINE_NONEMIT;
    }
    if (!strcmp(kw, "STRING")) {
        pre_command_delay();
        type_string(string_arg(p));   /* verbatim text (preserves leading whitespace) */
        return LINE_EMIT;
    }
    if (!strcmp(kw, "STRINGLN")) {
        pre_command_delay();
        type_string(string_arg(p));
        nocsif_hid_kbd_key(0, HID_KEY_ENTER, DUCKY_KEY_HOLD_MS);
        return LINE_EMIT;
    }
    if (!strcmp(kw, "DELAY")) {
        pre_command_delay();
        vTaskDelay(pdMS_TO_TICKS(parse_delay(arg)));
        return LINE_EMIT;
    }
    if (!strcmp(kw, "DEFAULTDELAY") || !strcmp(kw, "DEFAULT_DELAY")) {
        s_default_delay_ms = parse_delay(arg);
        return LINE_NONEMIT;
    }
    if (!strcmp(kw, "DEFAULTCHARDELAY") || !strcmp(kw, "DEFAULT_CHAR_DELAY")) {
        s_char_delay_ms = parse_delay(arg);
        return LINE_NONEMIT;
    }
    if (!strcmp(kw, "LOCALE")) {
        char loc[16];
        token_upper(arg, loc, sizeof(loc));
        if (!nocsif_hid_kbd_set_locale(loc)) {
            ESP_LOGW(TAG, "unknown LOCALE '%s' — keeping %s", loc, nocsif_hid_kbd_locale());
        }
        return LINE_NONEMIT;
    }
    if (!strcmp(kw, "REPEAT")) {
        if (count) *count = (int)strtol(arg, NULL, 10);
        return LINE_REPEAT;
    }

    return process_combo(p);
}

/* ------------------------------------------------------------------ */
/* Execution over the in-RAM macro buffer                              */
/* ------------------------------------------------------------------ */

/* Splits the buffer into lines (handling CRLF) and executes each in turn,
 * replaying the last emitting line on REPEAT. */
static void execute(char *buf)
{
    s_default_delay_ms = 0;
    s_char_delay_ms = 0;
    s_started = false;
    s_in_repeat = false;

    /* Static, not stack: this is the worker's single run, and DUCKY_LAST_MAX is large. */
    static char last[DUCKY_LAST_MAX];
    bool have_last = false;
    last[0] = '\0';

    char *p = buf;
    while (*p) {
        char *nl = strchr(p, '\n');
        char *line = p;
        if (nl) {
            *nl = '\0';
            p = nl + 1;
        } else {
            p += strlen(p);
        }
        size_t len = strlen(line);
        if (len && line[len - 1] == '\r') {   /* strip CRLF line endings */
            line[len - 1] = '\0';
        }

        int count = 0;
        line_result_t r = process_line(line, &count);
        if (r == LINE_REPEAT) {
            if (have_last && count > 0) {
                s_in_repeat = true;
                for (int i = 0; i < count; i++) {
                    process_line(last, NULL);
                }
                s_in_repeat = false;
            }
        } else if (r == LINE_EMIT && !s_in_repeat) {
            strncpy(last, line, sizeof(last) - 1);
            last[sizeof(last) - 1] = '\0';
            have_last = true;
        }
    }
}

/* Reads the whole macro file into a NUL-terminated RAM buffer (caller frees).
 * Returns NULL on open/read/alloc failure and sets *why accordingly.
 * Holds the /sd lock only for the duration of the read. */
static char *read_macro(const char *path, nocsif_ducky_state_t *why)
{
    if (!nocsif_sdcard_lock(3000)) {
        *why = NOCSIF_DUCKY_ERR_OPEN;
        return NULL;
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "fopen(%s) failed", path);
        nocsif_sdcard_unlock();
        *why = NOCSIF_DUCKY_ERR_OPEN;
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) sz = 0;
    if (sz > DUCKY_MAX_FILE) sz = DUCKY_MAX_FILE;

    char *buf = malloc((size_t)sz + 1);
    if (buf == NULL) {
        fclose(f);
        nocsif_sdcard_unlock();
        *why = NOCSIF_DUCKY_ERR_NOMEM;
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)sz, f);
    buf[got] = '\0';
    fclose(f);
    nocsif_sdcard_unlock();
    ESP_LOGI(TAG, "macro %s: %u bytes", path, (unsigned)got);
    return buf;
}

/* Runs a macro file over USB HID: waits for HID readiness, claims the SD
 * card, reads the macro into RAM, releases the card, then executes it. */
static void run_file(const char *path)
{
    /* Wait for the composite device and HID endpoint to be ready. */
    uint32_t waited = 0;
    while (!nocsif_usb_gadget_hid_ready()) {
        if (waited >= DUCKY_HID_WAIT_MS) {
            ESP_LOGW(TAG, "USB HID not ready — need a connected host with gadget mode ON");
            s_state = NOCSIF_DUCKY_ERR_HID_DOWN;
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
        waited += 20;
    }

    /* Claim the card deterministically (not a host eject). */
    esp_err_t err = nocsif_usb_gadget_claim_sd(2000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "claim SD failed: %s", esp_err_to_name(err));
        s_state = NOCSIF_DUCKY_ERR_SD_CLAIM;
        return;
    }

    /* Read the macro into RAM, then hand the card back to the host and play from RAM. */
    nocsif_ducky_state_t why = NOCSIF_DUCKY_ERR_OPEN;
    char *buf = read_macro(path, &why);
    nocsif_usb_gadget_release_sd();
    if (buf == NULL) {
        s_state = why;
        return;
    }

    /* Parse and execute; always release every key afterwards. */
    ESP_LOGI(TAG, "playing macro (%s layout)...", nocsif_hid_kbd_locale());
    execute(buf);
    nocsif_hid_kbd_release_all();
    free(buf);

    ESP_LOGI(TAG, "macro done");
    s_state = NOCSIF_DUCKY_DONE;
}

/* Waits up to DUCKY_HID_WAIT_MS for the given transport to accept reports. */
static bool wait_sink_ready(nocsif_ducky_sink_t sink)
{
    uint32_t waited = 0;
    for (;;) {
        bool ready = (sink == NOCSIF_DUCKY_SINK_BLE) ? nocsif_ble_hid_ready()
                                                     : nocsif_usb_gadget_hid_ready();
        if (ready) {
            return true;
        }
        if (waited >= DUCKY_HID_WAIT_MS) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
        waited += 20;
    }
}

/* Plays a macro file over BLE HID. There is no USB-MSC host to claim the
 * card from, so the file is simply read under the /sd lock and played from RAM. */
static void run_macro_ble(const char *path)
{
    if (!wait_sink_ready(NOCSIF_DUCKY_SINK_BLE)) {
        ESP_LOGW(TAG, "BLE HID not ready — pair a keyboard host first");
        s_state = NOCSIF_DUCKY_ERR_HID_DOWN;
        return;
    }
    nocsif_ducky_state_t why = NOCSIF_DUCKY_ERR_OPEN;
    char *buf = read_macro(path, &why);
    if (buf == NULL) {
        s_state = why;
        return;
    }
    nocsif_hid_kbd_set_sink(NOCSIF_HID_SINK_BLE);
    ESP_LOGI(TAG, "playing macro over BLE (%s layout)...", nocsif_hid_kbd_locale());
    execute(buf);
    nocsif_hid_kbd_release_all();
    free(buf);
    ESP_LOGI(TAG, "macro done (BLE)");
    s_state = NOCSIF_DUCKY_DONE;
}

/* Types a literal string over the given transport (no file involved). */
static void run_inline(const char *text, nocsif_ducky_sink_t sink)
{
    if (!wait_sink_ready(sink)) {
        ESP_LOGW(TAG, "HID not ready — inline type dropped");
        s_state = NOCSIF_DUCKY_ERR_HID_DOWN;
        return;
    }
    nocsif_hid_kbd_set_sink(sink == NOCSIF_DUCKY_SINK_BLE ? NOCSIF_HID_SINK_BLE
                                                         : NOCSIF_HID_SINK_USB);
    /* Reset pacing state for a live type (no default/char delays apply). */
    s_default_delay_ms = 0;
    s_char_delay_ms    = 0;
    s_started          = false;
    s_in_repeat        = false;
    type_string(text);
    nocsif_hid_kbd_release_all();
    s_state = NOCSIF_DUCKY_DONE;
}

/* Worker task: waits for a run/type request, executes it, then releases
 * keys and restores the default USB sink before waiting for the next request. */
static void ducky_task(void *arg)
{
    (void)arg;
    volatile uint8_t probe;                          /* stack-resident byte, used to test PSRAM placement */
    s_stack_ext = esp_ptr_external_ram((void *)&probe);
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        s_state = NOCSIF_DUCKY_RUNNING;
        if (s_req_inline) {
            run_inline(s_req_text, s_req_sink);
        } else if (s_req_sink == NOCSIF_DUCKY_SINK_BLE) {
            run_macro_ble(s_req_path[0] ? s_req_path : NOCSIF_DUCKY_DEFAULT_PATH);
        } else {
            run_file(s_req_path[0] ? s_req_path : NOCSIF_DUCKY_DEFAULT_PATH);
        }
        /* Guarantee key release on the transport used, then restore the USB default sink. */
        nocsif_hid_kbd_release_all();
        nocsif_hid_kbd_set_sink(NOCSIF_HID_SINK_USB);
    }
}

esp_err_t nocsif_ducky_init(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }
    /* Stack allocated in PSRAM: the worker only plays macros through the HID
     * sink and reads scripts via FatFs, never DMAs from its own stack, so it
     * doesn't need to compete for the scarce internal-DMA memory. Never deleted. */
    if (xTaskCreateWithCaps(ducky_task, "ducky", 6144, NULL, 4, &s_task, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "failed to create ducky task");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "DuckyScript player ready (idle) — default macro %s", NOCSIF_DUCKY_DEFAULT_PATH);
    return ESP_OK;
}

bool nocsif_ducky_stack_is_psram(void)
{
    return s_stack_ext;             /* set true once the worker first schedules on a PSRAM stack */
}

void nocsif_ducky_request_run_ex(const char *path, nocsif_ducky_sink_t sink)
{
    if (s_task == NULL || s_state == NOCSIF_DUCKY_RUNNING) {
        return;   /* not ready, or a run is already in progress */
    }
    s_req_inline = false;
    s_req_sink   = sink;
    if (path != NULL && path[0] != '\0') {
        strncpy(s_req_path, path, sizeof(s_req_path) - 1);
        s_req_path[sizeof(s_req_path) - 1] = '\0';
    } else {
        s_req_path[0] = '\0';
    }
    xTaskNotifyGive(s_task);
}

void nocsif_ducky_request_run(const char *path)
{
    nocsif_ducky_request_run_ex(path, NOCSIF_DUCKY_SINK_USB);
}

void nocsif_ducky_request_type(const char *text, nocsif_ducky_sink_t sink)
{
    if (s_task == NULL || s_state == NOCSIF_DUCKY_RUNNING) {
        return;
    }
    s_req_inline = true;
    s_req_sink   = sink;
    strncpy(s_req_text, text ? text : "", sizeof(s_req_text) - 1);
    s_req_text[sizeof(s_req_text) - 1] = '\0';
    xTaskNotifyGive(s_task);
}

nocsif_ducky_state_t nocsif_ducky_state(void)
{
    return s_state;
}
