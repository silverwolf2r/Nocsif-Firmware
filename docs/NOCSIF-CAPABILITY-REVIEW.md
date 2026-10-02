# NocSif — Capability Review (for your eyes, not for publishing yet)

> ⚠️ **Internal reference — do not push to the public repo as-is.** This file enumerates security
> weaknesses (open APs, missing auth gates, the passcode scheme) that are fine for an
> authorized-testing tool but read badly out of context. It's preserved here because the chat that
> produced it never committed it and that session's container is ephemeral. Keep it local, or move
> it somewhere private.

A code-verified pass over the whole firmware. Every capability below was checked against the actual
source (not the docs), with the file it lives in noted so you can re-check. Three parts:

1. **Verified current capabilities** — the double-checked list.
2. **Flashy things** — demo-worthy / hacker-audience ideas, each grounded in a real capability.
   *(These now live in `docs/README-FLASHY-CAPABILITIES.md`; kept out of this file to avoid two
   copies.)*
3. **Accuracy notes** — where the existing docs are now wrong, stale, or overstated.

---

## 1. Verified current capabilities (double-checked)

> Legend: ✅ verified in code · ⚠️ verified **with a real limit/caveat** · 🆕 works but **not in
> `docs/CURRENT-FEATURES.md` as of the audit**.

### Core / power / platform (`main.c`, `power.c`, `pm.c`, `reliability.c`, `settings.c`, `rtc.c`, `i2c_scan.c`, `xl9555.c`, `buttons.c`)
- ✅ ESP32-S3 boot, quad-PSRAM gate, periodic I²C bus scan with a known-device table + fault logging.
- ✅ AXP2101 per-rail power gating (display/SD/NFC/sensor/speaker/LoRa/GNSS), fuel gauge (%/charge/VBUS/VBAT), software power-off.
- 🆕 Charger self-healing: re-asserts BATFET/charge-enable/limits (500 mA cap, 4.2 V CV, 1500 mA input) at boot, on USB plug, and from a charge-stall watchdog; charge-first boot at ≤5 %; low-battery alerts at ≤15 %/≤5 %.
- ✅ CPU DFS 240↔80 MHz (off by default; Battery Saver forces it on), screen timeout, dim-before-sleep, sleep-when-still, re-sleep-after-shake, Battery Saver (manual + auto ≤20 % unplugged).
- ✅ Reliability: coredump→NVS crash summary, reset-reason classification, 3-strike boot-loop → safe mode, LVGL-liveness task-WDT, bounded display-DMA stalls, flash-ring logbook teeing `ESP_LOG`, Diagnostics screen.
- ✅ Settings: typed NVS store, salted SHA-256 passcode (constant-time compare), device name, button bindings.
- ✅ RTC read/set/build-seed/cached strings; settable from GNSS UTC **and** NTP (NTP wins, GNSS backfills after ~10 min), home-tz + US-DST.
- ⚠️ "Alarms/timers" are **software** matched per-minute against the RTC — not PCF85063A hardware alarms.

### Display / UI / watch (`display.c`, `display_io.c`, `touch.c`, `ui.c`)
- ✅ CO5300 410×502 AMOLED (QSPI, no white boot-flash), brightness/sleep/wake, two-buffer ping-pong flush with a 3 s stall guard.
- ⚠️ CST9217 touch is **2-finger**; the UI uses point 0 only (no pinch/2-finger gestures). Palm-to-sleep uses the controller's cover-gesture bit.
- ✅ LVGL v9 shell, violet theme, orrery/sun/grimoire wallpapers, HSV accent wheel, type scale, planet-carousel Home + watchface with edit mode, Control Center, PIN lock, nav stack.
- ✅ Unified Alert Center with 9 sources: System/Phone/Alarm/Timer/Battery/Weather/Signal/Wi-Fi/Tracker.
- 🆕 Timers & Alarms: Stopwatch, Countdown, World Clock, Alarms (persisted, editor, PWR-ack/FN-snooze).
- 🆕 Audio hub: WAV/MP3 Player (soundboard / cart tones), instrument Tuner + chromatic Piano Tuner (mic YIN pitch), Voice Memos.
- ✅ GeoFence / Movie mode (GPS-anchored, auto-exit beyond ~250 m). (NFC Fencing + Gesture shortcuts are dimmed, planned stubs — **not** live.)

### WiFi (`wifi.c`)
- ✅ Station: scan, join, saved networks (per-network reconnect/forget; auto-join is one global flag), NVS creds, bounded reconnect + auth fail-fast.
- ✅ Identity: MAC randomize/restore/manual; DHCP hostname from device name.
- ✅ Promiscuous monitor (hop/lock, per-type + per-channel tallies, rate, RSSI); passive parser (AP list w/ security + OUI vendor, station list, probe/SSID harvest, raw-IE view).
- ✅ Capture: radiotap PCAP to SD + live PCAP over USB-CDC (Wireshark extcap, bundled host plugin).
- ✅ Handshake/PMKID passive capture, crackable-detect, hashcat-22000 export + EAPOL PCAP.
- 🆕 **WEP key recovery** (`wep_recover.c`): Klein/PTW + FMS, 40/104-bit, ARP-replay IV farming, auto-rejoin on success.
- ✅ Anomaly detectors: deauth/disassoc rate; duplicate-SSID / evil-twin (security-mismatch).
- ⚠️ Active ops: deauth/disassoc TX (broadcast-to-AP-clients in practice), beacon TX (arbitrary SSIDs, managed list), SoftAP + client list, captive portal (DNS redirect + HTTP + request/POST log + SD-hosted HTML). Comments say "own networks only" — **there is no technical scoping**, it's policy framing.
- 🆕 **Companion web remote** (`do_companion_*`): SoftAP + HTTP/WS; live screen mirror + remote touch, launch app, type/keys, brightness/volume, FN/PWR, cast/blank, wake, **reboot**, and a wireless SD file browser (list/get/put/delete). ⚠️ No auth gate — anyone on the AP controls the watch.
- 🆕 Persisted omit list (MAC filter at ingestion); NTP time sync; WiFi-geolocation place store; Show-Wi-Fi-QR.
- ⚠️ Raw 802.11 TX is unlocked globally (`ieee80211_raw_frame_sanity_check` override) — the master primitive under deauth/beacon/ARP-replay.
- ❌ Not present: WPS, Karma/probe-response matching (harvest + beacon exist but no auto-response glue), airtime/utilization stats.

### On-LAN network tools (`nettools.c`)
- ✅ Host-discovery sweep (ICMP + ARP MAC + OUI + NBNS name); TCP connect-scan + banner + service guess; "nmap/masscan" presets (honest: presets, not the real tools); deep-dive fingerprint (OS + device-type guess); DNS (A/NS/CNAME/SOA/PTR/MX/TXT/AAAA/SRV); traceroute; SSDP/mDNS discovery; TLS-cert recon; hping-style packet crafter; netcat. Optional SD log. Needs a live STA link; no 802.11 TX.

### Gateway / travel router (`gateway.c`)
- ✅ NAT share (SoftAP + NAPT over the STA uplink, open/WPA2, client list, throughput).
- ✅ Pi-hole-style DNS blocklist/allowlist (SD-backed, live counters).
- ✅ WireGuard tunnel for forwarded traffic (WireGuard-over-lwIP, configurable UDP port, handshake state).
- ✅ Sign-in portal (SD-hosted page gating downstream clients; signed-in count).
- ✅ Uplink captive-portal sign-in (auto-accept a Terms page, or relay a venue portal to a downstream phone).
- ⚠️ DNS-tunnel transport: scaffolded only, reported as "experimental".
- ⚠️ Owns the one SoftAP, so it's mutually exclusive with wifi.c's SoftAP features (AP/portal/companion/monitor).

### Saved-network store (`netstore.c`) + web downloader (`webdl.c`)
- ✅ Per-SSID network files on SD (BSSID/channel/security/vendor/hostname/PSK/raw IEs/location/portal), merge-on-resight, forget-but-keep, importable over USB File Share.
- 🆕 Generic URL→`/sd/storage` HTTP(S) downloader (progress/cancel); **resumable large-image download** (HTTP Range, for Bootable OS); grab the current network's captive/landing page and pair it with the SSID.

### BLE (`ble.c`, NimBLE)
- ✅ Active scan (name/vendor/addr-type/connectable/RSSI, raw ADV kept).
- ✅ Tracker detection (Apple Find My/AirTag, Tile, Samsung SmartTag) + ⚠️ time-based "following you" alert (keyed on address, so a MAC-rotating AirTag resets its clock).
- ✅ Drone Remote-ID (OpenDroneID/ASTM F3411: Basic ID, drone loc/vector, operator pos/ID) — ⚠️ BT4-legacy adverts only (no BT5 ext-adv scan).
- ✅ Card-skimmer scan (HM-10-class 0xFFE0 / Nordic-UART / module-name heuristic) — ⚠️ heuristic, false-positive-prone; classic-BT HC-05/06 can't appear on a BLE-only radio.
- ✅ GATT explore (connect, walk services/chars, tap-to-read, SIG names) — read-only (no write/fuzz).
- ✅ Advertise / iBeacon broadcaster (persisted); advert PCAP to SD (received adverts).
- 🆕 **BLE Spam** (all OS targets verified): Apple Continuity Nearby-Action + Proximity-Pairing, Google Fast Pair, Samsung EasySetup buds/watch, Microsoft Swift Pair; random MAC per cycle, bounded auto-stop, optional bond-the-tapper mode.
- ✅ Phone companion: ANCS notifications + AMS media remote (bond persist/forget).
- ✅ BLE HID keyboard (HOGP) + DuckyScript over BLE (+ mouse / consumer-media reports).
- 🆕 Persisted BLE omit list (device invisible across all screens).
- ❌ Not present: classic-BT anything, GATT write/fuzz, connection-flood, pairing brute-force.

### Sub-GHz (`lora.cpp`, SX1262) — **much bigger than the docs say**
- ✅ P2P messaging (text frames, 16-slot inbox, MAC-derived node id, chat/compose UI) — ⚠️ plaintext, no ACK/mesh; RX decode unverified without a 2nd node.
- ✅ Channel Activity (15-point RSSI sweep + CAD preamble detection, 902.4–927.6 MHz).
- ✅ Band Survey (52×500 kHz bins, max-hold, median noise floor, contiguous-signal detection, up to 8 signals → Signal Hunt); window re-spannable **150–960 MHz**; persisted omit list.
- 🆕 **Signal Alerts (background band watch)** → Alert Center: named window + sensitivity (RSSI Near/Med/Far/custom), duty-cycled survey, sound/light alert on threshold, optional crude distance.
- 🆕 **Packet capture/replay** (`.sub`): capture FSK/LoRa **packets** → RAM ring → Flipper-compatible `.sub` on SD (read **and** write) → replay. ⚠️ No raw/OOK *receiver* (packet-only; needs syncword/preamble). Energy trace shows undecodable RF (e.g. a 433.92 OOK burst) but is not replayable.
- 🆕 **OOK transmit** (CW-gated, crude): auto-routed `.sub` TX (RAW timings; fixed-code synth for Princeton/PT2262·CAME·Nice FLO·Holtek/HS2XX; de Bruijn; captured replay) + **de Bruijn B(2,n) brute-force (OpenSesame)**. ⚠️ Static-code only, useless vs rolling codes.
- 🆕 **Signal Blast** = bounded CW carrier (parked/swept) for antenna/VSWR + RX-resilience.
- 🆕 Stuck-radio self-recovery.
- ⚠️ Every TX path is dead-man-bounded + UI Arm + ISM-gated, but the clamp allows 150–960 MHz; antenna is matched for 915 MHz so out-of-band TX/RX is inefficient. **Emits RF — licensing is on the operator.**

### GNSS (`gnss.c`) + weather (`weather.c`) + almanac (`almanac.c`)
- ✅ Live Fix (full NMEA parse), power-loss-safe GPX logging, Wardrive → WiGLE-1.4 CSV + a Wardrive Map screen, Sync-Clock-from-GNSS (in `ui.c`).
- ⚠️ GNSS hardware was found **not transmitting on the developer's board** (self-test "dead" verdict) — the full code path is implemented but may be exercising non-functional hardware on that unit.
- ✅ Weather: Open-Meteo current + 3-day, GPS auto-follow, °C/°F toggle (cached convert), geofence anchor forced-refresh, keep-last-on-failure.
- 🆕 Sun/moon almanac (`almanac.c`): NOAA sunrise/sunset + Meeus moonrise/set/phase, powering the watchface sun-&-moon line. (Note: sunrise/sunset come from here, **not** from weather.c.)

### Signal Hunt (`ui.c` + per-radio engines)
- ✅ Cross-radio RSSI direction finder (BLE / WiFi-AP / LoRa), GAMERV no-magnetometer bearing dial, anti-aliased needle on a PSRAM canvas, eyes-free "warmer" audio cue, step dead-reckoning, position-gradient bearing fit, burst-breadcrumb bearing for bursty emitters.

### IMU / audio / mic / USB / SD (`imu.c`, `audio.c`, `mic.c`, `usb_gadget.c`, `bootos.c`, `ducky.c`, `hid_kbd.c`, `sdcard.c`, `sd_bounce.c`, `bridge.c`)
- ✅ BHI260AP (~117 KB firmware RAM-upload), accel streaming, shake-to-wake, stillness, step counter, relative heading.
- ✅ MAX98357A speaker: tone gen, named cues, mute/volume, **WAV + MP3 streaming player** (soundboard / cart tones / voice-memo playback).
- ✅ PDM mic: RMS/peak level meter, gain, voice-memo record (PSRAM→WAV), **YIN pitch detector** (tuners).
- ✅ Composite USB (TinyUSB): Detached / CDC / HID / MSC File Share / **Bootable OS**, single-class descriptor-rewrite switching; SD claim/release/rescan/**format**; live PCAP over CDC.
- 🆕 **Bootable OS** (`bootos.c`): serve a `.iso`/`.img` from SD as a read-only bootable USB disk (MBR/GPT/ISO9660 probe, <2 GiB), raw card stays app-owned.
- ✅ HID keyboard (US/GB/DE), USB + BLE; full DuckyScript grammar; run-from-RAM scripts; built-in "Grab Wi-Fi from PC" payload.
- ✅ microSD FAT over SDSPI + access lock + static bounce-pool keeping SD DMA-safe under BLE+WiFi+LoRa load.
- 🆕 **USB desktop bridge** (`bridge.c`): JSON control — version/status/**health census**, screenshot + live mirror, SD file ops, SD format, UI control (launch/back/home/type/key/brightness/button/touch/cast/wake), USB-mode + gateway control, **reboot**. Runs even in safe mode; ⚠️ never checks the passcode.

### NFC (`nfc.cpp`, ST25R3916)
- ⚠️ NFC-A read (tag UID + length via RFAL `rfalNfcDiscover`) + RF antenna self-test. Real code, not a stub — but reports `TX_DEAD` where the front-end is broken (the developer's unit), so reading works only on healthy hardware; emulate/clone/write are future work.

### Connectivity Governor (`governor.c`)
- ✅ Learns BSSID-keyed "places" (saved AP + the GPS location where you linked, up to 8), **parks the idle Wi-Fi station** and wakes it at known places (100/200/500 m geofences + hysteresis, GPS duty-cycle gated by IMU stillness, modem-sleep when linked-idle). Never deinits a driver. ⚠️ Parks the **Wi-Fi station only** — not BLE/LoRa/NFC, and it's decoupled from battery %.

### OTA (`ota.c`)
- ✅ A/B + rollback, SD installer (scan/validate/install, WDT-safe erase, brown-out-guarded).
- ⚠️ Network update (GitHub manifest check → resumable SHA-256-verified download **to SD** → SD install). Two-step (a card is required); integrity only (no firmware signature / anti-rollback; a downgrade is offered too).

---

## 3. Accuracy notes (what's wrong/stale/overstated in the current docs)

These are worth fixing before anyone leans on the docs:

- **Sub-GHz is badly undersold.** `README.md` says "monitor + test-TX today — not a Flipper-style
  capture-and-replay… no OOK… not 'clone your garage remote.'" The code now has `.sub` packet
  capture/replay, Flipper `.sub` read+write, OOK transmit with fixed-code encoder synthesis
  (Princeton/CAME/Nice FLO/Holtek), and a de Bruijn brute-force. The true limit is narrower: **no raw/OOK
  *receiver*** (packet-only capture) and a 915-matched antenna. `HARDWARE.md`'s "no OOK, no raw capture"
  is only half-right now.
- **"Sync Clock from GNSS" is real** (verified — `ui.c:21238`/`21307`), despite living in `ui.c`
  rather than `gnss.c`/`rtc.c`. Keep it.
- **Governor parks the Wi-Fi station only**, not "radios" plural — BLE/LoRa/NFC are untouched, and it's
  decoupled from battery %. The README wording ("turn off and on radios") overstates it.
- **Network OTA is two-step** (download to SD, then SD-install), needs a card, and is integrity-only
  (no signature / anti-rollback; downgrades offered). `CURRENT-FEATURES.md` documented only the SD path.
- **These are implemented but were missing from `CURRENT-FEATURES.md` at audit time:** WEP recovery; the
  companion web remote (full control + screen mirror + SD browser + reboot); the on-LAN network tools; the
  gateway / travel router; the saved-network store + web downloader; BLE Spam; BLE card-skimmer scan; the
  BLE/LoRa omit lists; Signal Alerts; sub-GHz capture/replay + OOK TX + de Bruijn + Signal Blast; Timers &
  Alarms; the Audio hub + tuners; Bootable OS; the USB desktop bridge; NFC-A read; the charger
  self-healing; the sun/moon almanac; Wardrive Map. *(The consolidated `CURRENT-FEATURES.md` now folds all
  of these in.)*
- **Security realities to be aware of (not bugs to hide, but worth knowing):** the "own networks only"
  framing on Wi-Fi active ops has no technical enforcement; the companion AP and the USB bridge have no
  auth/passcode gate; the passcode is single-round SHA-256 with no lockout. The captive portal logs POSTed
  form fields and serves arbitrary SD HTML — i.e. credential-harvesting is a trivial combination. All of
  this is fine for an authorized-testing tool, but it's the kind of thing a reviewer will call out.
- **Minor:** tracker "following" detection is keyed on address, so a MAC-rotating AirTag resets its clock;
  drone detection is BT4-legacy only; touch is really 2-finger; GNSS hardware was dead on the dev unit;
  NFC reports `TX_DEAD` on the dev unit.

---

*Provenance: this file reconstructs the code-verified review produced by the "Firmware capabilities
audit" chat (branch `clankert/wizardly-volta-39pa0s`), which was never committed there. Section 2
(the flashy bullets) has been split into `docs/README-FLASHY-CAPABILITIES.md`.*
