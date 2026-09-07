/*
 * NocSif — DuckyScript player (M4-P4). See ducky.h.
 *
 * One command per line; the first whitespace-delimited token (case-insensitive) is the
 * keyword. Text is emitted through the active keyboard layout (hid_kbd.c), so STRING honours
 * LOCALE. The whole macro is read into RAM up front and played from there, so releasing the
 * card back to the host mid-run cannot disturb playback. Every exit path releases all keys.
 */
#include "ducky.h"
#include "hid_kbd.h"
#include "usb_gadget.h"
#include "sdcard.h"
#include "ble.h"          /* nocsif_ble_hid_ready — the BLE HID transport readiness gate */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"   /* xTaskCreateWithCaps — PSRAM worker stack (RAM-BUDGET remake #7) */
#include "esp_heap_caps.h"            /* MALLOC_CAP_SPIRAM */
#include "esp_memory_utils.h"         /* esp_ptr_external_ram — PSRAM-stack self-test probe */

#include "tinyusb.h"    /* HID_KEY_* + KEYBOARD_MODIFIER_* */
#include "esp_log.h"

static const char *TAG = "ducky";

#define DUCKY_MAX_FILE     (64 * 1024)   /* macro size cap */
#define DUCKY_KEY_HOLD_MS  6             /* press-hold before release */
#define DUCKY_DELAY_MAX_MS 60000         /* clamp absurd DELAY/DEFAULTDELAY values */
#define DUCKY_HID_WAIT_MS  1000          /* budget to wait for HID-ready before ERR_HID_DOWN */
#define DUCKY_LAST_MAX     1024          /* longest line REPEAT can replay */

static TaskHandle_t          s_task;
static volatile bool         s_stack_ext;        /* worker stack in PSRAM (set on 1st schedule)       */
static volatile nocsif_ducky_state_t s_state = NOCSIF_DUCKY_IDLE;
static char                  s_req_path[128];
static char                  s_req_text[192];   /* inline-type request buffer                        */
static volatile nocsif_ducky_sink_t s_req_sink;  /* transport for the pending request (USB / BLE)    */
static volatile bool         s_req_inline;       /* true = type s_req_text; false = run s_req_path    */

/* --- per-run executor state (single worker task, no reentrancy) --- */
static uint32_t s_default_delay_ms;
static uint32_t s_char_delay_ms;
static bool     s_started;     /* default-delay is applied before every command after the 1st */
static bool     s_in_repeat;   /* guard: a replayed line must not itself REPEAT */

typedef enum { LINE_NONEMIT = 0, LINE_EMIT, LINE_REPEAT } line_result_t;

/* ------------------------------------------------------------------ */
/* Small token helpers                                                 */
/* ------------------------------------------------------------------ */

/* Copy up to n-1 chars of the first whitespace-delimited token at *p, uppercased, into out.
 * out is always NUL-terminated. */
static void token_upper(const char *p, char *out, size_t n)
{
    size_t i = 0;
    while (*p && !isspace((unsigned char)*p) && i < n - 1) {
        out[i++] = (char)toupper((unsigned char)*p);
        p++;
    }
    out[i] = '\0';
}

/* Advance past the current token, then past following whitespace, to the next token (or the
 * terminating NUL). */
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

/* Text argument for STRING/STRINGLN: the text after the keyword and exactly ONE delimiter
 * character, returned VERBATIM. Unlike next_token (which eats all trailing whitespace), this
 * preserves intended leading indentation and multiple spaces in the typed text. */
static const char *string_arg(const char *p)
{
    while (*p && !isspace((unsigned char)*p)) p++;   /* past the keyword */
    if (*p) p++;                                      /* past exactly one delimiter char */
    return p;
}

/* Modifier mask for a modifier token, or 0 if the token is not a modifier. */
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

/* Keycode for a named key token, or 0 if not a named key. */
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

static uint32_t parse_delay(const char *arg)
{
    long v = strtol(arg, NULL, 10);
    if (v < 0) v = 0;
    if (v > DUCKY_DELAY_MAX_MS) v = DUCKY_DELAY_MAX_MS;
    return (uint32_t)v;
}

/* Apply the inter-command default delay before every emitting command after the first. */
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

/* Type a literal string through the active layout, char_delay between characters. */
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

/* Combo/key line: OR the leading modifier tokens, the first non-modifier token is the single
 * final key (named key, or single printable char). All-modifiers => tap the modifier(s). */
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
        /* A combo uses its explicit modifiers; a bare char uses its own (shifted) mapping. */
        nocsif_hid_kbd_key(mod ? mod : cmod, ckc, DUCKY_KEY_HOLD_MS);
        return LINE_EMIT;
    }

    ESP_LOGW(TAG, "unknown command token '%s' — skipped", tok);
    return LINE_NONEMIT;
}

/* Process one macro line. *count receives the REPEAT count when LINE_REPEAT is returned. */
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

static void execute(char *buf)
{
    s_default_delay_ms = 0;
    s_char_delay_ms = 0;
    s_started = false;
    s_in_repeat = false;

    /* Off the stack: this is the worker's single run, and DUCKY_LAST_MAX is large. */
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
        if (len && line[len - 1] == '\r') {   /* P3-dropped files are CRLF */
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

/* Read the whole macro file into a NUL-terminated RAM buffer (caller frees). Returns NULL on
 * open/read/alloc failure; *why is set to the matching error state. Holds the /sd lock. */
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

static void run_file(const char *path)
{
    /* 1. Require the composite up and the HID endpoint ready. */
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

    /* 2. Deterministically claim the card (NOT a host eject). */
    esp_err_t err = nocsif_usb_gadget_claim_sd(2000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "claim SD failed: %s", esp_err_to_name(err));
        s_state = NOCSIF_DUCKY_ERR_SD_CLAIM;
        return;
    }

    /* 3. Read the whole macro into RAM, then hand the card back to the host and play from RAM. */
    nocsif_ducky_state_t why = NOCSIF_DUCKY_ERR_OPEN;
    char *buf = read_macro(path, &why);
    nocsif_usb_gadget_release_sd();
    if (buf == NULL) {
        s_state = why;
        return;
    }

    /* 4. Parse + execute; always release every key afterwards. */
    ESP_LOGI(TAG, "playing macro (%s layout)...", nocsif_hid_kbd_locale());
    execute(buf);
    nocsif_hid_kbd_release_all();
    free(buf);

    ESP_LOGI(TAG, "macro done");
    s_state = NOCSIF_DUCKY_DONE;
}

/* Wait up to DUCKY_HID_WAIT_MS for the given transport to be ready to accept reports. */
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

/* Play a macro FILE over BLE HID. Unlike the USB path there is no USB-MSC host to claim the card away
 * from, so the whole macro is simply read under the /sd lock (read_macro) and played from RAM. */
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

/* Type a literal string over the given transport (no file). */
static void run_inline(const char *text, nocsif_ducky_sink_t sink)
{
    if (!wait_sink_ready(sink)) {
        ESP_LOGW(TAG, "HID not ready — inline type dropped");
        s_state = NOCSIF_DUCKY_ERR_HID_DOWN;
        return;
    }
    nocsif_hid_kbd_set_sink(sink == NOCSIF_DUCKY_SINK_BLE ? NOCSIF_HID_SINK_BLE
                                                         : NOCSIF_HID_SINK_USB);
    /* Reset the per-run pacing state type_string reads (no default/char delays for a live type). */
    s_default_delay_ms = 0;
    s_char_delay_ms    = 0;
    s_started          = false;
    s_in_repeat        = false;
    type_string(text);
    nocsif_hid_kbd_release_all();
    s_state = NOCSIF_DUCKY_DONE;
}

static void ducky_task(void *arg)
{
    (void)arg;
    volatile uint8_t probe;                          /* an on-stack byte: is the stack in PSRAM? */
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
        /* Guarantee release on the transport that was used, then restore the USB default sink. */
        nocsif_hid_kbd_release_all();
        nocsif_hid_kbd_set_sink(NOCSIF_HID_SINK_USB);
    }
}

esp_err_t nocsif_ducky_init(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }
    /* Stack in PSRAM (xTaskCreateWithCaps + SPIRAM, RAM-BUDGET remake #7): the ducky worker plays
     * macros through the HID sink (USB TinyUSB / BLE GATT) and reads scripts via FatFs — it never DMAs
     * from its own stack and never runs with the flash cache disabled (no on-task NVS/flash writes), so
     * its 6 KB no longer competes for the scarce internal-DMA hole. Never deleted -> no WithCaps delete. */
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
