# Current Features

A full list of NocSif's current, hardware-backed capabilities. Every entry is backed by
shipping firmware; known hardware and scope limits are called out inline rather than hidden.

## Current capabilities

### Core, power & platform
- Boot on ESP32-S3 with 16 MB flash + 8 MB PSRAM; periodic I²C bus scan (known-device table +
  fault log if the always-present parts — PMU / expander / RTC — don't ACK); an honest
  terminal-style boot splash that reveals one real bring-up line per subsystem (PMU / display /
  touch / RTC / storage / USB), timing out gracefully instead of hanging or lying.
- **AXP2101 PMU** — per-rail power control (display / SD / NFC / sensor / speaker / LoRa / GNSS),
  battery fuel gauge (% · charge state · VBUS · VBAT mV), USB-plug detection, software power-off.
- **Charger ownership & self-healing** — decodes the AXP2101 charger block (state machine / limits
  / CV / battery voltage) and re-asserts a known-good config (BATFET on, cell-charge enable,
  500 mA charge cap, 4.2 V CV, 1500 mA input limit) at boot, on every USB plug-in, and from a
  charge-stall watchdog, so a latched-off or drifted charger recovers across a reboot. Charge-first
  boot at ≤5 % (minimal init to dodge a brown-out loop); low-battery alerts at ≤15 % / ≤5 %.
- **Button decode** — PWRKEY (short / long / double, firmware-owned) and FN (short / double), on a
  worker task marshalled to the UI. PWR is a sleep/wake toggle; FN-short is Back; FN-double and
  PWR-double launch user-assignable app shortcuts.
- **PCF85063A RTC** — read / set, build-time seed on oscillator-stop, cached clock/date strings,
  settable from GNSS UTC **and** NTP (NTP takes precedence; GNSS backfills when nothing has synced
  for ~10 min), home-timezone + US-DST conversion.
- **Reliability layer** — crash/coredump capture to NVS + reset-reason classification, boot-loop
  guard → safe mode (skips risky subsystems after repeated crashes), UI-liveness watchdog (DMA-hang
  detection), and a flash-ring **logbook** that tees `ESP_LOG` across reboots. Surfaced in a
  Diagnostics screen with compile-gated self-tests and an amp test-tone.
- **Settings store** — typed NVS store, hashed passcode (salt + SHA-256, constant-time verify),
  device name, button bindings, per-app preferences.
- **Power management** — CPU DFS (240 ↔ 80 MHz), screen timeout (15 s … never), dim-before-sleep,
  sleep-when-still (set-down detection), shake-to-wake, re-sleep-after-shake, double-shake-to-sleep,
  single/double tap-to-wake, palm-to-sleep cover gesture, and Battery Saver (manual + auto below
  20 %, caps brightness / timeout / motion).

### Display, UI & watch experience
- **CO5300 AMOLED** (410×502, QSPI, no white boot-flash) bring-up with brightness control,
  sleep/wake, and a pipelined two-buffer DMA flush with a 3 s stall guard (the fix that eliminated
  the display-hang; zero runtime internal-DMA bounce).
- **CST9217 multi-touch** (2-finger controller; the shell uses point 0 — no pinch / two-finger
  gestures), including a cover-screen palm-to-sleep gesture and double-tap-to-wake.
- **LVGL v9** shell — grayscale + violet theme, embedded serif / mono / numeric / icon fonts, a
  render-once orrery background (rings + engraved stars) with reduced-motion-aware comets, and a nav
  stack with slide/fade transitions and a live-label header (clock / battery refresh on one tick).
- **Launch-by-id app registry** — 60+ real screens, lazily built and cached, freed on nav-back; the
  single entry point every binding (FN button, planets, gestures, phone companion) targets.
- **Home** — rotating category-planet carousel with an edit mode (long-press → add / drag-to-trash /
  rearrange / re-skin each planet's icon, persisted) and selectable layouts (Ring / Dual dials /
  Bottom arc) with step or fluid spin.
- **Watchface / peek** — a fully configurable readout stack (time · date · live weather chip ·
  battery + charging bolt · sunrise/sunset · moonrise/set · moon phase, reorderable, any subset or
  none) plus a planet carousel; a top-left DND / Bluetooth indicator cluster and a
  background-activity pill.
- **Control Center** (swipe-down over any screen) — brightness slider, flashlight overlay, WiFi /
  BLE / Airplane toggles that drive the real radios (reflecting true live state), a live media card
  + phone-volume sync, and DND. Brightness/volume stay in sync with the phone companion.
- **Lock / PIN** — watchface-as-lock state machine, 4–8-digit passcode keypad, a single unlock gate
  for every exit (planet tap / swipe-up / shortcut) that fails *open* if it can't build, and
  idle→sleep auto-lock.
- **Theme / Wallpaper / Font** — custom-colour HSV accent wheel (with presets), a
  Compact/Default/Large type scale, and three wallpapers (Orrery · Engraved Sun · Grimoire), each
  with its own star colour, per-layer toggles, and comet setting, all persisted.
- **Alert Center** — one dismissible log every source funnels into (System / Phone / Alarm / Timer /
  Battery / Weather / LoRa Signal / WiFi / Tracker), swipe-to-dismiss cards, a tappable preview
  banner that deep-links to the alert's screen, and per-type sound / volume / screen-wake / banner
  settings. Silenced in DND / Movie mode.
- **GeoFence / Movie mode** — a location-triggered session mode that dims to a cinema level,
  silences alerts, and drops shake-to-wake, auto-lifting once GNSS says you've left the anchor
  (~250 m).
- **Phone companion web-remote & live mirror** — the watch stands up an open SoftAP + `nocsif.local`
  and serves a live JPEG screen mirror a phone/laptop drives with no app: tap / swipe / drag the
  shell, type into the focused field, work the side-buttons, brightness and volume, cast (blank the
  panel and stream only), wake, or reboot, plus a wireless microSD file browser (list / download /
  upload / delete). Scan-to-join QR, optional WPA2 password, and optional internet-share (NAPT) to
  the joined phone. A persistent on-screen dot shows whenever the watch is remotely controllable.
  *(Open AP with no auth gate — treat as owner-only.)*
- **Desktop-bridge screen view** — a full-res RGB565 mirror + dirty-rect delta stream over USB and a
  one-frame screenshot, for the NocSif Desktop Bridge's live view (mouse acts as touch). (Protocol
  detail under *IMU, audio & USB → USB desktop bridge*.)

### Apps
- **Notes** — microSD text CRUD (`/sd/nocsif/notes`) with an on-watch keyboard; first line is the
  title, large files open read-only so a save can't truncate them.
- **Files** — a real SD file manager: folder drill-down, text/hex viewer (4 KB / 512-byte dumps),
  new folder, move/rename (collision-safe), single-file delete and recursive folder delete (two-tap
  / long-press armed), all under an SD access lock.
- **Voice Memos** — record (PSRAM buffer → 16 kHz WAV on SD, 30 s cap) with a live input-level bar,
  then play / rename / delete; recording continues in the background with an on-screen record dot.
- **Audio Player** — play `.wav` / `.mp3` from `/sd/nocsif/Audio` through the Class-D amp
  (8–48 kHz, 8/16/24/32-bit or float, mono/stereo), folder browser, background playback. Doubles as
  the tone player for the shopping-cart unlock tone and any other audio file you drop on the card.
- **Tuner / Piano Tuner** — mic-based YIN pitch detection: a chromatic tuner and per-instrument
  tuners (guitar / violin / cello / ukulele / bass / banjo) with a Guitar-Tuna-style cents dial;
  tapping a string plays its reference tone (also an acoustic speaker→mic self-test).
- **Level** — an IMU bubble level / inclinometer with live roll + pitch and a "LEVEL" flag.
- **Weather** — Open-Meteo current conditions + a 3-day forecast, GPS-located, °C/°F, feels-like,
  humidity, wind, condition glyphs (day/night aware); feeds the watchface chip. Keep-last-reading
  when offline. (Sunrise/sunset and moon data come from the on-watch almanac, not Open-Meteo.)
- **Time** — a live clock hub with **Stopwatch** (laps, background-running), **Timer** (HH:MM:SS
  scroll picker, background countdown that rings on any screen), **World Clock** (15 cities, tap to
  set home — the single timezone source), and **Alarms** (up to 8, Once/Daily/Weekdays/Weekends,
  self-disarming one-shots). Alarms are software, matched per-minute against the RTC (not
  PCF85063A hardware alarms); they fire in the background and raise a full-screen ring (PWR = stop,
  FN = snooze).
- **Connectivity** and **About** — read-only live hubs for the radio links, the governor policy, and
  device/firmware/runtime info.

### WiFi
- **Station** — scan, join (on-watch keyboard, show/hide password), saved networks with per-network
  manage / auto-join / reconnect / forget and a **saved-password reveal**, NVS + SD-folder
  credentials (`.net` files you can drop in from a computer), bounded reconnect + auth fail-fast.
- **Identity** — MAC randomize / restore / manual entry, "adopt" a seen client's MAC, plus hostname
  (DHCP / mDNS).
- **Grab Wi-Fi from PC** — USB-HID keystroke injection that types a self-contained Windows / macOS /
  Linux command to export the plugged-in PC's saved Wi-Fi SSIDs + passphrases into
  `/sd/nocsif/Networks`, then reclaims the card and counts them — no PC software.
- **Promiscuous monitor** — channel hop / lock, per-type and per-channel frame tallies, live rate,
  RSSI last/peak, and a per-channel activity spectrum.
- **Passive parser** — nearby-AP list (security class + OUI vendor + raw IE decode), station/client
  list, probe-request and SSID harvest, each with a detail panel and a per-device log
  (`/sd/nocsif/Device Wifi Log/<MAC>.log`).
- **Capture** — PCAP to microSD (radiotap) and live PCAP over USB-CDC (Wireshark extcap, bundled
  host plugin).
- **Handshake / PMKID** — passive 4-way-handshake and PMKID capture, crackable-detect, EAPOL PCAP,
  and a hashcat-22000 export written straight to the card (WPA\*01 / WPA\*02) — crack-ready with no
  PC-side hcxtools step.
- **WEP key recovery** — Klein/PTW + FMS statistical recovery of a 40/104-bit key from collected IVs
  (aircrack-class, pure-compute core), with optional ARP-replay IV farming and one-tap rejoin on
  recovery (legacy RC4 WEP; authorized / own-network auditing).
- **Anomaly detectors** — deauth / disassoc rate + peak and duplicate-SSID / evil-twin
  (security-mismatch) with a severity meter.
- **Active operations** (own networks only — policy framing, not a technical lock) — 802.11
  management-frame transmission (deauth / disassoc, source spoofed as the target BSSID), beacon
  transmission with a managed SSID list + bulk decoy generation, software AP with a DHCP server +
  client list, and a captive portal (DNS redirect + HTTP server + request log that records client
  IP / method / path / **submitted form fields** to SD + selectable landing page). A one-tap flow
  clones a seen AP (SSID + beacon IEs) or saves a probed SSID into the emulation folder to beacon
  it. *(There is no automatic probe-response matching — i.e. no live KARMA auto-respond; SSID
  harvest and beaconing exist, the auto-response glue does not.)*
- **Access Point hub** and a tap-a-network flow that hands a chosen AP straight into Signal Hunt,
  handshake capture, WEP recovery, deauth, or emulation.
- **Omit list** — a persisted MAC filter applied at ingestion, so your own/known devices drop out of
  every passive table (APs / stations / probes / handshakes / hunt).
- **Time sync & place-store** — NTP over WiFi keeps the system clock (and RTC) fresh for TLS /
  WireGuard; connecting ties the AP's BSSID to the GPS location where you linked, feeding the
  Connectivity Governor.
- *Raw 802.11 TX is unlocked globally — the master primitive under deauth / beacon / ARP-replay.*

### On-LAN network tools
*Pure lwIP-socket tooling over the LAN the watch is joined to — no 802.11 scan or TX; needs a live
STA link; authorized testing on your own network. Optional result log to SD.*
- **Host discovery** — ICMP sweep of the local /24, enriched per up-host with ARP-cache MAC, an
  OUI → vendor guess, and a best-effort NetBIOS (NBNS) name.
- **Port scan + banner** — bounded TCP connect-scan with a first-bytes banner grab and a
  likely-service guess; curated "nmap" / "masscan" port-set presets over the same primitive (honest
  in-UI that these are presets, not the real tools).
- **Deep-dive fingerprint** — ping (RTT/TTL) + top-ports scan + banners + ARP/NBNS, producing an OS
  guess and a device-type guess (router / printer / NAS / camera / …).
- **DNS toolkit** — A / AAAA / CNAME / MX / TXT / NS / PTR / SOA / SRV queries against the link's
  resolver or a chosen server.
- **Traceroute** — ICMP, increasing-TTL hop list.
- **Service discovery** — SSDP/UPnP M-SEARCH + an mDNS / DNS-SD browse of common service types.
- **HTTP + TLS recon** — request line / methods / headers, and a zgrab-style certificate peek
  (subject / issuer / validity / key / SANs / negotiated version + cipher).
- **Packet crafter** — hping-style ICMP / TCP / UDP probes with a chosen (spoofable) source IP, raw
  TCP flags, and an optional payload.
- **Netcat** — a raw TCP/UDP console session with scrollback (tap an open port to connect straight
  to it).

### Gateway / travel router
*Turns the watch into a GL.iNet/OpenWRT-style travel router over its own Wi-Fi uplink; every layer
is SD-config-driven (from `/sd/nocsif/gateway`) and default-OFF, with a live forwarded-throughput
readout. It owns the single SoftAP, so it is mutually exclusive with the WiFi SoftAP / portal /
companion / monitor features.*
- **Share (NAT)** — raises a SoftAP and NAPT-forwards downstream clients through the STA uplink
  (open or WPA2), with a live client list.
- **DNS filter (Pi-hole style)** — an SD-backed blocklist/allowlist resolver for downstream clients
  (0.0.0.0 / NXDOMAIN on a match), with live queries / blocked / forwarded / cached counters.
- **WireGuard tunnel** — routes forwarded traffic through a WireGuard tunnel configured from SD
  (WireGuard-over-lwIP; configurable UDP port, e.g. 53 / 67, for restrictive uplinks), with
  handshake / up state.
- **Sign-in portal** — a local sign-in page served from SD that downstream clients must load before
  their traffic forwards, with a signed-in client count.
- **Uplink captive-portal sign-in** — detects a hotel/cafe portal on the watch's own STA link and
  can auto-accept a simple Terms page or relay the venue portal to a downstream phone to complete
  the login.
- **DNS-transport (dnst) fallback** — scaffolded (iodine/dnstt-style), reported honestly as
  experimental / not yet functional.

### Saved-network store
- **Networks folder** — a per-SSID file on microSD holding everything the watch knows about a
  network (BSSID / channel / security / vendor / hostname / PSK / raw beacon IEs / location / an
  associated portal page), the common sink for "clone this AP," "save this probe's SSID," and manual
  import over USB File Share; merges on re-sight; forget-but-keep.
- **Download to SD** — a generic URL → `/sd/nocsif/storage` HTTP(S) downloader (resumable via HTTP
  Range for large images), and a one-tap grab of the connected network's captive/landing page
  paired with its SSID record.

### BLE (NimBLE)
- **Device scan** — name / vendor / address-type / connectable + RSSI, paged, with a full raw
  advertising-data + scan-response decode in the detail panel.
- **Detection** — item-trackers (Apple Find My / AirTag, Tile, Samsung SmartTag) with an anti-stalk
  "a tracker is following you" background alert (persisted allowlist + per-device mute; keyed on
  address, so a MAC-rotating AirTag resets its timer), and drones (OpenDroneID / ASTM F3411
  Remote-ID: Basic ID, drone location/vector, **operator/pilot position + operator ID**; BT4-legacy
  adverts only).
- **Card-skimmer scan** — flags the cheap BLE serial modules skimmers use (HM-10-class 0xFFE0 /
  Nordic-UART + module-name heuristic) — passive heuristic, false-positive-prone.
- **GATT Explore** — connect, walk services / characteristics, tap-to-read, SIG-UUID names
  (read-only).
- **Advertise / Beacon** — named advert or Apple iBeacon (broadcaster), persisted; advert PCAP to
  SD.
- **BLE Spam** (advertisement-resilience test, authorized targets) — floods nearby devices with
  spoofed pairing/notification pop-ups (Apple Continuity / Proximity Pairing, Google Fast Pair,
  Samsung EasySetup, Microsoft Swift Pair), a fresh random MAC per cycle, low/medium/high intensity,
  timed auto-stop, behind an explicit consent gate; optionally connectable so a tapped pop-up bonds
  the device to the watch.
- **Phone companion** — one bonded iOS link multiplexes ANCS notification mirror (bond persist /
  forget), AMS media remote (now-playing / transport / volume), and the HID controllers; a master
  Bluetooth toggle, saved-device list, and a one-tap iOS on-screen-keyboard toggle.
- **BLE HID controllers** — a composite keyboard / live QWERTY / trackpad mouse (hold+drag select,
  scroll wheel) / media keys / presenter remote (Keynote) / numeric keypad, plus **BLE Ducky**:
  DuckyScript playback over BLE HID.
- **Omit list** — a persisted address filter dropped at ingestion, so a device goes invisible across
  every BLE screen.

### LoRa / Sub-GHz (SX1262)
- **P2P messaging** — broadcast text frames, inbox (sender id · RSSI · age), node id, with an
  on-watch chat / compose UI (plaintext; no ACK / mesh).
- **Channel Activity / Band Survey** — a 52 × 500 kHz full-band RSSI sweep (902–928 MHz, retunable
  150–960 MHz by hold-and-drag) with max-hold, noise floor, CAD preamble detection, and a
  detected-signal list; a per-frequency detail panel with a US band-plan guess, signal character
  (bandwidth/continuity), and zoom-in, that hands a frequency to Signal Hunt. Persisted omit list
  for known carriers.
- **Signal Alerts** — a background, duty-cycled band-watch over a chosen window + sensitivity (RSSI
  Near/Med/Far/custom) that posts to the Alert Center (with an optional rough distance estimate) and
  stands down around File Share / downloads / foreground radio use.
- **Packet Capture / Replay** — FSK/LoRa **packet** capture (configurable freq / SF / BW / CR /
  bitrate / fdev / rxbw / sync) to a Flipper-format `/sd/nocsif/subghz/<name>.sub` (read + write),
  with an on-watch browser and replay. *(No raw/OOK receiver — capture is packet-only and needs a
  matching syncword/preamble; a live RSSI energy trace still shows any RF, even signals it can't
  decode or replay.)*
- **Sub-GHz transmit** (armed + ISM-gated + dead-man-timed) — crude CW-gated OOK transmit of saved
  `.sub` files: RAW timing replay, fixed-code encoder synthesis (Princeton/PT2262 · CAME · Nice FLO
  · Holtek/HS2XX), captured-packet replay, and a de Bruijn B(2,n) brute-force sweep (OpenSesame-
  style) for tolerant fixed-code garage/gate remotes — plus a bundled preset library. (Fixed/static-
  code only — useless against rolling-code security.)
- **Signal Blast (CW carrier)** — a bounded continuous-wave source, parked or swept across a range,
  for antenna/VSWR characterization and receiver-resilience bench work.
- **Stuck-radio self-recovery** — detects a BUSY-stuck SX1262 and recovers it (reset + re-config +
  rail power-cycle), restoring the prior mode.
- *Every TX path is dead-man-bounded, UI-Armed, and ISM-checked, but the tuning clamp allows
  150–960 MHz and the antenna is matched for 915 MHz, so out-of-band TX/RX is inefficient. These
  emit RF — transmit only where you're licensed or in a shielded setup.*

### GNSS (u-blox M10)
- **Live Fix** — NMEA parse to fix, sats used / in-view, lat/lon/alt/HDOP/speed/course, UTC, antenna
  status, stream liveness.
- **GPX Log** — background track logging to microSD, power-loss-safe.
- **Wardrive** — geotagged WiFi survey to WiGLE-1.4 CSV (GNSS + WiFi monitor together),
  background-running, with an on-watch **Wardrive Map** view.
- **Sync Clock** — set the RTC from GNSS UTC with home-tz auto-DST, plus the background NTP/GNSS
  auto-sync.
- *On the developer's reference unit the GNSS module self-tested as not transmitting; the full code
  path is implemented but may be exercising non-functional hardware on that board.*

### Signal Hunt (cross-radio direction finder)
- Live-RSSI proximity hunt across **BLE / WiFi (APs · clients · probe-requesters) / LoRa
  frequency**, with an **IMU rotation-sweep bearing dial** (GAMERV fusion, no magnetometer →
  relative bearing) rendered as a filled compass needle on a PSRAM canvas, step-count dead-reckoning
  + an RSSI-gradient bearing fit (walk + turn once, no spinning), a burst-"breadcrumb" bearing for
  bursty emitters, GNSS track-mode hand-over at speed, a rough TX-power distance estimate, a
  "warmer/colder" eyes-free audio cue, per-radio detail panels with offensive hand-offs, and
  automatic re-pin across BLE random-address rotation.

### IMU, audio & USB
- **IMU (BHI260AP)** — firmware RAM-upload + boot (~117 KB), accelerometer streaming, shake-to-wake,
  stillness tracking (sleep-when-still), step counter (hunt dead-reckoning), bubble-level, and
  relative heading for Signal Hunt.
- **Speaker (MAX98357A)** — tone generator, named cues (boot / wake / sleep / tick / alert / USB),
  master mute + volume, WAV / MP3 playback. The alerting channel in the dead haptic's place.
- **Mic (PDM)** — level meter (RMS + peak-hold), adjustable gain, YIN pitch engine (tuners),
  voice-memo recording (PSRAM buffer → WAV on SD).
- **microSD** — FAT32 over SDSPI (shared SPI3) with an access lock, directory listing, no-reboot
  rescan of a reseated card, on-watch reformat, and a static bounce-buffer pool that keeps SD reads
  DMA-safe under BLE + WiFi + LoRa load.
- **Composite USB** (TinyUSB) — runtime mode picker: Detached / CDC console / HID keyboard / MSC
  File Share / **Bootable OS**, one class at a time via descriptor rewrite (no re-install).
- **Bootable OS** — serve an `.iso` / `.img` from `/sd/nocsif/bootos` to a PC as a read-only
  bootable USB disk (MBR/GPT/ISO9660 probe; boots a live OS such as Tails without repartitioning
  the card; nothing written back), with a bootability probe and a resumable large-image/Tails
  downloader.
- **HID keyboard** — connect-on-entry / long-press latch, US / GB / DE keymaps, over USB and BLE.
- **DuckyScript engine** — full grammar (REM, STRING/STRINGLN, ENTER, DELAY, DEFAULTDELAY,
  DEFAULTCHARDELAY, modifier combos, named keys, F1–F12, REPEAT, LOCALE); plays over USB or BLE;
  macro picker from `/sd/nocsif/ducky`; run-from-RAM scripts and a built-in "Grab Wi-Fi from PC"
  payload.
- **USB desktop bridge** — a JSON-over-USB-Serial/JTAG protocol for the NocSif Desktop Bridge:
  version / status / hardware-health census, screenshot + live mirror, microSD file ops, SD format,
  UI control (launch / back / home / type / key / brightness / button / touch / cast / wake),
  USB-mode + gateway control, and reboot. Runs even in safe mode. *(Does not check the passcode —
  treat a USB connection as physical trust.)*

### NFC (ST25R3916, HF 13.56 MHz)
- **Read Tag** — one NFC-A discovery cycle reporting UID (+ length) and type via RFAL, driven by a
  worker task, plus an RF / antenna self-test. Emulate / Key Recovery / NDEF / Saved Cards /
  Write-Copy are registered stubs for a future release. *Note: on the reference unit the NFC RF
  transmit path is dead (`TX_DEAD`), so reading works only where the front-end is healthy; NFC is
  effectively non-functional on that hardware.*

### Connectivity Governor (battery & performance)
- Radios as activity leases over drivers that stay resident. The Governor learns where your saved
  Wi-Fi networks live by pairing a GPS fix with an active link (BSSID-keyed "places," up to 8), then
  **parks the idle Wi-Fi station** to save battery and **wakes it when you return to a known place**
  — 100/200/500 m geofences with exit hysteresis, a GPS duty-cycle gated by an IMU stillness test,
  and modem-sleep when linked-and-idle; it also refreshes weather on arrival. It never deinitializes
  a driver (the coexistence DMA pool stays fixed). *Scope: it parks the **Wi-Fi station only** — BLE
  / LoRa / NFC are left to their own per-screen activity — and it is decoupled from battery %.*

### Over-the-air update (OTA)
- A/B partition scheme with automatic rollback and three install paths — an on-watch microSD
  firmware installer (scan → validate → install), a GitHub over-internet check / download
  (configurable owner/repo source), and a File-Share drop (copy an image over USB, then install from
  SD) — with WDT-safe erase, DMA-safe SD reads under active radio load, a two-tap install confirm,
  and a low-battery-without-USB block.
- *Network update is two-step (download to SD, then SD-install) and integrity-only: the image is
  SHA-256-verified but not firmware-signed, there is no anti-rollback, and a downgrade will be
  offered.*

---
