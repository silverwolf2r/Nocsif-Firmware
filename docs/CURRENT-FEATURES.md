# Current Features

A full list of NocSif's current, hardware-backed capabilities.

## Current capabilities

### Core, power & platform
- Boot on ESP32-S3 with 16 MB flash + 8 MB PSRAM; periodic I²C bus scan; honest terminal-style
  boot splash that reveals one real bring-up line per subsystem (PMU / display / touch / RTC /
  storage / USB), timing out gracefully instead of hanging or lying.
- **AXP2101 PMU** — per-rail power control (display / SD / NFC / sensor / speaker / LoRa / GNSS),
  battery fuel gauge (% · charge state · VBUS), USB-plug detection, charge-stall self-heal, software
  power-off.
- **Button decode** — PWRKEY (short / long / double, firmware-owned) and FN (short / double), on a
  worker task marshalled to the UI. PWR is a sleep/wake toggle; FN-short is Back; FN-double and
  PWR-double launch user-assignable app shortcuts.
- **PCF85063A RTC** — read / set, build-time seed on oscillator-stop, cached clock/date strings,
  settable from GNSS UTC or NTP, with a background auto-sync (NTP-over-WiFi first, GNSS backfill when
  nothing has synced for ~10 min).
- **Reliability layer** — crash/coredump capture to NVS + reset-reason classification, boot-loop guard
  → safe mode (skips risky subsystems after repeated crashes), UI-liveness watchdog (DMA-hang
  detection), and a flash-ring **logbook** that tees `ESP_LOG` across reboots. Surfaced in a
  Diagnostics screen with compile-gated self-tests and an amp test-tone.
- **Settings store** — typed NVS store, hashed passcode (salt + SHA-256, constant-time verify),
  device name, button bindings, per-app preferences.
- **Power management** — CPU DFS (240 ↔ 80 MHz), screen timeout (15 s … never), dim-before-sleep,
  sleep-when-still (set-down detection), shake-to-wake, re-sleep-after-shake, double-shake-to-sleep,
  single/double tap-to-wake, palm-to-sleep cover gesture, and Battery Saver (manual + auto below 20 %,
  caps brightness / timeout / motion).
- **Connectivity Governor** — radios as activity leases over drivers that stay resident: parks an idle
  unlinked WiFi station, modem-sleeps a linked-idle one, and *learns each saved network's GPS location*
  to geofence-wake the radio near "home" and refresh weather on arrival — selectively powering radios
  up and down to stretch battery life without a driver teardown.

### Display, UI & watch experience
- **CO5300 AMOLED** bring-up with brightness control, sleep/wake, and a pipelined two-buffer DMA flush
  (the fix that eliminated the display-hang; zero runtime internal-DMA bounce).
- **CST9217 multi-touch**, including a cover-screen palm-to-sleep gesture and double-tap-to-wake.
- **LVGL v9** shell — grayscale + violet theme, embedded serif / mono / numeric / icon fonts, a
  render-once orrery background (rings + engraved stars) with reduced-motion-aware comets, and a nav
  stack with slide/fade transitions and a live-label header (clock / battery refresh on one tick).
- **Launch-by-id app registry** — 100+ real screens, lazily built and cached, freed on nav-back; the
  single entry point every binding (FN button, planets, gestures, phone companion) targets.
- **Home** — rotating category-planet carousel with an edit mode (long-press → add / drag-to-trash /
  rearrange / re-skin each planet's icon, persisted) and selectable layouts (Ring / Dual dials /
  Bottom arc) with step or fluid spin.
- **Watchface / peek** — a fully configurable readout stack (time · date · live weather chip · battery
  + charging bolt · sunrise/sunset · moonrise/set · moon phase, reorderable, any subset or none) plus a
  planet carousel; a top-left DND / Bluetooth indicator cluster and a background-activity pill.
- **Control Center** (swipe-down over any screen) — brightness slider, flashlight overlay, WiFi / BLE /
  Airplane toggles that drive the real radios (reflecting true live state), a live media card + phone-
  volume sync, and DND. Brightness/volume stay in sync with the phone companion.
- **Lock / PIN** — watchface-as-lock state machine, 4–8-digit passcode keypad, a single unlock gate for
  every exit (planet tap / swipe-up / shortcut) that fails *open* if it can't build, and idle→sleep
  auto-lock.
- **Theme / Wallpaper / Font** — custom-colour HSV accent wheel (with presets), a Compact/Default/Large
  type scale, and three wallpapers (Orrery · Engraved Sun · Grimoire), each with its own star colour,
  per-layer toggles, and comet setting, all persisted.
- **GeoFence / Movie mode** — a location-triggered session mode that dims to a cinema level, silences
  alerts, and drops shake-to-wake, auto-lifting once GNSS says you've left the anchor (~250 m).
- **Alert Center** — one dismissible log every source funnels into (System / Phone / Alarm / Timer /
  Battery / Weather / LoRa Signal / WiFi / Tracker), swipe-to-dismiss cards, a tappable preview banner
  that deep-links to the alert's screen, and per-type sound / volume / screen-wake / banner settings.
  Silenced in DND / Movie mode.
- **Phone companion web-remote & live mirror** — the watch stands up an open SoftAP + `nocsif.local`
  and serves a live JPEG screen mirror a phone/laptop drives with no app: tap / swipe / drag the shell,
  type into the focused field, work the side-buttons, brightness and volume, cast (blank the panel and
  stream only), wake, or reboot. Scan-to-join QR, optional WPA2 password, and optional internet-share
  (NAPT) to the joined phone. A persistent on-screen dot shows whenever the watch is remotely
  controllable.
- **Desktop-bridge screen view** — a full-res RGB565 mirror + dirty-rect delta stream over USB-CDC and
  one-frame screenshot, for the NocSif Desktop Bridge's live view (mouse acts as touch).

### Apps
- **Notes** — microSD text CRUD (`/sd/nocsif/notes`) with an on-watch keyboard; first line is the
  title, large files open read-only so a save can't truncate them.
- **Files** — a real SD file manager: folder drill-down, text/hex viewer (4 KB / 512-byte dumps), new
  folder, move/rename (collision-safe), single-file delete and recursive folder delete (two-tap /
  long-press armed), all under an SD access lock.
- **Voice Memos** — record (PSRAM buffer → 16 kHz WAV on SD, 30 s cap) with a live input-level bar,
  then play / rename / delete; recording continues in the background with an on-screen record dot.
- **Audio Player** — play `.wav` / `.mp3` from `/sd/nocsif/Audio` through the Class-D amp (8–48 kHz,
  8/16/24/32-bit or float, mono/stereo), folder browser, background playback. Doubles as the tone
  player for the shopping-cart unlock tone and any other audio file you drop on the card.
- **Tuner / Piano Tuner** — mic-based YIN pitch detection: a chromatic tuner and per-instrument tuners
  (guitar / violin / cello / ukulele / bass / banjo) with a Guitar-Tuna-style cents dial; tapping a
  string plays its reference tone (also an acoustic speaker→mic self-test).
- **Level** — an IMU bubble level / inclinometer with live roll + pitch and a "LEVEL" flag.
- **Weather** — Open-Meteo current conditions + a 3-day forecast, GPS-located, °C/°F, feels-like,
  humidity, wind, condition glyphs (day/night aware); feeds the watchface chip and the sunrise/sunset
  almanac. Keep-last-reading when offline.
- **Time** — a live clock hub with **Stopwatch** (laps, background-running), **Timer** (HH:MM:SS scroll
  picker, background countdown that rings on any screen), **World Clock** (15 cities, tap to set home —
  the single timezone source), and **Alarms** (up to 8, Once/Daily/Weekdays/Weekends, background
  firing, self-disarming one-shots). Alarms/timers raise a full-screen ring (PWR = stop, FN = snooze).
- **Connectivity** and **About** — read-only live hubs for the radio links, the governor policy, and
  device/firmware/runtime info.

### WiFi
- **Station** — scan, join (on-watch keyboard, show/hide password), saved networks with per-network
  manage / auto-join / reconnect / forget and a **saved-password reveal**, NVS + SD-folder credentials
  (`.net` files you can drop in from a computer).
- **Identity** — MAC randomize / restore / manual entry, "adopt" a seen client's MAC, plus hostname
  (DHCP / mDNS).
- **Grab Wi-Fi from PC** — USB-HID keystroke injection that types a self-contained Windows / macOS /
  Linux command to export the plugged-in PC's saved Wi-Fi SSIDs + passphrases into `/sd/nocsif/Networks`,
  then reclaims the card and counts them — no PC software.
- **Promiscuous monitor** — channel hop / lock, per-type and per-channel frame tallies, live rate, RSSI
  last/peak, and a per-channel activity spectrum.
- **Passive parser** — nearby-AP list (security class + OUI vendor + raw IE decode), station/client
  list, probe-request and SSID harvest, each with a detail panel and a per-device log
  (`/sd/nocsif/Device Wifi Log/<MAC>.log`).
- **Capture** — PCAP to microSD (radiotap) and live PCAP over USB-CDC (Wireshark extcap).
- **Handshake / PMKID** — passive 4-way-handshake and PMKID capture, crackable-detect, EAPOL PCAP, and
  a hashcat-22000 export written straight to the card (WPA\*01 / WPA\*02) — crack-ready with no PC-side
  hcxtools step.
- **WEP key recovery** — IV collection with optional ARP-replay injection and a recovered hex/ASCII key
  plus one-tap rejoin (authorized / own-network auditing).
- **Anomaly detectors** — deauth / disassoc rate + peak and duplicate-SSID / evil-twin
  (security-mismatch) with a severity meter.
- **Active operations** (own networks only) — 802.11 management-frame transmission (deauth / disassoc,
  source spoofed as the target BSSID), beacon transmission with a managed SSID list + bulk decoy
  generation, software AP with a DHCP server + client list, a captive portal (DNS redirect + HTTP
  server + request log that records client IP / method / path / **submitted form fields** to SD +
  selectable landing page), and a one-tap flow to clone a seen AP (SSID + beacon IEs) or a probed SSID
  (KARMA-style lure) into the emulation folder.
- **Access Point hub** and a tap-a-network flow that hands a chosen AP straight into Signal Hunt,
  handshake capture, WEP recovery, deauth, or emulation.
- **On-LAN Network Tools** (live station link only) — ICMP host-sweep of the /24 with ARP MAC + OUI
  vendor + NBNS name enrichment, TCP connect-scan + banner grab, curated nmap/masscan-style port-set
  presets, a one-tap device **deep-dive fingerprint** (OS guess + device type: router/printer/NAS/
  camera), targeted ping, traceroute, a DNS toolkit (A/AAAA/CNAME/MX/TXT/NS/PTR/SOA/SRV), SSDP/UPnP +
  mDNS/DNS-SD service discovery, a raw TCP/UDP netcat console (tap an open port to connect straight to
  it), HTTP + TLS recon (methods / headers + an unverified zgrab-style certificate peek), and an
  hping-style packet crafter (ICMP/TCP/UDP, raw TCP flags, source-IP spoofing). Optional result log to
  SD.
- **Travel Router (gateway)** — re-share the station uplink to a device-hosted SoftAP with NAT, with
  independently toggled layers: a Pi-hole-style **DNS blocklist** (SD-backed), a **WireGuard tunnel**
  (import / view / edit endpoint-port-DNS from `/sd/nocsif/wireguard`), an uplink **captive-portal
  sign-in** helper (auto-accept terms, or relay the venue login to a downstream phone), a local sign-in
  portal, and a **DNS-transport (dnst)** scaffold (experimental, not yet functional). Live
  forwarded-throughput readout; config file-driven from `/sd/nocsif/gateway`.

### BLE (NimBLE)
- **Device scan** — name / vendor / address-type / connectable + RSSI, paged, with a full raw
  advertising-data + scan-response decode in the detail panel.
- **Detection** — item-trackers (Apple Find My / AirTag, Tile, Samsung SmartTag) with an anti-stalk
  "a tracker is following you" background alert (and per-device mute), card-skimmers (BLE-serial-module
  signatures, heuristic), and drones (OpenDroneID / ASTM F3411 Remote-ID: Basic ID, drone
  location/vector, **operator/pilot position + operator ID**).
- **GATT Explore** — connect, walk services / characteristics, tap-to-read, SIG-UUID names.
- **Advertise / Beacon** — named advert or Apple iBeacon (broadcaster), persisted; advert PCAP to SD.
- **BLE Spam** (advertisement resilience test, authorized targets) — floods nearby phones with spoofed
  pairing/notification pop-ups (Apple Continuity / Proximity Pairing, Google Fast Pair, Samsung
  EasySetup, Microsoft Swift Pair), low/medium/high intensity, timed auto-stop, behind an explicit
  consent gate; optionally connectable so a tapped pop-up pairs the device to the watch.
- **Phone companion** — one bonded iOS link multiplexes ANCS notification mirror (bond persist /
  forget), AMS media remote (now-playing / transport / volume), and the HID controllers; a master
  Bluetooth toggle, saved-device list, and a one-tap iOS on-screen-keyboard toggle.
- **BLE HID controllers** — a composite keyboard / live QWERTY / trackpad mouse (hold+drag select,
  scroll wheel) / media keys / presenter remote (Keynote) / numeric keypad, plus **BLE Ducky**:
  DuckyScript playback over BLE HID.

### LoRa / Sub-GHz (SX1262)
- **P2P messaging** — broadcast text frames, inbox (sender id · RSSI · age), node id.
- **Channel Activity / Band Survey** — a 52 × 500 kHz full-band RSSI sweep (902–928 MHz, retunable
  150–960 MHz by hold-and-drag) with max-hold, noise floor, CAD preamble detection, and a detected-
  signal list; a per-frequency detail panel with a US band-plan guess, signal character
  (bandwidth/continuity), and zoom-in, that hands a frequency to Signal Hunt.
- **Signal Alerts** — a background, duty-cycled band-watch over a chosen window + sensitivity that
  posts to the Alert Center (with an optional rough distance estimate) and stands down around File
  Share / downloads / foreground radio use.
- **Packet Capture / Replay** — FSK/LoRa packet capture (configurable freq / SF / BW / CR / bitrate /
  fdev / rxbw / sync) to a Flipper-format `/sd/nocsif/subghz/<name>.sub`, with an on-watch browser and
  replay.
- **Sub-GHz transmit** (armed + ISM-gated + dead-man-timed) — bounded CW "Signal Blast" (single or
  swept, for antenna/VSWR + receiver-resilience testing) and OOK transmit of saved `.sub` files: RAW
  timing replay, fixed-code encoder synthesis (Princeton / CAME / Nice FLO / Holtek), and a de Bruijn
  brute-force sweep (OpenSesame-style) for fixed-code garage/gate remotes — plus a bundled preset
  library. (Fixed-code only; crude CW-gated OOK, useless against rolling-code security.)

### GNSS (u-blox M10)
- **Live Fix** — NMEA parse to fix, sats used / in-view, lat/lon/alt/HDOP/speed/course, UTC, antenna
  status, stream liveness.
- **GPX Log** — background track logging to microSD, power-loss-safe.
- **Wardrive** — geotagged WiFi survey to WiGLE-1.4 CSV (GNSS + WiFi monitor together), background-
  running.
- **Sync Clock** — set the RTC from GNSS UTC with home-tz auto-DST, plus the background NTP/GNSS
  auto-sync.

### Signal Hunt (cross-radio direction finder)
- Live-RSSI proximity hunt across **BLE / WiFi (APs · clients · probe-requesters) / LoRa frequency**,
  with an **IMU rotation-sweep bearing dial** (GAMERV fusion, no magnetometer → relative bearing)
  rendered as a filled compass needle on a PSRAM canvas, step-count dead-reckoning + an RSSI-gradient
  bearing fit (walk + turn once, no spinning), GNSS track-mode hand-over at speed, a rough TX-power
  distance estimate, a "warmer/colder" eyes-free audio cue, per-radio detail panels with offensive
  hand-offs, and automatic re-pin across BLE random-address rotation.

### IMU, audio & USB
- **IMU (BHI260AP)** — firmware RAM-upload + boot, accelerometer streaming, shake-to-wake, stillness
  tracking (sleep-when-still), step counter (hunt dead-reckoning), bubble-level, and relative heading
  for Signal Hunt.
- **Speaker (MAX98357A)** — tone generator, named cues (boot / wake / sleep / tick / alert / USB),
  master mute + volume, WAV / MP3 playback. The alerting channel in the dead haptic's place.
- **Mic (PDM)** — level meter (RMS + peak-hold), adjustable gain, YIN pitch engine (tuners), voice-memo
  recording (PSRAM buffer → WAV on SD).
- **microSD** — FAT32 over SDSPI (shared SPI3) with an access lock, directory listing, no-reboot
  rescan of a reseated card, and on-watch reformat.
- **Composite USB** (TinyUSB) — runtime mode picker: Detached / CDC console / HID keyboard / MSC File
  Share / **Bootable OS**, one class at a time via descriptor rewrite (no re-install).
- **Bootable OS** — serve an `.iso` / `.img` from `/sd/nocsif/bootos` to a PC as a read-only bootable
  USB disk (boots a live OS such as Tails without repartitioning the card; nothing written back), with
  a bootability probe and a resumable Tails downloader.
- **Download to SD** — a generic URL → `/sd/nocsif/storage` HTTP(S) downloader (resumable for large
  images), and a one-tap grab of the connected network's captive-portal page.
- **HID keyboard** — connect-on-entry / long-press latch, US / GB / DE keymaps, over USB and BLE.
- **DuckyScript engine** — full grammar (REM, STRING/STRINGLN, ENTER, DELAY, DEFAULTDELAY, modifier
  combos, named keys, F1–F12, REPEAT, LOCALE); plays over USB or BLE; macro picker from
  `/sd/nocsif/ducky`.

### NFC (ST25R3916, HF 13.56 MHz)
- **Read Tag** — one NFC-A discovery cycle reporting UID + type, driven by a worker task, plus an RF /
  antenna self-test. Emulate / Key Recovery / NDEF / Saved Cards / Write-Copy are registered stubs for
  a future release. *Note: on the reference unit the NFC RF transmit path is dead (`TX_DEAD`), so NFC
  is effectively non-functional on that hardware.*

### Over-the-air update (OTA)
- A/B partition scheme with automatic rollback and three install paths — an on-watch microSD firmware
  installer, a GitHub over-internet check / download (configurable owner/repo source), and a File-Share
  drop — with WDT-safe erase, DMA-safe SD reads under active radio load, a two-tap install confirm, and
  a low-battery-without-USB block.

---
