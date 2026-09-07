# NocSif — implementation meta-prompt: Companion phone control (remote surface) + on-watch typing

> Paste this whole block into a fresh **coding** chat to build the feature. It follows the NocSif
> one-milestone-per-branch → PR flow. **This is a design-authored spec; produce a phased plan and pause for
> review before writing any code.**

---

You are implementing the **companion control surface + on-watch typing fallback** for **NocSif**, a modular
security-testing firmware for the **LilyGo T-Watch Ultra (ESP32-S3)**, used for **authorized security testing
only**. You are working in the firmware repo (`D:\Docker\NocSif_Firmware`), on `main`, one milestone per branch.

**Neutral device-class wording is mandatory** — in code comments, commits, and any subagent/Workflow prompt.
Use "companion control surface", "802.11 management-frame transmission", "credential-capture form", "USB HID
keyboard", "card emulation", "RF transceiver". **Never** "attack / offensive / payload / weaponize / victim /
target / red-team" — charged vocabulary hard-fails Anthropic's automated cyber-safeguard and has killed a whole
research workflow. Keep load-bearing format names (**DuckyScript**, `.sub`, `.nfc`).

## Read these first
- `docs/RESUME.md` — current firmware state (start-here for what's built).
- `docs/HARDWARE.md` — **authoritative** pinout / buses / rails / I²C addresses (cite exact bus/pins/rail for
  every part you touch).
- `docs/ARCHITECTURE.md` + `docs/PLAN.md` (§4.8) — platform layers (this is **L4**) + the script-trust open
  question (folded into PLAN §4.8).
- `docs/PLAN.md` — **§7.4 "Companion phone control" (DECIDED 2026-08-10)** is the scope of record; also §4.4
  (L4), §4.2 (M5 WiFi / M7 BLE), §4.1 (the UI-shell registry + settings store), §7.3 (speech-to-text, ANCS).
- `docs/design/full-app-mockup.html` — the **done-state UI** this fills: **System › Companion** screen + the
  phone-side control panel mocked below the watch + the **Notes-editor** input method (on-watch QWERTY). The
  UI is already designed; you are wiring real behaviour into existing structure, not inventing screens.
- `docs/research/capability-expansion-2026-08-08.md` — M5/M7 milestone context.
- `docs/design/DESIGN-CHAT-HANDOFF.md` §6 — the framing behind the decision.

## Current firmware state (where code lives)
M0–M4 + UI-shell P1–P3.2 + P4.1–P4.2 are done & verified on-device; P4.3 (battery) in progress. Relevant code
under `firmware/src/…`:
- `ui.c` / `ui_nav.{c,h}` — the menu-first nav shell + the **app/launch-target registry**
  (`nocsif_app_launch(id)` — the single launch-by-id entry point). The **Companion** screen already has a place
  in the IA (`System › Companion Control`).
- `power.c` — AXP2101 PMU **rail gating** (enable the right rail before using a part) + PWRKEY.
- `sdcard.c` — microSD **FAT32 over SDSPI / SPI3** (session log + file browse/upload/download target).
- `usb_gadget.c`, `ducky.c`, `hid_kbd` — USB composite + **DuckyScript player** (the "run a saved macro" action
  the companion triggers reuses this).
- `display`, `touch`, `ui_theme`, `fonts/`, `ui_background` — CO5300 QSPI **RGB888 / BGR** AMOLED via LVGL v9
  `esp_lvgl_port`; CST9217 touch (native coords); the P1 colour-token / `lv_style_t` set.
- The shared **NVS/SD settings store** (UI-shell P4.4) — companion enable-state + PIN-hash + which capabilities
  are exposed persist here.

**Not yet built (hard prerequisites — be honest about the ordering):** the **WiFi stack (M5)** and the **BLE
stack (M7)** are unbuilt. Both transports depend on them. Only the on-watch QWERTY fallback (LVGL) and the
typing-avoidance plumbing are buildable with zero radio work; sequence accordingly.

## Feature scope (device-class terms)
A **companion control surface**: a phone connects to the watch and drives its features from the phone (the watch
is the worn field instrument; the phone is the full keyboard + control panel). **Two co-equal transports:**
1. **App-free WiFi web UI** — the watch serves an HTTP + WebSocket UI at **`nocsif.local`** on its own **SoftAP**
   (or a joined network); any phone browser controls it. On-SoC **WiFi radio, rail DC1** (M5). This is the only
   path that gives **app-free typing** (a browser cannot forward raw keystrokes over BLE).
2. **BLE companion app** — a GATT service mirroring the control API; always-on, low-power. On-SoC **BLE stack,
   rail DC1** (M7). Pairs with the ANCS notification-mirroring slice (§7.3).

**Full remote scope:** launch apps + run saved macros (reuse `nocsif_app_launch` + `ducky.c`); **type into any
focused watch text field** + configure settings; **file browse / upload / download on `/sd`** (reuse `sdcard.c`)
— all **co-equal across both transports**; and **live view** — mirror the display framebuffer to the phone +
relay taps back — which **rides the WiFi / web-UI transport only** (framebuffer streaming is impractical over BLE).

**Pairing / guardrails — light (owner-friction-optimised):** **PIN** pair on the local link (a QR encodes SoftAP
SSID + URL so joining is typing-free); **off by default** (enable in `System › Companion Control`); a **subtle
indicator while linked**; **session log to `/sd` optional**; **no per-action confirmation gate**. Credential-class
features (management-frame transmission, card emulation) keep their **own existing on-watch armed indicator**
(the Auto-NFC pattern), independent of the companion surface. **Local-link confidentiality — address
deliberately:** HTTP/WebSocket over the SoftAP is plaintext, so a WiFi password typed via the web UI crosses the
local link in the clear — accept for owner use on a private SoftAP, or offer optional TLS; and **rate-limit /
lock out** repeated PIN attempts.

**On-watch typing fallback (for when the phone isn't present) — v1 = QWERTY + voice:** a compact offline
**LVGL QWERTY** (`lv_keyboard`) for short strings, wired into the Notes editor and any focused field; **voice
dictation** (PDM mic → **cloud STT over WiFi**; user accepts the round-trip). **Scribble** (single-letter
handwriting) is **experimental** (no recognizer yet); **predictive swipe / glide is not planned**.
Typing-avoidance is first-class: remembered WiFi (enter a password once via companion/QR), presets, auto-named
files, BLE/QR credential hand-off — design on-watch text entry out of the common flows.

## Hardware / LVGL specifics to reuse
- **WiFi (M5, DC1)** — `esp_http_server` + WebSocket for control/live-view; `esp_mdns` for `nocsif.local`;
  SoftAP + STA can run simultaneously (single radio time-shares — a convenience surface, not a router).
- **BLE (M7, DC1)** — NimBLE GATT; define a versioned control-characteristic contract shared with the web-UI
  command set so both transports drive one API.
- **Display/live-view** — CO5300 QSPI **RGB888, BGR**, the M3 LVGL stack, the **2-px width rounder**; stream a
  **downscaled/compressed** framebuffer (never the full 410×502 RGB888 per frame) over WebSocket; cap frame rate.
- **LVGL is single-threaded behind `esp_lvgl_port`** — network/SD/USB work runs off the LVGL task; the
  phone→field-fill and phone→tap bridges must marshal onto the LVGL task (`lv_async_call` / a UI notify), never
  touch LVGL objects from the HTTP/BLE task.
- **microSD** — `sdcard.c` (FAT32, SPI3) for file transfer + session log; shared SPI bus → park unused CS HIGH.
- **Mic + speaker (M11)** — PDM mic + MAX98357A on **I²S BCLK9 / WCLK10 / DOUT11, rail BLDO2**; used only by the
  voice-dictation phase.
- **Settings store** (UI-shell P4.4, NVS/SD) — companion enable, PIN-hash, exposed-capability flags, remembered
  networks. **PIN stored hashed.**
- **UI** — the Companion screen + Notes-editor input chips already exist in `full-app-mockup.html`; match the
  locked design language (grayscale + one soft accent, serif titles, mono/typewriter data font, dither grain,
  **no scanlines, no hot colours**) and the P3 nav model.

## Phased plan (front-load the riskiest unknown; each phase isolated + flashable + on-device-verified)
- **P0 — on-watch QWERTY + typing-avoidance (buildable now, no radio).** Wire `lv_keyboard` into the Notes
  editor and any focused text field, gated by the settings store; add remembered-WiFi list + auto-named-file
  plumbing. *Risk:* LVGL keyboard integration + focus/field model. *Independent of M5/M7 — do this first.*
- **P1 — web-UI transport + field-fill (needs M5).** Front-load the biggest unknown: serve an HTTP/WebSocket
  page from the SoftAP at `nocsif.local`, and prove the **phone→focused-field text bridge** (marshalled onto the
  LVGL task). Light PIN pair. *Verify:* type on the phone → text lands in the watch field.
- **P2 — full web-UI control + file transfer (needs M5).** Launch-by-id (`nocsif_app_launch`), run saved macros
  (`ducky.c`), configure settings, **browse/upload/download `/sd`**, optional session log. Off-by-default +
  linked indicator.
- **P3 — live view (needs M5).** Downscaled/compressed framebuffer stream over WebSocket + phone-tap relay onto
  the input layer. Gate frame rate + region hard for power.
- **P4 — BLE companion transport (needs M7).** GATT service mirroring the P2 control command set (launch / run /
  type / configure / files — **not** live view, which stays on WiFi); pairs with the ANCS notification slice.
  (The phone companion app itself is out of firmware scope — define the GATT contract.)
- **P5 — voice dictation (needs M5 + M11).** PDM mic capture (BLDO2) → cloud STT over WiFi → fill field; ties to
  §7.3 speech-to-text.

## On-device verification (per phase)
- Build from **PowerShell with `-j 2`** (default parallelism OOMs the Windows paging file), never Git Bash:
  `python -m platformio run -j 2 -d D:\Docker\NocSif_Firmware\firmware`.
- **Flash** `esptool --no-stub` (download mode: hold BOOT → tap RST → release BOOT); watch is **COM7**.
- P0: keyboard appears, fills the Notes field, persists nothing sensitive in plaintext. P1: phone browser at
  `nocsif.local` fills a live field. P2: launch/run/configure/file-transfer round-trip; session log written to
  `/sd`. P3: live view refreshes + taps relay without starving the LVGL task. P4: GATT control from a test
  central. P5: spoken phrase transcribes into a field.
- Watch the serial log for LVGL-task-safety violations (any LVGL call off the LVGL task).

## Conventions
- **One milestone per branch → PR → squash-merge to `main`**; verify each phase on-device before the next.
- Ground every part in `docs/HARDWARE.md`; enable the right AXP2101 rail before use; park unused SPI CS HIGH.
- Update `docs/RESUME.md` (+ `docs/PLAN.md` status) at each stopping point; hand off cleanly if context runs high.
- Do not commit anything unless the user asks.

**Produce a phased plan and pause for my review before writing any code.**
