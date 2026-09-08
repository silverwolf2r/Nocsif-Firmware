<!-- ★ NocSif ★ -->
# NocSif

**A modular security-testing firmware for the LilyGo T-Watch Ultra (ESP32-S3).**

> ✦ ˚ · ｡ NocSif — a wireless-testing multitool on the wrist ｡ · ˚ ✦

Hi! I coded this firmware for my T-wawtch Ultra just to kind of take advantage of the idea of having a wearable device that can do a bunch of penetration testing and signal monitoring things. 
Unfortunately my T-watch Ultra came with broken NFC and Haptics so I am currently unable to code features for those parts right now. Fortunately LilyGo was really nice and gave me a refund. 
As soon as they have more of the watch back in stock I will buy again and make the firwmare capable for those 2 features as well. (highkey the NFC is the feature I was most excited for so this was a blow for me)

I want to be very upfront this whole firmware was a kind of fever dream vibe coded app that I made in about a month and a half because there frankly wasnt any firmware out there i liked. I have done dev work on the Flipper
as well as some other devices and work in Cyber Security so this was a kind of fun little project for me to have a chunky looking watch on my wrist.

Full Disclosure this is a hobby project so maintenance can be spotty at times but if you submit issues I will do my best. Feature requests are more likely to get my attention though cause those are more exciting.

-------------------------------------------------------------------------------------------------

NocSif is a from-scratch firmware for the T-Watch Ultra that turns the watch into a wearable
wireless-and-sensor testbench: WiFi, BLE, LoRa, GNSS, USB-HID, audio and motion tooling, all driven
from a clean on-watch touch UI. It is built for **authorized security testing and research only**.

The design goal is a *platform that runs things*, not a fixed toolkit — a launch-by-id app shell where
every capability is a real, hardware-backed module, plus (planned) drop-in import of Flipper-format
files and your own scripts.

> **Full disclosure:** NocSif was vibe-coded in about a month and a half, in a fit of depression.

## Status

**Active — running on real hardware.** Every capability in the list below is wired to a working screen
and a hardware-backed worker, verified on-device. All risk-first milestones — core/HAL, UI shell,
reliability, WiFi, BLE, LoRa, GNSS, IMU/audio, power management, and the full application shell — are
complete and on `main`. Remaining work is capability expansion (see **Future state**).

**NFC and haptics are hardware-faulted on my personal test watch** — its NFC transmit path (ST25R3916)
won't key the antenna and its haptic motor (DRV2605) is dead (stock LilyGo firmware fails identically,
so it's the unit, not NocSif). The NFC driver is already written and complete; **full NFC and haptic
support will ship as soon as I get a replacement watch from LilyGo.** In the meantime the speaker
serves as the alerting channel in the haptic's place.

## Hardware

LilyGo T-Watch Ultra — ESP32-S3, **16 MB flash + 8 MB PSRAM**, 2.06" 410×502 CO5300 QSPI AMOLED
(24-bit RGB888), CST9217 capacitive touch, AXP2101 PMU + fuel gauge, XL9555 I²C expander, BHI260AP
IMU (sensor hub), u-blox M10 GNSS, SX1262 (915 MHz) LoRa, ST25R3916 NFC, MAX98357A speaker + PDM mic,
PCF85063A RTC, microSD, on-SoC WiFi + BLE. Authoritative pinout / rails / I²C map: `docs/HARDWARE.md`.

---

## Install NocSif on your watch

No toolchain, nothing to build — the desktop app does everything:

1. **Download the app** — **[NocSif Desktop Bridge for Windows](https://github.com/silverwolf2r/Nocsif-Firmware/releases/latest/download/NocSifBridge-windows-x64.exe)** (one file, no installer; Windows SmartScreen may warn once — the build is unsigned). On macOS or Linux, [run it from source](#desktop-app).
2. **Plug the watch into your computer** over USB-C. The app finds it on its own and tells you what it is — a stock LilyGo watch, a blank board, or a watch already running NocSif.
3. **Click _Flash new watch_.** It writes NocSif, creates the microSD folders NocSif expects, and reboots into the firmware. On a stock or used watch pick _erase first_; the app offers to **back the watch up** beforehand, so you can put it back exactly as it was — LilyGo's own factory firmware included — whenever you like.

The same app also runs a hardware self-check, manages the microSD, mirrors the watch's screen to your computer (mouse acts as touch), backs the watch up and restores it, and keeps NocSif up to date. More in [Desktop app](#desktop-app).

**Already running NocSif?** Update straight from the wrist — **System › Update → Check → Download → Install** — no cable needed.

Rather build it yourself? See [Build from source](#build-from-source).

---

## Current capabilities

### Core, power & platform
- Boot on ESP32-S3 with 16 MB flash + 8 MB PSRAM; periodic I²C bus scan.
- **AXP2101 PMU** — per-rail power control (display / SD / NFC / sensor / speaker / LoRa / GNSS),
  battery fuel gauge (% · charge state · VBUS), USB-plug detection, software power-off.
- **Button decode** — PWRKEY (short / long / double, firmware-owned) and FN (short / double), on a
  worker task marshalled to the UI.
- **PCF85063A RTC** — read / set, build-time seed on oscillator-stop, cached clock/date strings,
  settable from GNSS UTC.
- **Reliability layer** — crash/coredump capture to NVS + reset-reason classification, boot-loop guard
  → safe mode (skips risky subsystems after repeated crashes), UI-liveness watchdog (DMA-hang
  detection), and a flash-ring **logbook** that tees `ESP_LOG` across reboots. Surfaced in a
  Diagnostics screen with compile-gated self-tests.
- **Settings store** — typed NVS store, hashed passcode (salt + SHA-256, constant-time verify),
  device name, button bindings.
- **Power management** — CPU DFS (240 ↔ 80 MHz), screen timeout, dim-before-sleep, sleep-when-still,
  re-sleep-after-shake, and Battery Saver (manual + auto below 20%, caps brightness / timeout / motion).

### Display, UI & watch experience
- **CO5300 AMOLED** bring-up with brightness control, sleep/wake, and a pipelined two-buffer DMA flush
  (the fix that eliminated the display-hang; zero runtime internal-DMA bounce).
- **CST9217 multi-touch**, including a cover-screen palm-to-sleep gesture.
- **LVGL v9** shell — grayscale + violet theme, embedded serif / mono / numeric / icon fonts, a
  render-once orrery background (rings + engraved stars) with reduced-motion-aware comets, and a nav
  stack with slide/fade transitions and a live-label header (clock / battery refresh on one tick).
- **Launch-by-id app registry** — 60+ real screens, lazily built and cached, freed on nav-back.
- **Home** — rotating category-planet carousel with an edit mode (long-press → add / drag-to-trash /
  rearrange, persisted) and selectable layouts (Ring / Dual dials / Bottom arc).
- **Watchface / peek** — live time · date · battery + planet carousel + a live weather temperature chip.
- **Control Center** (swipe-down) — brightness slider, flashlight overlay, WiFi / BLE / Airplane
  toggles that drive the real radios, live media card + phone-volume sync, and DND.
- **Lock / PIN** — watchface lock state machine, passcode keypad, unlock gate, idle→sleep auto-lock.
- **Theme / Wallpaper / Font** — custom-colour HSV accent wheel, a Compact/Default/Large type scale,
  and three wallpapers (Orrery · Engraved Sun · Grimoire), each with its own star colour, layer
  toggles, and comet setting, all persisted.
- **Apps** — on-watch **Notes** (microSD text CRUD with an on-watch keyboard), **Files** (SD browser
  with folder drill-down, text/hex viewer, and delete), a unified **Alert Center** (one dismissible log
  every source funnels into, with accent glow while alerts are held), **Flashlight**, **Weather**
  (Open-Meteo current + 3-day forecast, GPS-located), and read-only **Connectivity** and **About** hubs.
- **GeoFence / Movie mode** — a location-triggered session mode that dims to a cinema level, silences
  alerts, and drops shake-to-wake, auto-lifting once GNSS says you've left the anchor.

### WiFi
- **Station** — scan, join (on-watch keyboard), saved networks with per-network manage / auto-join /
  reconnect / forget, NVS credentials.
- **Identity** — MAC randomize / restore / manual, plus hostname (DHCP / mDNS).
- **Promiscuous monitor** — channel hop / lock, per-type and per-channel frame tallies, live rate, RSSI.
- **Passive parser** — nearby-AP list (security class + OUI vendor), station/client list, probe-request
  and SSID harvest.
- **Capture** — PCAP to microSD (radiotap) and live PCAP over USB-CDC (Wireshark extcap).
- **Handshake / PMKID** — passive 4-way-handshake and PMKID capture, crackable-detect, hashcat-22000
  export + EAPOL PCAP.
- **Anomaly detectors** — deauth / disassoc rate and duplicate-SSID / evil-twin (security-mismatch).
- **Active operations** (own networks only) — 802.11 management-frame transmission (deauth / disassoc),
  beacon transmission with a managed SSID list, software AP with client list, and a captive portal
  (DNS redirect + HTTP server + request log + selectable landing page).
- **Access Point hub** and a tap-a-network flow that hands a chosen AP straight into Signal Hunt or
  handshake capture.

### BLE (NimBLE)
- **Device scan** — name / vendor / address-type / connectable + RSSI, paged.
- **Detection** — item-trackers (Apple Find My / AirTag, Tile, Samsung SmartTag) and drones
  (OpenDroneID / ASTM F3411 Remote-ID: Basic ID, drone location/vector, operator position/ID).
- **GATT Explore** — connect, walk services / characteristics, tap-to-read, SIG-UUID names.
- **Advertise / Beacon** — named advert or Apple iBeacon (broadcaster), persisted; advert PCAP to SD.
- **Phone companion** — ANCS notification mirror (bond persist / forget) + AMS media remote
  (now-playing / transport / volume, driving the Control-Center media card).
- **BLE HID keyboard** — HID-over-GATT keyboard with DuckyScript playback over BLE.

### LoRa (SX1262)
- **P2P messaging** — broadcast text frames, inbox, node id.
- **Channel Activity** — 902–928 MHz RSSI sweep + CAD preamble detection.
- **Band Survey** — 52 × 500 kHz fine sweep with max-hold, noise floor, and a signal list that hands a
  frequency to Signal Hunt.

### GNSS (u-blox M10)
- **Live Fix** — NMEA parse to fix, sats used / in-view, lat/lon/alt/HDOP/speed, UTC, antenna status.
- **GPX Log** — background track logging to microSD, power-loss-safe.
- **Wardrive** — geotagged WiFi survey to WiGLE-1.4 CSV (GNSS + WiFi monitor together).
- **Sync Clock** — set the RTC from GNSS UTC with home-tz auto-DST.

### Signal Hunt (cross-radio direction finder)
- Live-RSSI proximity hunt with an **IMU rotation-sweep bearing dial** (GAMERV fusion, no magnetometer
  → relative bearing) rendered as a filled compass needle on a PSRAM canvas, plus an eyes-free "warmer"
  audio cue. The hunt radio is switchable across **BLE / WiFi-AP / LoRa**.

### IMU, audio & USB
- **IMU (BHI260AP)** — firmware RAM-upload + boot, accelerometer streaming, shake-to-wake, stillness
  tracking (sleep-when-still), and relative heading for Signal Hunt.
- **Speaker (MAX98357A)** — tone generator, named cues (boot / wake / sleep / tick / alert / USB),
  master mute + volume, WAV playback. The alerting channel in the dead haptic's place.
- **Mic (PDM)** — level meter (RMS + peak-hold), adjustable gain, voice-memo recording (PSRAM buffer →
  WAV on SD), with a Voice Memos app (record / play / delete / rename).
- **microSD** — FAT32 over SDSPI (shared SPI3) with an access lock and directory listing.
- **Composite USB** (TinyUSB) — runtime mode picker: Detached / CDC console / HID keyboard / MSC File
  Share, one class at a time via descriptor rewrite (no re-install).
- **HID keyboard** — connect-on-entry / long-press latch, US / GB / DE keymaps, over USB and BLE.
- **DuckyScript engine** — full grammar (REM, STRING/STRINGLN, ENTER, DELAY, DEFAULTDELAY, modifier
  combos, named keys, F1–F12, REPEAT, LOCALE); plays over USB or BLE; macro picker from `/sd/ducky`.

### Over-the-air update (OTA)
- A/B partition scheme with rollback and an on-watch microSD firmware installer (scan → validate →
  install), with WDT-safe erase and DMA-safe SD reads under active radio load.

---

## Future state (planned)

Every risk-first milestone is shipped; what follows is capability expansion. This is a priority-ordered
menu, not a hard sequence — items are built one work-package per branch. Backlogged and long-horizon
ideas are intentionally excluded (see below).

- **WiFi recon & detection** — finish rogue-AP / evil-twin, add responder / management-frame-flood /
  deauth-flood detectors, flag nearby tools (Flipper-BLE / pwnagotchi / deauther / Pineapple), a
  cross-radio (WiFi+BLE) presence census, an audible/visual radio-event alerter, camera-glasses
  (Ray-Ban Meta) detection, and Flock ALPR-camera detect-and-direction-find (now ungated by BLE +
  Signal Hunt + GNSS).
- **WiFi active & network tools** — ESP-NOW device-to-device link, DNS-sinkhole / walled-garden on the
  own AP, host discovery / port scan on a joined network, DIAL / Chromecast control, a
  wireless-HID-over-WiFi console with an auditable keystroke witness log, and USB-Ethernet emulation.
- **BLE expansion** — GATT write / subscribe-notify / characteristic testing; HID mouse / media / gamepad;
  Nordic-UART serial bridge; persistent + coded-PHY (long-range) beacon/scan; custom GATT-server
  emulation; BLE mesh node; a bond-manager UI; and decoders for Continuity / Handoff / AirDrop and Fast
  Pair / Swift Pair, plus card-skimmer detection and an anti-stalking "a tracker is following me" alert.
- **Off-grid & LoRa** — mesh telemetry / store-and-forward / traceroute, MQTT internet gateway, MeshCore
  and Reticulum/RNode stacks, repeater/router node, a narrowband in-band FSK `.sub` subset, cross-band
  BLE/web→LoRa bridge, and GNSS-time-synced collision-avoiding mesh slots.
- **Connectivity Governor** — geofenced, lease-driven radio power management (radios on only when
  useful): a per-radio lease + idle-timer, cyclic GPS + geofence engine, an auto-learned coords↔network
  geo-store for geofenced auto-connect, and a policy/UI layer. Phased P1–P4.
- **Companion control surface** — an on-network (same-LAN) phone/browser remote over the shipped SoftAP:
  mDNS `nocsif.local`, an open AP by default with an optional WPA2 password (no pairing code — the
  off-by-default toggle and the on-watch "linked" dot are the guardrails), a phone→watch command channel
  into the app registry, a watch→phone WebSocket live-push + interactive screen mirror (touch, side
  buttons, casting), a wireless `/sd` file browser (download / upload / delete), and a first-class
  Approvals lane (approve/deny/confirm) that sidesteps the iOS notification ceiling.
- **Watch utilities & audio apps** — image / GIF viewer, QR display, bubble level / inclinometer, and
  audio tools (theremin, data-over-sound acoustic modem, dB/SPL meter, live spectrum analyzer + tuner).
- **USB host mode & platform** — USB host with peripheral class drivers, U2F/FIDO CTAP over HID,
  MicroPython drop-in script layer, Flipper-format parsers + an app/script manager, and a self-hosted
  OTA path.

- **NFC & haptics** — the NFC driver is complete and haptic support is a small add; both are blocked
  only by the faulty hardware on my personal test watch and **will ship once I receive a replacement
  watch from LilyGo.** NFC brings read / write / emulate / NDEF and tap-to-pair/-type combos; haptics
  restores the tactile alerting channel.

**Excluded (backlogged / long-horizon, by operator decision):** step-count / workout / sleep tracking,
GPX route navigation & track-back, localization / i18n, an on-watch web browser, and the long-horizon
concept set (head-coupled 3D idle scene, cloud TTS voice assistant, the USB-C "Backpack" co-processor,
and the home-server thin-client architecture).

## Build from source

_Most people don't need this — the [desktop app](#install-nocsif-on-your-watch) flashes prebuilt NocSif in a
click._ To build the firmware yourself, from **PowerShell** (ESP-IDF refuses MSYS), with `-j 2` (default
parallelism OOMs the Windows paging file):

```bash
python -m platformio run -e nocsif-twatch-ultra -j 2
```

Flash with `esptool --no-stub`, or point the desktop app at your own `firmware.bin`. Full pinout, rails, and
build/flash notes are in `docs/`.

### Performance trade-offs (not bugs)
Two deliberate settings trade a little speed to run **BLE and WiFi at the same time**: while Bluetooth
is on, WiFi uses a reduced-speed "lean" buffer profile (turn Bluetooth off to restore full throughput),
and most allocations are routed to slower PSRAM firmware-wide. Details and measurements: `docs/LESSONS.md`.

## Desktop app

**NocSif Desktop Bridge** (`tools/nocsif_bridge/`) is the computer-side companion — plug the watch in over
USB-C and it connects on its own: flash / update / provision a watch (a blank board included), run the
hardware-defect check, manage the microSD, back the watch up and restore it, and drive the watch from the
computer with a live view of its screen. It works with any T-Watch Ultra — stock LilyGo firmware, a blank
board, or NocSif — and can put LilyGo's factory firmware back.

Windows: download **[NocSifBridge-windows-x64.exe](https://github.com/silverwolf2r/Nocsif-Firmware/releases/latest/download/NocSifBridge-windows-x64.exe)**
from the [releases page](https://github.com/silverwolf2r/Nocsif-Firmware/releases) — a single file, nothing to
install. macOS and Linux: run from source:

```bash
pip install -r tools/nocsif_bridge/requirements.txt
python tools/nocsif_bridge/nocsif_bridge_app.py
```

The app talks to the watch over its USB-Serial/JTAG console (a small JSON protocol, `firmware/src/bridge.h`)
and flashes through `esptool`. Details in `tools/nocsif_bridge/README.md`.

## Layout

- `docs/` — living design docs: `PLAN.md` (master plan), `HARDWARE.md`, `ARCHITECTURE.md`,
  `LESSONS.md`, `RESUME.md`, plus `design/` and `research/`.
- `firmware/` — the NocSif firmware (ESP-IDF / PlatformIO; app sources in `firmware/src/`).
- `tools/` — host-side tooling (flashing, capture helpers).

## Safety & scope

**Authorized security testing and research only.** Active WiFi and BLE transmission operate against
your own networks and devices. Attach the antenna before any LoRa transmit. Keep charge current ≤ 500 mA.
