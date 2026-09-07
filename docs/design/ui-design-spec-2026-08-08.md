# NocSif UI — design spec (agent-ready)

Implementation spec for the NocSif on-watch UI (LVGL v9 on the CO5300 410×502 AMOLED). As of
**2026-08-08**. This is the visual + behavioral contract a coding agent follows when building the UI
for the M5+ capability milestones. Neutral device-class wording throughout, per `docs/RESUME.md`.

## Visual source of truth
- **`docs/design/ui-mockup.html`** — a self-contained, interactive mockup in this repo. Open it in a
  browser. It is the pixel/behavior reference; when this document and the mockup disagree, the mockup wins
  for *look*, this document wins for *on-device implementation notes*.
- The `<style>` block in that file is the de-facto token sheet — every value below is lifted from it.
- The mockup is drawn at **410×502 CSS px = device px 1:1**, so all sizes here are in **device pixels**.

## 1. Design language (one paragraph)
A near-black, grayscale-first control surface with a single, very-dark violet used as a sparse highlight.
Menu-first: a home list of capability categories drills into submenus, then into action screens. The
character comes from three quiet moves borrowed from the *linux-antiquity* theme — **serif display titles
over monospace data**, **hand-drawn "engraved" celestial stars**, and a **faint antique orrery** (concentric
+ dashed orbital rings) behind everything. No boxes, no gradients, no idle/decorative motion. Every mark on
screen either carries information or is a star.

## 2. Color tokens
Grayscale ground; violet is a highlight only. Hex is sRGB.

| Token | Hex | Use |
|---|---|---|
| `void` | `#070708` | screen background |
| `pit` | `#0D0D0F` | raised/selected surface (used sparingly; UI is mostly flat on `void`) |
| `pit-on` | `#121215` | pressed/active row wash |
| `edge` | `#1C1C20` | hairlines, the dashed header rule |
| `edge2` | `#28282D` | hover hairline |
| `ash` | `#42424A` | dim text, idle glyphs, chevrons |
| `steel` | `#78787F` | secondary text, module icons at rest |
| `bone` | `#BFBFC4` | primary text (module names, clock) |
| `white` | `#E4E4E8` | titles, emphasis |
| `violet` | `#655578` | **accent** — active tab, back arrow, running module, ~1 star |
| `violet-dk` | `#1E1A26` | violet-tinted fill/border (rare) |
| `gold` | `#C9AD82` | reserved alternate accent (not used in the default theme; see §12) |

**LVGL color-order gotcha (carried from M3, load-bearing):** the panel runs **24-bit RGB888 with
`rgb_ele_order = BGR`**, and LVGL v9's `lv_color_t` is `{b,g,r}` in memory. Define colors with
`lv_color_make(r,g,b)` / `lv_color_hex(0xRRGGBB)` as normal — the BGR handling lives in the panel/flush
config already set up in `ui.c` (M3). Do **not** hand-swap bytes in widget code. Keep
`CONFIG_LV_COLOR_DEPTH_24=y` (note: `=y`, a bare `=24` is silently ignored — M3 finding).

## 3. Typography
Two families. **Serif display for titles, monospace for everything else.** This pairing is the identity —
do not collapse it to one family.

| Role | Family | Size / style | Token in mockup |
|---|---|---|---|
| Boot wordmark | serif | 30 px, letter-spacing ~0.2em | `.boot .bt` |
| Screen title | serif | 21 px, near-0 tracking, sentence case | `.head .ttl` |
| Section caption | serif **italic** | 13 px | `.sub` |
| Clock | mono | 15 px, tabular figures | `.head .clk` |
| Battery | mono | 13 px, tabular | `.head .bat` |
| Module name | mono | 14 px, 0.05em | `.row .nm` |
| Status tag | mono | 11 px, 0.08em | `.row .tg` |
| Scan/USB data | mono | 12–12.5 px, tabular | `.ap`, `.iface` |

**Serif face:** the mockup uses a stack (`Recia`, `Boska`, → Georgia fallback). Ship the real face:
**Recia** or **Boska** (both free from Fontshare; high-contrast editorial serifs matching *linux-antiquity*).
**Mono face:** any clean mono (Monaco / JetBrains Mono / the SF-Mono metrics used in the mockup).

**LVGL font embedding:** convert with `lv_font_conv` (or the online LVGL converter), **4 bpp**, ASCII
0x20–0x7E plus `°·✦–` and any glyphs used. Generate only the sizes actually used to save flash:
serif **30** (boot) and **21** (titles) regular + serif **13 italic** (captions); mono **11, 13, 14, 15**.
Register as `lv_font_t` and set per-widget via `lv_obj_set_style_text_font`. Titles that would otherwise
render in the default Montserrat must explicitly select the serif font — do not rely on a global default.

## 4. Layout & spacing
- **Canvas:** 410×502, portrait. Origin top-left; native touch coords already match (M2 — no rotation).
- **Horizontal safe inset:** content lives within **24 px** left/right (list/`sub`/header use 24–26 px).
- **Header:** 26 px top padding, ~64 px tall, with a **1 px dashed `edge` rule** beneath (inset 20 px each
  side). The dashed rule is the one recurring antique motif — keep it.
- **List rows:** 16 px vertical padding, 16 px gap between icon and name. **No row dividers, no boxes.**
  Rhythm comes from the aligned icon column (left) and chevron column (right) plus whitespace.
- **Curved corners (physical):** the AMOLED is a flat rectangle, but the watch glass rounds the four
  corners. Keep all interactive/text content within the 24 px safe inset (already satisfied) **and** draw
  four opaque `void` rounded-corner masks (≈40 px radius) on the top LVGL layer so any full-bleed layer
  (the star background) doesn't bleed under the curve. Do **not** round the framebuffer itself.
- **Tap targets:** every actionable row ≥ 44 px tall (current rows clear this). Touch is the only input —
  the hardware buttons are RST/BOOT/PWR only, so there is **no focus cursor / no selected-row highlight**.

## 5. Navigation model
A **screen stack**, not tabs.
- **Home** = list of categories: WiFi · NFC · Bluetooth LE · USB Gadget · System.
- Tapping a category pushes its **submenu** (module list). Tapping an actionable module pushes an **action
  screen** (e.g. WiFi → Scan). A **back arrow** (top-left, `violet`) pops one level. Home has no back.
- **Transition:** directional slide + fade, ~240 ms. Forward = new screen in from the right; back = in from
  the left. This is the only "navigational" motion and it encodes hierarchy — keep it, keep it subtle.
- Implement as a stack of LVGL screens or a container with swappable child views; animate with
  `lv_obj_set_style_translate_x` + opacity, or `lv_scr_load_anim(..., LV_SCR_LOAD_ANIM_MOVE_LEFT/RIGHT, 240, 0)`.

## 6. Components
**Status bar (header).** Left: back arrow (submenus/actions only) + serif title. Right: mono clock + mono
battery %. Dashed `edge` rule beneath. Home header shows only clock + battery (no title, no name — the
firmware name never appears in the running UI, only on boot).

**Menu row.** `[icon] name … [tag?] [chevron]`. Icon `steel` (→`violet` on press), name `bone` (→`white`),
chevron `ash`. No background, no border. Press feedback = brighten name/icon (+ optional `pit-on` wash).

**Status tag (right-aligned, before chevron).** Show a tag **only** when there is something to say:
`ready` (`steel`), `running` (`violet`), or a count/short value like `14` or `ch 6`. **Never render an
`idle` tag** — idle modules show no tag. This keeps the eye on what's live.

**Section caption (`sub`).** Italic serif, `steel`, under the header on submenus — e.g. *802.11 · 2.4 GHz*,
*13.56 MHz · ST25R3916*, *Composite · CDC · MSC · HID*.

**Action screens** (the only screens with live motion — see §8):
- *WiFi → Scan:* a status line (`scanning… / N networks`, `ch` right), a 1 px **sweep** bar (a `violet`
  segment traversing left→right on loop), and a list of discovered APs (signal bars + SSID + `ch·rssi` +
  lock/open glyph) that **reveal progressively** as they're found.
- *USB Gadget:* a caption, a 4-row interface readout (`phy / cdc / msc / hid`), and a bracket-text control
  `[ start gadget ]` ⇄ `[ stop gadget ]`. Starting plays the **enumeration handoff** (§8).

**Boot screen.** Full-screen `void`. Engraved `violet` star, serif `NOCSIF` wordmark, then a `[ ok ]`
bring-up log that reveals line by line, ending on `ready`, then fades to Home. See §8.

**Background layer.** Behind all screens (z-below): the **orrery** — 2 sets of concentric + dashed circles
bleeding off the top-right and bottom-left corners (`#3C3C44`, ~0.2 opacity), scattered gray star dots
(`#B6B6BD`, 0.2–0.4 opacity), and 4 engraved stars (mostly gray, **one** `violet`). Static. Render once to
a layer/canvas; it must not repaint per frame (power).

## 7. Iconography
Thin-stroke line icons, `currentColor`, ~1.3–1.5 px stroke at 21–24 px, rounded joins. One icon per module:
wifi, nfc, ble, usb, sys (gear), scan, monitor (waveform), access-point, captive-portal, handshake,
drive/SD, lock/open, key-recovery (lock), back-chevron, right-chevron, and the **engraved star** (a
four-point celestial star drawn as a thin outline with concave sides — used for boot, About, and the
background). Store as small SVG→LVGL image descriptors or draw with `lv_canvas`/vector where practical; the
star and chevrons can be LVGL images. Keep the star's hand-drawn outline character — do not substitute a
geometric sparkle.

## 8. Motion (functional only)
Rule: **animation exists only to show real firmware behavior or navigation.** No ambient/idle animation
(power + the design brief). All timed reveals must be driven by real task state where the state exists;
use the timings below only as pacing when the underlying op is fast.

| Animation | Mirrors | Spec |
|---|---|---|
| **Boot log** | actual bring-up order (`HARDWARE.md`) | reveal lines in order: power rails (AXP2101) → display (CO5300) → touch (CST9217) → storage → USB composite → `ready`; ~230 ms/line; fade out ~550 ms. Drive from real init callbacks when available. |
| **Scan sweep + discover** | promiscuous WiFi scan | sweep bar loops ~1.7 s while scanning; each discovered AP row fades/rises in (~220 ms) as the scan task reports it; header count increments; stop sweep when scan completes. |
| **USB enumeration handoff** | deferred TinyUSB install (P2/P3) | on `[ start gadget ]`: `phy serial/jtag` → `released`, then `cdc`→`com`, `msc`→`/sd`, `hid`→`ready` light up in sequence (~380 ms apart), driven by real `tud_mount`/interface-ready events. Reverse on stop. |
| **Nav slide** | screen hierarchy | 240 ms directional slide+fade (§5). |
| **Cursor blink** (boot/`ready`) | terminal idiom | 1.05 s steps blink; boot only. |

Respect a global reduced-motion / power flag: collapse all reveals to instant, keep the final state.

## 9. State bindings (what real data drives the UI)
- **Clock** ← RTC **PCF85063A** (0x51). **Battery %** ← AXP2101 (0x34).
- **Running tags** ← the owning capability task's state (e.g. WiFi monitor task active → `running` on the
  WiFi home row and the Monitor submenu row). Home-row tag reflects "any module in this category running."
- **Counts** (e.g. NFC `saved cards 14`) ← live `/sd` listing.
- **Scan list** ← the scan task's result callback (SSID/ch/rssi/auth), not a static list.
- **USB interfaces** ← TinyUSB enumeration state; the `[ start/stop ]` control signals the gadget worker
  task (never blocks the LVGL callback — clone the M4 `xTaskNotifyGive` pattern).
- **All heavy work (radio/SD/USB) runs off the LVGL task**; UI callbacks only signal workers and hold
  `lvgl_port_lock()` when mutating widgets (M3 rule).

## 10. LVGL v9 implementation notes (reuse M3)
- Build on the existing **`esp_lvgl_port` 2.8 + lvgl 9.3** setup in `firmware/src/ui.c` (M3). Keep
  `buff_dma=false` + `color_format = LV_COLOR_FORMAT_RGB888` + the **2-px rounder_cb** (`x1&=~1; y1&=~1;
  x2|=1; y2|=1`) — CO5300 needs even column/row alignment or partial flushes shear (M3 finding).
- Partial draw buffers stay in **internal RAM**; the star background layer can live in PSRAM (framebuffer
  precedent from M1). Render the orrery once; don't invalidate it every frame.
- Prefer LVGL built-in widgets: `lv_list`/flex `lv_obj` rows, `lv_label`, `lv_image` (icons), `lv_bar` or a
  thin animated `lv_obj` for the sweep. Style via a shared `lv_style_t` set keyed to the §2 tokens so a
  theme swap is one place (see §12).
- Touch indev already feeds native coords (M2/M3). Keep ~33 Hz poll or move to INT (GPIO12) later.

## 11. Screen map → capability milestones
The UI is the front-end for the M5+ features in `docs/research/capability-expansion-2026-08-08.md`.
Build a category's submenu when its milestone lands; stub un-built modules as present-but-disabled.

| Home category | Submenu modules | Milestone |
|---|---|---|
| WiFi | scan networks, monitor, access point, captive portal, handshake | **M5** |
| NFC | read tag, write/clone, emulate, saved cards, key recovery | **M6** |
| Bluetooth LE | scan devices, advertise, hid keyboard, beacon | **M7** |
| USB Gadget | (gadget action: cdc/msc/hid + start/stop) | **M4 (done)** + extends |
| System | display, power, storage, about | anytime (M11 polish) |

GNSS/LoRa (M8/M9) get their own categories when built. Keep module names in neutral device-class terms.

## 12. Open decisions / future
- **Serif face:** Recia vs Boska (both fit). Decide at font-embed time; pick whichever renders cleaner at
  21 px on-panel.
- **Theme variants:** *linux-antiquity* ships selectable deity-named palettes (each = one accent over the
  dark ground). NocSif could do the same later — the token set in §2 is structured for it (swap `violet`
  for `gold`, etc.). A "System → Theme" picker is a natural M11 add. Not required for v1.
- **CRT scanline overlay:** present in the mockup as texture; optional on-device (costs a full-screen
  overlay). Default **off** for power; revisit only if wanted.
- **Serif in the browser mockup** currently falls back to Georgia; embedding the real face is done at
  build time per §3, so on-device titles will look a notch more refined than the mockup renders.
