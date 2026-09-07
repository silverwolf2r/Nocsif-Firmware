# NocSif — UI v2 restructure (implementation meta-prompt)

*Authored by the design-ideas hub, 2026-08-12. Paste the block below into a fresh **coding** chat to build the
v2 UI. This is **UI-shell P8** — the next build after the P1–P6 shell (P7 depth-parallax is backlogged).*

---

> You are implementing **NocSif UI v2** on the **LilyGo T-Watch Ultra (ESP32-S3)**. NocSif is a modular
> **security-testing** firmware for **authorized security testing only** (the device owner writes/runs their own
> firmware). Local repo: `D:\Docker\NocSif_Firmware`.
>
> **Neutral device-class wording everywhere** (code comments, commits, any subagent prompt): "802.11
> management-frame transmission", "captive portal", "USB HID keyboard", "companion control surface". **Never**
> "attack / offensive / payload / weaponize / victim / target / red-team" — charged vocab **hard-fails
> Anthropic's automated cyber-safeguard** and has killed a workflow before. Keep load-bearing format names
> (DuckyScript, `.sub`, `.nfc`).
>
> ## What you're building
> The **v2 UI restructure**: replace the shipped **flat three-band Home + P4.6 peek** with three things —
> 1. a **3-circle Home hub** (**Life / Cyber / System**), each circle opening that world's scrolling menu;
> 2. a **rotating-dial peek/watchface v2** (weather at centre · battery + accent Bluetooth glyph top-right ·
>    one swipe-cycle activity chip · two edge dials of add/removable planets); and
> 3. a **swipe-down Control Center** (quick-settings shade).
>
> This lands the **structure + interactions**; live data (weather, phone battery, media, per-app alerts, BT-link
> glyph, phone-first time) is gated on later milestones (M5/M7/M8/M11) — use **placeholder / stub data** for
> those and leave clean seams. Design language is **locked** (grayscale + one soft antique accent, no hot
> colours, no scanlines, serif titles) — reuse the existing tokens, don't restyle.
>
> ## Read these first (in order)
> 1. **`docs/RESUME.md`** — exact current firmware state + how to build/flash.
> 2. **`docs/LESSONS.md`** — gotchas. **Two are load-bearing for v2:** (a) the **render-path floor** — a
>    full-screen redraw sits at ~**8–9 fps** software-render (this is what backlogged P7's live parallax); (b)
>    the **display DMA-hang** — the full-frame flush's DMA bounce OOMs under memory pressure, fixed by a **small
>    `max_transfer_sz` (≈4 rows)**. Both directly shape the v2 dials (below).
> 3. **`docs/PLAN.md`** — the authoritative spec: **§4.1 "P8 — UI v2 restructure"** (the sub-phases), **§7.4**
>    *Whole-watch IA + watchface redesign (v2)* + its *Refinements*, *Timekeeping*, and *Ghost ESP parity* notes,
>    and **§4.5** *Power management* (the rail-gating / tickless / deep-sleep foundation that proceeds alongside).
> 4. **`docs/HARDWARE.md`** — pinout / rails / I²C (CO5300 display on ALDO2 + XL9555 GPIO7; CST9217 touch;
>    AXP2101 PMU; PCF85063A RTC).
> 5. **`docs/design/full-app-mockup.html`** — ⚠ **still shows v1-flat**; it is being **rebuilt to v2 by the
>    design chat**. **Build to the §7.4 spec, not the current mockup.** Ask the design chat for the v2 mockup
>    before the pixel-layout phases (v2.4).
> 6. As needed: `docs/ARCHITECTURE.md`, `docs/design/ui-design-spec-2026-08-08.md` (tokens / nav model),
>    `docs/research/ui-shell-2026-08-09.md`.
>
> ## Current state + where the code lives (`firmware/src/`)
> **P1–P6 shell is complete & on-device-verified; P7 backlogged; M5+ not started.** Relevant modules:
> - **`ui.c` / `ui.h`** — the app layer: the **menu tree built as a launch-by-id registry**
>   (`nocsif_app_launch(id)` is the single entry point for menu rows / peek planets / FN / gestures;
>   `nocsif_menu_add_row(...)` builds rows), the **P4.6 peek/watchface** (locked = the peek is the active
>   screen; big time via the `nocsif_num_64` font cut, italic date `nocsif_serif_16i`, battery via a header
>   tick; 6 orbital planets each drawn in the visible crescent; **palm-to-sleep**; **passcode-gated unlock**
>   from P4.6c — a single `lock_unlock_to(dest)` choke point over a transparent keypad), and header hooks
>   (a live-label tick + `nocsif_nav_set_header_badge(...)`).
> - **`ui_nav.c` / `ui_nav.h`** — the **root-based nav stack** (Home → submenu → action, `build_header`, a
>   custom `set_x` + opacity slide ~220 ms; a full-screen slide is coarse at the flush rate). **LVGL objects
>   come from a FIXED ~96 KB pool, not the system heap → free screens on nav-back.**
> - **`ui_theme.c` / `ui_theme.h`** — embedded fonts + colour tokens + shared `lv_style_t` handles (serif +
>   mono cuts, the tintable icon font, `nocsif_num_64`, `nocsif_serif_16i`; muted WHITE `0xA6A6AC` / BONE
>   `0x8C8C92`). Add font cuts via `gen_fonts.py` → CMakeLists SRCS + `ui_theme.h` extern.
> - **`ui_background.c` / `ui_background.h`** — the static orrery/star field **rendered once** into
>   `lv_layer_bottom()` + the 4 corner masks (respect the ~56 px corner radius).
> - **`settings.c` / `settings.h`** — the **NVS/SD key-value store** (theme/font, planet assignments, hashed
>   PIN, button bindings). v2 config (dial contents, Home-circle motifs, AOD toggle, Focus, etc.) persists here.
> - **`power.c` / `power.h`** (AXP2101: battery %, soft-off, backlight/brightness), **`rtc.c`** (PCF85063A),
>   **`buttons.c`** (PWR via AXP2101 PWRKEY IRQ + FN=GPIO0; **FN single = back / double = launch**, a
>   user-requested deviation — keep it), **`touch.c`** (CST9217, 2-point, no contact-area; exposes the
>   **cover-screen gesture** `nocsif_touch_cover()` used for palm-to-sleep), **`display.c`** (CO5300 QSPI;
>   keep `max_transfer_sz` small per the DMA-hang lesson).
>
> ## Scope, precisely (device-class terms) — from §7.4
> - **3-circle Home hub:** swipe-up from the peek → three circles **Life / Cyber / System**; tap → that world's
>   scrolling menu (re-home today's registry rows into the three trees in §7.4). Each circle is a themeable
>   **sun / moon / ringed planet** motif (defaults Life=sun · Cyber=moon · System=ringed planet; **long-press to
>   change**; persisted) + a **status badge** (e.g. Cyber "2 running"). **Swipe-back from Home → the peek.**
> - **Peek v2:** **weather** (temp + glyph, placeholder) above the big time + italic date at **centre**;
>   **battery % top-right** with an **accent-tinted Bluetooth glyph** beside it when linked (present-only, **not
>   blue**); **no top-left alert count** (a Focus/DND indicator may live there); **one slim activity chip** below
>   the time showing the top live state, **swipe the chip to cycle**; **two independent edge dials** —
>   **drag a side to rotate**, planets **add/remove/reorder** (persisted), a planet binds to **any launch-by-id
>   target incl. the 3 hubs**; **long-press planet → customise**, **long-press background → wallpaper/theme**,
>   plus an explicit **Edit mode** in **Settings › Peek & Shortcuts** (long-press is fragile on the 2-point
>   CST9217). Keep the **deliberate-wake model** (PWR / **double-tap** / wrist-raise-stub) + the post-wake
>   input-lock + palm-to-sleep. *(Design specs double-tap-to-wake; the shipped build wakes on single tap —
>   switch to double-tap here.)*
> - **Control Center:** pull **down from the top edge** (from any surface; open only from the top edge / list
>   overscroll so it never hijacks a scroll) → a shade with **DND** (top-left) · **Alerts** circle (top-right) ·
>   **Flashlight** (centre) · a **brightness slider** (live, CO5300 backlight) · **Wi-Fi / BLE / Airplane**
>   toggles · a **progress bar** (current long task) · **media transport** (placeholder). **Swipe up from the
>   very bottom edge closes it.**
> - **Cross-cutting:** a **"Current Activity" service** (one active-task feed → the peek chip + the Control-Center
>   progress bar + the "running/monitor" row tags — model it once, stub its producers); **Focus > per-app >
>   DND** (Focus = notification rule-sets only, no face/planet/radio swap); connected/charging state = accent
>   tint + presence, never a hot colour.
>
> ## Phased plan — risk-first (front-load the render unknown)
> - **v2.1 — dial-motion perf spike (do this FIRST, isolated & flashable).** The peek's rotating dials move
>   planets across the screen; the render-path floor is exactly what backlogged P7's live motion. Prove a dial
>   can rotate **acceptably** by moving **only the planet regions** (partial refresh / sprite-blit; NOT a
>   full-frame redraw), respecting the small `max_transfer_sz`. **Decision gate:** if it's smooth → continuous
>   rotate; **if not → a discrete detented snap** (one planet-step per flick, snap-animated — the P7 "settle,
>   not live-motion" fallback). Report fps + the chosen model before building the rest on it. **Target geometry
>   (from the design mockup): a depth carousel** — planets ride the near side of an *off-screen* ring drawn as a
>   faint arc guide (half the ring is off-screen), each planet placed by angle and **sized by depth** (largest at
>   the centre bulge, shrinking + fading toward the corners), moving **only** the planet regions per step.
> - **v2.2 — 3-circle Home hub + Life/Cyber/System menus.** Replace the flat Home in `ui.c` / `ui_nav`; re-home
>   the existing registry rows into the three domain trees (§7.4); tap circle → domain menu; swipe-back → peek;
>   long-press circle → motif (persisted). Keep every launch-by-id target working.
> - **v2.3 — Control Center.** The swipe-down shade + its gesture routing (top-edge/overscroll open, bottom-edge
>   swipe-up close). Wire the levers that work today (brightness, Flashlight, radio toggles as stubs, DND flag);
>   Alerts/media/progress render with placeholder data behind the Current-Activity seam.
> - **v2.4 — peek v2 layout + dials wired.** The centre weather/time/date + top-right battery/BT glyph + the
>   swipe-cycle activity chip + the v2.1 depth-carousel dials with **per-side add / remove / reorder** + **Edit
>   mode** (Settings › Peek & Shortcuts) + long-press customise + hub-as-planet targets. **Go deep on the dial
>   editing** (user-requested): a planet can be added to **either** side, and **an empty side is a valid, persisted
>   state** — 0 planets → render **no arc and no planets** on that side, so a one-dial or a clean dial-free
>   watchface is allowed. Migrate the P4.6 planet config forward. *(Get the design chat's v2 mockup first for exact
>   geometry — it demonstrates the carousel, spin, long-press re-target, and empty-side handling.)*
> - **v2.5 — polish + migration.** Connected-state accent styling, hub status badges, "high-draw" hint-tag
>   groundwork, reduced-motion respect, and fold in whatever the §4.5 power foundation exposes.
>
> ## Hardware / LVGL specifics to reuse (don't reinvent)
> - The **launch-by-id registry** (`nocsif_app_launch`) is the spine — dial planets and Home circles bind to it.
> - The **fixed ~96 KB LVGL pool** — build v2 screens from it and **free on nav-back**; a deeper nav or heavier
>   peek may need the pool bumped (edit `sdkconfig.<env>`, not just `sdkconfig.defaults`).
> - **Motion must be partial-refresh / sprite-based** — never animate via full-frame redraws (render-path floor +
>   the DMA-hang under memory pressure). Reuse the **P6 shooting-star** sprite approach and the **P5 motion
>   helpers**; honour the **reduced-motion / power flag**.
> - Reuse **`ui_theme`** tokens/fonts (incl. `nocsif_num_64` for the peek time), **`ui_background`** render-once
>   layer, **`ui_nav`** slide/stack, **`settings`** for all v2 persistence, **`power`** for brightness/backlight,
>   the **header live-label tick** + `nocsif_nav_set_header_badge`, and the **palm-rejection / deliberate-wake**
>   model (§4.5 gap + touch.c cover gesture).
> - Respect **RGB888 BGR + the ~56 px corner radius** (corner masks) — inset the top-right status cluster.
>
> ## On-device verification (per phase, on COM7)
> - **v2.1:** measure dial-rotate fps on-device; confirm no display hang under a USB-install + SD memory-pressure
>   repro; record the continuous-vs-snap decision.
> - **v2.2:** every re-homed row still launches its correct target; swipe-back Home→peek; motif long-press
>   persists across reboot.
> - **v2.3:** shade opens only from the top edge / overscroll, closes on bottom-edge swipe-up; brightness slider
>   actually changes the backlight; radio toggles flip their stub state; no scroll hijack in a long menu.
> - **v2.4:** dials add/remove/reorder persist; a planet bound to a hub opens that hub; long-press + Edit-mode
>   both reach customise; wake = PWR / double-tap only (a single stray touch does **not** wake); palm-to-sleep
>   still works.
> - **v2.5:** reduced-motion off = no dial/motion animation; badges + connected-state styling render; reboot
>   restores all v2 settings.
> - Each phase: **heap headroom + no `wait_for_flushing` wedge** (the DMA-hang signature — frozen frame, dead
>   buttons, heartbeat alive). Verify builds by **binary content**, not cached compile-time.
>
> ## Build / flash / PR conventions
> - Build from **PowerShell with `-j 2`** (default parallelism OOMs the Windows commit charge; drop to `-j 1` or
>   reboot if it still OOMs), **never Git Bash**. Flash **`esptool --no-stub`** (download mode: hold BOOT → tap
>   RST → release BOOT); watch is **COM7**.
> - **One sub-phase per branch → PR → squash-merge to `main`**; **verify on-device before the next**; refresh
>   `docs/RESUME.md` at each stopping point.
> - **Front-load v2.1** as the isolated, flashable first phase; do not build v2.2–v2.5 on an unproven dial model.
>
> **Produce a phased plan and pause for my review before writing any code.**
