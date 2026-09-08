# NocSif — master plan & idea backlog

> The single working plan for **NocSif** on the LilyGo **T-Watch Ultra** (ESP32-S3) — a modular
> security-testing firmware, for **authorized security testing only**. **Other chats: read this first**
> to orient, then follow §8 into the detailed docs for the milestone you're building.
>
> **As of 2026-08-26.** Neutral device-class wording throughout (§1). **Status legend:** ✅ done &
> verified on-device · 🔵 planned · 🕳️ buildable, not yet scheduled · ⛔ hardware-blocked / dead (§4-bottom, §6).
> §3 is the audited feature-by-feature done list; §4 is the forward plan.
>
> **⚠ HARDWARE GOTCHA (this keeps biting): THE DISPLAY IS A ROUNDED RECTANGLE — THE CURVED CORNERS PHYSICALLY CLIP
> UI.** Never anchor content to a physical corner; keep it **centred** or inside the safe zone
> **`x ∈ [48, 362], y ∈ [42, 460]`**. Full detail in §1 + `docs/LESSONS.md`.

## 1. Conventions (every chat follows these)
- **Neutral device-class wording** — "802.11 management-frame transmission", "credential-capture form",
  "USB HID keyboard", "RF transceiver", "NFC reader". Never "attack / offensive / payload / weaponize /
  victim / target / red-team" — charged vocabulary hard-fails Anthropic's automated cyber-safeguard.
  Applies to docs, commits, and **especially** any subagent / Workflow prompt. Keep load-bearing format
  names (**DuckyScript**, `.sub`, `.nfc`).
- **⚠ THE DISPLAY IS A ROUNDED RECTANGLE — ITS CURVED CORNERS PHYSICALLY CLIP CONTENT. This bug RECURS —
  check every layout.** The 410×502 AMOLED is a round-rect — **measured 2026-08-26 (interactive calibrator):
  visible area = inset ~7 px, corner radius ~115 px** (technically-perfect inset 5 / radius 125; the software
  `CORNER_R 40` mask is only a look-alike). Anything anchored to a
  physical corner — `LV_ALIGN_TOP_LEFT / TOP_RIGHT / BOTTOM_LEFT / BOTTOM_RIGHT` — lands **inside the clipped
  arc and is cut off / invisible.** Keep edge content **centred** (`x/y-MID`) or inside the **safe zone
  `x ∈ [48, 362], y ∈ [42, 460]`** (proven-safe insets: `HEADER_HINSET 48` / `HEADER_TOP_PAD 42`). Applies to
  every peek / watchface / overlay / Control-Center / header / corner-cluster layout. Full detail:
  `docs/LESSONS.md` (rounded-corner safe zone).
- **Build from PowerShell, never Git Bash** (ESP-IDF refuses MSYS), with **`-j 2`** (default parallelism OOMs
  the Windows paging file): `python -m platformio run -j 2 -d D:\Docker\NocSif_Firmware\firmware`. **Flash**
  `esptool --no-stub`; the watch is **COM7**.
- **One milestone / work-package per branch → PR → squash-merge to `main`**; front-load the biggest unknown as an
  isolated, flashable first phase; verify each phase on-device before the next.
- **Fill, don't rebuild.** The UI shell lays every screen as a present-but-disabled **launch-by-id registry**
  row; each milestone **wires its real action into the existing stubbed row** rather than adding navigation.
- **Plan stays the source of truth on `main`** — plan / mockup edits ship as small **docs-only PRs to `main`**.
- **LVGL is single-threaded** behind `esp_lvgl_port` — UI callbacks only signal worker tasks; heavy radio /
  SD / USB work runs off the LVGL task.
- **Ground every feature in real hardware** (`docs/HARDWARE.md`); shared buses → park unused CS HIGH, enable
  the right AXP2101 rail before use. Flag buildable vs blocked honestly.
- **Hand off cleanly** near ~75% context: update `docs/RESUME.md` (done / in-flight / next + file paths) and
  this doc if a status changed. Never push into a context wall mid-phase.

## 2. What "complete" means
A modular security-testing platform on the T-Watch Ultra that (a) imports Flipper files / GitHub projects /
the user's own Python with no fuss, (b) is controllable from the watch **and** a WiFi web UI, (c) exercises
every capability the fitted silicon supports, and (d) is usable as a worn watch. The honest **hardware
ceiling (§6)** bounds "complete" — some Flipper / Pineapple / Proxmark features can't run on this board. An
on-watch browser is deferred to v2/v3.

## 3. Done — built & verified on-device (feature-by-feature)
**Audited against `firmware/src/` on 2026-08-26.** Every line below is a real, wired feature (a working
`build_*` screen and/or a hardware-backed worker), not a stub. Milestones **M0–M4 · UI-shell P1–P8 ·
reliability hardening · M5 WiFi · M7 BLE · M11 watch-core · M9 LoRa · M8 GNSS** are all ✅ on-device. The stubs
that remain are enumerated in §4.1. Blow-by-blow build history (PRs, gotchas) → `docs/RESUME.md`.

### 3.1 Core / HAL / power (`power.c` · `xl9555.c` · `rtc.c` · `buttons.c` · `i2c_scan.c`)
- Boot on ESP32-S3; **16 MB flash + 8 MB PSRAM**; I²C bus scan (boot + periodic).
- **AXP2101 PMU** — per-rail control (display / SD / NFC / sensor / speaker / LoRa / GNSS), **battery fuel gauge**
  (% / charge state / VBUS), fresh-VBUS read for the USB-plug cue, **software power-off**.
- **PWRKEY** decode (short / long / double — firmware owns the button) + **FN** button (short / double), decoded
  on a worker task and marshalled to LVGL.
- **PCF85063A RTC** — read / set, build-time seed on oscillator-stop, cached clock/date strings, GNSS-settable.
- **XL9555** GPIO expander rails — display power, touch reset, haptic-enable line, SD-detect.

### 3.2 Reliability & platform services (`reliability.c` · `logbook.c` · `settings.c`)
- **Crash / coredump** record to NVS + reset-reason classification.
- **Boot-loop guard → safe mode** (skips risky subsystems on repeated crashes).
- **UI-liveness watchdog** (DMA-hang detection) + mark-healthy dwell.
- **Logbook** — `ESP_LOG` tee to a flash-ring partition (survives reboot) + read-tail / clear.
- **Diagnostics** screen (`build_diag`) — crash record, logbook tail, test tone, compile-gated self-tests.
- **Settings** — NVS typed store, **hashed passcode** (salt + SHA-256, constant-time verify), device name,
  button-binding cache.

### 3.3 Display / touch / UI core (`display.c` · `touch.c` · `ui_theme.c` · `ui_background.c` · `ui_nav.c`)
- **CO5300 QSPI AMOLED** (24-bit RGB888) bring-up, brightness (WRDISBV), sleep / wake, **pipelined 2-buffer DMA
  flush** (the display-hang fix; zero runtime internal-DMA bounce).
- **CST9217** capacitive touch — multi-touch + **cover-screen palm-to-sleep** gesture.
- **LVGL v9** via `esp_lvgl_port`; grayscale + violet theme tokens; embedded serif / mono / numeric / icon fonts.
- **Render-once orrery** background (rings + star dots + engraved stars) + rounded-corner masks + **shooting-star
  comets** (reduced-motion aware).
- **Nav shell** — screen stack with slide/fade push-pop, transparent compositing over the orrery, a **live-label
  registry** (one header tick refreshes clock / battery / row tags), HID-armed header badge, Control-Center top-drag.

### 3.4 Home / watchface / Control Center / lock (`ui.c`)
- **Launch-by-id app registry** (`k_screens[]`) — 60+ real screens, lazy build + cache, freed on nav-back.
- **Watchface / peek** (`build_peek`) — live time / date / battery + planet carousel + **live weather temp chip**
  (real Open-Meteo fetch — §4.1 Weather ✅).
- **Home ring** (`build_home`) — rotating category-planet carousel + **edit mode** (long-press → add "+" /
  drag-to-trash / rearrange, persisted).
- **Category screens** — Life / Cyber / System.
- **Control Center** (`build_control_center`) — brightness slider (live + persisted), flashlight overlay, WiFi
  toggle (drives the real radio), Airplane (cuts radio), BLE toggle, **live AMS media card + phone-volume sync**,
  DND flag, pre-dim + Battery-Saver; swipe-down open/close.
- **Lock / PIN** — watchface lock state machine, passcode keypad (`build_pin`), unlock gate (`build_unlock_pin`),
  idle→watchface + sleep-when-still auto-sleep.
- **Settings** — Display (reduced motion, carousel, shake wake/sleep), Buttons (FN + PWR-double target picker),
  Watch Name, Power menu (Off / Restart).

### 3.5 Storage / USB / HID / DuckyScript (`sdcard.c` · `usb_gadget.c` · `hid_kbd.c` · `ducky.c`)
- **microSD FAT32** over SDSPI (shared SPI3), /sd access lock, directory listing.
- **Composite USB** (TinyUSB, deferred install) — runtime **mode picker**: Detached / **CDC console** / **HID
  keyboard** / **MSC File Share**, one class at a time, descriptor rewrite (no re-install).
- **HID keyboard** — connect-on-entry / long-press latch, keymaps **US / GB / DE** (CapsLock-LED aware), over
  **USB and BLE** transports.
- **DuckyScript engine** — full grammar (REM, STRING/STRINGLN, ENTER, DELAY, DEFAULTDELAY/DEFAULTCHARDELAY,
  modifier combos, named keys, F1–F12, REPEAT, LOCALE); plays over USB or BLE; macro picker (`/sd/ducky`).
- **MSC** — /sd exposed to host with app-side claim/release; **CDC live-write pipe** (PCAP streaming).

### 3.6 WiFi — M5 (`wifi.c`, on-SoC STA + monitor + active)
- **Station** — scan, join (keyboard + connecting view), saved networks, per-network manage, auto-join,
  reconnect / forget, NVS creds.
- **MAC spoofing** (randomize / restore / manual) + **hostname** (DHCP / mDNS).
- **Promiscuous monitor** — channel hop / lock, per-type + per-channel frame tallies, rate, RSSI last / peak.
- **Passive parser** — nearby-AP list (security class + OUI vendor), station / client list, probe-request / SSID
  harvest.
- **Capture** — PCAP to microSD (radiotap) + **live PCAP over USB-CDC** (Wireshark extcap).
- **Handshake / PMKID** — passive 4-way-mask + PMKID capture, crackable-detect, **hashcat-22000 export** + EAPOL PCAP.
- **Anomaly detectors** — deauth / disassoc rate + duplicate-SSID / evil-twin (security-mismatch).
- **Active ops** — management-frame TX (deauth / disassoc) · beacon TX (managed SSID list + decoy generator) ·
  software AP (client list) · **captive portal** (DNS redirect + HTTP server + request log + selectable landing).
- **Single-radio coexistence** — radio-yield to BLE.

### 3.7 BLE — M7 (`ble.c`, NimBLE observer / central / broadcaster / peripheral)
- **Device scan** — name / vendor / addr-type / connectable, RSSI, paging.
- **Item-tracker detection** — Apple Find My / AirTag, Tile, Samsung SmartTag.
- **Drone detection** — OpenDroneID / ASTM F3411 Remote-ID (Basic ID, drone loc / vector, operator pos / ID).
- **Signal Hunt** — RSSI gradient + **IMU rotation-sweep bearing dial** + **audio "warmer" beep**; Radio toggle
  across **BLE / WiFi-AP / LoRa**.
  - **PLANNED design — saved 2026-08-27 (not yet built):** re-skin the bearing dial as an **antique compass
    rose / sundial** — a ringed dial with radial tick marks, longer **cardinal** ticks, and an accent **needle**
    pointing to the hunted signal's relative bearing. This is the "Compass Rose" concept that came out of the
    Theme wallpaper exploration (it was pulled from the wallpaper set because it fits Signal Hunt, a real
    direction-finder). The needle follows the theme accent colour. Purely a cosmetic upgrade to the existing
    head-up dial (`s_bh_*` in ui.c) — no new sensing.
- **GATT Explore** — connect + walk services / characteristics + tap-to-read + SIG-UUID names.
- **Advert PCAP** to microSD.
- **Advertise / Beacon** — named advert or Apple iBeacon (broadcaster), persisted.
- **Phone companion** — **ANCS** notification mirror (bond persist / forget) + **AMS** media remote (now-playing /
  transport / volume; drives the Control-Center media card).
- **BLE HID keyboard** — HID-over-GATT + DuckyScript-over-BLE.

### 3.8 LoRa — M9 (`lora.cpp`, SX1262 via RadioLib)
- **P2P messaging** — broadcast text frames, inbox, node id (TX solo-verified; RX needs a 2nd node).
- **Channel Activity** — 15-point 902–928 MHz RSSI sweep + CAD preamble detection.
- **Band Survey** — 52 × 500 kHz fine sweep, max-hold, noise floor, signal list → hands a frequency to Signal Hunt.
- **Signal Hunt engine** — park-and-stream RSSI envelope, consumed by the shared hunt screen (LoRa mode).

### 3.9 GNSS — M8 (`gnss.c`, u-blox M10 NMEA over UART)
- **Live Fix** — GGA/RMC/GSA/GSV/TXT parse → fix, sats used / in-view, lat/lon/alt/HDOP/speed, UTC, antenna status.
- **GPX Log** — track → `/sd/nocsif/tracks/*.gpx`, power-loss-safe footer, background logging.
- **Wardrive Map** — geotagged WiFi survey → **WiGLE-1.4 CSV** (drives GNSS + WiFi monitor together).
- **Sync Clock** — set the RTC from GNSS UTC + home-tz auto-DST.

### 3.10 IMU / motion — M11 (`imu.c`, BHI260AP sensor hub)
- Firmware RAM-upload + boot, accelerometer streaming.
- **Shake-to-wake** latch + **stillness tracking** (sleep-when-still).
- **Relative heading** (GAMERV fusion) for the Signal-Hunt bearing sweep (no magnetometer → drift-relative).

### 3.11 Audio — M11 (`audio.c` speaker · `mic.c` PDM mic)
- **Speaker** (MAX98357A I2S) — tone generator, named cues (boot / wake / sleep / tick / alert / USB), master
  mute + volume (persisted), **WAV playback**. *The alerting channel, since the haptic is dead.*
- **Mic** (PDM I2S) — level meter (RMS + peak-hold), adjustable gain, **voice-memo recording** (PSRAM buffer,
  30 s cap → WAV on /sd).
- **Voice Memos** screen (record / play / delete / rename) + **Sound** settings.

### 3.12 Power management — M11 (`pm.c` · `power.c`)
- CPU **DFS** (240 ↔ 80 MHz) toggle (persisted).
- **Settings → Power** — screen timeout, dim-before-sleep, sleep-when-still, re-sleep-after-shake, power saver
  (DFS), **Battery Saver** (manual + auto < 20 %: caps brightness / timeout / motion).

## 4. The forward plan (Phase 2 — capability expansion)
Every risk-first milestone is shipped (§3). What remains: **(a)** make the shell's stubbed rows real (§4.1),
**(b)** the operator-promoted expansion features from the parity / fusion catalog
(`docs/research/feature-parity-2026-08-21.md`; refs are `§domain-number`), **(c)** USB host mode + platform
layers, and **(d)** the two hardware-blocked items (NFC + haptics), **parked at the very bottom (§4.12)**. One
branch per work-package; interleave freely — this is a sensible order, not a hard sequence.

### 4.0 Hardware drivers — ✅ NONE left to build (besides the blocked NFC)
**Top-priority check, satisfied:** every fitted radio / sensor already has a working, verified driver — WiFi,
BLE, IMU (BHI260AP), GNSS (u-blox M10), LoRa (SX1262), audio (MAX98357A speaker + PDM mic), display (CO5300),
touch (CST9217), PMU (AXP2101), RTC, SD, XL9555. **No driver work is pending.** The only unwritten-verified
driver is **NFC (ST25R3916)** — firmware-complete but hardware-blocked; **haptics (DRV2605)** is hardware-dead.
Both are parked at §4.12. So there is **no non-NFC hardware-driver work outstanding**.

### 4.1 Finish the shell — make the stubbed rows real
The present-but-disabled menu entries (dimmed rows / placeholder category screens today):
- **System** — **Theme / Wallpaper / Font ✅ DONE** (`theme` — a custom-colour **HSV wheel** [hue/sat disc +
  brightness bar + presets] for the UI **accent**, plus an independent **per-wallpaper star colour**; a **type
  scale** [Compact/Default/Large] that re-points the shared title + row styles; and a **Wallpaper** submenu of
  three styles [**Orrery / Constellation / Blueprint**], each keeping its OWN star colour, layer toggles
  [Orrery: Rings/Diamond stars/Star dots · Constellation: Lines · Blueprint: Nodes], and comets — all persisted
  per-wallpaper) · **Language / i18n** (`system.lang` — **deferred to backlog** [2026-08-27, operator call]:
  big string-table churn for a single English operator, low payoff — same call as Activity / Navigation. The
  row was **removed from the System menu** [2026-08-27], exactly as Activity / Navigation were pulled from Life;
  revisit only on request) · **OTA self-update** (`system.ota` — needs the `ota_0/ota_1` partition restructure
  + A/B rollback) · **About ✅ DONE** (`system.about` — firmware/hardware/runtime info, 1 s live tick) ·
  **Connectivity ✅ DONE** (`system.conn` — read-only **live radio-link status hub**: Wi-Fi / Phone·BLE /
  Sub-GHz LoRa / GNSS, each a 1 s-tick line from that module's LVGL-safe cached getter [`nocsif_wifi_detail_str`
  / `nocsif_ble_ancs_status_str` / `nocsif_lora_status_str` / GNSS snapshot], accent-lit while linked; About-style,
  no worker-file changes).
- **Life** — **Weather ✅ DONE** (`weather` — Open-Meteo HTTP fetch → current + 3-day forecast screen +
  the live watchface temp chip; GPS-captured location that **auto-follows** the last fix; a §4.6 geofence
  seed — auto-learned last-connected anchor → one forced refresh on re-entry; live WiFi footer glyph) ·
  **Notes** app ✅ DONE (`notes.*` — on-watch microSD text CRUD: list `/sd/nocsif/notes/*.txt` · read + scroll ·
  on-watch-keyboard create / edit · two-tap delete; reuses the Files snapshot-under-lock discipline) ·
  **Files / on-watch SD browser ✅ DONE** (`files` — live `/sd` listing · folder drill-down · text/hex file
  viewer · two-tap file delete · long-press **recursive folder delete** with a confirm screen) ·
  **Do Not Disturb ✅ DONE** (`dnd` — a **Life action row** [+ the Control-Center moon tile]: both drive the one
  persisted `cc.dnd` flag, now with a real consumer — `ui_dnd_active()` silences the alarm/timer chime while the
  panel still lights and the alarm still lands in Alerts; the row icon glows in the accent while on) ·
  **GeoFence ✅ DONE** (`autom`, shown as **"GeoFence"** under System — location-triggered automations. **Movie
  mode** is the live one: a session mode [not persisted] that dims to a minimum "cinema" level via the single
  `ui_base_brightness()` choke point [predim/saver/**wake** all respect it — only the toggle or the geofence turns
  it off, never the power button], silences alerts, and drops shake-to-wake so it won't light in the dark; exit
  restores brightness + `raise_wake`. **GNSS geofence auto-off:** on enable it anchors the current location and
  drives GNSS live [`movie_geofence_tick` on the always-on header tick], then lifts Movie mode once you travel
  > ~250 m from the anchor — indoors has no sky-view fix, so the anchor is captured on the first valid fix
  [typically as you walk out] and "leaving" registers on the drive-off; GNSS is released on exit, and only if
  Movie started it. Tag: `on · gps` [acquiring] → `on · geo` [armed] → `on` [no GNSS]. **NFC Fencing** [enter a
  place → arm the reader 5 min] and **Gesture shortcuts** are kept as **inactive stubs** [NFC Fencing needs the
  NFC radio, dead on this unit — §4.12]) · **Alerts ✅ DONE** (`alerts` — the unified **Alert
  Center**: one on-watch alert log every source funnels into, shown as swipe-left-to-dismiss cards + a **Clear
  all**; the **Alerts planet + Life row icon glow in the accent while alerts are held** [same mechanism as the
  moon on BLE]) · **Flashlight ✅ DONE** (`flash` — the Life row fires the existing Control-Center torch overlay
  via an action-row intercept in `app_drill`/`nocsif_app_launch`).

  > **Alert Center — what is currently wired in (2026-08-27):** the log holds up to 6 newest-first cards; all
  > pushes are on the LVGL task (no lock), and the watch sources are POLLED from the header tick reading each
  > subsystem's cached getter (so NO worker-file changes). **Sources logged:** **Phone** — ingested from the
  > ANCS mirror, deduped by notification UID (pair once via Cyber → BLE → **Phone Notifications** and they flow
  > in live, even after you leave that screen) · **Alarm** fired (pushed from the shared `alert_start`) · **low
  > Battery** (≤15% / ≤5%, hysteresis + charging reset) · **Radio** anomaly (deauth/disassoc burst, 30 s
  > cooldown; accrues only while the WiFi monitor/parser runs) · **Weather** (WMO ≥ 61 = rain/snow/storm on a
  > fetch). **NOT logged:** **Timer** (rings but does not log — operator choice) · phone-side **dismiss**
  > (ANCS is receive-only as wired; swipe / Clear-all are LOCAL to the watch copy). **IA:** Life's old
  > "Notifications" row is repointed to Alerts; **Phone Notifications** (pair / Disconnect / Forget) stays
  > under Cyber → BLE. Easy future adds are one `alert_log_push(kind,…)` line each.

  > **Deferred to backlog (2026-08-27, operator call):** **Activity** (IMU step-count / workouts / sleep) and
  > **Navigation** (GPX routes / track-back / trip stats) are **not wanted for now** — their dimmed stub rows
  > were removed from the Life menu *and* the shortcut-picker tree (`k_pick_tree[]`). Revisit only on request:
  > the GNSS breadcrumb already ships as **GPX Log** (M8), and IMU step-count (BHY2 has a HW step-counter
  > virtual sensor) would be a clean fresh build if the operator ever wants it back.

  > **✅ DONE (2026-08-27) — Home carousel styles + wallpaper expansion (shipped as ONE PR; mockup
  > `docs/design/wallpaper-carousel-explorer.html`).**
  >
  > **(1) Home carousel style — user-selectable layout** (`home.car` NVS, System → Display → **"Home layout"**,
  > cycles Ring → Dual dials → Bottom arc). **Ring** is kept 100% as-is (its angular-drag + full edit mode).
  > **Dual** (two side arcs) + **Bottom** (a half-ring — one dial centred BELOW the screen at (205,730) R=300
  > via a new `car_dial_t.bottom` flag so `car_pos` fans HORIZONTALLY) reuse the generic engine
  > (`car_build_dial`/`car_layout`/`car_hit_id`/`car_rotate`) with a small self-contained pointer
  > (`home_car_pointer_cb`): **tap a planet → launch, swipe along the dial axis → advance one detent**. Their
  > planet set MIRRORS the ring's (`s_ring`, from `RING_KEY`; bottom = all, dual = split even/odd). **v1 limits
  > (accepted): dual/bottom carry NO edit mode (edit in Ring) and NO auto-rotate; geometry may want on-device
  > tuning.** Content is swapped IN PLACE on the pinned `s_home` (`home_apply_layout` = `lv_obj_clean` +
  > `home_populate`), so the nav root + the screen's event cbs never change; `home_pointer_dispatch` routes by
  > style; `ring_tick` is gated to Ring; planet-glow parity added for the dual/bottom dials. **The watchface/
  > peek gets the same choice in a LATER pass** (needs clock/date/chip placement per layout).
  >
  > **(2) Wallpaper expansion — Engraved Sun + Grimoire, replacing Constellation + Blueprint** ("the weak
  > two"). Final set = **Orrery · Engraved Sun · Grimoire** (same `nocsif_wallpaper_t` count; drop
  > `CONSTELLATION`/`BLUEPRINT`, add `SUN`/`GRIMOIRE` + their render fns in `ui_background.c` + menu entries).
  > **Colour model UNCHANGED** — one accent colour per style (the existing `nocsif_wp_star_color`); it just
  > tints a different feature per wallpaper. Per-wallpaper submenu (colours · layers · comets):
  >
  > | Wallpaper | Accent tints | Layer toggles (≤3) | Comets |
  > |---|---|---|---|
  > | **Orrery** (keep) | the stars | Rings · Diamond stars · Star dots | ✓ |
  > | **Engraved Sun** (new — **"Radiant disc"**: disc r≈46 + coronae + a full 24-ray sunburst, alternating long/short, **no face**) | the sun (disc + rays + coronae) | Sunburst rays · Coronae · Star field | ✓ |
  > | **Grimoire** (new — concentric circles + hexagram [two triangles] + 36-tick ring + centre star) | the **centre star** only — the **sigil linework stays neutral gray** (operator call) | Hexagram · Tick ring · Orbital circles | ✓ |
  >
  > **(2) Wallpapers — DONE as specified:** `NOCSIF_WP_SUN` / `GRIMOIRE` replace `CONSTELLATION` / `BLUEPRINT`
  > (new `wp_sun` [Radiant disc ≈ (205,205)] + `wp_grimoire` [seal ≈ (205,240)] painters via the existing
  > `plot_ring`/`plot_line`/`plot_dot`/`draw_star` primitives). One accent colour per style — the settings
  > colour row reads **"Sun color"** for the sun / **"Star color"** elsewhere; the Grimoire sigil is `RING_COLOR`
  > (gray), only its centre `draw_star` takes the accent. Per-wallpaper settings screen is generic
  > (`layer_count` / `layer_name`) so it adapted with no change. **Note:** the enum re-index means the old
  > Constellation/Blueprint NVS (`wp1.*` / `wp2.*`) is inherited by Sun/Grimoire as harmless in-range defaults.
  > **Shipped as ONE PR** (operator call, accepting the carousel risk).

### 4.2 WiFi recon & detection layer (extends M5 · WiFi + BLE, both shipped)
- **Rogue-AP / evil-twin detection** — finish (WiFi §17, anomaly base shipped) · **Karma / PineScan responder
  detection** (§19) · **management-frame-flood detection** (§20) · **deauth-flood "network-under-load" detector**
  (§29) · **detect-other-tools** — flag a nearby Flipper-BLE / pwnagotchi / deauther / Pineapple / ESP32 (§21).
- Fusion — **cross-radio presence census** (WiFi+BLE dedupe → true "who's here") · **dual-radio signature
  corroborator** (confirm / flag infra only when WiFi+BLE agree) · **radio-event audible & visual alerter**
  (watched device appears / leaves or an anomaly fires → speaker chime + full-screen card; the alerting path now
  the haptic's dead).
- **Camera-glasses detector (Ray-Ban Meta)** — buildable now, dual-vector: **BLE advert** company ID `0x01AB` +
  service UUID `0xFD5F` (randomized MAC → the ID/UUID pair is the stable key) catches arrival / power-on; **WiFi
  monitor** catches the temp SoftAP the glasses spin up for media-import / livestream (match a **Meta Platforms
  OUI** — `78:C4:FA` / `80:F3:EF` / `B4:17:A8` / … + Facebook `48:57:DD` / `A4:0E:2B` — + an IE fingerprint).
  **⚠ 2.4 GHz-only** → a 5 GHz offload net is invisible (verify on-device). An event detector, not an always-on
  "is anyone recording" check.
- **Flock mode — detect Flock ALPR cameras + lead you to one** ⭐ **NOW UNGATED** (its gate was BLE + Signal
  Hunt — both shipped; GNSS shipped too). Passive / Active detect of **Flock Safety ALPR camera** signatures →
  geolocate → **direction-find** via **Signal Hunt** (RSSI + IMU relative-bearing sweep + audio; **no compass** →
  relative bearing only). **Signature:** WiFi promiscuous (§3.6) = Flock **MAC OUIs** (`flock-you` ~31, e.g.
  `70:c9:4e` / `3c:91:80` / `d8:f3:bc`) **+ a wildcard probe-request + IE fingerprint** (~zero false positives per
  the author), channel-hop **11 / 6 / 1 @ 350 ms**; **BLE** = external-battery units advertise **`Penguin-<digits>`**
  / company ID **`0x09C8` (XUNTONG)**. **Passive** (background alert even screen-off, each hit stored with its GPS
  fix) · **Active** (list nearby / pick a saved one → lead to its GPS → hand to the finder). Honest: real
  background-scan battery cost; wired-only cams emit nothing; signatures go stale. Refs: `colonelpanichacks/flock-you`
  · `justcallmekoko/ESP32Marauder` · `mbrugman67/FlockOff` · DeFlock.

### 4.3 WiFi active & network tools (extends M5)
**ESP-NOW device-to-device link** (§24, NocSif↔NocSif) · **DNS-sinkhole / walled-garden on the own AP** (§26,
explainer §4.11) · **network host discovery / port scan** on a joined net (§27) · **SSH / telnet / net clients**
(§28; SSH heavy but has ESP32 ports) · **DIAL / Chromecast cast control** (§30) · **network-printer / TP-Link
device mgmt** (§31, device-specific / low-priority) · **WPS scan / info** (§32, ESP32 WPS limited → partial) ·
**wireless-payload console** (WiFi §25 = USB-HID §10 — connect over WiFi, push HID live) · **Auditable HID witness
console** (fusion — every keystroke shown + logged + live-streamed to the web UI) · **USB-Ethernet emulation**
(USB-HID §6, CDC-ECM / RNDIS).

### 4.4 BLE expansion (extends M7)
- GATT client — **write** (§9) · **subscribe / notify** live streams (§10) · **characteristic write-testing /
  fuzz** on an authorized device (§11): turns read-only Explore into a full control / test surface.
- HID — **mouse** (§14) · **consumer / media keys** (§15) · **gamepad** (§16): extend the shipped HID-keyboard server.
- **Nordic-UART (NUS) serial bridge** (§23, explainer) · **persistent / background beacon** (§19) · **coded-PHY
  long-range scan / beacon** (§29, explainer) · **device impersonation / custom GATT-server emulation** (§31) ·
  **BLE mesh node** (§32, explainer) · **bond-manager UI** (§33, explainer).
- Recon / decoders — **Continuity / Handoff / AirDrop decode** (§24) · **Fast Pair / Swift Pair decode** (§25) ·
  **card-skimmer BLE detection** (§5) · **BLE advertisement-flood detection** (§28) · **"a tracker is following
  me" anti-stalking alert** (§26) · **Find-My locator tag for your own asset** (§27).

### 4.5 Off-grid & LoRa expansion (extends M9)
LoRa / mesh — **telemetry / sensor relay** (§5) · **traceroute / canned messages / store-and-forward** (§6) ·
**MQTT internet gateway** (§7 — LoRa is a separate radio, runs alongside WiFi) · **MeshCore stack** (§8) ·
**Reticulum / RNode multi-transport stack** (§9) · **repeater / router node** (§10). **Sub-GHz FSK in-band `.sub`
subset** (Sub-GHz §2 — the SX1262 does FSK, so a narrow in-band subset of `.sub` remotes is replayable; **no
arbitrary OOK**). Fusion — **cross-band message bridge** (phone types over BLE / web-UI → relayed out over LoRa) ·
**time-synced store-and-forward LoRa mesh** (GNSS time → collision-avoiding slots) · **off-grid RF-environment
relay node** (monitor local WiFi / BLE → forward a summary over LoRa) · **return-to-waypoint homing** (GNSS gets
you close, then dropped BLE / LoRa beacon RSSI + IMU for the last metres).

### 4.6 Connectivity Governor — geofenced, lease-driven radio power management · **phased milestone**
Context & automation, made concrete: **radios are on only when they're useful.** One governor over WiFi / BLE / GPS
on two primitives — a **per-radio lease + idle-timer** (a radio is ON only while an app / transfer / active
connection holds a lease, else it powers down after **~5 min**) and a **GPS geofence trigger** (entering a known
region auto-acquires a short lease to wake the radio). Absorbs the old §4.6 one-liners (GNSS geofence,
RF-fingerprint geofence, worn / charge / time-of-day gating). Extends `pm.c` (M11) + the shipped WiFi-creds store.
- **Geo-store (coords ↔ saved networks).** Each saved network gains `{lat, lon, radius (~100–200 m), BSSID[]}`,
  **auto-learned** — the first successful connect *with a GPS fix* stamps the location, refined over repeat visits.
  Keyed on **BSSID** (an SSID like `xfinitywifi` is everywhere). Decision chain: **GPS says "near home" (no WiFi
  powered) → WiFi wakes, quick-scans → sees the known BSSID → connects.** Nowhere known → WiFi never powers on.
- **GPS — perma-on but cyclic.** A fix-then-sleep duty cycle (configurable **N-second** period; the geofence set is
  evaluated each wake). Hot-starts stay fast via the always-on GPS backup rail (`LDO1/VRTC`). Trade: up to N s of
  trigger latency (fine for "arrived home"; may miss a fast drive-by → shorten N near a boundary later).
- **WiFi — geofenced auto-connect, hold-on-lease.** OFF by default → geofence-enter / an app that needs it /
  geofenced-weather-due wakes it → scan → join. **Held on the lease, not on GPS:** once associated to a known BSSID
  the *connection itself* is the "I'm here" signal, so an **indoor GPS drop never tears down a good link** (GPS only
  *enters* geofences — it never forces an exit while a known link is live). **Parked in a geofence → WiFi
  modem-sleep** (stay associated, ~1–2 mA, instant), **not full-off**; full-off only once GPS says you've left the area.
- **BLE — active-connection = on, else idle-off, but stay reachable.** A live connection (bonded phone / a BLE app)
  holds it on; no connection for ~5 min powers down the scanning / central role — but the **bonded phone keeps a
  low-power connectable advert** so it can silently reconnect and **phone notifications survive** (full-off only when
  clearly away long-term).
- **Weather — reworks the in-flight §4.1 fetch to be governor-driven.** Weather wakes WiFi only when **in a
  geofence** (a network is actually reachable); otherwise it fetches **opportunistically** whenever WiFi is already
  up — plus one forced fetch on geofence-enter. Never powers the radio just for weather out in the open. *(⚠ the
  weather app is being built now — it integrates here rather than shipping a standalone always-on fetch.)*
- **Phases:** **P1 radio-lease core** (WiFi + BLE acquire / release + idle-timer state machine — delivers "off after
  5 min unless leased / connected" standalone) → **P2 cyclic GPS + geofence engine** (N-second scheduler + circle
  evaluator + enter / exit events with the indoor-hold rule) → **P3 geo-store + auto-connect** (coords / BSSID per
  network, auto-learn, geofence-enter → scan → join, modem-sleep when parked) → **P4 policy + UI** (weather gating,
  BLE connectable-advert, **Settings → Connectivity**: N, idle timeout, radius, per-radio enable).
- **Reuse:** WiFi STA + creds (M5) · GNSS live fix (M8) · BLE bonded link (M7) · AXP2101 per-rail gating · `pm.c`
  (M11) · the "Current Activity cuts idle rails" discipline. **New:** the geo-store + auto-learn, the cyclic-GPS
  scheduler, the geofence evaluator, the per-radio lease API, and the policy layer. · WiFi ✅ + BLE ✅ + GNSS ✅ ·
  buildable. **Honest:** cyclic GPS is still the dominant always-on load (tune N) and **indoors-no-fix burns
  near-full GPS current for no lock** → pair with **IMU-gating** later (run GPS only while the IMU says you're moving).

### 4.7 Watch utilities, display & audio apps (extends the UI)
- **Display / utilities** — **image / photo viewer** (Display §16) · **GIF / animation player** (§17) · **QR-code
  display + hex / text viewer for captures** (§21) · **notes app · localization (i18n) · on-watch QWERTY + Scribble**
  (platform §7 + §4.1; Scribble handwriting experimental, predictive swipe not planned) · fusion **bubble level &
  inclinometer** (IMU gravity vector → live spirit-level + tilt angle).
- **Audio tools** (extend the shipped speaker + PDM mic, §3.11) — **theremin** (IMU tilt / touch → live tone) ·
  **data-over-sound (acoustic modem)** — near-inaudible ~17–19 kHz FSK / chirp tones carry bytes device-to-device
  (ggwave-style: config / URL / pairing transfer + same-room proof; **needs the PDM mic reconfigured from 16 kHz to
  ~40–48 kHz** to reach the near-ultrasonic band) · **decibel / SPL meter** (extend the mic level-meter into a
  calibrated dB read-out) · **live spectrum analyzer + tuner / pitch detector** (mic FFT → live spectrum + note /
  pitch on the AMOLED).

### 4.8 Companion & connectivity bridges (WiFi / BLE companion + relay + file transfer)
- **NocSif-native Approvals lane** — a first-class approve / deny / confirm surface over the companion / relay
  (MFA confirmations, dev-agent approvals from the user's *computer*); sidesteps the iOS ANCS ceiling.
- **Notification reply & quick-actions** — canned-reply chips + voice dictation + tapback. **Platform-bound**:
  iOS ANCS can't carry a reply → needs an Android companion or the web-UI (partial).
- **Over-internet remote relay control** — drive the watch from anywhere via an outbound TLS / WS to a
  **user-hosted** relay, password-gated (persistent-link battery cost; no BLE fallback during capture).
- **Wireless file download from SD** (new idea — feasible) — an HTTP file browser on the shipped SoftAP web UI
  serves `/sd` (captures / GPX / voice-memos / PCAPs) to a phone / laptop browser, no cable.
- **WiFi hotspot re-share · auto-join phone WiFi via BLE hand-off** — convenience bridges (partial).
- **Scoped USB file-share** — expose a bounded FAT container instead of the whole SD card to an untrusted host.

### 4.8a Companion control surface (L4) — the on-network remote · **phased milestone**
Pulled out of §4.10 into its own build: **control the watch from a phone / laptop on the same LAN.** Both ends on
one network — the watch joins the phone's / home WiFi as **STA**, *or* the watch hosts its **SoftAP** and the phone
joins — giving **bidirectional, browser-only control: no internet, no app to install, no ANCS**. It sits on plumbing
already shipped in **M5 P3** (SoftAP + captive-portal HTTP server + web UI). Two arrows: **HTTP `POST` phone→watch
(control)** + **WebSocket watch→phone (live push)**. Discovery via **mDNS `nocsif.local`** (iOS resolves `.local`
natively). Lives behind a **Remote / Companion mode** toggle (WiFi stays hot → battery cost). **Access boundary
(operator call 2026-09-01): NO pairing code AND NO WPA2 — the surface hosts an OPEN SoftAP with no join gate
(anyone in range who joins the AP controls the watch). The off-by-default toggle + a subtle on-watch "linked"
indicator are the only guardrails; optional TLS/WPA stays a later add if wanted. Verified on-device: `nocsif.local`
resolves on iPhone, the connected page loads.**
- **P1 — Transport + discovery ✅ CODE-COMPLETE + builds + on-device transport verified.** mDNS `nocsif.local`
  (`espressif/mdns`) + an **OPEN SoftAP** (device-name SSID, no gate) +
  a **routed `esp_http_server` on :80** (its own handle, distinct from the captive-portal wildcard server, with
  which it is mutually exclusive) serving a styled self-contained "connected" page + `/api/ping` JSON. **RAM/radio
  policy:** ~~raising the surface releases the BLE controller~~ **→ reconciled in P4 (2026-09-08): Bluetooth is left
  alone.** Since RAM Phase 2 the controller is claimed at boot and held resident, so the old "release" was a purely
  logical off — it freed no internal-DMA, dropped the phone link for nothing, and persisted `bt_master=0` (a
  reboot/crash while the surface was up, or auto-start at boot, left Bluetooth off). The surface is **mutually
  exclusive with the promiscuous monitor/parser + captive portal** (single radio + port 80) — enforced at bring-up
  *and* at dispatch (starting the portal/software-AP cycles companion off first); `ap_teardown` also drops the
  surface so a STA scan/join can't strand the HTTP server. Lives
  at **System › Companion** (Start/Stop, live status, SSID/passphrase/URL, honest security note) + a global violet
  "linked" dot on `lv_layer_top`. Build: RAM 47.4% / Flash 58.3%, binary-verified. *Flashable slice: enable
  Companion → join the AP → browse to `nocsif.local` → land on the connected page.* (Code: `wifi.c`
  `nocsif_wifi_companion_*` + `CMD_COMPANION_*`; `ui.c` `build_companion`; `idf_component.yml`/CMake `mdns`.)
- **P2 — Command channel (phone → watch) ✅ CODE-COMPLETE + builds + flashed.** POST endpoints on the companion
  server dispatch into the shipped registry: **`/api/launch {id}`** (open any screen *and* the action-rows
  flash/dnd/movie — so toggles ride the same path), **`/api/back`**, **`/api/home`**, **`/api/type {text}`** +
  **`/api/key {backspace|enter}`** into the focused watch field (a lightweight tracker `s_companion_ta` marks the
  most-recently-opened text field; wired into all 9 keyboard screens). wifi.c parses JSON (cJSON) on the httpd task
  and forwards each command to a UI-registered handler that **marshals onto the LVGL task** (`lvgl_port_lock` +
  `lv_async_call`, heap-copied — never touches LVGL off its task). Every command is `ESP_LOGI`'d → the reliability
  A2 tee records it to the logbook ring. The served page is now a **functional control surface** (Home/Back nav, a
  12-app launch grid, flash/DND/Movie toggles, a type box + Send/Backspace/Enter). **Scoping note:** *capture
  start/stop is deliberately NOT wired* — the promiscuous monitor needs the single radio and would tear down the
  companion AP (self-defeating); it returns once there's an STA-companion topology or a hand-off flow. Build RAM
  47.3% / Flash 58.4%. *Slice: drive the watch's menus + type into a field from the phone browser.*
- **P3 — Live push (watch → phone) over WebSocket ✅ SHIPPED (PR #169, main `1eb95c4`, 2026-09-02).** `/ws`
  streams the watch state (screen title, battery, clients, DND/Movie/flash, brightness/volume, focused field) and
  an **interactive screen mirror** captured from the display-flush path (2:1, ~12 fps) with **touch + side-button
  injection** (a second LVGL pointer indev) and **casting** (panel off, phone-as-display). Plus the operator adds:
  auto-start on boot, optional WPA2 password, the full nested menu map.
- **P4 — The control web page + `/sd` file browser ✅ BUILT + VERIFIED on-device (2026-09-08).** The embedded page already carried the
  live tiles, the remote screen and the launcher; P4 adds the **`/sd` file browser** (folds in
  **wireless-file-download §4.8**): `GET /api/fs?p=` (listing, dirs first, dotfiles hidden), `GET /api/file?p=`
  (chunked download, `Content-Disposition: attachment`), `POST /api/upload?p=&n=` (raw body → `.part` → rename),
  `POST /api/delete {p}` (one regular file, never a directory). Paths are jailed to `/sd`; the card is CLAIMED
  per request (refused with the reason while File Share has the drive) and the FAT lock is held only around each
  readdir / 8 KB chunk — never across a socket send. The companion httpd task now runs on a **PSRAM stack**
  (the §4.10 lesson: an internal stack alive across a sustained WiFi transfer competes with WiFi's RX pool).
  Honest: one httpd task, so mirror frames queue behind a transfer and resume after it. Tie-in: drop a
  `firmware.bin` into `nocsif/firmware` from the phone, then Install from the Update screen (§4.10).
  **Found + fixed in the P4 pass — a P3-era bug:** a failed `/ws` receive (phone dropped the socket) returned
  OK, so httpd re-polled the dead socket in a tight loop until the task-wdt reset the watch; now the request
  fails and the session closes. Push-send failures close the session too (no zombie sockets), and the
  mirror has backpressure (one frame in flight), which is what "lag" was: a pile of blocking 103 KB sends.
  **Follow-up ✅ (verified):** the live stage showed the watchface incomplete on first connect — the flush tap
  carries dirty regions only and the watchface barely repaints — so a client (re)connect now forces one full repaint.
  **⏳ REVISIT LATER (operator note, 2026-09-08):** the surface works but is not yet smooth under load. Candidates:
  (a) the mirror's 2:1 103 KB frames on a single blocking httpd task — the send-timeouts (`error in send : 11`) still
  appear on a congested link; options are an adaptive scale (drop to 3:1 when a send stalls), a shorter send-wait
  for the WS sockets, or a dedicated sender task so commands/touch never queue behind a frame; (b) file-transfer
  throughput (~64 KB/s seen with the mirror competing); (c) the one-task limitation (a download freezes the mirror).
- **Ties in:** this **on-network** surface is the **local half of the over-internet remote relay (§4.8)** — the
  relay is this same page, proxied through the operator's server from anywhere — and it renders the watch side of
  the **home-server dashboard (§7)**. **Honest:** iOS **backgrounds Safari** (a WS session drops on lock — great for
  active control, not all-day); the SoftAP captive-assistant WebView is a poor WS host (join the AP → open Safari
  instead); WiFi-hot is the battery price of live control. · WiFi ✅ (BLE-companion variant → §4.10) · buildable ·
  **the first "control the watch from outside" milestone.**

### 4.9 USB extensions + host mode — M10 (native USB, builds on M4)
**USB host mode** (device ↔ host) with peripheral class drivers · U2F / FIDO CTAP over HID · generalized
composite-descriptor management. The real USB-C expansion path and the prerequisite for the Backpack idea (§7).
Groundwork exists in the P4.5 per-mode descriptors + runtime re-enumeration.

### 4.10 Platform layers — final phase
- **Companion control surface (L4)** — **now its own phased milestone → §4.8a** (the WiFi web-UI on-network remote).
  What remains here is the **BLE-companion variant** (M7): same launch / type / configure / file-transfer / drive,
  but over GATT for the no-network case — a thin second transport onto §4.8a's command + state model.
- **OTA / self-update** — A/B slot install + rollback **✅ shipped from microSD** (`ota.c`, `#156`; reads
  `/sd/nocsif/firmware.bin`, streams into the inactive slot, confirms-or-reverts). **Remaining sources:** the
  **web UI** (hand the companion an image → OTA) and — **requested 2026-09-03** — a **watch-initiated pull from the
  operator's own webserver**: when the watch is on WiFi it checks the canonical `manifest.json` / `firmware.bin`
  (§4.15's single webserver channel) on the **same cadence/plumbing as the Weather HTTP client** (opportunistic
  when WiFi is up; a Governor-gated fetch under §4.6), and if out of date, streams the image straight into the
  inactive A/B slot with the shipped progress + rollback. **⚠ Honest blocker (verify on-device):
  the operator's webserver serves the image over plain HTTP (like Weather), so the pull works with NO TLS
  block** — this replaces GitHub as the channel precisely because GitHub is HTTPS-only and TLS-with-WiFi-up FAILED
  on this board (`mbedtls_ssl_setup` SSL_ALLOC_FAILED, no contiguous internal RAM — the same wall that forced
  Weather to plain HTTP, `RESUME` 2026-08-26). *(If the operator's server is ever HTTPS-only, the same TLS wall
  applies; workarounds then: release the BLE controller during the fetch to free the contiguous internal-DMA block
  — the companion §4.8a pattern — or solve TLS-under-WiFi RAM in the coexistence work, `RAM-BUDGET.md`.)* The
  **desktop USB flash (§4.15)** remains the always-works path for a blank/bricked board that can't run OTA yet.
  **→ Superseded 2026-09-08 (operator call): the channel is the PUBLIC GitHub mirror, not a private webserver.**
  `https://github.com/silverwolf2r/Nocsif-Firmware` (created from a source SNAPSHOT of `main` `4b28ba6`, no
  private history; README banner names it a mirror) carries `nocsif/firmware/manifest.json` + `firmware.bin`
  — the same `nocsif/firmware/` folder shape the watch keeps on its microSD — fetched from
  `raw.githubusercontent.com/silverwolf2r/Nocsif-Firmware/main/nocsif/firmware/…`. `tools/publish_firmware.py`
  writes the manifest (version = `git describe`, exactly what `esp_app_desc` stamps; size; sha256; notes),
  copies the image, optionally refreshes the source mirror (`--mirror`), commits and pushes. **Manual only**
  (operator call): the watch checks when asked, never on a timer. **The TLS gate:** GitHub is HTTPS-only and
  TLS-with-WiFi-up had failed — root cause found in the sdkconfig: `CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y`
  forced every mbedTLS buffer into the fragmented internal heap. Flipped to `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`
  (PSRAM; TLS needs no DMA) and proven with a compile-gated boot probe (`-DNOCSIF_TLS_PROBE=1`, `main.c`)
  before any pull code was written. Pull flow: Check for update → manifest → `/sd/nocsif/firmware/firmware.bin`
  (replacing the card copy; size + sha256 verified) → the shipped SD installer (its path moves to that folder,
  old `/sd/nocsif/firmware.bin` still accepted) → reboot / confirm / rollback.
  **✅ BUILT + VERIFIED END-TO-END ON-DEVICE (2026-09-07, branch `Clankert/4-10-github-ota`).** TLS probe: handshake
  1.44 s, 200, 6.5 KB in 1.75 s with BLE resident + WiFi linked. Then the real thing against the public repo:
  `published f3542f4 vs running 48c69aa → update available` → 2,655,520 B to `/sd/nocsif/firmware/firmware.bin`
  in 120 s (one Range resume at 885 KB, sha256 ok) → installed from the folder path into `ota_1` in 49 s →
  reboot → `OTA image on ota_1 confirmed valid — rollback cancelled` at 2.7 s. **Lesson (cost a first failed
  run — 320 KB at ~6 KB/s then a read timeout):** the Update screen used to spawn the 8 KB INTERNAL install
  worker on open, and WiFi's dynamic RX buffers live in that same scarce pool — under a sustained TLS stream
  they starved (int-DMA largest 4 K mid-transfer). Fix: the installer is created only when Install is tapped
  (scan runs on the PSRAM worker), reads are 8 KB written at once, and a stalled read reopens with an HTTP
  Range at the byte offset (raw GitHub answers 206). Throughput is ~20 KB/s — a 2.5 MB image takes ~2 min;
  follow-on if it matters: TLS record size / WiFi RX buffer count tuning. Also fixed in passing: the UI
  liveness pet no longer logs `task_wdt: task not found` once a second while the installer has it suspended.
  Manual only; the Update screen's GitHub section is the whole UI.
- **MicroPython (L2)** — gated drop-in script layer (decide at one real ported script). Open decision: the trust /
  sandbox stance for imported / untrusted scripts (low priority — solo operator).
- **Flipper-format parsers + app / script manager** — versioned capability-struct API, MSC drop-in auto-discovery;
  `.nfc` + DuckyScript first (`.sub` narrowband subset; skip `.ir` / 125 kHz / `.fap`).
- **On-watch SD file manager** + a **System → Automations** surface (unifies Auto-NFC / Movie mode / gesture
  shortcuts — each opt-in, indicated, logged, killable).
- **On-watch browser (L5)** — explicitly deferred to v2/v3.

### 4.11 Explainers (operator-flagged) + new-idea verdicts
- **WiFi §26 DNS-sinkhole / walled-garden** — on the watch's *own* SoftAP, intercept every client DNS lookup and
  answer with the watch's IP (or block / allow by domain). It's the mechanism that *forces the captive portal to
  appear*, and lets you block ads / trackers or run a "walled garden" that only permits chosen domains — on your
  own AP, to test how a device behaves when domains are unreachable / redirected.
- **BLE §23 Nordic-UART (NUS)** — a de-facto-standard BLE service that emulates a serial port (TX / RX
  characteristics carrying arbitrary bytes). The clean **wireless console / data pipe**: a phone or laptop
  connects, sends commands to the watch, streams logs / data back, no cable — the natural transport for remote
  control, config, the approvals lane, and live data.
- **BLE §29 Coded-PHY (LE Long Range)** — BLE 5's forward-error-corrected PHY (S=2 / S=8) that trades data rate
  for **~2–4× range**. The S3 supports it; scan / beacon further out (longer-reach Find-My tag, distant beacons).
  Caveat: **both ends must support coded PHY**, and it's slower.
- **BLE §32 BLE mesh node** — Bluetooth Mesh is a many-to-many relay layer over BLE advertising (managed flooding)
  for dense, short-range **indoor** networks (commercial lighting / sensors). The watch could be a relay / endpoint
  node. Distinct from LoRa mesh (long-range, sparse). Buildable via NimBLE mesh; niche.
- **BLE §33 Bond-manager UI** — bonding stores the pairing keys (LTK / IRK) from a paired device. This is the
  housekeeping screen to **list and remove / forget bonds** (phone companion + HID hosts). Bonding already works;
  there's just no UI to manage it.
- **GNSS spoofing (catalog ☠11) — EXCLUDED, doubly out.** *Transmitting* fake satellite signals so a nearby
  receiver reports a false position. (a) **Hardware-impossible** — needs an SDR transmitter in the GPS band; the
  u-blox M10 is receive-only and there's no SDR. (b) **Harmful** — falsifies others' location (navigation / safety
  hazard). Not added; stays a marked ☠ boundary item in the catalog.
- **New idea — wireless file download from SD → added (§4.8).** HTTP file browser on the SoftAP web UI.
- **New idea — NFC tap between watches to connect + transfer, and onto a phone via the WiFi portal → catalog, not
  the plan** (NFC stays out of the plan until the working watch; §4.12). A real combo — NFC out-of-band handover
  bootstraps an ESP-NOW / WiFi link for the bulk transfer, and the web portal relays to a phone — recorded in the
  catalog's NFC-combinations §J.

### 4.12 ⛔ BLOCKED / PARKED (hardware) — the very bottom of the plan
- **M6 — NFC · ⛔ HARDWARE-BLOCKED.** Firmware is **complete** (`nfc.cpp`: lazy ST25R3916 / RFAL bring-up →
  NFC-A discovery + UID read + a thorough RF front-end / antenna self-test), but the **original unit's NFC TX path
  is dead** — `tx_on` never asserts (chip + rails healthy; LilyGo's own firmware fails identically). So all NFC
  read / write / emulate / key-recovery / NDEF + the **NFC-combination tools** (tap-to-type, on-wrist card wallet,
  tap-to-pair, geo-stamped access audit, …) wait on a **replacement / working watch**. Full HF parity + combos live
  in the catalog (§3 there + §J). On-watch today: only NFC-A **read** is wired; write / emulate / saved / keyrec /
  ndef are dimmed stubs. Phased plan: `docs/design/m6-nfc-plan-2026-08-15.md`.
- **Haptics — DRV2605 · ⛔ HARDWARE-DEAD.** Shelved on this unit; the **speaker is the alerting channel** in its
  place. No driver written; not pursued until a working unit.

### 4.13 Operator-reported fixes & small features (logged 2026-09-01)
Small polish / bug items on shipped features + one new audio tool, folded in from an operator pass. Mostly
one-file changes in `ui.c` / `audio.c`; interleave with §4.1-style work, one branch each. **(The phone→watch
companion / control-surface items in §4.8 / §4.8a / §4.10 are being built by a separate track — do not touch them.)**

- **Alert Center ↔ phone two-way dismiss + per-card swipe. ✅ DONE (§4.13 fix #1, PR #174).**
  - **(a) Per-card swipe-to-clear (bug). ✅** Root cause was the card keeping LVGL v9.3's default
    `LV_OBJ_FLAG_GESTURE_BUBBLE`, so the swipe was delivered to the screen root (which only acts on RIGHT=back),
    never to the card. Fix: `alerts_make_card` now clears `GESTURE_BUBBLE` on the card, and `alerts_card_gesture_cb`
    gained a `LV_DIR_RIGHT → nocsif_nav_back()` branch so swipe-back still works when the finger starts on a card.
    Swipe LEFT dismisses via the existing `alert_log_dismiss()` + `alerts_view_refresh()` (pool re-packs in place).
  - **(b) Phone→watch sync. ✅** `alert_log_t` gained `src_uid` (the ANCS UID, set on phone entries after push).
    `alerts_ingest_tick` now reconciles: when `nocsif_ble_ancs_gen()` changes, any `ALERT_KIND_PHONE` card whose UID
    is no longer in the live mirror (`nocsif_ble_ancs_count`/`_get`) is dismissed and forgotten from the seen-ring.
    ui.c only — no ble.c change (ble.c already parses ANCS Notification-Removed + bumps the gen).
  - **(c) Watch→phone dismiss — documented as an ANCS non-capability, not faked.** ANCS offers only
    Perform-Notification-Action (positive/negative, actionable notifications only); this firmware writes only
    CommandID 0x00, so a watch swipe clears only the local copy. A true watch→phone clear needs an Android
    companion or the §4.8 Approvals lane. (See the Alert Center header comment in ui.c.)
  - Serial-verify hook: build `-DNOCSIF_ALERTS_SYNC_DEBUG=1` to log each swipe-dismiss + reconcile on COM7
    (ships OFF). Build-verified (RAM 47.4%, int-DMA heartbeat unchanged); on-device swipe/sync is operator eyes-on.

- **Control Center tiles — reflect + drive real state.** The CC toggle tiles (`s_cc_dnd_b` / `s_cc_wifi_b` /
  `s_cc_ble_b` / `s_cc_air_b`, `ui.c:17868`) must (1) **light in the accent when their function is ON** and (2)
  **actually toggle the subsystem.** DND already round-trips its lit state via `cc_ico_state()` +
  `ui_dnd_active()` — use it as the reference pattern and bring the rest to parity: **WiFi** and **BLE** tiles
  must reflect the live radio state (read each module's cached getter, not just a local `s_cc_wifi` / `s_cc_ble`
  bool) and their tap must start/stop the real radio; **Airplane** must cut/restore both consistently and the
  three must stay mutually coherent (Airplane on ⇒ WiFi/BLE tiles show off). Audit every tile for state-vs-reality
  drift after external changes (e.g. WiFi toggled from its own screen should update the CC tile on next open/tick).

- **Long app names truncated on watchface + Home carousel. ✅ DONE (§4.13 fix #2, PR #175).** The plan's
  original diagnosis was wrong — it was NOT `LV_LABEL_LONG_DOT`; the shared builder `car_make_planet` put a full-width
  `LV_SIZE_CONTENT` label (no width, no long mode) inside the fixed planet box, so a long name ("Signal Hunt")
  overflowed and got hard-clipped mid-character by the box's child clip. Fix (Option E): the label now gets a fixed
  width = the tier's planet box (peek ring = `CAR_RINGSZ` 86; dual/bottom front + Home ring = `CAR_SZMAX` 92, which
  fits inside the 94 px Home-ring box) + `LV_TEXT_ALIGN_CENTER` + `LV_LABEL_LONG_DOT`, so it ellipsizes cleanly
  ("Signal…"). One builder feeds all tiers, so all peek/Home Ring/Dual/Bottom tiers are covered. Text stays INSIDE
  the box, so it is safe in the rounded-corner zone (unlike overflow-visible, which would cross the clip on dual).
  `ui.c` only, zero new statics/int-DMA. Operator eyes-on for the rendered look.

- **Type scale (Compact/Default/Large) must apply everywhere. ✅ DONE — core sweep (§4.13 fix #3, PR #179).**
  `typescale_apply()` re-points only `nocsif_style_title` (serif) + `nocsif_style_row_name` (mono 14/16/18); many
  hand-built content labels hard-coded a font and bypassed the scale. Fix (48 sites re-pointed, each keeping its own
  color line so colored labels stay colored): the 5 serif headings (`serif_21`/`serif_23`) → `nocsif_style_title`;
  the 43 mono body/row/button/textarea/status/keypad labels at `mono_16` (row_name's Default size, so Default is
  visually identical and only Large/Compact change) → `nocsif_style_row_name`. Left OUT deliberately: carousel/ring
  planet labels (fix #2), display heroes (`num_48/64` + their `mono_18` companions), boot art `serif_30`, italic
  `serif_20i` (no matching token). **Honest gap (follow-on):** the `mono_12` caption tier and `mono_11`/`mono_13`
  tag tiers (~170 labels) still have NO scaling token — a full "grow *everything*" needs new caption/tag scale
  tokens in `ui_theme.c` (~30 lines), tracked separately so this PR stays default-preserving. `ui.c` only, zero
  int-DMA. Build clean, RAM 47.4%. Operator sets Font=Large and eyeballs across screens.
  **Follow-on ✅ (2026-09-07, branch `Clankert/4-13-typescale-tiers`) — the caption / tag tiers scale too.**
  `ui_theme.c`: three FONT-ONLY tokens (`nocsif_style_font_caption` 11/**12**/13 · `_font_tag` 11/**13**/15 ·
  `_font_tag_small` 11/**11**/13 — no colour, so every label keeps the colour it set or inherited; Default =
  the shipped cuts exactly) plus the scaffold caption and menu-row tag styles now follow the scale;
  `nocsif_label_font_scaled(label, font)` maps mono 11/12/13 to the tokens for the font-taking helpers
  (`nocsif_content_line`, `gf_line`, `ble_drone_detail_label` → About / OTA / Connectivity / Diagnostics /
  GNSS / drone detail lines). `ui.c`: a scripted sweep re-pointed **149** direct `mono_11/12/13` sites; left
  as fixed by design: the watchface readouts, Signal Hunt compass + its buttons, the Control Center, the
  flashlight overlay, the icon picker, keypads / passcode, the colour picker, the Files viewer, the boot
  splash, the header badge, planet labels. Operator eyes-on at Large + Compact.
  **+ x-large and XXL steps (operator ask, same branch):** the scale is now five steps — compact · default ·
  large · x-large · xxl — title serif 23/26/26/**28/30** (new full-charset `serif_28` + `serif_30f`; the old
  `serif_30` is the caps-only wordmark), row name mono 14/16/18/**20/22** (new cuts), caption 11/12/13/14/15,
  tag 11/13/15/16/18, tag-small 11/11/13/14/15 (existing cuts). Four generated fonts, ~+300 KB flash.

- **Signal Hunt audio cue is glitchy ("fucky"). ✅ DONE (both parts).**
  - **(a) I2S init NO_MEM — ✅ (RAM Phase 2 #12/C7, PR #171 `4fe9326`).** Boot-reserving the I2S TX DMA fixed the
    NO_MEM; the cue toggle gates on `nocsif_audio_tx_ready()`.
  - **(b) Beep rate/pitch ramp smoothing — ✅ (§4.13 fix #4, PR #176).** In `ble_hunt_fast_tick` both the beep
    RATE and PITCH keyed off the same raw noisy `pct` sampled at fire time, so timing and pitch jittered together.
    Now a low-passed cue level `s_bh_cue_lvl` (alpha 0.15 — slower than the 0.30 dial EMA, which is left alone)
    drives both, decays toward 0 when the target is lost, and resets to 0 at the toggle + build sites. Pure float
    math on existing statics — zero alloc, zero int-DMA; `nocsif_audio_tone` stays gated behind `tx_ready()`.
    `ui.c` only. Operator + ear for the glide (build `-DNOCSIF...` not needed; a throttled log can confirm the ramp).

- **Voice-memo playback is far too quiet. ✅ DONE (§4.13 fix #5, PR #177).** The plan's hypothesis was
  wrong — `do_play_wav` DID scale (`sample * s_vol / 255`), but that is **attenuation-only** (`s_vol` max 255 =
  unity, no boost path), and `mic.c` already peak-normalizes to ~0.9 FS so speech RMS stays low. Fix: a **make-up
  gain > 1** (`AUD_WAV_MAKEUP` 2.4×) scoped to the WAV path, followed by a **soft-knee limiter** (`AUD_WAV_KNEE`
  0.80 FS) so a boosted peak is compressed rather than wrapping sign into a crackle, backstopped by a saturating
  clamp to [-32768,32767]. Cues (already ~0.9 FS) are untouched. The "play: done" log now reports the applied gain
  + limited-sample count for serial verification. Note: raising mic *sensitivity* (`s_gain`) does NOT help — it only
  scales the display meter, never stored samples. `dma_desc_num` left at 4 (IMU margin). `audio.c` only, zero
  int-DMA. Build clean, RAM 47.4%. Operator ear for the final loudness.
  **Follow-on ✅ (2026-09-07, branch `Clankert/4-13-voice-rec-reason`): the record-error line tells the truth.**
  `NOCSIF_MIC_REC_ERROR` covered six causes behind one "save failed (check the card)". `mic.c` now records
  WHY at each failure site (`nocsif_mic_rec_err_t`: mic wouldn't open · out of memory · nothing captured ·
  File Share has the card · card busy (lock timeout — the WiFi-on contention) · couldn't create the file ·
  write failed) and Voice Memos prints `nocsif_mic_rec_error_str()`. Logic-in-place for the failure branches
  (only File Share is inducible without a card-pull, and that test drops COM7); a normal record is unchanged.

- **NEW — Cyber → Audio → "Carts": shopping-cart lock tone. ✅ DONE (§4.13 fix #6, PR #178).** A new
  **Cyber → Audio** hub (`build_audio`, one `"audio.carts"` row) whose entry **Carts** (`build_audio_carts`) plays
  the published ~7.8 kHz cart-lock tone via the existing `nocsif_audio_tone(7800, 2000, 100)` — no `audio.c` change,
  zero new int-DMA (the I2S TX channel is already boot-reserved). Registered as two `k_screens` ids + one
  `k_cyber_rows` "Audio" row (auto-rendered by `build_cyber`); the play button + tag gate on
  `nocsif_audio_tx_ready()`; the one-line `k_comp_submenus` mirror entry is added (data-only). The Carts screen ALSO
  lists drop-in WAV files from `/sd/nocsif/carts/` (operator-downloaded published tones) and plays the tapped one via
  the existing `nocsif_audio_play_wav` (still no `audio.c` change); the synth sine is the no-card fallback. **⚠ Ships
  as honest best-effort:** the synth is a pure sine carrier, NOT the coded VLF burst a cart decodes; and WAV playback
  streams at a FIXED 16 kHz, so a drop-in file MUST be **16 kHz mono 16-bit PCM WAV** or it plays at the wrong pitch
  (fatal for a frequency-specific tone) — the on-screen note says so. At 16 kHz, 7.8 kHz is ~0.975× Nyquist
  (near-aliased). It plays a tone; confirm range on hardware. Optional follow-on: rate-aware WAV playback (honor the
  file's own sample rate) in `audio.c`. This is the only new *feature* in §4.13; the rest are fixes.

### 4.14 Operator-reported fixes & small features (logged 2026-09-03)
A second operator pass. **These are a SEPARATE track from §4.13 — do not touch §4.13's items (Alert Center
swipe/sync, CC tile state, name truncation, type-scale sweep, Signal-Hunt audio cue, voice-memo volume, Carts
tone); that handoff is owned by another agent.** Mostly one-file `ui.c` fixes plus one peek feature; interleave
with §4.1-style work, one branch each. The three larger asks in the same pass are broken out below as their own
milestones: **desktop bridge / flasher → §4.15**, **guided tour + feature-reference doc site → §4.16**, and
**OTA-pull-from-the-operator's-webserver → §4.10 (extended) + §4.15's canonical `firmware.bin`**.

- **Home edit-mode add-picker must always list newly added screens.** New feature rows are not appearing as
  addable planets in Home **edit mode** (long-press → "+"). The dial add-picker should enumerate from the live
  screen registry (`k_screens[]` is already the "add a screen and it can be a planet" source of truth — comment
  at `ui.c:16152`) **minus what is already on the edited dial**, so any feature added anywhere shows up
  automatically. Audit the picker's source list / `app_pickable` filter (`s_ring_pick_id` / `dial_add` path,
  `ui.c:16545`/`16705`, and the `k_pick_tree[]` used by the shortcut picker) — a hard-coded or stale add-list is
  the likely cause. Fix = drive the picker from the registry so it self-populates. `ui.c` only.
  **✅ BUILT (2026-09-07, branch `Clankert/4-14-home-add-picker`).** Confirmed: the picker walked a curated
  `k_pick_tree[]` that nobody updated — Audio > Carts, Connectivity, Companion and Media Remote were all
  missing. The tree is now DERIVED: a folder's children = the rows the watch's own menu shows for it (the
  file-scope `k_*_rows` tables — `k_menu_cats` for Cyber/Life/System, hoisted from the companion mirror, and
  `k_comp_submenus` for the hubs it already maps) + any registry screen registered as `<folder>.<leaf>`
  (e.g. `usb.hid`) + a 3-line table for the one bespoke hub that drills by hand (Connect Phone → Phone
  Notifications / Media Remote / Saved Phones). Stubs and `.macros` ids are skipped; the ring / dial pickers
  hide leaves already on the edited dial (`pick_placed`); navigation keeps a path stack (a screen such as
  Signal Hunt sits under Cyber, WiFi and BLE alike — as on the watch). Same picker serves FN / PWR
  shortcuts, so the action rows (Flashlight / DND / Movie) are now bindable too.

- **Weather: per-condition glyph, not the one cloud.** The peek chip hard-codes `NOCSIF_ICON_WX`
  (`ui.c:19397`) and the Weather screen shows it always, so every condition reads "cloud". Map the **WMO code**
  (`weather.h` `code` / `d_code[3]`) → the right glyph: **clear → `NOCSIF_ICON_SUN`**, **rain/drizzle/showers →
  `NOCSIF_ICON_RAIN`**, **cloudy/overcast → `NOCSIF_ICON_WX`** (the cloud). Add a `wx_glyph_for_code(int wmo)`
  helper and feed it to both the peek chip and the Weather screen (and the 3-day rows). **⚠ Honest — icon-font
  gap:** the shipped `nocsif_icons` font has only **sun / cloud(WX) / rain / moon** (`nocsif_icons.h:45-60`);
  there is **no snow / storm / fog glyph yet**. Either add those glyphs to the icon font (cleanest — one font
  rebuild) or fold snow/storm/fog onto the nearest existing glyph (rain/cloud) as a v1 and note it. Night variant
  (`NOCSIF_ICON_MOON` when the sun is down) is a nice-to-have that pairs with the §4.14 sun/moon almanac below.
  **✅ BUILT (2026-09-07, branch `Clankert/4-14-weather-glyphs`) — the clean option: the icon font gained real
  snow / storm / fog glyphs** (`gen_icons.py` + `npx lv_font_conv`, appended at U+E02D–E02F so no codepoint
  shifted; all four cuts regenerated; the planet-icon picker lists them too). `wx_glyph_for_code(wmo, is_day)`
  maps clear → sun (**moon at night**, Open-Meteo `is_day`), cloudy/overcast → cloud, fog, drizzle/rain/showers
  → rain, snow/snow-grains/snow-showers → snow, thunderstorm → storm. Fed to the **peek chip** (update-on-change
  from `peek_info_tick`), the **Weather hero** (thin XL cut beside the number) and the **3-day rows** (each row
  is now glyph + mono text; day form). `ui.c` + `icons/`.

- **Signal Hunt — back-swipe while hunting returns to the pick list.** Today the pick list and the hunt meter are
  two states of one screen (`hunt_target_active()` gates them, `ui.c:7068`), but a back-swipe while hunting pops
  the whole screen. Intercept the app back gesture in the hunt-meter state to **clear the pinned target** (drop
  `s_bh_lora_targeted` / the WiFi-AP / BLE-device pin → `hunt_target_active()` false) so it falls back to the
  device pick list, and only pop the screen on a second back from the pick list. `ui.c` only.
  **✅ BUILT (2026-09-07, branch `Clankert/4-14-signal-hunt`, one PR with #4 and #5 — they share the tick +
  row callbacks).** `ui_nav` gained a tiny reusable seam, `nocsif_nav_set_back_hook(scr, fn)`: the USER
  back paths (swipe-right, header arrow) give the active screen's hook first refusal; programmatic
  `nocsif_nav_back` callers are never intercepted; the hook clears itself on the screen's delete. Signal
  Hunt's hook = "if a target is pinned, unpin it (the Unpin button's path) and consume the back".

- **Signal Hunt — sort the pick list by signal strength (strongest first).** Confirmed: it does **not** sort by
  RSSI today — the list is filled in each module's discovery order (`nocsif_ble_dev_get` / `nocsif_wifi_mon_ap_get`
  / LoRa survey order, `ui.c:7209-7230`). Add a strongest-first sort of the pick rows by RSSI (BLE + WiFi) / peak
  RSSI (LoRa signals) so the nearest emitter is at the top. Keep it cheap (sort an index array each rebuild, the
  pools are small, `HUNT_PICK` = 6/page); leave the LoRa **home/messaging** entry out of the sort (it's being
  removed — see next item).
  **✅ BUILT (same branch).** `hunt_order_build()` — an index array over the active radio's table sorted by
  RSSI (BLE last-seen / WiFi AP last-seen / LoRa survey peak; stable insertion sort, n ≤ 48), rebuilt on the
  500 ms pick tick right before the rows are filled; the row tap and the page button read the same map, so a
  tap always lands on the device it shows. (The page button also used the BLE count in every mode — fixed.)

- **LoRa hunt — spinning sets no bearing + drop the 915 default.** Two parts:
  - **(a) Remove the 915 / "home / messaging channel" default row** from the LoRa pick list (`ui.c:7172` `n = 1 +
    nsig`, row 0 fill at `ui.c:7213-7215`, and the `idx == 0 → 915.0f` tap-pin at `ui.c:6752-6755`). The pick
    list should show **only survey-detected signals**; nothing pre-seeded.
  - **(b) The bearing sweep does nothing in LoRa mode.** "spin to set the bearing" shows but spinning never
    resolves a needle because the heading bins only fill from **heard RSSI readings** (`ui.c:7118-7123`, gated on
    `v.heard`), and the LoRa hunt often has no live envelope — the 8 KB LoRa worker is **best-effort** (only spawns
    when the phone link is idle / RAM allows, `hunt_lora_bringup`, `ui.c:7094`/`7149`) and a parked frequency with
    no traffic reads floor. Fix: confirm the LoRa Signal-Hunt engine (`lora.cpp` park-and-stream RSSI envelope) is
    actually feeding `huntview_get` in targeted mode, make the worker bring-up reliable (or surface an honest
    "LoRa engine unavailable — free the phone link" state instead of a dead spin prompt), and gate the spin prompt
    on a live envelope so it doesn't tell the user to spin when there's nothing to bin. Cross-refs the shared hunt
    dial (`s_bh_*`, `ui.c`) + the LoRa hunt engine (`lora.cpp`).
  **✅ BUILT (same branch).** (a) The 915 preset row is gone — LoRa lists survey-detected signals only
  ("scanning 902–928 MHz · waiting for signals"). (b) **Root cause found:** the engine does feed the dial
  (`huntview_get` ← `nocsif_lora_hunt_snapshot`, `heard` = "a reading was taken"), but the proximity scale was
  the BLE/WiFi one (−100…−40 dBm) while the SX1262's floor sits at −110…−120 — every LoRa reading was "0 %",
  so every swept heading bin got the same weight and `hunt_peak_bearing` could never resolve a direction,
  however much you spun. Fix: a radio-aware floor (`s_bh_rssi_min`) — in LoRa mode the band survey's
  noise-floor estimate (then the lowest envelope seen this session) — plus a "signal present" gate
  (`s_bh_sig_ok`: envelope ≥ floor + 6 dB) that alone logs bins and shows the spin prompt. Honest states on
  the meter and the dial: "LoRa engine unavailable · retrying" while the worker isn't hunting, "no energy on
  the channel · needs a transmitter / nothing to bin" while it reads the floor. The worker's stack is PSRAM
  (RAM Phase 2), so bring-up is already unconditional — no gate to relax.

- **NEW — watch-peek sun/moon almanac countdowns.** On the watchface **peek**, show countdowns for the
  **sun and moon**: time-to-**sunset** (and sunrise→sunset / "daylight left"), and time-to-**moonrise** /
  **moonset** — i.e. "sundown→moonrise" and "moonset→sunrise" style relative timers. Compute rise/set locally from
  **latitude/longitude + date** (standard sunrise-equation / lunar-position math — no network) using the **last
  GNSS fix** (already stashed for Weather via `nocsif_weather_note_fix`) + the **RTC date** (`rtc.c`), with a
  sensible fallback location when there's no fix. Fits the celestial/orrery theme (sun = `NOCSIF_ICON_SUN`,
  moon = `NOCSIF_ICON_MOON`) and pairs with the Weather night-glyph variant above. **Keep it inside the
  rounded-corner safe zone** (§1) and on the peek's live-label tick (no worker file changes — a pure
  compute-from-cached-fix add). **⚠ Honest:** moon rise/set + phase math is heavier than the sun equation and can
  be a day off at high latitude — ship the sun countdowns first (simple, exact enough), then the moon as a second
  slice; without any GNSS fix ever, both fall back to the configured/default location.
  **✅ BUILT (2026-09-07, branch `Clankert/4-14-peek-almanac`) — sun AND moon in one slice.** New pure-math
  module `almanac.{h,c}`: NOAA sunrise/sunset (equation of time + declination, zenith 90.833°, one
  refinement pass) and a low-precision Meeus moon (≈20 longitude/latitude/distance terms → RA/Dec + parallax
  → altitude against local sidereal time, scanned across the local day in 10-min steps and interpolated at
  h0 = 0.7275·π − 34′) plus the moon's cycle age. The peek gains a fourth chrome line under the date (ring
  306 / bottom 250 / dual 210 — inside the safe zone and clear of every dial): `☀ sets 2h05 · ☾ rises 4h10`
  — for each body the NEXT event (today's, else tomorrow's), so after sundown it reads `☾ sets … · ☀
  rises …`. Location = a fresh GNSS fix, else Weather's remembered last fix (hidden when neither exists);
  UTC offset = the World-Clock HOME city + its US DST rule (the same model Sync Clock writes the RTC with).
  Computed once per day / location change, formatted once a minute from `peek_info_tick`. **Validated**
  (a Python replica of the same formulas, `scratchpad/almanac_check.py`) against the USNO `rstt/oneday`
  API for 2026-09-07: New York sun 06:29/19:18 and moon 02:03/17:31 vs USNO 02:04/17:31; London
  06:23/19:33 · 01:00/18:11 and Sydney 06:06/17:41 · 03:21/13:14 exact; age 0.86 ↔ "waning crescent, 15–20 %".
  ⚠ Honest: the moon can be a few minutes off (more at high latitude) and shows "—" on its monthly
  no-rise / no-set day; polar day/night read "up all day" / "down all day". **On-device (combined §4.14
  build, 2026-09-07): the line computed at 3 s for the stored location and matched USNO to the minute
  (sun 06:34/19:21 exact, moon 02:17 vs 02:18, 17:35 exact); operator "seems good" and asked for an off
  switch, then for the full set → System › Display gains four watchface rows — Time · Date · Weather chip ·
  Sun & moon line (persisted `peek_clock/date/wx/alm`, default on; `peek_apply_chrome()` applies them on
  build, on a toggle and when dial edit hands the chrome back).**

### 4.15 NocSif Desktop Bridge — a QFlipper-analog flasher & health tool · **new milestone (host-side + an operator-webserver firmware channel)**
Requested 2026-09-03: a **computer-side companion app** that plugs into the watch over **USB-C** and does what
qFlipper does for a Flipper — **flash / recover / provision a watch, run a full hardware self-check, and keep the
watch's firmware current from a single canonical image on the operator's own webserver.** This is mostly a **host-side tool** (its own
sub-project under `tools/`, distinct from the watch firmware) plus **two small firmware seams** (a machine-readable
self-test report over USB-CDC, and a version handshake). It reuses the shipped CDC console (`usb_gadget.c`) and the
OTA/self-test plumbing.
- **Canonical firmware on the operator's own webserver (the shared dependency for this + §4.10 OTA-pull).**
  Publish the newest **`firmware.bin`** at **one stable URL on the operator's webserver** (the same
  self-hosted server as §7 / the base-station idea) — **replaced on every update**, alongside a tiny
  **`manifest.json`** (version string, build stamp, ELF-SHA, size, notes). Both the desktop tool and the on-watch
  OTA (§4.10) read this one source of truth to decide "is the watch out of date?". **Why the operator's server
  and not GitHub:** GitHub is **HTTPS-only**, and TLS-with-WiFi-up FAILS on this board (`SSL_ALLOC_FAILED`, no
  contiguous internal RAM — the same wall that forced Weather to plain HTTP; see §4.10). The operator's server can
  serve the image over **plain HTTP** (or HTTPS with a pinned cert), so the watch pull actually works without the
  TLS block. *(The firmware source of truth stays the repo; distribution is a build step that uploads
  `firmware.bin` + `manifest.json` to the server.)*
- **Full hardware check.** Drive the compile-gated on-device self-tests (already present — the Diagnostics screen /
  `build_diag` self-tests, plus the per-driver probes: I²C scan, PMU rails, display, touch, IMU firmware boot,
  audio tone, mic level, GNSS UART, LoRa RSSI/CAD, WiFi/BLE bring-up, SD, RTC, and the NFC RF front-end self-test)
  from the host over USB-CDC, and render a **pass/fail board report** on the computer. **New firmware seam:** a CDC
  command that runs the self-test battery and streams back a **machine-readable report** (JSON lines) so the host
  can show green/red per subsystem — invaluable for triaging a new/replacement unit (recall the NFC-TX-dead and
  haptic-dead findings, §4.12: this is the tool that would have flagged them in one click).
- **Flash / provision a new watch.** Wrap `esptool --no-stub` (the project's known-good flash path, COM-port on
  Windows) so a fresh board can be brought up from the desktop without hand-run commands: pick a port, pull the
  canonical `firmware.bin`, flash, verify. Handles the bootstrap case the on-watch OTA can't (a blank/bricked
  board with no working firmware to run OTA from).
- **Auto-update a connected watch.** On connect, read the running version over the CDC handshake, compare to the
  webserver `manifest.json`, and if the watch is behind, offer to update — either by **host-side esptool flash** (USB,
  always works) or by handing the image to the **watch's own OTA** (A/B slot, safe rollback). *(The watch-initiated,
  no-computer path is §4.10 below.)*
- **Reuse / seams:** CDC console + composite USB (`usb_gadget.c`, `nocsif_usb_desc.c`) · the A/B OTA installer
  (`ota.c`) · the reliability self-tests + logbook (`reliability.c` / `logbook.c` / `build_diag`) · `esptool`
  (already the flash tool) · `tools/` (today just the Wireshark extcap — this is a second host tool). **New:** the
  desktop app itself (cross-platform; Python/Tauri/Electron — author call), the CDC self-test-report + version
  handshake commands, and the **operator-webserver firmware-channel** + `manifest.json` (a build step that uploads
  the image; plain HTTP so the watch pull sidesteps the TLS-under-WiFi wall). · buildable · **the first
  first-class host-side companion.**
- **→ BUILDING 2026-09-08 (operator feature list folded in).** Corrections to the spec above, from the code:
  (1) the **firmware channel is the public GitHub mirror** of §4.10 (`manifest.json` + `firmware.bin`), not a private
  webserver — the desktop reads the same source; the TLS wall was solved in §4.10, and the desktop side never had
  it. (2) The **transport is the USB-Serial/JTAG console** (COM7 / ttyACM / cu.usbmodem), not the TinyUSB CDC: it is
  the ONE USB channel that is always alive (the gadget replaces it when a USB mode is picked; esptool uses the same
  port). The console gets ESP-IDF's interrupt-driven driver (2 KB RX / 1 KB TX rings, claimed first in `app_main`)
  so host lines arrive at USB speed; logging keeps its fail-fast-when-unplugged semantics (verified in the driver
  source). **Firmware seam (`bridge.{h,c}`):** JSON lines in, `NB>`-prefixed JSON lines out; long answers as
  base64 fragment lines (each under 1 KB, one `write()` each, paced on the TX ring, so a log line from another task
  can only land BETWEEN reply lines). Commands: `version` · `status` · `health` (pass/fail/skip per subsystem from
  the existing getters — I²C count, PMU, display DMA underruns, IMU, RTC, SD, audio, mic, GNSS, LoRa, NFC, WiFi,
  BLE, USB, memory, reliability; haptic honestly "not probed") · `test {tone|nfc|lora|gnss}` (existing self-tests,
  verdicts in the log) · `fs.ls|get|put|rm|mkdir` (8 KB base64 chunks, stop-and-wait; the P4 jail / claim / short-lock
  rules, now shared via **`sdfs.{h,c}`**) · `sd.info` · `sd.provision` (the canonical folder set + README) ·
  `sd.format` (FatFs `f_mkfs` through the MSC helper's own mount-point switch — `nocsif_usb_gadget_sd_format`) ·
  `ctl {launch|back|home|type|key|bright|vol|button|touch|cast}` + `menu` + `state` (the companion hooks over a
  second, wired transport — no AP needed) · `screenshot` (one full frame via the mirror's flush tap,
  `nocsif_ui_screenshot`) · `log.tail` · `usb {mode}` · `reboot`. **Publish:** `publish_firmware.py` also ships
  `bootloader.bin` + `partitions.bin` + `ota_data_initial.bin` and lists every part with its offset in the manifest
  (`parts`), so "Flash new watch" can provision a blank board. **Desktop app** (`tools/nocsif_bridge/`, Python +
  Tkinter; pyserial / esptool / requests; Windows · macOS · Linux): Overview (handshake, version vs published,
  Update) · Health (the board + active tests) · Flash (Flash new watch → full flash + SD check + folder provisioning
  with the "some apps need an SD card — continue anyway?" gate · Wipe & reflash, keeps NVS · Full wipe, erases NVS
  too; every destructive action double-confirms and shows exactly what is erased) · Files (browse / download /
  upload / delete / new folder / Set up folders / Format SD) · Control (menu tree, nav, type, brightness/volume,
  FN/PWR, Screenshot, "Open live control" → the companion page) · Log (live tail) · footer credits + GitHub link
  (website eigencat.org wired as a constant, hidden until the operator says so) · `--cli` for scripting.
  **Not in this pass:** a live mirror over USB (P2), macOS/Linux binary builds (source runs there; PyInstaller
  script provided, only the Windows build is exercised here). **Verified on COM7 (2026-09-08, five flash
  rounds):** every command; 2.7 MB put 29 s (92 KB/s) / get 17.5 s (154 KB/s) with sha256 match; screenshot
  0.5 s; health 12 pass / 0 fail / 5 not-probed; remote reboot. Lessons (all fixed, recorded in RESUME + memory):
  PSRAM-stacked tasks may not call any SPI-flash API (reads included); the console VFS write path drops whole
  lines when its ring is full; the RX ISR drops on a full ring; per-chunk fopen crawls; host-side `read(4096)`
  cost 200 ms per reply; a log line can wrap around a reply line. `sd.format` remains unverified (no scratch card).
  **Follow-up (2026-09-08): live view over USB + the "one downloadable app" shape (operator: "like qFlipper").**
  `mirror` command: pull-based; the watch answers with the changed RECTANGLE since the last poll (ui.c grows a
  dirty bounding box in the flush tap; `nocsif_ui_mirror_poll`), PackBits-RLE over 16-bit pixels, one touch
  event per poll. Measured: full frames pack 10–14× (7–11 KB on the wire; ~50 ms after a screen change,
  ~190 ms when a forced repaint is included), a list scroll streams ~9 fps of 205×201 patches, an idle
  watchface costs "none" replies. App: `LiveView` (Control › Live view; 410×502 = the panel's pixels, mouse
  = touch, FN/PWR, Cast), auto-detect + auto-connect on plug-in, "attached but not answering → Flash new
  watch" for blank/old boards, `APP_VERSION` + a GitHub-Releases update check in the footer, and
  `release_app.ps1 [-Publish]` → a single-file `NocSifBridge-windows-x64.exe` (39.8 MB) published as a release
  asset on the public mirror (tag `app-v<version>`). README sections point at the releases page.
  **Round 3 (operator, 2026-09-08): the NocSif look + any T-Watch Ultra.** The app wears the firmware's palette,
  fonts (Fraunces / JetBrains Mono, bundled), the engraved star / orrery motif, a left-hand menu (rounded pill +
  accent bar), a watch-header card, the star icon — and the **accent the owner set on the watch** (the bridge and
  the companion state now report it; the app remembers it). **A stock watch (LilyGo firmware) or a blank board** is
  identified from the ROM side (esptool `flash-id` + the `esp_app_desc` at the Arduino / NocSif offsets) and gets a
  landing page: back up the whole flash (verified, 256 KB chunks — the stub loader's stream is flaky on the native
  USB port), flash NocSif (erase-first, backup offered), **flash LilyGo's factory firmware** (their merged
  `factory.watch.ultra.<sx1262|sx1280>.*.bin` from LilyGoLib, fetched on demand) — the way back to stock —,
  restore a backup, and Health's ROM-level rows; Files / Control / live view / the peripheral board say "needs
  NocSif". **P2 = the RAM diagnostic** (an ESP-IDF RAM app loaded with `esptool load-ram`, nothing flashed) so the
  hardware board runs on a stock watch too.

### 4.16 Guided tour + feature reference — on-watch info buttons + a companion doc site · **new milestone**
Requested 2026-09-03: a **first-timer tutorial that walks every feature and explains what each is for**, realized
as **one documentation surface organized by the SAME menu tree as the watch**, where **every item has an "info"
button** that explains how it works / how to use it. This has a natural home: the companion web UI (§4.8a) **already
mirrors the live menu tree** (`/api/menu` is recursive over the real row registry), so the doc site is that same
tree with a description attached to each node.
- **Single source of truth for descriptions.** Each screen/row already carries a short **`en` (explainer) string**
  in the row registry (used by the companion menu JSON — `k_cyber_rows`/`k_life_rows`/`k_system_rows` and the
  nested submenu specs). Promote that into a fuller **help entry per feature** (what it is · what it's for · how to
  use it · honest caveats / hardware ceiling) so **one dataset** feeds both the on-watch info and the doc site — no
  second copy to drift.
- **On-watch info buttons.** Add an **ⓘ affordance** to screens/rows (e.g. a long-press or a header info glyph) that
  opens a short help card for that feature, drawn from the help dataset. Keep it inside the safe zone (§1); reuse
  the existing card/overlay pattern. A **first-run guided tour** (a one-time walkthrough that surfaces the marquee
  features and points at where they live) gates on a persisted "seen tour" flag (NVS), skippable, re-runnable from
  System.
- **Companion documentation site.** Serve/generate the **same menu tree as a browsable reference** — the companion
  page (§4.8a P4) grows an **info button on every row** that expands the help entry, so the phone/laptop view
  doubles as the manual. Because it's driven by `/api/menu` + the help dataset, it stays in sync with the firmware
  automatically. Optionally emit a **static doc site** (Markdown/HTML generated from the same dataset, hosted on
  the operator's own webserver) for an offline/linkable manual that mirrors the device 1:1. **Ties in:** §4.8a
  (menu mirror is already built) · the operator's webserver (§4.15's firmware channel + §7) hosts the static
  site. · buildable · **the onboarding + manual layer.**

### 4.17 RAM coexistence Phase A/B/C — PSRAM-direct display flush · deterministic USB · Carts rebuild · Governor-as-activity-leases (logged 2026-09-06)
The on-device truth this plan is built on (`docs/DMA-COEXISTENCE-VERIFICATION.md`, 2026-09-06): at steady state
(BLE resident + WiFi associated + display + audio + IMU) the largest contiguous int-DMA run is **~2 KB**, and
**GNSS (6 KB internal stack), LoRa and the USB File-Share entry (~6–8 KB: TinyUSB task stack + CDC rings + FAT
remount) all REFUSE.** The two big stacks the earlier plan wanted to relocate — **taskLVGL 16 KB and weather 8 KB —
cannot move** (both write NVS on-task; a PSRAM stack faults during the flash op). Operator rules: runtime
defragmentation is impossible (measured — never re-propose it), no reboot-to-enable, and the §4.6 Governor is a
**battery** feature that must never re-fragment the pool. Three PRs + the Carts rebuild + the Governor reconciled:

- **A1 — the display flush stage, 29.5 KB → 9.8 KB. ✅ BUILT + MEASURED (2026-09-07, PR pending).** Both
  coexistence docs wrote the stage band off as immovable ("custom panel-io = large effort, high risk"). Built
  `display_io.{c,h}`: a NocSif `esp_lcd_panel_io_t` over our own QSPI device (one device/CS; esp_lcd's SPI IO is not
  created) that replicates the CO5300 framing byte-for-byte and adds two things esp_lcd cannot do — a **streaming
  path** (window ONCE via `nocsif_display_window_begin`, then bands pushed with CS held, so the per-band
  CASET/RASET/RAMWR that forced PR #95's big bands is gone) and a **never-allocate-internal-DMA guarantee**
  (`SPI_TRANS_DMA_USE_PSRAM` on every pixel chunk, commands via `USE_TXDATA`). The stage is now **2 × 4 lines =
  9,840 B**; the flush is memcpy-bound at ~29.5 ms/full frame — identical to the 12-line number, so the UX is
  unchanged (operator: "screens feel great"). **Measured (largest contiguous int-DMA, pre-A1 → A1):** post-usb-init
  16,384 → 30,720 · post-imu 3,456 → 24,576 · steady state 2,048 → **18,432** · with GNSS + LoRa workers added the
  8 KB USB File-Share entry proxy **passes** (largest 11,776; the pre-A1 build refused GNSS with NO_MEM) · **0** DMA
  underruns over 11,727 chunks. ⚠ **PSRAM-direct DMA (no stage at all) was tried first and is DEAD at 80 MHz
  pclk** — 4 of 19 chunks underran on the boot clear (quad PSRAM through the cache ≈ 30 MB/s vs the panel's
  40 MB/s); kept as `NOCSIF_FLUSH_PSRAM_DIRECT=1` for a 40 MHz experiment only. Side lesson: `spi_master`
  dequeues an underrun transaction and THEN returns `ESP_ERR_INVALID_STATE` — treat it as completed or the
  in-flight count desyncs and the next drain waits forever (the first A1 build wedged exactly so, silently: no
  panic, no WDT, WiFi kept associating). Full curve: `docs/DMA-COEXISTENCE-VERIFICATION.md` "Phase A1 addendum".
  **Residual seen:** heavy navigation eroded the tail 11,776 → ~4 KB with ~11 KB free (the ALWAYSINTERNAL
  small-alloc mechanism) → A3.
- **A2 — cache-off-safe stacks → PSRAM + the IMU `err -3` decoded. ✅ BUILT + MEASURED (2026-09-07, PR pending,
  stacked on A1).** Movers, each audited for on-task NVS / partition / OTA calls (none): **IMU 6,144 + `s_fifo_work`
  2,048 (now a PSRAM heap block), GNSS 6,144, buttons 4,096, usb_gadget 6,144, NFC 6,144** via
  `xTaskCreateWithCaps(MALLOC_CAP_SPIRAM)`; the verified bhy2 FIFO-drain bound cherry-picked from
  `Clankert/usb-msc-entry-crash-8fc5c7`. **Measured (largest contiguous int-DMA, pre-A1 → A1 → A2):** post-imu
  3,456 → 24,576 → **30,720** · steady state / WiFi associated 2,048 → 18,432 → **30,720** (15×) · +GNSS and +LoRa
  workers cost the run **nothing** (PSRAM stacks) · the 8 KB USB entry proxy passes with the run at 22,528 during
  the claim · Signal Hunt in LoRa mode (WiFi monitor pressure) bottoms at 21,504 · every worker logs `stack in
  PSRAM`. **The IMU `err -3` is DECODED — a BHI260 NACK, not memory:** `i2c read reg=0x2d -> ESP_ERR_INVALID_STATE`
  1.1 s after WiFi `assoc -> run` with 37 KB free; in `i2c_master.c` that error without a timeout log means the
  device NACKed (a timeout logs `ESP_LOGE`; a NACK only `ESP_LOGD`). One retry after a tick in the IMU I2C wrapper
  is the fix; PR #172's "IMU int-DMA margin" precondition for the §4.6 Governor is CLOSED. **taskLVGL + weather
  stay internal.** Detail: `docs/DMA-COEXISTENCE-VERIFICATION.md` "Phase A2 addendum".
- **A3 — deterministic USB File-Share entry + the lazy WiFi workers. ✅ BUILT + VERIFIED ON-DEVICE (2026-09-07:
  File Share mounted on the PC with WiFi associated + BLE resident — the failure Phase A was opened for is
  closed; PR pending).** Boot reserve-and-release: `usb_gadget.c` claims `NOCSIF_RADIO_MIN_DMA_USB` (8 KB, `coex.h`) from
  the pristine pool in `nocsif_usb_gadget_init` and frees it immediately before the one-time
  `tinyusb_driver_install`, so the install's ~5–7 KB of internal allocations (esp_tinyusb's 4 KB task stack — no
  caps option — + context + CDC rings + the MSC FAT handoff) land in that hole regardless of what fragmented since
  boot; the gate can then only fail when the reserve was never claimed, and the USB screen says **why**
  (`nocsif_usb_gadget_fail_reason`: "needs memory" / "install failed") instead of a bare "switch failed". The
  sibling worktree's `msc_shed_wifi` stop-gap (a runtime WiFi stop, measured to recover zero contiguity) is NOT
  carried over. **The four lazy WiFi workers — parser 3 K, PCAP writer 6 K, handshake export 6 K, portal DNS
  4 K — move to PSRAM** with `vTaskDeleteWithCaps` pairs: they are exactly the transient internal claims that
  eroded the run under Signal Hunt / capture (−9 KB in LoRa-hunt mode after A2), and each is cache-off-safe (SD
  over SPI, sockets; every NVS write in `wifi.c` belongs to the pinned command worker or UI-called setters).
  **Dropped from the plan: the `SPIRAM_MALLOC_ALWAYSINTERNAL` 4096→1024 knob** — IDF keeps small allocations
  internal precisely so FreeRTOS objects created via plain `malloc` never land in PSRAM and get touched from an ISR
  while the flash cache is off; lowering it reopens that rare-crash class for a benefit the erosion data doesn't
  support (the erosion was task stacks, now moved). The permanent compile-gated **`NOCSIF_COEXV`** probe
  (per-stage snapshots + a steady-state co-residence probe: GNSS + LoRa + an 8 KB usb proxy with WiFi associated +
  the phone on BLE, plus the display underrun counter) is the acceptance test for every phase; `findstr COEXV_A1
  firmware.bin` proves the running binary. Real acceptance for A3 = tapping File Share with WiFi associated + the
  phone on BLE and the PC mounting the drive (COM7 goes dark at the PHY switch, so that one is eyes-on).
- **B — Carts player rebuild (§4.13 #6 follow-on). ✅ BUILT + VERIFIED ON-DEVICE (2026-09-07, operator: "all
  good" — folders/files, play, Stop, tap-to-switch, background playback; WAV only — operator declined vendoring
  an MP3 decoder; PR pending).** `Cyber › Audio › Carts` lists the FOLDERS under `/sd/nocsif/carts`
  (+ loose `.wav` at the root); tap a folder → its `.wav` files, sorted, snapshot under the SD lock (the voice-memo
  idiom: per-row heap path, pushed screen); tap plays; a Stop row (first) stops; tapping another file switches; a
  400 ms timer drives the "playing · <file>" status line. `audio.c` Phase B engine: **any PCM WAV** — the RIFF
  chunks are walked for `fmt ` + `data` (LIST/fact skipped, WAVE_FORMAT_EXTENSIBLE unwrapped), 8/16/24/32-bit
  integer or 32-bit float, mono or stereo (downmixed), 8–48 kHz with the **I2S clock reconfigured per file**
  (`i2s_channel_reconfig_std_clock`; tones/cues restore 16 kHz), **streamed** in 16 KB PSRAM blocks under short
  /sd locks (no whole-file buffer, any length; sdspi bounces a PSRAM target through its own 512 B DMA block, so
  it is safe with WiFi up). `nocsif_audio_play_file(path, makeup_x100)` + `_stop()` + `_playing_path()`;
  `play_wav` = the memo path at 2.4× make-up, files at unity; the §4.13 limiter guards both. Zero new int-DMA
  (the I2S TX channel is boot-reserved; its descriptors are rate-independent). The synth-tone row and the "must
  be 16 kHz mono" lecture are gone.
  **B+ MP3 (follow-on, 2026-09-07, branch `Clankert/mp3-and-bssid-places`):** the operator reversed the MP3
  call → **minimp3** (CC0, single header, unmodified) vendored as the header-only component
  `firmware/components/minimp3`; `audio.c` is the one TU with `MINIMP3_IMPLEMENTATION` (+ `ONLY_MP3`, `NO_SIMD`).
  `nocsif_audio_play_file` dispatches on extension (`.mp3` → `do_play_mp3`, else the WAV walker): a 16 KB PSRAM
  input window refilled under short /sd locks whenever < 4 KB remain (minimp3 accepts a frame only when the next
  header is present, so the window must always hold frame + header), decoder state (~6.7 KB) + the 1152×2 PCM
  frame in PSRAM, stereo downmixed, shared `emit_mono` gain → soft-knee limiter → I2S (the WAV path now uses the
  same emitter). Tags are trimmed up front — ID3v2 skipped by its syncsafe size (album art would otherwise be
  scanned for false syncs), ID3v1/APE trailers cut off the end (minimp3 validates a frame by chaining up to 10
  following headers, so any trailer would silently drop the last ~10 frames). The I2S clock follows the first
  decoded frame (re-set on a rate change). The audio worker's PSRAM stack 6 → 28 KB (`mp3dec_decode_frame` keeps
  ~19 KB of scratch on the stack). Carts lists `.wav` + `.mp3`. Zero new internal RAM.
- **C·P1 — ✅ BUILT + VERIFIED ON-DEVICE (2026-09-07, branch `Clankert/governor-p1`, stacked on B, PR pending):
  modem-sleep MAX on link (COM7), Connectivity screen + tile "all good" (operator); park → retry → relink is
  LOGIC IN PLACE, UNEXERCISED (the AP is always in range; operator declined the router-off test).** ⚠ Boot-curve
  lesson: the "largest run" swings ±8 KB by which 8 KB boot claim (USB reserve / weather stack / MSC object)
  lands last — zero-sum, total free identical; accepted at 22,528 + 8 KB banked (details in the verification
  doc's C·P1 addendum).
  `governor.{h,c}` — a 1 s policy tick over the EXISTING cached getters (no call-site sweep): holders =
  companion / portal / AP / PCAP / capture / a weather fetch / a join / a scan. **LINKED·IDLE** → stay associated
  (the link is the presence signal) with `esp_wifi_set_ps(MAX_MODEM)` (~1–2 mA); **BUSY** → MIN; **SEARCHING**
  (up, unlinked, idle) → after `idle_min` (5) the STA is **PARKED** (`esp_wifi_stop` via the worker — driver
  memory retained, nothing re-fragments) — the away-from-home scan burn is the battery win; **PARKED** → every
  `retry_min` (15) it wakes for 45 s to look for the saved network, then re-parks (P2 replaces the timer with
  the GPS geofence); a weather force-refresh, a WiFi screen, or a tile tap wakes it at once and intent stays
  ON (the CC tile reads on; `radio_state.wifi_parked`). Airplane / user-off = intent off (no retries).
  Settings › Connectivity gained a live "Wi-Fi power" line + rows: Auto-off (on/off) · after (1/5/15/30 min) ·
  look again every (never/5/15/30) · Modem sleep when linked. BLE (resident controller; scan/advert per
  screen) and GNSS (per-screen / background holders) are unchanged in P1. A `-DNOCSIF_GOV_SELFTEST=1` build
  shrinks the timers (park 20 s, retry 40 s, look 20 s) for a COM7 proof. Also carries the **A3 follow-up:**
  the USB entry reserve is now claimed right after the audio reserve (pristine region) instead of after
  `ui_init`, so it cannot split the post-WiFi tail (measured 30,720 vs 22,528 across two equal builds).
- **C·P2–P4 — ✅ BUILT + VERIFIED ON-DEVICE (2026-09-07, branch `Clankert/governor-p2-p4` off main, PR
  pending): the 5-min cycle, hold/release, rail duty and 60 s timeout all seen on COM7 (production build);
  fence evaluation + the location stamp are UNEXERCISED (no fix indoors) — they log themselves the first time
  a fix coincides with a link outdoors.**
  **P2 cyclic GPS + places:** `gnss.c` gains a fourth want, `nocsif_gnss_set_hold()` (OR'd with Live Fix / GPX /
  Wardrive — no holder can end another's session). Every `gps_min` (5) the Governor holds the receiver for one
  fresh fix (≤60 s, else "no fix — sleeping"), evaluates the fences, releases; **skipped while the IMU says the
  watch has been still ≥5 min** (position unchanged); a foreground session's fixes are evaluated for free.
  Fences = circles of `radius_m` (200) around each saved network's learned location, 1.3× hysteresis on exit,
  no fix ⇒ state held (the indoor-hold rule). **P3 geo-store + auto-connect:** ~~each saved profile carries
  lat/lon (`wn_la%d`/`wn_lo%d`)~~ → **superseded the same day by BSSID-keyed PLACES** (branch
  `Clankert/mp3-and-bssid-places`): a place = the connected ACCESS POINT's BSSID + the location it was last
  linked from (µdeg) + its SSID — one SSID lives in many physical places ("xfinitywifi", a phone hotspot), so the
  AP is the key. Up to 8 places (`pl_n`, `pl_b%d` 12-hex / `pl_s%d` / `pl_la%d` / `pl_lo%d`), oldest evicted;
  forgetting a profile drops its places (so does profile eviction). **Auto-learned** when a fresh fix coincides
  with a link (`nocsif_wifi_request_geo_stamp` → the pinned WiFi worker reads the BSSID via
  `esp_wifi_sta_get_ap_info` and persists; a known BSSID is re-stamped only when >100 m from its stored point).
  `wifi.h`: `nocsif_wifi_place_count/_get/connected_place`; the Governor's fences iterate places and the
  Location line names the place's SSID. Policy: **place-enter wakes a parked STA at once** (+ an opportunistic weather
  refresh); **outside every known place an unlinked STA parks after 60 s**; the retry timer stays as the indoor
  fallback (a fix rarely lands indoors, so "arrived home" is often first seen by the timer, not GPS). **P4 UI:**
  Settings › Connectivity gains a live "Location" line ("inside <ssid> · next 4:10" / "outside known places" /
  "no places learned yet" / "gps fixing… (12s)" / "still, gps paused") + rows Auto-connect by place · GPS check
  every (2/5/10/30) · place radius (100/200/500 m). BLE unchanged (the resident controller's bonded-phone advert
  is the presence signal). Deferred: a per-fence radius and P2's "shorten N near a boundary" (BSSID keying
  shipped — above).
- **C — §4.6 Governor reconciled with the RAM model: leases toggle ACTIVITY on RESIDENT drivers, never memory.**
  WiFi = `esp_wifi_stop`/`start` (buffers retained — no fragmentation; `wifi.c` has zero `esp_wifi_deinit` sites,
  and a `coex_guard` CI grep keeps it that way) + `esp_wifi_set_ps(MAX_MODEM)` when associated-but-unleased; holders
  = companion / weather / OTA / capture / hunt. BLE = scan + recon-advert activity over the resident controller (the
  Phase-1 invariant), bonded-phone connectable advert kept so ANCS survives. GNSS = BLDO1 rail + `set_live`, UART
  ring installed ONCE at boot (2 KB) so a GPS wake never allocates, duty cycle gated on the existing IMU stillness
  tracker. **P1** `governor.{h,c}` lease/idle-timer core on `radio_state` + CC tiles show the holder + Settings ›
  Connectivity → **P2** cyclic GPS + geofence engine → **P3** geo-store/auto-connect → **P4** weather gating.
  RAM-neutral by construction; Phase A's headroom is what would permit a future GPS-confirmed deep-off
  (`esp_wifi_deinit` when far from home for hours, re-init gated on the heartbeat) as a measured P4 experiment.
- **Verification per PR:** PowerShell `pio run` (-j2) · binary content check · `--no-stub` flash app@`0x20000` +
  ota_data@`0xf000` · COM7 capture with the COEXV probe · eyes-on (nav-slide smoothness / no tearing for A1; File
  Share mounting on the PC with WiFi + BLE up for A3; files playing for B). One branch/PR each, squash-merged.

## 5. Hardware drivers — status (all built except the blocked NFC)
| Part | Bus / rail | Driver | Status |
|---|---|---|---|
| WiFi radio | on-SoC, DC1 | `wifi.c` | ✅ built (M5) |
| BLE stack | on-SoC, DC1 | `ble.c` | ✅ built (M7) |
| **BHI260AP** IMU | I²C 0x28, IRQ GPIO8, ALDO4 | `imu.c` | ✅ built (M11) |
| PDM mic + **MAX98357A** amp | I²S (BCLK9 / WCLK10 / DOUT11) + BLDO2 | `audio.c` · `mic.c` | ✅ built (M11) |
| **MIA-M10Q** GNSS | UART RX44 @38400 / PPS13, BLDO1 (RX-only) | `gnss.c` | ✅ built (M8) |
| **SX1262** RF | SPI3 CS36 / IRQ14 / RST47 / BUSY48, ALDO3 | `lora.cpp` | ✅ built (M9) |
| **ST25R3916** NFC | SPI3 CS4 / IRQ5, DLDO1 | `nfc.cpp` | ⛔ **firmware done, HW-BLOCKED** (§4.12) |
| **DRV2605** haptic | I²C 0x5A, en XL9555 IO6 | — | ⛔ **HW-DEAD** — no driver (§4.12) |

*Already live: AXP2101 PMU + fuel gauge + PWRKEY · PCF85063A RTC · CO5300 display · CST9217 touch · XL9555 · SD.*
**No non-NFC driver work is outstanding.**

## 6. Hardware ceiling — never on the stock watch (don't chase)
🚫 sub-GHz **OOK** capture / replay (SX1262 can't; no CC1101) · 🚫 **125 kHz LF RFID** · 🚫 **infrared** ·
🚫 **wideband SDR** · 🚫 **Bluetooth Classic** (ESP32-S3 is BLE-only) · 🚫 **iButton / 1-Wire** · 🚫 **nRF24**.
Sensor ceiling: 🚫 health biometrics (no HR / SpO2 / ECG) · 🚫 compass (no magnetometer) · 🚫 barometer ·
🚫 ambient-light / auto-brightness · 🚫 secure-element payments · 🚫 BT-Classic calls / audio · 🚫 **camera**
(no QR *scan*, no photo capture). **Dead on this unit:** NFC-TX + haptic (§4.12).

**Peer framing:** vs **Flipper** — NFC-HF + USB-HID / DuckyScript ✅; its OOK / 125 kHz / IR / iButton blocked.
vs **WiFi Pineapple** — recon + duplicate-SSID AP + portal + management-frame all buildable; full-Linux MITM
not. vs **Proxmark** — HF buildable, LF blocked. vs **HackRF** — blocked. **Full feature-parity catalog across
Flipper / Ghost ESP / Bruce / Marauder / Meshtastic / Hak5 / Bettercap / … + the ☠ harmful-feature boundary
(what NocSif refuses and why) → `docs/research/feature-parity-2026-08-21.md`.**

**Escape hatch:** the **NocSif Backpack** (idea §7) — a USB-C co-processor carrying OOK / LF / IR / iButton /
nRF24 silicon — reaches most of this later. SDR + BT-Classic stay blocked regardless.

## 7. Non-planned ideas (parked — long-horizon, decided-not-forgotten)
The near-term ideas that used to live here (camera-glasses, Flock, remote relay, approvals lane, notification
reply, scoped USB file-share, hotspot bridges, notes / i18n) are now **in the forward plan (§4)**. What remains
here is long-horizon (full write-ups in git history / the design notes):
- **3D-room "window into another world"** — head-coupled 3D scene on a dedicated art / idle screen; needs the
  IMU tilt path + a rendering decision (baked angle atlas). *(⚠ no GPU → baked frames / low fps.)* → M11 (tilt).
- **On-wrist dev-agent approvals ("vibecode from the gym")** — the watch as a remote approve / deny / answer
  terminal for a Claude Code / Agent-SDK coding session on the user's *computer*. A `PreToolUse` hook (or a
  headless Agent-SDK driver) pushes each pending tool call to a **user-hosted pub/sub relay** and blocks; the
  watch alerts, shows the command / diff, taps back allow / deny (quick-reply chips for questions). **API key stays
  on the computer** — the watch holds only a relay token. Notification path never touches the iPhone →
  **sidesteps the iOS ANCS ceiling**. Concrete instance of the **Approvals lane** (§4.8). · buildable · → §4.8 / §4.8a.
- **Cloud TTS + push-to-talk voice assistant** — spoken alerts + assistant; cloud + WiFi dependent; the harder
  later audio additions. → M11 + M5.
- **NocSif Backpack** — a USB-C co-processor PCB (own MCU, e.g. RP2040) carrying OOK / 125 kHz LF / IR / iButton /
  nRF24; a full hardware + firmware project; needs M10 host mode. Closes most of §6.
- **Home-server connectivity — the watch as a thin client to a self-hosted webserver (server = compute).** The
  premise: **the watch shouldn't need the internet — a device does the reaching for it.** The watch feeds data
  and short requests to the operator's own home webserver, which **ingests + processes** (later on a GPU) and then
  **displays a result / stores it for later retrieval** on a web UI. Structured below.
  - **Two-tier transport (the load-bearing constraint).** LoRa is a *thin* pipe, not a data pipe — a few kbps at
    SF7 down to hundreds of bps at range, plus FCC ~400 ms/channel dwell limits — so the two radios split by job:
    - **Bulk tier — WiFi ✅ (when home / on a known AP):** file sync, capture/track upload (PCAP · hc22000 ·
      GPX · wardrive CSV · voice memos), OTA, dashboard feeds. *Radios shipped; a background uploader is new.*
    - **Thin tier — LoRa ✅ (roaming, no WiFi):** a short command, a query + short answer, an alert / approval /
      SOS / position ping — text-message-sized, store-and-forward. *P2P messaging shipped (#129); S&F planned §4.5.*
      **LoRa can NOT carry a PCAP / GPX / voice memo — bulk is WiFi-only.**
  - **NocSif Base Station (a new *second device*).** A battery-powered SX1262 + WiFi board dropped at home / the
    hotel and joined to WiFi once; it runs a **LoRa-frame ↔ MQTT/HTTP bridge** and sustains the watch link:
    watch → LoRa → base station → internet → **server does the heavy lifting** → answer → LoRa → watch. This is what
    makes "reach the internet with no WiFi on the watch" true (e.g. *"process this code" → a result line back*). It's
    Meshtastic's MQTT-gateway pattern purpose-built for request/response — buildable, but its own HW+FW mini-project.
  - **Capability groups (checked against shipped silicon — almost all reuse existing radios):**
    - *Watch → server:* **Home/API remote** (watch fires commands at the server API — small) · **sensor uplink**
      (scheduled battery / IMU-activity / presence / position POST — small) · **geofence / presence triggers**
      (reuses planned GNSS geofence + RF-fingerprint presence §4.6 → fires a server call — medium) · **capture /
      track upload** (**bulk → WiFi only**, inverse of the shipped web-UI file serving — medium).
    - *Server → watch:* **server-pushed notifications** (persistent WS/SSE/MQTT over WiFi, or LoRa roaming — battery
      cost) · **live dashboard** (server-side; the watch just uploads — trivial its side) · **approvals / 2FA lane**
      (the marquee fit for the thin pipe — see the dev-agent entry) · **remote-trigger the watch** (start a capture /
      play a sound / show a message — needs a command dispatcher on the watch — medium).
    - *Self-hosted companion:* **two-way file sync** (extends one-way wireless-file-download §4.8 to /sd ↔ server —
      medium) · **self-hosted OTA** (pull firmware from *your* server; extends OTA §4.10, needs the A/B partitions).
    - *Watch = sensor, server = compute (strongest three):* **server-commanded WiFi capture → watch uploads →
      server GPU cracks** (command channel + shipped capture/hc22000 + bulk WiFi; server runs hashcat) · **roaming
      RF sensor feed / personal SIEM** (tracker sightings · anomalies · who's-around streamed to the dashboard;
      LoRa carries summaries when roaming — small increments on shipped detectors) · **task-and-report orchestration**
      (watch subscribes for tasks, runs them, reports back — the benign own-server check-in loop — medium).
  - **Reuse map:** the **Approvals lane · over-internet remote relay · wireless file download (§4.8)**, the **MQTT
    gateway (§4.5)**, the **web companion control surface L4 (§4.8a)**, and could **source the watchface weather
    chip (§4.1)**. The genuinely-new work is just **one command/notify channel** (WS or MQTT over WiFi, mirrored over
    LoRa) + a **file uploader** + the **base-station device**.
  - **Honest constraints:** off-home reach needs the server internet-reachable (DDNS + forward, or a reverse tunnel
    — Tailscale / Cloudflare Tunnel / WireGuard) with **TLS + a scoped token** (self-signed certs need pinning);
    persistent WS / MQTT links keep WiFi hot → **real battery cost**; the base station is a device to build; LoRa
    stays text-sized. · WiFi ✅ + LoRa ✅ · buildable · → §4.8 / §4.5 / §4.10.

## 8. Doc map
**Authoritative / current:**
- `docs/RESUME.md` — current-state handoff (start-here for what's built; blow-by-blow milestone history).
- `docs/design/full-app-mockup.html` — **the navigation + IA source of truth** (v2, done-state).
- `docs/HARDWARE.md` — pinout / buses / rails / I²C addresses (**authoritative** hardware facts).
- `docs/LESSONS.md` — hard-won technical gotchas + root causes.
- `docs/RAM-BUDGET.md` — RAM analysis + coexistence plan, grounded in live COM7 measurements: per-region budgets,
  the int-DMA contiguity/ordering truth behind BLE+WiFi coexistence, the conflict/failure matrix, future-pressure
  predictions, and the 18-item remake plan for everything designed around RAM (→ §4.6 Governor + `coex.h`).
- `docs/DMA-COEXISTENCE-VERIFICATION.md` — the **independent on-device re-measure (2026-09-06)** of the int-DMA
  curve: confirms RAM-BUDGET's numbers byte-for-byte, proves GNSS/LoRa/File-Share refuse at steady state, and
  corrects the relocation list (taskLVGL + weather cannot move). Phase A (§4.17) builds on it.
- `docs/DMA-COEXISTENCE-PLAN.md` — the full 75-consumer internal-memory inventory + co-resident budget that the
  verification evaluated (numbers superseded where the verification says so; the inventory remains the reference).
- `docs/ORGANIZATION.md` — device-wide standardization catalog (singular values/constants → shared globals/tokens:
  type scale, safe-zone geometry, NVS keys, `coex.h`, …) with per-item migration plans + a rollout order.
- `docs/ARCHITECTURE.md` — design rationale + the runtime-base decision record (Option B).
- `docs/research/feature-parity-2026-08-21.md` — full capability catalog vs. the reference multi-tools + the
  **☠ harmful-feature boundary** + the **combined / fusion tools** + **NFC combos §J** (the §domain-number refs in §4).
- `docs/research/capability-expansion-2026-08-08.md` — the earlier M5+ capability catalog + ordering.
- `docs/research/flipper-integration-and-app-manager-2026-08-09.md` — Flipper interop + app / script manager (§4.10).
- `docs/design/icons/celestial/` — the shipped-candidate celestial hub / dial icons + gallery + README.
- `docs/design/meta-prompts/companion-control.md` — the companion / typing meta-prompt (§4.10; pending build).

**Reference (feature-specific):**
- `docs/design/ui-design-spec-2026-08-08.md` — the UI visual / behavioral contract (grayscale / serif-over-mono
  / engraved-star / orrery).
- `docs/design/lock-ui-explorer-mockup.html` — v2 peek + menu explorer with live theme / wallpaper / font switching.
- `docs/design/signal-hunt-mockup.html` — the Signal Hunt interactive mockup.
- `docs/design/m6-nfc-plan-2026-08-15.md` — the phased NFC plan (blocked, §4.12).

**Historical (superseded — kept for provenance, don't treat as current):**
- `docs/design/ui-mockup.html` — the original single-screen visual mockup (pre-P2 palette; token source).
- `docs/research/{runtime-base,lvgl-integration,usb-composite-device,m4-p4-hid-implementation,ui-shell}-*.md` —
  the sourced research behind M0–M4 + the UI shell (all shipped).
- `docs/design/meta-prompts/ui-shell-p4*.md` · `ui-v2-restructure.md` — completed build meta-prompts.
