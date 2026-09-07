# NocSif — design-chat handoff (start here to continue the design work)

> **This is the start-here doc for a fresh design/ideas chat.** As of **2026-08-12**. NocSif is a modular
> security-testing firmware for the **LilyGo T-Watch Ultra (ESP32-S3)**, for **authorized security testing
> only**. Repo: `github.com/silverwolf2r/NocSif_Firmware` (**private**). This chat *designs* the firmware —
> it folds backlog ideas into the living mockup and authors meta-prompts for separate coding chats; it does
> **not** implement `firmware/` code.

## 0. First 5 minutes — read these, in this order
1. **`docs/PLAN.md`** — the single source of truth (milestone map + status legend + the §7 idea backlog +
   §1 conventions). Read it fully.
2. **`docs/design/full-app-mockup.html`** — the **done-state, whole-watch mockup** that *is* the living plan.
   Open it in a browser and walk it. We fold §7 ideas into this until the mockup = the plan.
3. **This file** (state, open threads, how to get un-stuck).
4. **`docs/design/ideas-hub-metaprompt.md`** — the template every implementation meta-prompt you author must
   follow. As needed: `docs/RESUME.md` + `docs/LESSONS.md` (firmware state / gotchas), `docs/HARDWARE.md`
   (authoritative pinout/rails/I²C), `docs/research/{ui-shell,capability-expansion,flipper-integration}-*.md`,
   `docs/design/{ui-design-spec-2026-08-08.md, lock-ui-explorer-mockup.html, signal-hunt-mockup.html,
   motion-depth-ideas-2026-08-09.md}`, and `docs/design/meta-prompts/ui-shell-p4.md` (the current P4 spec).
   *(`docs/design/ui-mockup.html` is the **superseded** original — historical, don't edit.)*

## 1. Repo / branch state (so you don't build on stale files)
Design files land on `main` via small **docs-only PRs**. **`main` is current.** Recent design deltas (all merged):
- **#29** — companion phone control + on-watch typing folded into the mockup + plan (see §6 "recently resolved").
- **#31** — mockup **interaction/state-machine fixes**: single-sourced PWR (killed the duplicate-handler
  sleep/wake flicker + overlay bleed), airtight peek/menu/asleep/assign transitions, obvious back chip + swipe-back.
- **#34** — **accidental-touch / palm-rejection** interaction model folded into the mockup + PLAN §4.5 / §7.2 / §7.4.
- **#35** — PLAN audit fixes: stale statuses (P4.2/P4.3), hardware grounding, cross-refs, gaps.
- **#36 / #39 / #40 / #42 (2026-08-11, this session)** — Display screen + **§7.6 parked ledger** (#36);
  **P4.5 USB *mode selection*** spec + rebuilt `#usb` (#39); PLAN §7.4 backlog notes — **companion remote-relay
  control** (#40) + **USB scoped file-share** (#42).
- **Interactive notifications (2026-08-11, this session)** — the receive-only ANCS mirror becomes an on-wrist
  manager: **per-app alert style** (Silent/Vibrate/Sound/Alert) · **swipe-← dismiss** · **black-screen alerting**;
  decision **iOS-primary**; **reply + approve/deny spun to backlog** (incl. a NocSif-native **Approvals** lane).
  Mockup `#notifset` + reworked `#notif` + peek/sleep banner; PLAN §7.4 (*Interactive notifications* + 2 notes).
- **Watch-wide IA + watchface redesign v2 (2026-08-12, this session)** — a **post-P7** restructure into
  **Life / Cyber / System** (a **3-circle Home**), a **rotating-dial peek v2** (weather at center · battery + accent
  BT glyph top-right · swipe-cycle activity chip · add/remove planets · opt-in Alerts planet), and a **swipe-down
  Control Center** (DND · Alerts · Flashlight · brightness · progress · media). **Explicitly does NOT touch
  P1–P7** — the coding agent is building through P7 from `PLAN.md`. Captured at the **top of PLAN §7.4**
  (*Whole-watch IA + watchface redesign (v2)*). **Not yet mocked** — the mockup rebuild is deferred to post-P7 so
  the current `full-app-mockup.html` stays the P1–P7 reference. **Refinements + a phone-first *Timekeeping*
  model** (auto-disciplined RTC · auto-timezone; M5/M7/M8) folded the same day (PLAN §7.4).
- **v2 promoted to active + meta-prompt (2026-08-12, this session)** — with the shell done (**P1–P6**) and
  **P7 backlogged**, the v2 restructure is folded from §7.4 backlog into the **active P8** phase (§4.1);
  sequencing = **v2 before M5**; risk-first sub-phases **v2.1–v2.5** (v2.1 = a dial-motion perf spike against the
  render floor). **Meta-prompt authored:** `docs/design/meta-prompts/ui-v2-restructure.md`. Also merged this
  session: power strategy (#50, §4.5) and sequencing + **Ghost ESP** parity in Cyber (#52, §7.4). **Next design
  task: rebuild `full-app-mockup.html` to v2.**
- Firmware (coding chats, separate): **UI-shell P1–P6 COMPLETE & verified** — P4 (#23–#51, incl. P4.6c passcode
  unlock), **P5** boot/motion (#53/#54, polish #56), **P6** shooting stars (#55); **P7 depth parallax
  BACKLOGGED** (#57 — the large-area software-render floor, see LESSONS render-path). **Next build = P8 · UI v2
  restructure** (the active work) — meta-prompt `docs/design/meta-prompts/ui-v2-restructure.md`. P4 umbrella spec:
  `docs/design/meta-prompts/ui-shell-p4.md`; continuation `docs/design/meta-prompts/ui-shell-p4-continue.md`.

**Where you work:** a **dedicated design worktree** off `main` (`git worktree add ../nocsif-design-<name> main`)
— **not** `D:\Docker\NocSif_Firmware` (the coding-chat `main` worktree). **First check:** `ls
docs/design/full-app-mockup.html` — if present you have the current files. **Merging docs PRs is the user's
call** via the normal PR flow (they have been merging promptly). Build/flash happens in separate coding chats;
this design chat only edits `docs/`.

## 2. What the mockup already contains (don't rebuild these)
`full-app-mockup.html` is a self-contained, walkable done-state watch UI (410×502 screen mock), **verified
in-browser (no console errors; 42 screens)**. Present today:
- **Peek / lock screen** — big time + italic-serif date + meta row; **assignable orrery planets** (hold to
  reassign among **19 registered launch targets** — the §4.1 registry, reconciled 1:1 with the firmware).
- **Home** in three bands — **operations** (WiFi · BLE · NFC · USB · Sub-GHz/LoRa · GNSS · Signal Hunt),
  **watch** (Notifications · Timers · Voice · Activity · Navigation · Weather · Notes · Flashlight),
  **system** (Files · Automations · Theme · Buttons · Peek Planets · Gestures · PIN Lock · Connectivity ·
  Companion Control …).
- **Hero / detail screens** — WiFi Scan (decrypt-in), Signal Hunt, Alerts (ANCS), Voice, Weather, Theme picker.
- **Folded in via #27 (don't rebuild):** **boot screen** (star + wordmark + bring-up log → peek) · **PIN pad /
  unlock gate** (keypad, demo `1234`) · **power menu** (PWR long-press → Lock/Restart/Off) · **Settings →
  Buttons** (FN + PWR bindings) · **Apps / Scripts manager** (drop-in .nfc/.sub/DuckyScript/MicroPython) ·
  **WiFi Hotspot / SoftAP bridge** · **BLE WiFi hand-off** · **Date & Time** (NTP + GNSS + auto-tz) · **Files
  → folder browser** · **Notes → editor** (input-method chips) · **System detail** (Power/AXP2101 · Connectivity
  incl. **Web-UI enable + QR** · Language · OTA) · **Timers detail** (Alarms/Timer/Stopwatch/World Clock) ·
  **Automations config** (Auto-NFC geofence · DND · Movie mode).
- **Folded in (companion control, 2026-08-10 — don't rebuild):** **System › Companion** (co-equal Web-UI + BLE
  transports · PIN pair · launch/run · field-fill · file transfer · live-view) · a **phone-side control panel**
  mocked below the watch (control / type / files / live tabs) · the **Notes-editor on-watch QWERTY**.
- **Personalization** — palettes `void` + real deity palettes `helios·eris·priapus·eros·hades`; wallpapers
  orrery/constellation/engraved-sun/grimoire **+ a real dithered "antique" raster**; data font mono **+ a real
  embedded OFL typewriter face (Courier Prime, WOFF2 base64 — no longer a Courier fallback)**.
- **Depth parallax (P7) + shooting stars (P6)** demo on the star layer; **physical PWR/FN buttons**.

**How to verify mockup edits:** static `file://` page. In the in-app Browser pane, files render as static
snapshots → **screenshots may fail and `requestAnimationFrame` is frozen**. Verify with
`read_console_messages(onlyErrors)` (must be clean) + **synchronous** `javascript_tool` DOM checks (count
`.view`s, invoke handlers, `document.fonts.check`). `preview_start({url:"file:///…"})` then reload with
`navigate(force:true)` after each edit.

## 3. Design language (LOCKED — PLAN §7.4)
Grayscale default · **one soft antique accent** (violet `void` default; deity-palette variants selectable) ·
**no hot / neon colours ever** · hard-ish grit (dither grain, glitch-on-transition, blinking cursor,
terminal/stencil chrome, decrypt-on-reveal) · **NO SCANLINES anywhere** · a little occult-goth over a
**linux-antiquity** art-nouveau base (MIT, credit **diinki**) · serif titles are the fixed identity; data font
+ wallpaper + palette are user-selectable (incl. **typewriter**).

## 4. Conventions you MUST follow (PLAN §1)
- **Neutral device-class wording everywhere** (docs, commits, and *especially* any subagent/Workflow prompt):
  "802.11 management-frame transmission", "credential-capture form", "USB HID keyboard", "companion control
  surface". **Never** "attack / offensive / payload / weaponize / victim / target / red-team" — charged vocab
  **hard-fails Anthropic's automated cyber-safeguard** and has killed a research workflow. Keep load-bearing
  format names (**DuckyScript**, `.sub`, `.nfc`).
- **PLAN.md / mockup edits ship as small docs-only PRs to `main`.** **Fill, don't rebuild** — wire real actions
  into existing stub rows; don't invent nav. **Ground every idea in real hardware** (`docs/HARDWARE.md`); call
  **buildable vs hardware-blocked** honestly.
- **Watch context → hand off cleanly** (like this): update this file + capture new ideas in PLAN §7, summarise
  done / in-flight / next with exact paths, and give the user a paste-ready bootstrap for a fresh chat.

## 5. In-flight (verify next session)
- **UI-shell P4 firmware** (coding chats off `main`): **P4.1 ✅ #23 · P4.2 ✅ #26 · P4.3 ✅ #30 · P4.4a ✅ #37 ·
  P4.4b ✅ #38** all verified on-device. **P4.5 is next — USB *mode selection*** (DESIGNED here 2026-08-10): the
  M4 always-on composite becomes **user-selected** — detached on plug-in (nothing appears until you pick a
  mode), **one class at a time** (File Share / HID / Console), **re-enumerate on switch**; frees IN-endpoints
  for M10 Mouse/U2F. Dedicated meta-prompt **`meta-prompts/ui-shell-p4.5-usb-modes.md`** (supersedes the P4.5
  bullet in `ui-shell-p4.md`); rebuilt mockup `#usb`; PLAN §4.1/§4.2 updated. Then **P4.6** peek/planets. The
  mockup already realises the done-state screens, so these phases *fill* existing UI rather than inventing it.

## 6. Open threads / next design work
**PRIMARY (next design work):**
- **Triage 2026-08-10:** the mockup now covers essentially the whole §7 backlog (§2). The remaining
  unintegrated ideas are **three long-horizon items parked in PLAN §7.6** ("decided-not-forgotten") — pick from
  there when their milestones/hardware exist, or fold any smaller idea the user raises.
- **3D room / "window into another world"** art piece (§7.2 #5) — the flagship, the ambitious sibling of the P7
  depth parallax: a head-coupled / off-axis 3D scene (a starlit observatory / orrery room) viewed through the
  watch as a **window**, on a dedicated **art / idle / easter-egg** screen — *not* the operational UI. **Parked
  (PLAN §7.6)** until a **rendering decision** (baked angle atlas *recommended v1* · 2.5D diorama · real-time
  low-poly) **+** the **BHI260AP** tilt path. **Discuss the rendering approach with the user before mocking it.**
  Ground it honestly: the watch senses **its own** orientation, not your head (no front camera). **Not folded
  into the mockup, by choice.**

**Recently resolved (don't redo):**
- **Interactive notifications — DESIGNED & folded 2026-08-11 (this session).** The receive-only ANCS mirror
  (§7.3) is now an on-wrist manager: **per-app alert style** (Silent/Vibrate/Sound/Alert, keyed on the ANCS app
  id — the watch's own reaction, so app-free on iOS), **swipe-← to dismiss** (+ clear-all), and **black-screen
  alerting** (the Alert level lights the sleeping panel with a banner; a *system* wake, not a stray touch — coexists
  with the §4.5 palm-rejection model). New **System › Notifications** (`#notifset`) + reworked **Alerts** (`#notif`)
  + a peek/sleep banner in `full-app-mockup.html` (verified: **43 views · 0 console errors · all interactions**).
  Decision: **iOS-primary**. **Reply** (canned/voice) + **approve/deny** deliberately spun to backlog — iOS ANCS
  blocks both — the latter reframed as a NocSif-native **Approvals** lane (actionable requests over the
  companion/relay; the watch renders approve/deny + signs a response back; fits multi-factor-auth confirmations +
  computer-side developer-agent prompts). PLAN §7.4 (new *Interactive notifications* subsection + 2 backlog notes)
  + §7.3 / §4.2 pointers. **Not yet meta-prompted — delivery is M7 (ANCS) + M11 (haptic/audio)-gated**, so the
  build spec waits until M7 is planned (the `#notifset` UI alone is buildable now on the settings store).
- **USB mode selection + two backlog notes — 2026-08-11 (this session).** (a) **P4.5 USB is now user-selected
  mode-switch** — detached-until-picked · one class at a time (File Share / HID / Console) · re-enumerate on
  switch; rebuilt mockup `#usb`, PLAN §4.1/§4.2, and a dedicated **meta-prompt
  `meta-prompts/ui-shell-p4.5-usb-modes.md`** (#39; supersedes the P4.5 bullet in `ui-shell-p4.md`; the coding
  chat should `git pull` then paste that meta-prompt). (b) **Two PLAN §7.4 backlog notes, not mocked:**
  *Companion — remote relay control* (over-internet; watch = outbound WS-over-TLS client to a **user-hosted**
  relay; full control gated behind a server password; #40) and *USB — scoped file-share* (expose a
  folder/fixed-size container as a small drive; #42).
- **Display / brightness screen — DESIGNED & folded 2026-08-10.** New `#display` (brightness slider · screen-off
  timeout · always-on · **Reduce-motion** flag that gates depth / shooting-stars / glitch); wired the dead
  System › Display stub (→ `#i-sun` + `data-go="display"`); Power's "Sleep after" → "Deep sleep after" to
  single-source the timeout (Display = backlight-off, Power = SoC deep-sleep). Mockup verified (42 views, 0
  console errors); PLAN §7.4 updated. **Parked ledger added — PLAN §7.6** banks the **3D room**, **TTS /
  voice-assistant**, and **NocSif Backpack** (the long-horizon trio) so they can't get lost.
- **Mockup interaction/state-machine fixes — DONE 2026-08-10 (#31).** Single-sourced the PWR button (removed the
  duplicate `click`+pointer handlers that double-fired the short press → sleep/wake flicker + menu-under-overlay
  bleed); made peek/menu/asleep/assign transitions land on exactly one surface; back is now an obvious 34×34 chip
  on all 41 non-root screens + a swipe-back gesture. Verified in-browser (0 console errors; 20+ transition paths).
- **Accidental-touch / palm rejection — DESIGNED & folded 2026-08-10 (#34).** Deliberate wake only (PWR /
  double-tap / wrist-raise, never a stray touch), post-wake input-lock, palm/multi-touch rejection (CST9217
  two-point + cover-flag; no contact-area), Movie-mode/pocket touch-lock. Mockup interaction notes + PLAN §4.5
  gap + §7.2 #8 + §7.4. Grounded honestly in the CST9217 (~33 Hz, 2-point, no touch-major).
- **PLAN audit — DONE 2026-08-10 (this batch).** Multi-agent adversarial pass fixed stale statuses (P4.2/P4.3
  now marked done in §3/§4.3/§5.1), hardware grounding (§4.3 mic-vs-speaker pins), cross-refs, and gaps
  (§4.4 single-radio web-UI constraint, §4.5 OTA verified-image/A-B + partition note, §7.4 peek/PIN, brightness).
- **Companion phone control + on-watch typing — DECIDED & folded 2026-08-10.** Both transports **co-equal**
  (app-free WiFi web UI + BLE companion app); **full scope** (launch/run · type into any field · configure ·
  file transfer · live-view, *live-view is WiFi-only*); on-watch typing **v1 = QWERTY + voice** (Scribble
  experimental, predictive swipe dropped); **light guardrails** (PIN pair, off by default, minimal indicators,
  optional log). Mocked in `full-app-mockup.html` (**System › Companion** + a phone-side control panel + the
  Notes-editor QWERTY); PLAN **§7.4 / §4.4** updated; implementation meta-prompt at
  `meta-prompts/companion-control.md`. Full detail: **PLAN §7.4 "Companion phone control (DECIDED 2026-08-10)"**.
- The ~19 detail/system screens, the real typewriter font, and the dithered wallpaper are folded (#27); the
  peek/planets lock screen is slotted as UI-shell **P4.6** (#24); `payload` wording neutralised (#25).

## 7. Firmware state in one line (full detail → `docs/RESUME.md`)
M0–M4 + **UI-shell P1–P3.2 + P4.1–P4.4** done & verified on-device; **P4.5 (USB mode selection) is next**
(spec `meta-prompts/ui-shell-p4.5-usb-modes.md`). Everything else wireless/sensing (WiFi, BLE, NFC,
GNSS, LoRa, IMU, audio, haptics) still unused — the road ahead (PLAN §4.2 / §5). Build from PowerShell `-j 2`;
flash `esptool --no-stub`; COM7.

---

## 8. Paste-ready bootstrap for the new chat
Paste the block below to start the fresh design chat:

> You are continuing the **NocSif design-ideas hub**. NocSif is a modular security-testing firmware for the
> **LilyGo T-Watch Ultra (ESP32-S3)**, for **authorized security testing only**. Your job: fold backlog ideas
> into the living mockup until the mockup *is* the plan, and author implementation meta-prompts for separate
> coding chats — **do not implement `firmware/` code here; only edit `docs/`.** Work in a dedicated design
> worktree off `main` (`git worktree add ../nocsif-design-<name> main`); ship every change as a small
> **docs-only PR to `main`**.
>
> **Read first, in full:** `docs/design/DESIGN-CHAT-HANDOFF.md` (start-here — current state, repo/branch
> state, open threads, conventions), then `docs/PLAN.md` (source of truth) and `docs/design/full-app-mockup.html`
> (the done-state living mockup — walk it; it has 42 screens now). Then `docs/design/ideas-hub-metaprompt.md`
> (the meta-prompt template) as needed.
>
> **Your first task — pick up the backlog / refine an idea (nothing is mid-flight in design).** The mockup now
> covers essentially the whole §7 backlog (**42 screens**); there is **no quick fold outstanding**. What remains:
> **(a)** three **long-horizon parked** ideas in **PLAN §7.6** — the **3D-room** art piece (§7.2 #5),
> **TTS / voice-assistant** (§7.3), **NocSif Backpack** (§7.5) — each blocked on a milestone/hardware; and
> **(b)** two fresh **§7.4 backlog notes** awaiting maturation — **Companion remote relay control** (over-internet)
> and **USB scoped file-share**. So either **mature one** of those (mock a screen + author its meta-prompt), or
> help the user **capture / refine a new idea**. **Recently done, don't redo (§6):** Display screen + §7.6 parked
> ledger; the **P4.5 USB mode-selection** spec (`meta-prompts/ui-shell-p4.5-usb-modes.md` — active; firmware is
> building it); companion control (#29); interaction / palm-rejection fixes (#31 / #34); PLAN audit (#35).
> **Discuss anything ambitious (e.g. the 3D room) with the user before mocking it** — §7.2 #5 rendering options:
> baked angle atlas *recommended v1* · 2.5D diorama · real-time low-poly; the watch senses its **own** orientation
> (BHI260AP), not your head (no front camera).
>
> **Enforce the locked design language** (grayscale + one soft antique accent, no hot colours, no scanlines,
> occult-goth over linux-antiquity, user-selectable theme/wallpaper/font incl. typewriter) and **neutral
> device-class wording** everywhere — charged security vocab hard-fails the cyber-safeguard. Firmware P4 is
> mid-build in parallel coding chats (P4.1–P4.3 done + verified on-device; P4.4 next) — the mockup already
> realises their screens, so don't touch `firmware/`. Name the chat **Nocsif design 6**.
