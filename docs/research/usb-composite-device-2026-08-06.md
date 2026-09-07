# M4 plan — composite USB device (SD file-drop + USB HID keyboard macro)

Sourced + adversarially-verified research → phased implementation plan. As of **2026-08-06**.
Read this before coding any M4 USB phase. Neutral framing per `docs/RESUME.md` (Wording convention).

## Decision (user-approved 2026-08-06)
Build a **composite USB device** on the ESP32-S3 native USB: a **USB Mass-Storage (MSC)** interface
backed by the microSD (host copies files onto the card) **+** a **USB HID keyboard** that plays a
DuckyScript text macro read from that card. Delivered as **four independently-flashable milestones**,
front-loading the two items the fact-check flagged as risky so each is proven in isolation:

| # | Milestone | Proves | Effort |
|---|-----------|--------|--------|
| **P1** | SD FAT mount over the shared SPI bus, **no USB** | ALDO1 rail + shared-bus + SDSPI→FAT, over COM7 | ~0.5–1 d |
| **P2** | TinyUSB + CDC-ACM log console behind a UI toggle | PHY handoff + log survival, isolated | ~1 d |
| **P3** | +MSC → CDC+MSC composite (host copies files to card) | SDSPI-backed MSC + single-owner FAT handoff | ~1–2 d |
| **P4** | +HID → CDC+MSC+HID DuckyScript player | hand-written composite descriptor + keymap | ~2–3 d |

Rejected: **composite-in-one-shot** (stacks 3 unknowns into one un-bisectable flash).
Documented contingency: **HID-first with an embedded/flash macro** — trigger only if P3's SDSPI-MSC
proves unreliable on-device (P1 mounting `/sd` natively means the HID macro-read path does not depend
on MSC working, which is what keeps this fallback cheap).

## The three findings that shaped it
1. **Console handoff is solved cleanly.** `tinyusb_driver_install()` re-routes the one internal USB PHY
   from USB-Serial/JTAG to USB-OTG → **COM7 goes dark the instant TinyUSB starts**. Fix: **do NOT install
   TinyUSB at boot** — gate it behind an on-screen "USB Gadget" toggle. Everyday `flash → read COM7` is
   unchanged; only when gadget mode is entered does the PHY hand over, and logs continue over a TinyUSB
   **CDC-ACM** port via `esp_tusb_init_console(TINYUSB_CDC_ACM_0)`.
   - Rejected: `CONFIG_ESP_CONSOLE_USB_CDC` (Espressif documents it incompatible with the TinyUSB stack);
     a hardware-UART console (no confirmed free pad — UART0 GPIO43/44 are the on-board GNSS); burning the
     `USB_PHY_SEL` eFuse (permanently flips the boot default to USB-OTG → kills the JTAG console **and**
     the ROM flash path — never do this).
   - Accepted limitation: bootloader + pre-enumeration logs never reach the CDC port. Fine — the post-flash
     read happens on COM7 with gadget mode **off**.
2. **The one real unknown is SDSPI-backed MSC.** No official Espressif example exposes an *SPI-mode* SD over
   MSC (all use the SDMMC peripheral). `sdmmc_card_t` is host-agnostic in principle (block R/W route through
   `sdmmc_read/write_sectors`, which work over SDSPI), but it is **verify-on-device**. P1 therefore brings the
   card up as a plain FAT mount with **no USB** so a card/rail/bus failure is diagnosed over the comfortable
   COM7 console before TinyUSB ever blinds it.
3. **Endpoints fit exactly.** S3 USB-OTG is full-speed: EP0 + 5 IN endpoints max. CDC(2 IN) + MSC(1 IN) +
   HID(1 IN) + EP0 = exactly 5 IN → **zero headroom** but it fits. Dropping CDC (log to UART) would free 2 IN.

## Console strategy (load-bearing)
HYBRID, deferred. Keep `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` as boot + everyday-dev console. Never call
`tinyusb_driver_install()` at boot during bring-up — gate it behind the UI toggle (clone `ui.c`'s
`lv_button` + `lvgl_port_lock` pattern; the click cb only **signals a worker task**, it must not block on
USB/SD). In gadget mode: include a CDC-ACM interface and `esp_tusb_init_console(TINYUSB_CDC_ACM_0)`;
`esp_tusb_deinit_console` reverts. Flashing stays the manual BOOT/RST + `esptool --no-stub` dance (ROM
downloader forces USB-Serial/JTAG regardless of the app's USB-OTG use).

## Phases in detail

### P1 — SD FAT mount over shared SPI (no USB)  ← **DONE, verified on-device 2026-08-08**
**Result:** 60 GB card mounts over SPI3, write/read-back of `/sd/nocsif_test.txt` verified, root listing
reads the card's own files. All hardware facts (ALDO1 reg 0x92/0x1C, SD on SPI3 vs display on SPI2,
NFC/LoRa CS parked, 4 MHz mount) confirmed. **Two findings that each cost a reflash:**
- **Card must be FAT32.** 64 GB SDXC ships **exFAT**, which ESP-IDF FatFs cannot mount (`FF_FS_EXFAT 0`
  hard-coded in `ffconf.h`, **no Kconfig toggle** — enabling it means patching the framework component,
  not worth it) → mount fails with `FR_NO_FILESYSTEM` (code 13). Reformat >32 GB cards to FAT32 with
  Rufus/guiformat (the Windows GUI won't). A 60 GB FAT32 card mounts fine — the "≤32 GB" note is only
  Windows' format limit, not a FatFs/hardware one.
- **Long filenames must be enabled.** ESP-IDF defaults to `CONFIG_FATFS_LFN_NONE` (8.3 names) → `f_open`
  rejects any base name >8 chars (our `nocsif_test.txt` failed to create). Added
  **`CONFIG_FATFS_LFN_HEAP=y` + `CONFIG_FATFS_MAX_LFN=255`** to `sdkconfig.defaults` (verify post-build:
  generated sdkconfig has `CONFIG_FATFS_LFN_HEAP=y`). Needed for the file-drop regardless.
- **Seat the card firmly:** a loose card gives `sdmmc_sd: send_if_cond … 0x108` at CMD8 (invalid response,
  not a timeout) — physical, fixed by reseating; distinct from the filesystem errors above.

Implementation as planned below (all landed):
- **power.c/.h:** add `nocsif_power_sd_rail(bool)` mirroring `nocsif_power_display_rail`: **ALDO1 voltage =
  reg 0x92 code 0x1C (3.3V)** BEFORE enable, **enable = reg 0x90 bit0 (0x01)**, read-modify-write.
  (0x90 b0=ALDO1 per the existing power.c header; 0x92 is the ALDO1 voltage reg by the AXP2101 0x92..0x95
  = ALDO1..ALDO4 sequence — the working ALDO2=0x93 next door corroborates it.)
- **main.c:** `nocsif_power_sd_rail(true)` + ~50 ms settle, then `nocsif_sdcard_init()`.
- **sdcard.c/.h (new):** `spi_bus_initialize(SPI3_HOST, …)` on the shared bus (MOSI=34, MISO=33, SCK=35);
  drive NFC CS=4 and LoRa CS=36 **HIGH** (native GPIO outputs) before any SD access; `esp_vfs_fat_sdspi_mount(
  "/sd", host{SPI3_HOST}, slot{.gpio_cs=21}, cfg{format_if_mount_failed=**false**}, &card)` at a conservative
  **4 MHz** first. Keep the `sdmmc_card_t*` (exposed) for P3's MSC.
  - **Display uses SPI2_HOST** (`display.c`), so SD **must** use SPI3_HOST.
  - **Never** set `format_if_mount_failed=true` — it would wipe the user's card.
- **CMakeLists.txt:** add `sdcard.c` to SRCS; `PRIV_REQUIRES += fatfs sdmmc esp_driver_sdspi`.
- **Verify (COM7):** boot log shows `/sd mounted, N sectors, X MB`; firmware writes `/sd/nocsif_test.txt`,
  reads it back byte-for-byte, lists root dir; pull card → confirm the file on a PC. No USB behavior yet.
- **Risks:** float NFC/LoRa CS → intermittent enumeration (drive high first); wrong ALDO1 volt code
  mis-volts the card (verify 0x92); SDSPI >4 MHz on the shared routing unproven (raise cautiously); wrong
  SPI host colliding with the display QSPI (confirmed SPI2 vs SPI3).

### P2 — TinyUSB + CDC-ACM console (PHY-handoff proof)  ← **DONE, verified on-device 2026-08-08**
**Result:** deferred install proven end-to-end. Boot with TinyUSB NOT installed → COM7 live, heartbeats
normal, `usb_gadget worker ready (idle)`, on-screen "USB Gadget: tap to start". Tap → screen shows
`ON (CDC)`, the ROM USB-Serial/JTAG device (PID 0x1001) **vanishes**, a new TinyUSB CDC device enumerates
(**COM8, VID 0x303A PID 0x4001**), and the ESP-IDF console redirects to it (heartbeats stream over the CDC
port). Everyday flash→read-COM7 is untouched because install only runs on the deliberate tap.
**Verified v2 API (esp_tinyusb 2.2.1) — differs from the v1 names the research cited:**
- `#include "tinyusb.h"` + **`tinyusb_default_config.h`** (the `TINYUSB_DEFAULT_CONFIG()` macro lives here, NOT
  in tinyusb.h — omitting it = `implicit declaration` build error), `tinyusb_cdc_acm.h`, `tinyusb_console.h`.
- `const tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG();` (full-speed, NULL descriptors → built-in default
  CDC on the S3) → `tinyusb_driver_install(&cfg)` → `tinyusb_cdcacm_init(&(tinyusb_config_cdcacm_t){.cdc_port=
  TINYUSB_CDC_ACM_0})` → `tinyusb_console_init(TINYUSB_CDC_ACM_0)`. (v1's `tusb_cdc_acm_init` /
  `esp_tusb_init_console` are the deprecated names — do not use.)
- Install runs on a worker task (never the LVGL callback); the toggle only `xTaskNotifyGive`s it.
- Flaky-compiler note: a full rebuild (sdkconfig change) hit a **transient GCC ICE** (IRA-pass segfault) on
  stock `esp_lcd_panel_rgb.c` — not our code; a plain rebuild cleared it.

Implementation as planned below (all landed; the P2 code notes above supersede the v1-flavoured API sketch):
- **idf_component.yml:** `espressif/esp_tinyusb: "^2"` (resolved to **2.2.1**; PlatformIO pulled it + the
  transitive `espressif/tinyusb` port cleanly — the flagged resolution unknown is settled).
- **sdkconfig.defaults:** `CONFIG_TINYUSB_CDC_ENABLED=y`, `CONFIG_TINYUSB_CDC_COUNT=1`; keep
  `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`; do **not** add `CONFIG_ESP_CONSOLE_USB_CDC`. (No
  `CONFIG_TINYUSB` master switch exists.)
- **usb_gadget.c/.h (new):** `nocsif_usb_gadget_start()` on a dedicated task: `tinyusb_driver_install()`
  (NULL descriptors — CDC has a default) → `tusb_cdc_acm_init(TINYUSB_CDC_ACM_0)` →
  `esp_tusb_init_console(TINYUSB_CDC_ACM_0)`. Idempotent guard.
- **ui.c:** clone the button pattern for a "USB Gadget" toggle whose cb only signals the gadget task.
- **Verify:** flash + read COM7 normally (install deferred). Tap toggle → USB-Serial/JTAG drops, host
  enumerates a **new** USB CDC device → heartbeat/ESP_LOG stream over it. Confirm re-flash still works.

### P3 — CDC+MSC composite backed by the SD  ← **DONE, verified on-device 2026-08-08**
**Result:** SDSPI-backed MSC works (the plan's biggest unknown — RESOLVED). The watch enumerates as a
composite CDC+MSC device (CDC console COM-port PID **0x4003**, + a removable disk). Host sees a FAT32 drive
(58.9 GB, "TinyUSB … MSC Storage"); wrote `NOCSIF_P3.txt` from the PC, read it back host-side, it persists.
After a **Windows tray "Safely Remove"**, the firmware switched to MOUNT_APP and listed `/sd` over CDC showing
**both `nocsif_test.txt` (P1) and `NOCSIF_P3.txt` (host-dropped)** — the full PC→card→firmware round-trip.
**Key implementation facts (from the esp_tinyusb 2.2.1 MSC test app + headers):**
- The MSC helper OWNS the FAT mount + the single-owner handoff. So `sdcard.c` was changed from
  `esp_vfs_fat_sdspi_mount` to a **raw** `sdmmc_card_t` init (`sdspi_host_init` → `sdspi_host_init_device` →
  `host.slot=dev` → `sdmmc_card_init`); mounting FAT there too would double-own the volume.
- **Init order:** raw card → `tinyusb_msc_new_storage_sdmmc(&{.medium.card, .fat_fs.base_path="/sd",
  .do_not_format=true, .mount_point=MOUNT_APP})` → `tinyusb_msc_set_storage_callback` → **then**
  `tinyusb_driver_install()` (MSC storage BEFORE the stack; CDC init AFTER — as P2). `new_storage` auto-installs
  the MSC driver; descriptors stay NULL → default CDC+MSC composite. `do_not_format=true` never formats the card.
- **Auto-handoff:** the helper switches MOUNT_USB↔MOUNT_APP automatically — to USB on host mount (`tud_mount`),
  to APP on `tud_msc_start_stop_cb` (SCSI eject) / `tud_umount` (VBUS disconnect). **Windows soft-eject of the
  drive letter does NOT reliably switch it** (Windows re-scans the still-connected media → bounces back to host);
  a user tray **"Safely Remove Hardware"** on the device, or a real unplug, does. `mountvol /P` needs admin.
  → **For P4, the app must claim the card DETERMINISTICALLY via `tinyusb_msc_set_storage_mount_point(MOUNT_APP)`
  before reading the DuckyScript**, not rely on host eject.
- `wear_levelling` added to `PRIV_REQUIRES` (tinyusb_msc.h includes wear_levelling.h). `CONFIG_TINYUSB_MSC_BUFSIZE
  =8192` for host-copy throughput. Firmware ~697 KB.

Original plan sketch (superseded by the notes above where they differ):
- **sdkconfig.defaults:** `CONFIG_TINYUSB_MSC_ENABLED=y`, `CONFIG_TINYUSB_MSC_BUFSIZE` ≥512.
- **usb_gadget.c:** extend to CDC+MSC. v2: `tinyusb_msc_new_storage_sdmmc(&cfg{.medium.card=<P1 card>}, &h)`.
  Keep device/config descriptors NULL (CDC+MSC both have defaults — cf. `tusb_composite_msc_serialdevice`).
  Wire MSC mount/eject events.
- **Single-owner FAT handoff:** on entering MSC mode **unmount** the app's `/sd` view so the host owns the
  raw card; app must not touch `/sd` while exposed; remount only after the host ejects. (Mount base-path is a
  **runtime** arg, not a sdkconfig symbol.)
- **Verify:** host mounts a removable drive of the right capacity; copy a multi-MB file + a `.txt` macro,
  safe-eject → firmware remounts `/sd`, logs the copied names + macro contents over CDC; re-read on PC,
  checksum matches (no corruption). CDC + MSC enumerate together.
- **Risk (highest single unknown):** SDSPI-backed MSC has no official example — validate block R/W early,
  be ready for the HID-first contingency. Simultaneous host+app access corrupts the card if the handoff is
  wrong. Throughput over single-lane SPI is far below SDMMC.

### P4 — CDC+MSC+HID composite: DuckyScript player
- **sdkconfig.defaults:** `CONFIG_TINYUSB_HID_COUNT=1` (an int, not a bool).
- **usb_gadget.c:** switch to a **hand-written** configuration descriptor — `TUD_CONFIG_DESCRIPTOR` +
  `TUD_CDC_DESCRIPTOR` + `TUD_MSC_DESCRIPTOR` + `TUD_HID_DESCRIPTOR`, ITF-num enum (CDC, CDC_DATA, MSC, HID),
  unique EP addresses, `bNumInterfaces` + total length matching the `CFG_TUD_*` counts. Add a keyboard report
  descriptor + `tud_hid_descriptor_report_cb` / `tud_hid_get_report_cb` / `tud_hid_set_report_cb`. Custom
  VID/PID (if wanted) goes in a hand-written device descriptor (the Kconfig VID/PID only feed the default path).
- **ducky.c/.h (new):** parse DuckyScript from `/sd` (native FAT read — independent of MSC): `REM`,
  `STRING`/`STRINGLN`, `ENTER`, `DELAY`/`DEFAULTDELAY`, `GUI`/`WINDOWS`/`CTRL`/`ALT`/`SHIFT` modifier + key.
  Typing engine: instantiate TinyUSB's `HID_ASCII_TO_KEYCODE` `[128][2] {shift,keycode}` US table (do **not**
  hand-roll it, `class/hid/hid.h`); per char set modifier=LEFTSHIFT if `conv[c][0]`, keycode=`conv[c][1]`,
  `tud_hid_keyboard_report(...)`, **then send an all-zero release report** (or keys stick/auto-repeat);
  special keys from `HID_KEY_*`. US-QWERTY only for v1.
- **ui.c:** "Run Macro" button → signals the ducky task; only fire when `tud_mounted() && tud_hid_ready()`.
  Do not read `/sd` while MSC exposes the card.
- **Verify:** caret in a PC text editor, tap "Run Macro" → the `/sd` macro types the expected string, ENTER,
  honors DELAY, executes a modifier combo (e.g. GUI+r) char-for-char, no stuck/repeated keys; CDC + MSC + HID
  all still enumerate.
- **Risks:** descriptor mismatch → **silent** enumeration failure; 5-IN ceiling hit exactly; missing release
  report → stuck keys; US-layout/ASCII-only (non-US hosts mistype); firing before host marks HID ready drops
  keystrokes.

## Partition / flash impact
**None for M4.** MSC medium is the **external** microSD, not the internal `storage` partition (which is
subtype `spiffs`, not FAT, and can't back MSC as-is). Factory app stays 0x10000/0x400000 (TinyUSB+FATFS+
SDMMC+SDSPI+HID add only tens of KB). Macro lives on the SD → zero flash impact. Flashing unchanged.
Contingency only: HID-first-with-flash-MSC would need `storage` subtype `spiffs`→`fat` (the one change that
edits `partitions.csv`) — not needed for the primary SD-backed plan.

## Key gotchas (carry into coding)
- No `CONFIG_TINYUSB` master switch — enable per class; HID is a **COUNT** (`CONFIG_TINYUSB_HID_COUNT=1`).
- `tinyusb_driver_install()` blanks COM7 instantly — **defer behind the UI toggle**; never at boot during
  bring-up (also breaks esptool auto-reset → keep manual BOOT/RST).
- `CONFIG_ESP_CONSOLE_USB_CDC` is incompatible with TinyUSB; use `esp_tusb_init_console` over a TinyUSB CDC
  interface. Never burn `USB_PHY_SEL`.
- **ALDO1 (SD rail) is not enabled today** — power.c only does ALDO2. Add `nocsif_power_sd_rail()`
  (reg 0x92 code 0x1C before reg 0x90 bit0, ~50 ms settle). Verify 0x92 vs XPowersLib.
- Shared SPI: SD(CS21)/NFC(CS4)/LoRa(CS36) share MOSI34/MISO33/SCK35 — **drive NFC/LoRa CS HIGH** (SoC GPIOs,
  not XL9555) before/while using SD or the card enumerates intermittently. **Display = SPI2_HOST → SD = SPI3_HOST.**
- SDSPI-backed MSC unproven by any official example — validate block R/W **early** (why P1 comes first).
- Single-owner FAT: host (MSC raw block) and app (FAT mount) must never touch the card simultaneously —
  coordinate via MSC mount/eject events.
- HID keymap: use `HID_ASCII_TO_KEYCODE` (US, printable-ASCII only); every key report needs an all-zero
  release report; special keys from `HID_KEY_*`.
- `esp_tinyusb` v2.0.0 is a breaking redesign vs v1.x — pin one major (like `lvgl ~9.3`) and match code+docs.
- CMake `PRIV_REQUIRES` for SD-backed MSC: `esp_tinyusb + fatfs + sdmmc + esp_driver_sdspi`.
- LVGL is single-threaded behind `esp_lvgl_port` — toggle cb + widget mutation off the LVGL task must hold
  `lvgl_port_lock()`; heavy USB/SD work runs on a **separate task**, never in the button callback.

## Open decisions (defaults chosen; overridable, none block P1)
- **esp_tinyusb pin:** default **`^2`** (SD-MSC helper + mount events fit the single-owner handoff).
- **Keep CDC console in the shipped device:** default **yes** (field debuggability; endpoints still fit).
- **DuckyScript v1 scope:** default **minimal** (`REM`, `STRING`/`STRINGLN`, `ENTER`, `DELAY`/`DEFAULTDELAY`,
  GUI/CTRL/ALT/SHIFT + key), US-QWERTY. Fuller set (REPEAT/loops, arrow/F-keys, non-US LOCALE) later.
- **HID identity (VID/PID):** to lock before P4 — Espressif default vs a hand-written generic-keyboard
  VID/PID (affects host driver matching). Default: decide at P4.

## Primary sources
- esp_tinyusb component + Kconfig + README (registry 2.2.1, espressif/esp-usb `device/esp_tinyusb`).
- IDF v5.5 examples: `tusb_composite_msc_serialdevice`, `tusb_hid`, `tusb_console`.
- ESP-IDF USB-OTG console + USB-Serial/JTAG console API guides (PHY sharing, CDC-console incompatibility).
- TinyUSB `cdc_msc_freertos` descriptors; `class/hid/hid.h` `HID_ASCII_TO_KEYCODE`.
- Workflow: `nocsif-m4-usb-research-v2` (2026-08-06), 4 dimensions + fact-check + synthesis.
