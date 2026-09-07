# NocSif — design & capability ideas (exploration)

Design/feature sandbox from the 2026-08-09 design session. **Not a committed plan** and does **not** block
the UI shell milestone (`docs/research/ui-shell-2026-08-09.md`); these are ideas to pull from later. Full
buildable roadmap with milestones lives in `docs/research/capability-expansion-2026-08-08.md` — this doc adds
motion/depth concepts, a couple of composite feature ideas, and a **current-state "what's actually missing"
snapshot** with specifics. Neutral device-class wording per `docs/RESUME.md`.

---

## 1. What the device can do *right now* (after M0–M4)
Grounding, so "missing" is precise. As of M0–M4 (all verified on-device), the watch can:
- Boot with 16 MB flash + 8 MB PSRAM; **CO5300 AMOLED display**, **CST9217 touch**, **LVGL v9 UI**.
- Mount a **microSD** card (FAT32) and read/write files.
- Act as a **USB composite device**: CDC serial console + **USB Mass-Storage** (host drops files on the card)
  + **USB HID keyboard** running **DuckyScript** (with a CDC+HID fallback when no card).

That is the whole current capability surface. **Everything wireless/sensing on the board is unused.**

## 2. What's truly missing right now (per subsystem)
Each item lists the on-board part and what building it unlocks. "Buildable" = hardware present, just no
firmware yet; "Blocked" = the silicon can't do it on the stock watch.

**WiFi — ESP32-S3 2.4 GHz radio (buildable; the single biggest gap).** Completely unused today. Missing:
AP/client **scanning**, promiscuous **packet monitor**, **management-frame TX** (deauth/disassoc), **beacon**
generation, software **access point**, **probe-response AP**, **captive portal** credential capture, **WPA
handshake / PMKID capture**, wardriving, WiFi analyzer. This is the core of Marauder / WiFi-Pineapple-class
tooling and it's all just unwritten firmware. → capability-expansion **M5**.

**BLE — ESP32-S3 (buildable).** Unused. Missing: device **scan/GATT explore**, **advertisement broadcast**
(pairing-notification types), **BLE HID keyboard** (wireless DuckyScript), beacon (iBeacon/Eddystone). → **M7**.

**NFC — ST25R3916, 13.56 MHz, CS=GPIO4 / IRQ=GPIO5 / DLDO1 (buildable).** No driver at all. Missing: tag/UID
**read**, MIFARE Classic/Ultralight/NTAG **read/write**, **card emulation** (present a stored `.nfc`),
**ISO14443-A/B + ISO15693**, key-recovery (MFKey32 / nested), sniff/relay. HF only — **no 125 kHz LF**. → **M6**.

**GNSS — UBlox MIA-M10Q, UART TX43/RX44, BLDO1 (buildable).** No driver. Missing: location fix / NMEA parse,
GPX track logging, geotagged wardriving, time-from-GNSS. → **M8**.

**Sub-GHz — SX1262, CS=36/IRQ=14/RST=47/BUSY=48, ALDO3 (partly buildable).** No driver. Buildable: LoRa
**P2P messaging**, **Meshtastic** interop, the in-band 915 MHz (G)FSK `.sub` subset. **Blocked:** OOK
capture/replay (SX1262 can't; would need an external CC1101, no GPIO header). → **M9**.

**IMU / motion — BHI260AP, 0x28, IRQ=GPIO8, ALDO4 (buildable).** Only I²C-probed today. Missing: step/activity,
**wrist-raise wake**, orientation, tilt/gesture — which is exactly what powers §6's motion controls, §3's
depth, and §5's art piece. → **M11 / motion milestone**.

**Audio — PDM mic T3902 + MAX98357A speaker (BCLK9/WCLK10/DOUT11), BLDO2 (buildable).** No driver. Missing:
record to SD, tone/alert, WAV playback, sound-level meter. → **M11**.

**Haptics — DRV2605, 0x5A (buildable).** Probed only. Missing: feedback patterns. → **M11**.

**RTC + battery gauge — PCF85063A (0x51) + AXP2101 (0x34) (buildable, imminent).** No RTC driver; PMU rails are
gated but the fuel gauge is never read. Both are being added in **UI-shell P4** (real clock + battery %).

**Platform gaps (buildable, larger).** No **WiFi web UI** companion (architecture L4); no **MicroPython** drop-in
script layer (the L2 decision gate); no **watch functions** (watchface, alarms, notifications, charge glyph).

**Hardware-blocked — never on the stock watch (don't chase):** sub-GHz **OOK** (no CC1101), **125 kHz LF RFID**
(no LF coil), **infrared** (no IR LED), **wideband SDR** (no HackRF-class radio), **Bluetooth Classic**
(ESP32-S3 is BLE-only), **iButton/1-Wire**, **nRF24**.

**Peer framing (one line each):** vs **Flipper** — NFC-HF (missing) + BadUSB (✅ done); its sub-GHz-OOK / 125 kHz
/ IR / iButton are blocked. vs **WiFi Pineapple** — recon + rogue-AP + portal + deauth all missing/buildable;
full-Linux MITM not feasible. vs **Proxmark** — HF attacks buildable; LF blocked. vs **HackRF** — nothing (blocked).

---

## 3. Visual depth — "a window, not a screen"
Live feel: the `🔭` parallax mockup. Star field sits *behind* the glass and barely moves, orrery a touch
more, menu stays crisp on the surface — tilting the watch reads like looking into a small box of sky.
- **How:** 2–3 depth layers on the LVGL bottom layer; tilt from the BHI260AP drives a small rotate+offset of
  the background group (deeper layers parallax less); UI kept flat so text stays sharp. Amplitude presets
  (Subtle / Off).
- **Power fix:** the shell keeps the background render-once. Depth **gates** to an interaction window (touch,
  motion above threshold, or ~N s after wrist-raise), then **eases back to the static frame** and stops
  repainting. Small max offset bounds the redraw region. Reuses the shell's reduced-motion/power flag.

## 4. Occasional shooting stars
A **very rare** star streak crossing the sky — visible in the `🔭` mockup (sped up for demo). It rides in the
star depth layer so it parallaxes with tilt.
- **Power:** this is deliberately *not* continuous ambient motion — one short (~0.8 s), small-region streak
  every few minutes, **only while the screen is on** (and optionally only during a depth/interaction window),
  so the redraw cost is negligible and it never runs against a dark/asleep screen. Rarity is the point — it
  should feel like a treat you occasionally catch, not a loop. Off under the reduced-motion/power flag.

## 5. 3D room — a window into another world (art piece)
The ambitious sibling of §3: not just parallax layers but a small **3D scene** — a room / diorama / world —
that you view through the watch as a **window**, so moving the watch lets you almost **look around inside** it,
peering into corners. "It fully understands where you are in relation to everything" = **head-coupled /
off-axis perspective**: the virtual camera is offset by your viewing angle so the screen behaves like a real
pane of glass onto a deeper space (the Johnny-Lee "VR window" effect), not a flat image that merely shifts.

- **What sells it:** true occlusion + perspective (near objects slide past far ones, walls open up as you tilt),
  a consistent space you build a mental map of, layered depth cues (parallax + scale + a little shading).
- **Hardware reality (honest):** the watch senses **its own orientation** (BHI260AP), not literally your head —
  there is **no front camera** for face tracking. So "where you are in relation to everything" is **approximated**
  from watch tilt + an assumed viewing pose. It reads convincingly for *tilt-the-watch*; true *move-your-head*
  tracking would need face sensing the hardware lacks — out of scope.
- **Rendering options (ESP32-S3, no GPU):**
  1. **Baked angle atlas / light-field** *(cheapest, recommended v1)* — pre-render the scene from a grid of
     viewing angles offline, store frames/planes on flash or SD, and **select/blend** by tilt at runtime. No
     real-time 3D math; cost is storage + a blend; smoothness scales with how many angles are baked.
  2. **2.5D layered diorama** — several parallax planes (like §3) plus per-plane perspective skew and occlusion
     sprites; a live middle ground between §3 and full 3D.
  3. **Real-time low-poly software render** *(most flexible, heaviest)* — a tiny software rasterizer drawing a
     handful of textured quads; feasible on the S3 at reduced res/framerate but the biggest CPU/power cost —
     gate hard to the art mode.
- **Where it lives:** a dedicated **art / idle / watchface or easter-egg screen**, not the operational UI, so
  its cost is contained and never fights the menu's power model. Could be what the boot "window" settles into,
  or a long-press "portal." Lean the world into the NocSif identity — a starlit observatory, an orrery room, a
  window onto the constellation.
- **Power:** active only in that mode, driven by tilt, eases to a still frame when you stop moving; off under the
  reduced-motion/power flag.

## 6. Motion as input — flick & tilt controls (BHI260AP)
Same tilt/accel stream, thresholded, becomes a control surface. **Touch stays primary**; motion augments it.
Every gesture individually toggleable in Settings; there's no physical crown, so motion is the natural stand-in.

| Gesture | Sensor basis | Proposed action |
|---|---|---|
| Wrist-raise | hub wake gesture (low-power) | wake + open the depth "window" / go Home |
| Tilt-to-scroll | orientation (roll/pitch) | roll a long list like a marble; tap to select — the crown substitute |
| Flick ← / → | accel jerk | back a screen · enter the highlighted row (drives the shell's `nav_back`/`nav_push`) |
| Flick ↑ / ↓ | accel jerk | page a list / jump to top/bottom |
| Twist (roll dial) | gyro | scrub a value (brightness, numeric field) / fast-scroll |
| Shake | significant-motion | cancel / dismiss / go Home (universal "back out") |
| Double-tap body | tap detector | soft button — confirm, or a bound shortcut |
| Face-down | orientation | sleep / mute |
| Peek (tilt-and-hold) | orientation past threshold | peek the layer beneath the current screen (z-navigation) |

Power-user option: a specific gesture could fire a bound action (e.g. a saved HID macro) without drilling
menus — opt-in only.

## 7. Location-triggered NFC (geofenced auto-activation)
**Idea:** an **Auto-NFC** mode. You tag saved NFC cards with coordinates; the watch watches GPS, and when you
come **near** a tagged card's location it **automatically powers on NFC for ~5 minutes** (then powers it back
down), so the card is ready exactly where you use it without digging through menus.

**How it would work (composite of M6 NFC + M8 GNSS):**
- **Data model:** extend the saved-cards store on `/sd` — each card file gains an optional geofence:
  `{lat, lon, radius_m, mode}` where `mode` = *emulate this card* / *arm reader* / *run a bound action*.
  A "set here" control captures the current GNSS fix as the trigger point; radius adjustable (default ~50–100 m).
- **Monitor loop (background, power-aware):** with Auto-NFC on, sample GPS on a **low duty cycle** (e.g. a fix
  every 30–60 s), and **gate it with the IMU** — only bother checking while actually moving (BHI260AP
  significant-motion), stay dark when stationary. Compute haversine distance to each geofenced card.
- **Trigger:** on entering a card's radius, **power NFC (DLDO1) on**, activate the card's `mode`, start a
  **~5-minute window** (configurable), show a clear on-screen + **haptic** cue that it armed, then auto-power
  NFC **off** at window end. **Hysteresis/cooldown:** trigger once per entry; require leaving + re-entering (or
  a cooldown) before re-arming, so lingering doesn't re-fire.
- **Why it fits the hardware:** GNSS and NFC each have their own rail (BLDO1 / DLDO1), so both can be duty-cycled
  independently; NFC's field draw is exactly why the 5-minute auto-off matters for battery.

**Guardrails (this one auto-acts on a credential, so be careful):**
- **Opt-in and off by default;** a visible "Auto-NFC armed" indicator whenever it's watching, and a distinct
  cue when it fires. Every activation **logged** to `/sd`.
- **Never silently emulate** without the user having explicitly bound that card + location; a global kill-switch.
- **GPS reality:** ~2.5–5 m open-sky, worse in urban canyon / indoors (the target may be *inside* where GPS is
  weak) — mitigate with a generous radius, last-known-fix fallback, and "arm on approach" rather than requiring
  a precise indoor fix. Handle multiple/overlapping geofences deterministically (nearest first).
- Neutral framing: this is **location-triggered NFC activation** / geofencing, described in device-class terms.

**Dependency:** needs both **M6 (NFC)** and **M8 (GNSS)** built first; it's a composite automation to layer on
after, likely a small feature under System → Automations.

---

## 8. Costs, guardrails, consent (cross-cutting)
- **Power** is the recurring cost: motion sensing is cheap in the BHI260AP hub, but **repaint during depth**,
  keeping the loop awake for gestures, and **GNSS fixes** all draw. Mitigations throughout: gate to interaction
  windows, duty-cycle GNSS, do wake/step/gesture in the sensor hub, cap redraw rate, auto-off timers.
- **False triggers:** flicks/shakes/geofence entries need thresholds + confidence + debounce + hysteresis;
  never bind a destructive or credential action to a raw trigger without a confirm/indicator.
- **Consent/visibility:** anything that auto-acts (auto-NFC, gesture shortcuts) is opt-in, visibly indicated,
  logged, and killable. Accessibility: motion is always optional; touch always works.

## 9. Suggested first cut (cheapest, highest value)
1. **Wrist-raise wake** (pure hub, low-power, immediate QoL; also the depth trigger).
2. **Gated parallax depth (Subtle)** + **rare shooting stars** — the signature look, bounded for power.
3. **Tilt-to-scroll + tap** (the crown substitute) and **shake-to-back**.
4. Later composites: **flick-navigation**, **geofenced auto-NFC** (after M6+M8), the **3D-room art piece** (§5,
   its own art/idle mode), gesture-bound shortcuts, twist-dial.

## 10. Open questions (nothing decided)
- Depth: background-only (recommended, crisp text) vs. whole-scene tilt? Shooting-star cadence on-device?
- Which gesture set ships v1 vs. experimental? Does motion warrant its own milestone or fold into M11?
- Auto-NFC: default radius/window; how to handle indoor GPS; does it emulate silently or require a tap-to-confirm
  on arrival (safer)? Where do geofence assignments live in the UI?

## Sources / basis
- `docs/HARDWARE.md` — BHI260AP @0x28 IRQ8 ALDO4; ST25R3916 @13.56 CS4/IRQ5 DLDO1; MIA-M10Q UART43/44 BLDO1;
  SX1262 CS36 ALDO3; ESP32-S3 WiFi/BLE; the blocked-list basis (no CC1101/LF/IR/SDR).
- `docs/research/capability-expansion-2026-08-08.md` — full M5+ milestone backlog (this doc's §2 is the
  current-state snapshot of it).
- `docs/design/ui-design-spec-2026-08-08.md` §6/§8 (static background + functional-only motion the depth/stars
  gate against); `docs/research/ui-shell-2026-08-09.md` (nav stack for flick-nav; P4 RTC/battery bindings).
- Parallax + shooting-stars feel: the `🔭` mockup from this session.
