# Research record — Runtime base decision (2026-08-06)

Method: 7-agent research workflow (5 parallel web-research agents → lead-architect synthesis →
adversarial skeptic verify). ~349k tokens, 87 tool calls. Question: identify "Fantasi firmware",
and decide whether MicroPython is too heavy/slow for the T-Watch Ultra → recommend a runtime base.

## Conclusion
**Option B — native C++/ESP-IDF core (hardware + LVGL + web + Flipper parsers) + embedded
MicroPython for user drop-in scripts.** Runner-up: Option A (MicroPython-as-platform). Rejected:
C (Berry/Lua/JS — sacrifices Python affinity) and D (Flipper-style native-ELF apps — no source
portability).

## Fantasi — what it actually is
- Repo: **github.com/soeinova/Fantasi** (GPL-3.0); fork rgomez31UAQ/Fantasi_Flipper_Multi-Firmware.
- Common **C / FreeRTOS** runtime + HAL + USB CLI + a **relocatable-ELF app loader** (`launch
  <path>`, no reflash; apps are architecture-specific compiled binaries — like Flipper FAPs).
- Working targets: Flipper Zero (STM32WB55), Kiisu (STM32WB55), Chameleon Ultra (nRF52840),
  Proxmark3 (AT91SAM7S). **T-Watch Ultra (ESP32-S3) = "Coming soon", unimplemented.**
- **NOT** a `.sub`/`.nfc` importer and **no Python** — zero Flipper-format parsing in the docs.
- Its "no-fuss import" reputation = **LittleFS presented to the host as a synthetic FAT volume
  over USB-MSC** (mount like a USB stick, drag files in). ~43 stars, ~10 commits, pre-release.
- **Borrow:** the USB-MSC file-drop UX. **Reject:** native-ELF-per-arch apps (kills Python reuse).

## MicroPython "too heavy / too slow"? — verdicts
- **Fit:** fine. ~1.6–2 MB image on 16 MB flash; heap in PSRAM (build the octal-SPIRAM S3 variant
  or PSRAM isn't exposed). The "too heavy" instinct is a classic-ESP32 hangover; doesn't apply.
- **Logic/UI speed:** fine. Interpreter is ~200–300× slower than C in tight loops, but parsing,
  menus, callbacks, web requests run at ms timescales. LVGL FPS (~30–50 full-frame at 410×502) is
  bounded by LVGL's C engine + QSPI bus — **language-independent**. MicroPython's real risks are
  **GC pauses + long-run UI degradation**, not throughput.
- **RF/NFC timing:** fine — the µs deadlines live in silicon (SX1262 modem, ST25R3916 framing
  engine, BLE controller), not CPU loops. Pure-Python drivers exist for SX1262 and BLE HID.

## The real decider (why B over A)
CO5300 QSPI AMOLED has a **proven, maintained C/ESP-IDF + LVGL driver** (`kodediy/esp_lcd_co5300`)
but **no tested MicroPython+LVGL driver** (only an untested, non-LVGL early lib). Option B reuses
solved work; Option A = first-of-kind CO5300 bring-up in MicroPython on the highest-visibility
deliverable. Plus keeping LVGL native sidesteps the GC/fragmentation UI-degradation risk.

## Adversarial corrections (adopted — change expectations & sequencing, not the destination)
1. **"Python parity" overstated.** MicroPython ≠ CPython — no numpy/cryptography/pyserial/requests/
   C-extensions, partial asyncio, trimmed stdlib. Only pure-logic parsing/orchestration ports.
2. **Import-Flipper-files + extend-without-reflash need NO interpreter** (data-driven, native).
   Only "reuse my Python" needs the VM → make embedding MicroPython a **gated spike** (M4), and
   prove value by porting one real PC script before committing.
3. **NFC = from-scratch ST25R3916 driver work.** The pure-Python libs are high-level I²C UID
   readers only (no emulation / raw framing needed for `.nfc` parity).
4. **USB HID keyboard: native USB HID (composite MSC+HID) > BLE HID** — no pairing prompt, works on
   locked/hardened hosts, reuses the file-drop USB stack. Make it the first capability.
5. **Sub-GHz worse than "no OOK TX":** SX1262 also **cannot do raw RF capture** (only demodulates
   a pre-set LoRa/FSK modem), and is single-band (868 *or* 915). Real Flipper `.sub` replay needs
   an external CC1101 — physically hard on a **sealed watch with no GPIO header** → treat true OOK
   sub-GHz as likely out-of-scope in stock form. Only the in-band FSK subset is replayable.
6. **USB-MSC concurrency footgun:** host + firmware on one FS at once corrupts FAT → SD-over-MSC or
   single-owner-at-a-time.

## Sources
- kodediy/esp_lcd_co5300 (C/ESP-IDF+LVGL CO5300 driver): https://components.espressif.com/components/kodediy/esp_lcd_co5300
- dobodu/Lilygo-Amoled-Micropython (untested, non-LVGL): https://github.com/dobodu/Lilygo-Amoled-Micropython
- LVGL forum CO5300/SH8601 driver thread: https://forum.lvgl.io/t/buggy-homebrew-display-firmware-help-me-fix-the-co5300-sh8601-driver-lvgl9-setup/21242
- UIFlow2 NFC (ST25R3916) high-level unit: https://uiflow-micropython.readthedocs.io/en/2.4.4/unit/nfc.html
- Semtech SX1261/2 datasheet (LoRa/(G)FSK only, no OOK): https://cdn.sparkfun.com/assets/6/b/5/1/4/SX1262_datasheet.pdf
- Fantasi: https://github.com/soeinova/Fantasi · fork: https://github.com/rgomez31UAQ/Fantasi_Flipper_Multi-Firmware
- T-Watch Ultra band variants (868/915): https://www.gsmgotech.com/2026/04/lilygo-t-watch-ultra-pre-orders-are.html
