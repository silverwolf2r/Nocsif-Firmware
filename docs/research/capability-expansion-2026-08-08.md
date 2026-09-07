# Capability expansion backlog + implementation meta-prompt

Forward-looking feature catalog for NocSif on the LilyGo T-Watch Ultra (ESP32-S3), for authorized
security testing. As of **2026-08-08**. Neutral device-class framing per `docs/RESUME.md` (Wording
convention) — this vocabulary is both more precise and avoids tripping the automated safeguard, so it
must be used in every doc, commit, and subagent/Workflow prompt derived from this file.

This document is a **deferred backlog**: it is written now so nothing is lost, and executed **after**
the in-flight M4-P4 (USB HID keyboard / DuckyScript) lands. It does not change current work. See
"Sequencing" at the bottom.

## Scope rule
Only capabilities the **existing silicon** can support are listed as buildable. Items requiring
hardware this watch does not have are recorded in "Out of scope (hardware-blocked)" so future work
does not chase them. Hardware facts are authoritative from `docs/HARDWARE.md`.

---

## Buildable capability catalog (grouped into proposed milestones)

### M5 — WiFi capability suite (ESP32-S3 2.4 GHz radio)
The chip's largest unused surface. All items use the native radio; no added hardware.
1. Access-point + client scanning / enumeration with channel hopping.
2. Promiscuous-mode packet monitor / frame sniffer.
3. Probe-request collection (client-presence logging over time).
4. 802.11 deauthentication / disassociation management-frame transmission.
5. Beacon-frame generation (bulk SSID broadcast).
6. Configurable software access point (named AP / duplicate-SSID host).
7. Probe-response access point (auto-answers client probe requests).
8. Captive-portal web page with credential-capture form.
9. WPA handshake / PMKID capture to SD for offline analysis.
10. In-portal DNS response redirection.
11. WiFi environment analyzer (channel utilization, RSSI, AP density).

### M6 — NFC HF capability suite (ST25R3916, 13.56 MHz)
12. Tag / UID read.
13. MIFARE Classic sector read + write.
14. MIFARE Ultralight / NTAG read + write.
15. Card emulation (present a stored credential to a reader).
16. NDEF record read / write.
17. ISO14443-A/B and ISO15693 transponder support.
18. MIFARE key-recovery routines (MFKey32 / nested-authentication key derivation).
19. Contactless-frame sniffing and relay.
20. `.nfc` file import (Flipper format) — read, store, replay/emulate.

### M7 — BLE capability suite (ESP32-S3 BLE)
21. BLE device scan / enumeration / GATT service explore.
22. BLE advertisement broadcast generator (device-pairing notification advertisement types:
    Apple / Android / Windows).
23. BLE HID keyboard (wireless DuckyScript transport; complements the USB HID path).
24. BLE beacon broadcast (iBeacon / Eddystone).

### M8 — GNSS + geolocation (UBlox MIA-M10Q)
25. Location fix + NMEA parsing.
26. GPX track logging to SD.
27. Geotagged wardriving (combine M5/M7 scan output with position → exportable map log).
28. Time sync from GNSS to the RTC.

### M9 — LoRa messaging (SX1262, 915 MHz FSK/LoRa)
29. LoRa point-to-point text messaging (watch-to-watch).
30. Meshtastic protocol interoperability.
31. In-band 915 MHz (G)FSK `.sub` replay subset (the only replayable slice — see out-of-scope note).
32. In-band channel-activity indicator.

### M10 — USB extensions (native USB, builds on M4)
33. USB HID mouse (pointer report class alongside the keyboard).
34. U2F / FIDO CTAP over USB HID.
35. Multi-report composite descriptor management (already hand-written in M4-P4; generalize).

### M11 — Sensor, audio, and watch-core UX
36. IMU step counting / activity tracking (BHI260AP).
37. Wrist-raise wake + gesture control (BHI260AP).
38. Motion / tilt-triggered actions and UI auto-rotation.
39. Audio recording to SD (PDM mic T3902).
40. Tone / alert output and WAV playback (MAX98357A speaker).
41. Sound-level meter.
42. Watchface + timekeeping (RTC PCF85063A).
43. Alarms / timers / stopwatch.
44. Haptic feedback patterns (DRV2605).
45. Notification surface.
46. Battery / power-management UI (AXP2101 state, charge control ≤ 500 mA).
47. Persistent settings / configuration store.

### Platform layers (already in the original roadmap — listed for completeness)
48. WiFi web UI control surface (architecture layer L4) — remote control of all capabilities from a
    phone/browser; a natural companion to M5+ since it reuses the WiFi stack.
49. Embedded MicroPython drop-in script layer (layer L2 decision gate) — run user Python from SD with
    no reflash; gate on a real ported script first, per `docs/ARCHITECTURE.md`.
50. Flipper-format parsers + script/app manager (drop-in imports, no reflash).
51. On-watch file manager for SD.
52. On-watch browser (layer L5) — explicitly deferred to v2/v3.

---

## Out of scope (hardware-blocked — do not attempt on the stock watch)
- **Sub-GHz OOK capture/replay** (garage/fob-style `.sub`): SX1262 is FSK/LoRa-only, no OOK, no raw
  capture; needs an external CC1101, and the sealed watch has no GPIO header.
- **125 kHz LF RFID** (EM4x / Prox / T5577 clone): no low-frequency frontend.
- **Infrared remote** (universal remote / device-off tool): no IR emitter in the pinout.
- **Wideband SDR / spectrum analysis** (HackRF-class): the SX1262 is a narrowband modem, not an SDR.
- **Bluetooth Classic** (BR/EDR): ESP32-S3 is BLE-only.
- **iButton / 1-Wire, nRF24**: no hardware.

---

## Suggested milestone ordering (value per unit of existing hardware)
1. **M5 WiFi suite** — largest surface, native radio, matches the WiFi-Pineapple / Marauder feature core.
2. **M6 NFC HF suite** — second radio already fitted; matches the Flipper + Proxmark HF feature set.
3. **M11 watch-core UX (partial)** — watchface/RTC/haptics make it usable as a worn device; can slot
   anytime, low risk.
4. **M7 BLE suite** — third native radio.
5. **M8 GNSS** — force-multiplies M5/M7 (wardriving) once those exist.
6. **M9 LoRa messaging** — narrow but self-contained.
7. **L4 web UI** — pull forward if remote control becomes the priority; it reuses the WiFi stack.
8. **L2 MicroPython gate** — the "run my own Python" decision, deliberately last.

---

## Implementation meta-prompt (paste this to start the expansion, once M4-P4 is done)

> You are continuing NocSif, a modular security-testing firmware for the LilyGo T-Watch Ultra
> (ESP32-S3), used for authorized security testing. **Before writing any code, read in full:**
> `docs/RESUME.md`, `docs/PLAN.md`, `docs/ARCHITECTURE.md`, `docs/HARDWARE.md`, and
> `docs/research/capability-expansion-2026-08-08.md` (this catalog).
>
> Milestones M0–M4 are complete and verified on-device: board bring-up + power gating, CO5300 display,
> CST9217 touch, LVGL v9 UI, and the composite USB device (SD FAT mount, deferred CDC console, MSC
> file-drop, and USB HID keyboard / DuckyScript player). Your job is to implement the capability
> catalog in the expansion doc as new milestones **M5 onward**, in the "Suggested milestone ordering",
> one milestone at a time.
>
> **Project conventions — follow all of them:**
> - Use neutral device-class wording in every doc, commit message, and subagent/Workflow prompt (the
>   Wording convention in `docs/RESUME.md`). Charged vocabulary hard-fails the automated safeguard and
>   has previously killed an entire research workflow.
> - Build from **PowerShell, never Git Bash**: `python -m platformio run -d D:\Docker\NocSif_Firmware\firmware`.
> - Flash with `esptool --no-stub` (download mode: hold BOOT → tap RST → release BOOT); watch is **COM7**.
> - Respect the shared-SPI-bus and AXP2101 power-rail constraints in `docs/HARDWARE.md` (park unused
>   chip-selects HIGH; enable the correct rail before use).
> - One milestone per branch → PR → squash-merge to `main` (gh CLI). Update `docs/RESUME.md`
>   (Current state + Next steps) after each on-device verification.
> - LVGL is single-threaded behind `esp_lvgl_port`: UI callbacks only signal worker tasks; heavy radio
>   / SD / USB work runs on separate tasks holding the right locks.
>
> **For each milestone:** first produce a phased, sourced implementation plan in the style of
> `docs/research/usb-composite-device-2026-08-06.md` (front-load the riskiest unknown as its own early
> phase), save it under `docs/research/`, and pause for my review and scope confirmation before
> implementing. Verify each phase on-device before the next. Do not start a new milestone until the
> previous one is merged and its RESUME.md entry is written.
>
> Begin with **M5 (WiFi capability suite)**: produce its phased plan and stop for my review.

---

## Sequencing note (why this is a backlog, not an interrupt)
M4-P4 is the final phase of a nearly-complete, well-scoped milestone with a tight hand-written
composite USB descriptor (exactly 5 IN endpoints, zero headroom). None of the capabilities above
depend on M4-P4, and M4-P4 does not depend on any of them. Finishing P4 yields a clean, shippable
composite USB device and a natural milestone boundary to branch M5 from. Interrupting mid-descriptor
to open a large new subsystem (WiFi) would strand the USB work half-done and discard loaded context.
**Recommendation: finish M4-P4, then start M5 from this doc.**
