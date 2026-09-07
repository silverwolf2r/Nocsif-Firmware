# Flipper-ecosystem interop + on-device app/script manager — research (2026-08-09)

> Feeds the **§4.4 platform layer** "Flipper-format parsers + script/app manager (drop-in imports, no
> reflash)". Neutral device-class wording throughout. Sourced from the "Fantasi firmware" project plus the
> Flipper firmware forks; source URLs at the end.

## 0. TL;DR
- **"Fantasi firmware" is a from-scratch FreeRTOS multi-device runtime that *replaces* stock firmware on
  Flipper Zero + other research devices — NOT a Flipper custom-firmware fork, and it parses *no* Flipper file
  formats.** It "integrates with Flipper" only in that the Flipper Zero is one of its hardware *targets*.
- So Fantasi is a **strong reference for the app/script-manager + USB-storage half** of our platform layer,
  and **no help at all on Flipper file-format interop** — for `.sub` / `.nfc` / DuckyScript parsing the
  references remain the actual Flipper firmwares (**Unleashed / Momentum / RogueMaster**) and the Flipper
  file-format spec.
- **Adopt** from Fantasi: a single **versioned capability-struct API** (NULL pointer = "this build lacks that
  hardware"), **two-tier persistent/volatile app dirs**, **MSC drop-in → immediately discoverable**, **one
  framed control protocol across USB/BLE**, an optional **web (WebUSB/Web-Bluetooth) drag-and-drop deployer**.
- **Don't adopt**: its RAM-only relocatable-ELF loader (ARM/Cortex-M specific) — on ESP32-S3 our **embedded
  MicroPython** layer (L2) is the natural drop-in script/app path; reuse Fantasi's *manager/loader UX and
  capability-API shape*, not its binary-loading mechanism.

## 1. What Fantasi actually is (framing correction)
- Repo: `github.com/rgomez31UAQ/Fantasi_Flipper_Multi-Firmware` (branch `dev`), described as forked from an
  upstream "Soeinova Fantasi" (upstream not independently locatable).
- A **FreeRTOS unified firmware** presenting one USB CLI + storage interface across several devices, then
  loading/running small apps pushed from the host. Hardware targets: Flipper Zero & Kiisu (STM32WB55),
  Chameleon Ultra (nRF52840), Proxmark3 (AT91SAM7S); Proxmark5 / Kiisu Smol / **Kode Dot (ESP32-S3)** listed
  "coming soon". The ESP32-S3 target is the closest architecture to the T-Watch Ultra but **not yet
  implemented**, so there is no ESP32-S3 reference code to read there yet.

## 2. Reusable patterns (the app/script-manager + storage half)
1. **Single capability-struct API, NULL-for-absent-hardware.** Each app gets one versioned function table
   (`abi_version` + capability pointers: print/printf, malloc/free, file r/w + size + remove, led/buttons/
   display, critical-section, delay) and links against nothing else. **A pointer is `NULL` where the device
   lacks that hardware** — that is how one binary safely spans devices with/without a display, radio, etc.
   Clean fit for NocSif's per-milestone bring-up (NFC / sub-GHz / GNSS may or may not be active in a build);
   **expose the same capability set to the MicroPython layer** so scripts and native apps share one contract.
2. **Two-tier app locations — persistent vs volatile.** `/apps/<name>` (persistent) and `/ramfs/<name>`
   (RAM, drop-and-run, lost on reboot). Mirror as **`/apps` on microSD** + a **RAM/volatile "run-once" slot**
   so a user can push a script for a one-shot run without committing it to card.
3. **Drop-in over USB MSC, no reflash.** Fantasi builds a synthetic FAT volume on the fly so the host mounts
   it "like a normal USB stick"; files copied over are then launchable on-device. This is exactly our
   "drop-in imports from SD, no reflash" goal — and **NocSif is better positioned** (we already have real
   microSD FAT + composite CDC+MSC from M4). Convention to adopt: **files copied over MSC are immediately
   discoverable by the on-device manager** (rescan on MSC eject / SD change).
4. **One framed control protocol across transports.** Fantasi uses protobuf (nanopb) over USB-CDC / WebUSB /
   BLE for filesystem + control. A single framed command/file protocol reusable over NocSif's **USB CDC and
   BLE** is a good model for the L4 web-UI and a future BLE companion.
5. **Optional web drag-and-drop deployer** (WebUSB / Web-Bluetooth): a hosted browser launcher that pushes
   scripts/payloads to the device with no local toolchain — a low-friction distribution UX worth keeping on
   the backlog (pairs with our L4 WiFi web-UI).
6. **ABI/versioned app↔firmware contract** so imported assets survive firmware updates gracefully.

## 3. What NOT to copy
- The **RAM-only relocatable-ELF loader** (copies sections to RAM, applies `R_ARM_ABS32` relocations,
  `-nostdlib` freestanding apps) is Cortex-M / ARM7-specific and largely redundant on ESP32-S3. Use
  **embedded MicroPython (L2)** as the drop-in script/app path; take Fantasi's *manager/loader UX + capability
  API shape*, not the binary-loading mechanism.
- Fantasi is **CLI-first** (serial/BLE command shell + host CLI + web launcher); it has **no graphical
  on-device asset browser**. Our on-watch **Files** + app manager (the mockup's `system › Files`, and a
  future "app/script manager") is our own design — nothing to lift there.
- Fantasi gives **zero** Flipper-format help; do not treat it as a `.sub`/`.nfc` reference.

## 4. Which Flipper formats matter for NocSif (bounded by our hardware ceiling)
Hardware ceiling: **HF-NFC only** (ST25R3916, 13.56 MHz), **915 MHz narrowband** SX1262 (no OOK), **no
125 kHz LF, no IR** (see PLAN §4.6).

| Flipper format | Relevance to NocSif | Action |
|---|---|---|
| **`.nfc`** (13.56 MHz HF) | **High** — matches ST25R3916 | Priority parser: import/export saved cards (→ M6 NFC, mockup `NFC › Saved Cards`). |
| **BadUSB / DuckyScript `.txt`** | **High** — HID player already built (M4) | Align on Flipper's DuckyScript dialect + file layout so payloads are cross-tool portable. |
| **`.sub`** (sub-GHz) | **Marginal** — format is OOK-centric; SX1262 is 915 MHz narrowband, no OOK | Parse container/metadata only; transmit **only** the narrow FSK/narrowband subset the SX1262 can actually emit. **Do not advertise general `.sub` compatibility.** (→ M9) |
| **`.ir`** (infrared) | **None** — no IR emitter | Skip the parser entirely. |
| **`.rfid`** (125 kHz LF) | **None** — no LF front-end | Skip the parser entirely. |
| **`.fap`** (Flipper apps) | **None** — Cortex-M ELF, wrong ISA | Not portable to ESP32-S3; our app layer = MicroPython, not `.fap`. |

*(The `.ir` / `.rfid` / OOK-`.sub` gaps are exactly what the §7.5 "NocSif Backpack" co-processor would later
unlock — those parsers become worth building only once that hardware path exists.)*

## 5. Recommendation for the §4.4 layer
Build the "Flipper-format parsers + script/app manager" as: a **capability-struct contract** (shared by native
code + MicroPython, NULL-for-absent-hardware), a **microSD app/asset tree** with persistent + volatile slots,
**MSC-drop-in auto-discovery**, and **format parsers scoped to `.nfc` + DuckyScript first** (`.sub` as
narrowband-subset-only; skip `.ir` / `.rfid` / `.fap`). Source the *manager/loader UX* from Fantasi and the
*format details* from the Flipper firmware forks. Slots after **M6 (NFC)** so there is a real `.nfc` consumer.

## 6. Sources
- Fantasi: `github.com/rgomez31UAQ/Fantasi_Flipper_Multi-Firmware` — `README`, `docs/{design,apps,features,
  radio_stack,cli}.md`, `apps/app_api.h`; web launcher `fantasi.cloud/launcher`.
- Flipper file formats (for the actual parsers): Unleashed / Momentum / RogueMaster firmwares + the Flipper
  file-format spec (not covered by Fantasi).
