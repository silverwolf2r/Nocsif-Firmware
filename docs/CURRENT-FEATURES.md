# Current Features

A full list of NocSif's current, hardware-backed capabilities.

<!-- Fill in the current feature list below. -->
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
