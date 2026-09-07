# M4-P4 implementation plan — CDC+MSC+HID composite (HID keyboard / DuckyScript player)

Sourced + adversarially-verified against the **real** `esp_tinyusb` 2.2.1 + TinyUSB headers/examples
on disk. As of **2026-08-09**. Read alongside `usb-composite-device-2026-08-06.md` (the M4 master plan).
Neutral device-class framing per `RESUME.md`. This doc supersedes the P4 API shorthand in the master
plan where they differ (the master plan was written from memory; this one from the headers).

Ground-truth component tree (extracted, read by the design workflow):
`D:\NocSif_Firmware\nocsif-m4-capability-b14b14\firmware\managed_components\` →
`espressif__esp_tinyusb` (v2.2.1 wrapper) + `espressif__tinyusb` (stock port, incl. `hid_composite` /
`cdc_msc` examples). Design workflow: `nocsif-m4-p4-hid-design` (6 agents: 4 verifiers + adversarial
descriptor auditor + synthesis), 2026-08-09.

## Locked decisions (user, 2026-08-09)
- **(a) Device identity = compile-time toggle.** Default a **spoofed generic-keyboard** `idVendor/idProduct`;
  Espressif `0x303A` selectable for dev/bring-up. HID binds to the host by **interface class 0x03**,
  independent of VID/PID (`TUD_HID_DESCRIPTOR` sets `bInterfaceClass=TUSB_CLASS_HID`), so VID/PID only
  changes how the composite is *identified*/allow-listed. Single `#define`-gated block. VID/PID lives in
  the hand-written **device** descriptor (composite-with-IAD → device class must be
  `TUSB_CLASS_MISC/MISC_SUBCLASS_COMMON/MISC_PROTOCOL_IAD`).
- **(b) DuckyScript = FULLER.** minimal-plus (REPEAT/loops + arrows + F1–F12 + nav keys) **plus** runtime
  **non-US LOCALE** via remappable keymap tables (US base; UK/GB + DE shipped as proof; more layouts =
  pure data tables).

## esp_tinyusb 2.2.1 API correction (read from `include/tinyusb.h:70-77,121`)
`tinyusb_config_t.descriptor` is a **struct** (`tinyusb_desc_config_t`), NOT a single pointer (the master
plan's shorthand). For the hand-written composite set:
- `.descriptor.device` → `&p4_desc_device` (VID/PID, IAD device class)
- `.descriptor.full_speed_config` → the CDC+MSC+HID config byte array (S3 is full-speed)
- `.descriptor.string` + `.string_count` → the string-descriptor array (count is **literal**, not NULL-terminated)
- `.descriptor.qualifier` = NULL, `.descriptor.high_speed_config` = NULL (FS-only S3)
Any NULL data-bearing field falls back to the built-in default CDC+MSC (which has **no HID**).

---

## 1. Hand-written descriptors — AUDITOR-VERIFIED (all 6 checks PASS)

### Device identity toggle (`nocsif_usb_desc.h`)
```c
#ifndef NOCSIF_USB_DEV_IDENTITY
#define NOCSIF_USB_DEV_IDENTITY 0   // 0 = generic-keyboard identity (default); 1 = Espressif dev identity
#endif
#if NOCSIF_USB_DEV_IDENTITY
  #define NOCSIF_USB_VID 0x303A     // Espressif Systems — dev/bring-up only
  #define NOCSIF_USB_PID 0x4004     // CDC+MSC+HID (dev) — MUST differ from the P3 CDC+MSC PID
#else
  #define NOCSIF_USB_VID 0x1A2C     // generic USB-keyboard vendor id (default)
  #define NOCSIF_USB_PID 0x2124     // generic keyboard product id — MUST NOT equal 0x303A / P3 PID
#endif
```
The exact generic pair is illustrative — **confirm on the target host** (a duplicate PID surfaces as a
stale cached driver, a config fix not a code fix).

### Interface enum + endpoint map (`nocsif_usb_desc.c`)
CDC must be interfaces 0–1 (its IAD groups two, and `TUD_CDC_DESCRIPTOR`'s functional descriptors
hard-reference `_itfnum+1` as the data interface, `usbd.h:270,274`).
```c
enum { ITF_NUM_CDC = 0, ITF_NUM_CDC_DATA, ITF_NUM_MSC, ITF_NUM_HID, ITF_NUM_TOTAL /* =4 */ };
// addresses already carry bit 7 for IN — pass to TUD_*_DESCRIPTOR AS-IS (do NOT re-OR 0x80):
#define EPNUM_CDC_NOTIF 0x81  // IN  interrupt
#define EPNUM_CDC_OUT   0x02  // OUT bulk
#define EPNUM_CDC_IN    0x82  // IN  bulk
#define EPNUM_MSC_OUT   0x03  // OUT bulk
#define EPNUM_MSC_IN    0x83  // IN  bulk
#define EPNUM_HID_IN    0x84  // IN  interrupt  (0x81 from the hid_composite example would COLLIDE with CDC notif)
```

### Endpoint budget — EXACTLY at the S3 hardware limit, ZERO IN headroom [auditor CHECK 1]
The binding constraint is `ep_in_count = 5` **including EP0** (`dwc2_esp32.h:51`), enforced by
`TU_ASSERT(allocated_epin_count < 5)` at `dcd_dwc2.c:226-228`; EP0-IN is counted (`dcd_dwc2.c:268`).
Overflow → `dcd_edpt_open` returns false → **silent enumeration failure, no build error**.

| Dir | Endpoints | Count |
|---|---|---|
| IN | EP0(0x80) + CDC-notif 0x81 + CDC-in 0x82 + MSC-in 0x83 + HID-in 0x84 | **5 / 5** |
| OUT | EP0(0x00) + CDC-out 0x02 + MSC-out 0x03 | 3 |

The plan doc's "EP0 + 5 IN" was wrong (would be 6 IN → overflow). Correct: **EP0-IN + 4 class-IN = 5 total.**
Use the single-IN `TUD_HID_DESC_LEN`(25), NOT the INOUT variant (32) — a keyboard needs no OUT EP.
**Hard ceiling for any future phase: no more IN endpoints can be added.**

### Device descriptor (composite-with-IAD) [auditor CHECK 5]
```c
static const tusb_desc_device_t p4_desc_device = {
    .bLength=sizeof(tusb_desc_device_t), .bDescriptorType=TUSB_DESC_DEVICE, .bcdUSB=0x0200,
    .bDeviceClass=TUSB_CLASS_MISC, .bDeviceSubClass=MISC_SUBCLASS_COMMON, .bDeviceProtocol=MISC_PROTOCOL_IAD,
    .bMaxPacketSize0=CFG_TUD_ENDPOINT0_SIZE, .idVendor=NOCSIF_USB_VID, .idProduct=NOCSIF_USB_PID,
    .bcdDevice=0x0100, .iManufacturer=0x01, .iProduct=0x02, .iSerialNumber=0x03, .bNumConfigurations=0x01,
};
```

### Configuration descriptor — wTotalLength = 123 [auditor CHECK 3]
`TUD_CONFIG_DESC_LEN=9` + `TUD_CDC_DESC_LEN=66` + `TUD_MSC_DESC_LEN=23` + `TUD_HID_DESC_LEN=25` = **123**.
Express symbolically so the compiler computes it.
```c
#define NOCSIF_CFG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN + TUD_HID_DESC_LEN)
static const uint8_t p4_desc_fs_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, NOCSIF_CFG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, STRID_CDC_INTERFACE, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, STRID_MSC_INTERFACE, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, STRID_HID_INTERFACE, HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof(desc_hid_report), EPNUM_HID_IN, CFG_TUD_HID_EP_BUFSIZE, 5),
};
```
Resolved arg choices: config attr `REMOTE_WAKEUP` + power 100 mA (P3 parity); CDC notif EP size 8 (P3
parity); HID boot proto `HID_ITF_PROTOCOL_KEYBOARD` (BIOS/login typing — safe only with the ID-less report
desc); HID poll interval 5 ms; HID EP size `CFG_TUD_HID_EP_BUFSIZE` (defaults 64).

### HID report descriptor — keyboard only, NO report ID
```c
uint8_t const desc_hid_report[] = { TUD_HID_REPORT_DESC_KEYBOARD() };  // empty () = no HID_REPORT_ID
```
8-byte boot-keyboard report (modifier, reserved, keycode[6]). **All send calls use `report_id = 0`** — a
non-zero id prepends a byte and corrupts every keystroke.

### String array + count [auditor CHECK 6]
```c
enum { STRID_LANGID=0, STRID_MANUFACTURER, STRID_PRODUCT, STRID_SERIAL,
       STRID_CDC_INTERFACE, STRID_MSC_INTERFACE, STRID_HID_INTERFACE };
static const char *p4_string_desc[] = {
    (const char[]){0x09,0x04}, "NocSif", "NocSif USB Gadget", "0001",   // serial MUST be non-NULL (STALL otherwise)
    "NocSif CDC", "NocSif MSC", "NocSif HID Keyboard",
};  // string_count = 7 (literal; cap is 8; do NOT append a trailing NULL)
```

### tinyusb_config_t wiring (`usb_gadget.c`)
```c
tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();   // drop const
tusb_cfg.descriptor.device            = &p4_desc_device;
tusb_cfg.descriptor.string            = p4_string_desc;
tusb_cfg.descriptor.string_count      = 7;
tusb_cfg.descriptor.full_speed_config = p4_desc_fs_configuration;
// qualifier / high_speed_config stay NULL
```

---

## 2. HID typing engine + keymap/LOCALE layer (`hid_kbd.{c,h}`)

Three mandatory callbacks (esp_tinyusb provides none): `tud_hid_descriptor_report_cb` → `desc_hid_report`;
`tud_hid_get_report_cb` → `return 0` (STALL, we never source input over control); `tud_hid_set_report_cb`
→ capture `buffer[0]` into `g_kbd_led_state` (Caps/Num lock).

Per-char press/release (gate BOTH on `tud_hid_ready()` or the report is silently dropped):
```c
keymap_entry_t e = active->map[(uint8_t)ch]; if (!e.keycode) return;  // unmapped -> skip, never press 0
uint8_t kc[6]={e.keycode,0,0,0,0,0};
while(!tud_hid_ready()) vTaskDelay(1); tud_hid_keyboard_report(0, e.modifier, kc);  // PRESS
vTaskDelay(key_hold_ms);
while(!tud_hid_ready()) vTaskDelay(1); tud_hid_keyboard_report(0, 0, NULL);         // RELEASE (mandatory)
vTaskDelay(inter_key_ms);
```
Never call `tud_task()` (esp_tinyusb runs it in the stack task). Release between presses is what makes
`ll` in "hello" register as two events not one auto-repeat.

Keymap = data: `keymap_entry_t{modifier,keycode}` (`keycode 0 = unmapped`); US base aliases
`HID_ASCII_TO_KEYCODE` (`[128][2] = {needs_shift, keycode}`); a locale = US base + sparse diff list.
- **GB/UK diff:** `@`/`"` swap; `#`,`~`,`\`,`|` → ISO keys `HID_KEY_EUROPE_1`(0x32)/`EUROPE_2`(0x64). `£`,`¬` unmapped.
- **DE/QWERTZ diff:** z/y swap; shifted-number symbols; `+/*` on `BRACKET_RIGHT`, `ß/?` on `MINUS`; ISO `# '`
  and `< >`; **AltGr(`RIGHTALT`=0x40) block** for `@ [ ] { } \ |`; dead keys `^` `` ` `` → accent+SPACE or unmapped.
- **LOCALE `<code>`** picks the active table by name; unknown → US + warn. `bCountryCode` stays 0 (host's
  active layout is authoritative — the whole reason the software keymap exists). **Validate on-device.**

CapsLock: keycodes are physical, so host lock state decides case — read `g_kbd_led_state` and invert the
Shift decision for alphabetics when CapsLock is set (or toggle at macro start). NumLock irrelevant (main block).

---

## 3. DuckyScript parser/executor (`ducky.{c,h}`)

One command per line; first uppercase token = keyword. Grammar (fuller):
`REM`, `STRING`, `STRINGLN`(=STRING+ENTER), `ENTER`/`RETURN`, `DELAY <n>`, `DEFAULTDELAY`/`DEFAULT_DELAY`,
`DEFAULTCHARDELAY`/`DEFAULT_CHAR_DELAY`, `GUI`/`WINDOWS` `CTRL`/`CONTROL` `ALT` `SHIFT` (combinable
prefixes; alone → tap that modifier), named keys (`ESC TAB SPACE BACKSPACE DELETE HOME END INSERT PAGEUP
PAGEDOWN`), `UP DOWN LEFT RIGHT`, `F1..F12` (`0x3A+(n-1)`), `CAPSLOCK PRINTSCREEN MENU/APP`, `REPEAT <n>`,
`LOCALE <code>`, single printable char. Unknown token → skip + `ESP_LOGW` (non-fatal; `DUCKY_STRICT 0`).

Executor state: `default_delay_ms`, `char_delay_ms`, `last_line`+`have_last` (REPEAT register), active `km`,
`in_repeat` guard. DEFAULTDELAY applied before each executable command once one has run. REPEAT replays the
stored previous **emitting** line n *additional* times (`DUCKY_REPEAT_INCLUSIVE 0`; duckencode semantics);
REM/blank/DEFAULT*/LOCALE do not update the register. Combo line = OR modifier tokens, first non-modifier is
the single final key → one held report then one all-zero release. STRING types each char through the active
keymap with `char_delay_ms` spacing. DELAY clamps absurd values to 60 s.

Public API (`ducky.h`): `nocsif_ducky_init()` (idle worker task, prio 4, ~4 KB, after `nocsif_usb_gadget_init`);
`nocsif_ducky_request_run(path)` (non-blocking, LVGL-cb-safe, NULL→`/sd/payload.txt`); `nocsif_ducky_run_file(path)`
(worker-only core); `nocsif_ducky_state()`. File read: `fopen`/`fgets` (`DUCKY_MAX_LINE=512`), strip `\n` then
`\r` (P3 drops are CRLF); long STRING lines continued chunk-by-chunk; other long lines truncate+warn.

Safety (every path): claim `MOUNT_APP` first (deterministic, not host eject); gate start on
`tud_mounted() && tud_hid_ready()` (≤1 s budget else `ERR_HID_DOWN`, no keys); `run_file` **always**
`release_all` on every exit (success/parse-error/abort) and every primitive self-releases → stuck keys
impossible; missing file → `ERR_OPEN`; can't claim → `ERR_SD_CLAIM`; unknown LOCALE → US + warn + continue.

Example `/sd/payload.txt`:
```
REM NocSif ducky FULLER demo
DEFAULTDELAY 200
DEFAULTCHARDELAY 5
GUI r
DELAY 500
STRING notepad
ENTER
DELAY 800
STRINGLN hello from nocsif
DOWN
REPEAT 2
F5
LOCALE DE
STRINGLN gruesse   z
```

---

## 4. Integration + card claim (`usb_gadget.{c,h}`, `ui.c`, `main.c`)

Mount enum (real names): `TINYUSB_MSC_STORAGE_MOUNT_USB`(=0 default) / `TINYUSB_MSC_STORAGE_MOUNT_APP`
(`tinyusb_msc.h:30-33`). The MSC storage helper never touches any descriptor (`tinyusb_msc.c`/`storage_sdmmc.c`
only install the MSC driver + map the LUN), so our `descriptor.*` does not conflict — init order is unchanged
from P3: raw card → `tinyusb_msc_new_storage_sdmmc` → `set_storage_callback` → set `descriptor.*` →
`tinyusb_driver_install` → CDC init. HID needs no init call (comes up from the descriptor; only the 3 callbacks).

Deterministic card claim (add accessors so ducky doesn't reach into `s_msc`):
```c
esp_err_t nocsif_usb_gadget_claim_sd(uint32_t timeout_ms);  // set MOUNT_APP + wait MOUNT_COMPLETE/timeout
void      nocsif_usb_gadget_release_sd(void);               // best-effort MOUNT_USB
```
`tud_msc_test_unit_ready_cb` returns true only in MOUNT_USB, so while the app owns the card the host sees
"no media" and cannot race the FAT. Chosen hardening: **deterministic MOUNT_APP claim + read macro into RAM
immediately + play from RAM** (a later host re-configure can't disturb a run once cached); escalate to
`auto_mount_off=1` only if on-device reliability demands it.

"Run Macro" button (`ui.c`): clone the "USB Gadget" toggle exactly — cb only `nocsif_ducky_request_run(NULL)`,
built under `lvgl_port_lock(0)`, status timer reflects `nocsif_ducky_state()`. Gate on
`nocsif_usb_gadget_state()==NOCSIF_USB_GADGET_ON`; worker fires only when `tud_mounted() && tud_hid_ready()`.

## 5. Build config
- `sdkconfig.defaults`: add `CONFIG_TINYUSB_HID_COUNT=1` (**int**, `range 0 4`, NOT `=y`). Without it the HID
  class driver is compiled out and the advertised endpoints never open → silent fail. Keep CDC/MSC from P2/P3.
  **Post-build verify:** `grep CONFIG_TINYUSB_HID_COUNT firmware/build/config/sdkconfig.h` → `1`.
- `CMakeLists.txt`: add `ducky.c`, `hid_kbd.c`, `nocsif_usb_desc.c` to SRCS. No new component (esp_tinyusb
  already in PRIV_REQUIRES supplies HID). `idf_component.yml`: no change (`esp_tinyusb: "^2"`).

## 6. On-device verification checklist
1. Enumerates as composite **CDC + MSC + HID** (Device Manager / `lsusb -v`), IAD device class EF/02/01,
   chosen VID/PID, no Code 10. 2. CDC log console still opens. 3. microSD still mounts as a drive when
   app-released; "no media" while app-owned. 4. HID caret: a test keystroke shows a char in a PC editor.
5. Run Macro gated ("needs USB host") until `tud_mounted() && tud_hid_ready()`; then types the `/sd` macro
   end-to-end. 6. DELAY honored (500/800 ms match wall-clock). 7. `GUI r` opens Run; `DOWN`/`F5` act; combos
   press+release once. 8. `LOCALE DE` types `gruesse   z` correctly on a German host. 9. No stuck keys at end /
   on unmount. 10. CDC+MSC+HID all still enumerated post-run.

## 7. Ranked risks
1. **Descriptor silent enumeration failure** — symbolic `NOCSIF_CFG_TOTAL_LEN`, all 4 `descriptor.*` set,
   auditor-verified values verbatim. 2. **IN-EP overflow (zero headroom)** — keep single-IN keyboard; 5 IN is
   the ceiling. 3. **`CONFIG_TINYUSB_HID_COUNT` unset/=y** — int `=1` + post-build grep. 4. **report_id≠0** —
   ID-less desc + `report_id=0` everywhere. 5. **Stuck keys** — guaranteed `release_all`. 6. **Readiness gating**
   — gate every press+release + Run-Macro. 7. **Card race** — deterministic claim + RAM-cache. 8. **NULL serial
   STALL** — real `"0001"`. 9. **Keymap vs host layout** — on-device validate, data-only fix. 10. **Host CapsLock**
   inverts case — read `g_kbd_led_state`.

## Primary sources (read on disk)
esp_tinyusb 2.2.1: `include/tinyusb.h`, `include/tinyusb_msc.h`, `Kconfig`, `descriptors_control.c`,
`usb_descriptors.c`, `tinyusb.c`, `tinyusb_msc.c`, `storage_sdmmc.c`, `test_apps/msc_storage`.
TinyUSB: `src/device/usbd.h` (TUD_*_DESCRIPTOR + *_DESC_LEN), `src/class/hid/hid.h`
(HID_ASCII_TO_KEYCODE, HID_KEY_*, modifier masks), `src/class/hid/hid_device.h` (tud_hid_* API),
`src/portable/synopsys/dwc2/dwc2_esp32.h` + `dcd_dwc2.c` (endpoint budget), examples
`device/hid_composite`, `device/cdc_msc`. Design workflow `nocsif-m4-p4-hid-design`, 2026-08-09.
