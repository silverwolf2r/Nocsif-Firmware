# NocSif Firmware — Code-Organization & Standardization Catalog

This document catalogs **singular values, constants, styles, and small boilerplate patterns that are currently duplicated across screens and modules** and that should be standardized into device-wide globals / tokens — font size, screen size, list-row pool caps, safe-zone insets, palette hexes, NVS keys, task-stack tiers, timing cadences, and so on. The goal is a device that is **visually and behaviorally consistent and single-point-tunable**: change the type scale, the accent color, the corner-safe inset, or a worker stack tier in exactly one place and have it reach every screen.

> **This is a planning / refactor document. No code has been changed.** Every entry records the *current* form (with concrete values and real `file:line` references), the *proposed* single global and the header it should live in, and a migration plan. It is meant to be executed as a series of small, independently verifiable branches — see [Rollout order](#rollout-order).

The catalog was produced by a cross-cutting audit of `firmware/src/`. It reinforces fixes already scheduled in `PLAN.md` (§1 rounded-corner safe zone, §4.1 Theme / type scale, §4.13 type-scale-everywhere) and lessons captured in `LESSONS.md` — see [Correlation with the plan](#correlation-with-the-plan).

---

## How to read this

**Priority** — how much consistency / risk value the item carries:

| Priority | Meaning |
|---|---|
| **high** | Directly blocks a planned feature (type scale, safe zone) or is a proven crash/behavior hazard; do first. |
| **medium** | Real drift or duplicated boilerplate with clear value; do after the high items. |
| **low** | Polish / house-style consistency; interleave freely once the structure exists. |

**Effort** — rough size of the change: `small` (mechanical find/replace, one or few files) · `medium` (a header plus a bounded sweep) · `large` (a broad sweep across many builders, verified per-screen).

**Current state** — how centralized the concept already is:

| State | Meaning |
|---|---|
| **scattered** | No token exists; the value/idiom is open-coded at every site. |
| **partial** | A token or helper exists but is bypassed, forked, or only partially applied. |
| **ok** | Already centralized (used here only as a reference point). |

**Item count:** 40 · **high** 6 · **medium** 16 · **low** 18.

---

## Summary table

Sorted by **priority**, then **category**. "Proposed global (+home)" names the single global and the header it should live in (see [Proposed token homes](#proposed-token-homes)).

### High priority

| Concept | Category | Current state | Proposed global (+home) | Priority |
|---|---|---|---|---|
| Radio internal-DMA coexistence gate (cap mask + per-radio thresholds) | heap/DMA gate | partial | `NOCSIF_DMA_CAPS` + `nocsif_int_dma_largest()` + `NOCSIF_RADIO_MIN_DMA_*` — **coex.h** | high |
| Corner-safe horizontal content inset (`26`) re-hardcoded | layout metric | partial | `NOCSIF_CONTENT_INSET` — **ui_metrics.h** | high |
| Panel dimensions `410×502` duplicated as `BG_W/BG_H` | layout metric | partial | `NOCSIF_DISP_W/H` — **display.h** (delete forks) | high |
| Rounded-corner clearance geometry (`HEADER_HINSET/TOP_PAD/CORNER_R`) file-static | nav/scaffold | partial | `NOCSIF_SAFE_INSET_X/Y`, `NOCSIF_CORNER_R` — **ui_metrics.h** | high |
| NVS namespace + key literals scattered as inline strings | NVS keys | partial | `NOCSIF_NVS_NS*` + `NVS_KEY_*` — **nvs_keys.h** | high |
| Direct font-face assignments bypass the type-scaled shared styles | typography | partial | `nocsif_style_status/body/meta/caption/…` — **ui_theme.{h,c}** | high |

### Medium priority

| Concept | Category | Current state | Proposed global (+home) | Priority |
|---|---|---|---|---|
| Dark surface / card background hexes bypass surface tokens | color | partial | `NOCSIF_CARD/SURFACE_*` — **ui_theme.h** | medium |
| Semantic alert / record red `0xC0392B` has no token | color | scattered | `NOCSIF_ALERT` — **ui_theme.h** | medium |
| Default / seed accent RGB `0x655578` duplicated ×4 | color | partial | `NOCSIF_ACCENT_DEFAULT_RGB` — **ui_theme.h** | medium |
| Active-state / result status-color idiom re-implemented per screen | color | scattered | `nocsif_status_color()` — **ui_theme.h** | medium |
| On-screen keyboard geometry + text-entry + primary button hand-built | layout metric | scattered | `nocsif_attach_keyboard/make_textentry/primary_button` + `NOCSIF_KB_*` — **ui_nav / ui_metrics.h** | medium |
| Row / header chrome metrics duplicated (nav macros vs theme literals) | layout metric | partial | `ROW_MIN_H/VPAD/ICON_GAP`, `HEADER_H/PAD/GAP` — **ui_metrics.h** | medium |
| LVGL fixed-row pool caps ad hoc per screen (`32/8/40/6/5`) | list & row pool | partial | `NOCSIF_LIST_ROWS/…_SM/PICK_ROWS/POOL_MAX_ROWS` — **ui_metrics.h** | medium |
| Lock-free status/readout double-buffer triplicated across radios | list & row pool | scattered | `nocsif_pubstr_publish()` — **nocsif_pubstr.h** | medium |
| Flat-row / clickable-action-row construction boilerplate duplicated | nav/scaffold | scattered | `nocsif_make_flat_row()`, `nocsif_menu_add_action_row()` — **ui_nav** | medium |
| Status-line / footnote / "more page" footer builders duplicated | nav/scaffold | scattered | `nocsif_status_label/footnote/livelist_more_footer` — **ui_nav** | medium |
| Shared SPI3 bus pinout, CS pins, `max_transfer_sz` redefined per device | nav/scaffold | scattered | `NOCSIF_SPI3_*` — **board_pins.h** | medium |
| Repeated user-facing status / message strings (no string table) | strings | scattered | `NOCSIF_STR_*` / `MSG_*` — **ui_strings.h** | medium |
| Shared text-glyph tokens (dot / ellipsis / dashes) forked & bypassed | strings | partial | `NOCSIF_DOT/DEG/NDASH/EMDASH/ELL` — **nocsif_glyphs.h** | medium |
| FreeRTOS worker stack sizes / priorities / queue depth magic numbers | task stack | scattered | `NOCSIF_STACK_*`, `NOCSIF_PRIO_*`, `NOCSIF_WORKER_QLEN` — **nocsif_task.h** | medium |
| Audio format contract (`16 kHz`, 44-byte WAV, int16 full-scale) split | threshold | scattered | `NOCSIF_AUDIO_SR/WAV_HDR_BYTES/PCM16_*` — **nocsif_audio_fmt.h** | medium |
| Brightness / power ladder and battery sentinels scattered | threshold | partial | `NOCSIF_LEVEL_MAX`, brightness ladder, `NOCSIF_BATT_*` — **power.h** | medium |

### Low priority

| Concept | Category | Current state | Proposed global (+home) | Priority |
|---|---|---|---|---|
| Hairline / rim border `0x2b2b31` (third near-dup of EDGE/EDGE2) | color | partial | reuse `NOCSIF_EDGE2` or add `NOCSIF_EDGE3` — **ui_theme.h** | low |
| Remaining off-palette grays & web-hex drift | color | partial | `NOCSIF_RING/STARDOT/ROW_DIVIDER` — **ui_theme.h** | low |
| Corner-radius scale (`18/14/8/6/4/3`) as bare numbers | layout metric | scattered | `NOCSIF_RADIUS_CARD/PILL/BTN/BAR/SM` — **ui_theme.h** | low |
| Button / control geometry (`44/46/50`, `pad 11/15`) hardcoded | layout metric | scattered | `UI_BTN_H/CTRL_H/BTN_PAD_VER` — **ui_metrics.h** | low |
| Progress meter bar and modal scrim+card rebuilt per screen | layout metric | scattered | `nocsif_meter_bar()`, `nocsif_modal_scrim/card()` + `UI_SCRIM_OPA` — **ui / ui_nav** | low |
| Shared I2C bus params (`400 kHz`, `100 ms`, 7-bit addr) redefined per driver | misc | scattered | `NOCSIF_I2C_HZ/TIMEOUT_MS` — **i2c_scan.h** | low |
| Shared geo/math constants (`DEG2RAD`, `111320`, `PI`, geofence radius) duplicated | misc | scattered | `NOCSIF_DEG2RAD/M_PER_DEG/PI/GEOFENCE_RADIUS_M` — **nocsif_geo.h** | low |
| DMA/cache-line `64`-byte alignment & buffer sizes as bare literals | misc | partial | `NOCSIF_DMA_ALIGN`, `NOCSIF_ID_MAX`, `NOCSIF_LOG_LINE_MAX` — shared header | low |
| Saturating-counter idiom, age sentinel, comet cadence local | misc | partial | `COUNTER_SAT16()`, `NOCSIF_AGE_UNKNOWN`, `NOCSIF_STREAK_*` | low |
| SD content base paths and default AP IP open-coded | strings | partial | `NOCSIF_SD_MOUNT/BASE`, `NOCSIF_AP_DEFAULT_IP` — **sdcard.h** | low |
| Product brand `NocSif` / default device name re-typed per module | strings | scattered | `NOCSIF_BRAND/DEVICE_NAME_DEFAULT/HOSTNAME` — branding header | low |
| MAC-address format string hand-written per site | strings | scattered | `NOCSIF_MACSTR` / `NOCSIF_MAC2STR()` — shared header | low |
| Gesture / tap / swipe thresholds re-hardcoded and inconsistent | threshold | partial | `NOCSIF_TAP_SLOP/SWIPE_*/…` — **ui_gesture.h** | low |
| RSSI / signal-hunt thresholds and sentinels overloaded | threshold | partial | `NOCSIF_RSSI_NONE`, `LORA_RSSI_*`, `NOCSIF_HUNT_RSSI_EMA_*` | low |
| WiFi 2.4 GHz band bounds (`1..13`, 14-slot) & crypto field sizes re-encoded | threshold | scattered | `WIFI_CHAN_MIN/MAX/COUNT`, `WPA_*_LEN` — **wifi.h** | low |
| LVGL per-screen refresh tick cadences as bare literals | timing/interval | scattered | `UI_TICK_FAST/METER/LIST/SLOW/MINUTE_MS` — **ui_metrics.h** | low |
| SD lock and USB `claim_sd` timeouts chosen ad hoc | timing/interval | scattered | `NOCSIF_SD_LOCK_MS_*`, `NOCSIF_SD_CLAIM_MS` — **sdcard.h / usb_gadget.h** | low |
| Worker pacing / settle / poll delays and dwell/confirm windows inline | timing/interval | partial | `NOCSIF_HID_READY_MS/POLL_TICK_MS/RAIL_SETTLE_*` | low |

---

## Color & Typography

### [high] Direct font-face assignments bypass the type-scaled shared styles
- **Current form.** Hundreds of labels call `lv_obj_set_style_text_font(x, &nocsif_mono_11/12/13/14/15/16/18 | &nocsif_serif_20i/21/23 | &nocsif_num_48)` inline. The System > Theme type scale re-points **only** `nocsif_style_title` + `nocsif_style_row_name` in `typescale_apply()`, so ~300+ hard-bound labels never reflow.
- **Current state.** partial — shared type-scaled styles exist in `ui_theme.c`, but only 2 are registered with the type-scale; every other label pins a concrete face.
- **Proposed global.** A small set of role styles in **ui_theme.{h,c}** — `nocsif_style_status/body/meta/caption/footnote/hero_num/note`, each pairing a font with its companion color — all registered in `typescale_apply()` so `nocsif_typescale_set()` reflows them. Screens call `lv_obj_add_style()` instead of naming a face.
- **Refs.** `firmware/src/ui.c:3422`, `firmware/src/ui.c:7443`, `firmware/src/ui.c:9917`, `firmware/src/ui.c:14041`, `firmware/src/ui.c:18778`, `firmware/src/ui_nav.c:480`, `firmware/src/ui_nav.c:688`.
- **Migration.** (1) Add role styles (font+color) in `ui_theme.c` and register each in `typescale_apply()` next to title/row_name (extend the `s_ts_*` tables to cover status/meta/caption/hero tiers). (2) Sweep every `build_*` in `ui.c` + `ui_nav.c`, replacing `lv_obj_set_style_text_font(...&nocsif_*)` with `lv_obj_add_style(x,&nocsif_style_ROLE,0)`, grouped by role. (3) Verify: flip System > Theme type scale Compact/Default/Large on-device and confirm watchface, Home, lists, settings **and** recon-screen data labels all grow (PLAN §4.13 acceptance).

### [medium] Dark surface / card background hexes bypass the surface tokens
- **Current form.** Near-black card/overlay fills spelled as raw hex — `0x0d0d11`, `0x161619`, `0x121216`, `0x0b0b0e`, `0x101015`, `0x070709`, `0x050506`, `0x241f38` — at many widget sites, while `NOCSIF_PIT/PIT_ON` exist.
- **Current state.** partial — `NOCSIF_VOID/PIT/PIT_ON` tokenized; raised-surface tints escaped.
- **Proposed global.** A surface-elevation set in **ui_theme.h** — `NOCSIF_CARD/SURFACE_0/SURFACE_1/SURFACE_SUNK/SURFACE_CELL` (+ `NOCSIF_VOID_DEEP 0x050506` for the stars-only ground).
- **Refs.** `firmware/src/ui.c:1493`, `firmware/src/ui.c:7350`, `firmware/src/ui.c:8343`, `firmware/src/ui.c:16688`, `firmware/src/ui.c:18452`, `firmware/src/ui.c:18539`.
- **Migration.** (1) Add surface tokens to ui_theme.h §2 palette. (2) Replace `lv_color_hex(0x..)` card/overlay/CC/media/cell fills with the nearest token (collapse the `0x121215/0x121216/0x161619` variants onto one `CARD` unless a difference is intentional). (3) Verify cards/overlays render unchanged and a `NOCSIF_CARD` tweak recolors all of them.

### [medium] Semantic alert / record red `0xC0392B` has no token
- **Current form.** The one warm status color (timer time's-up hero, recording dot) is a raw hex literal at every use — the lone bypass of the `NOCSIF_*` palette.
- **Current state.** scattered — no token.
- **Proposed global.** `#define NOCSIF_ALERT lv_color_hex(0xC0392B)` in **ui_theme.h** alongside `NOCSIF_GOLD`.
- **Refs.** `firmware/src/ui.c:10282`, `firmware/src/ui.c:19034`.
- **Migration.** (1) Add the token to ui_theme.h §2. (2) Replace both uses (and any future alert/record uses). (3) Verify timer-expiry text and the record dot unchanged.

### [medium] Default / seed accent RGB `0x655578` duplicated across theme + wallpaper seeds
- **Current form.** The default violet is hardcoded four times: `ui_theme.c` `palette[0]`, `s_accent_cur` init, the `ui.accentc` NVS default, and `ui_background.c`'s wallpaper-star seed default — a retune must change all in lockstep or the persisted accent and star drift.
- **Current state.** partial — `NOCSIF_VIOLET` is a runtime token but its seed value is inline magic.
- **Proposed global.** `#define NOCSIF_ACCENT_DEFAULT_RGB 0x655578u` in **ui_theme.h**; single source for `palette[0]` and every NVS default seed.
- **Refs.** `firmware/src/ui_theme.c:32`, `firmware/src/ui_theme.c:40`, `firmware/src/ui_theme.c:199`, `firmware/src/ui_background.c:430`.
- **Migration.** (1) Add the macro. (2) Replace all four literals. (3) Verify a first-boot (cleared NVS) shows the same accent and wallpaper star color.

### [medium] Active-state / result status-color idiom re-implemented per screen
- **Current form.** The "running = accent, idle = steel" (and "results = gold") coloring is re-expressed identically in ~15 ticks (`active ? NOCSIF_VIOLET : NOCSIF_STEEL`; `n>0 ? NOCSIF_GOLD : …`), encoding a UI convention with no single source.
- **Current state.** scattered — duplicated, easy to drift (a GOLD/VIOLET/STEEL triple mismatch already exists).
- **Proposed global.** An inline `nocsif_status_color(has_results, active)` (or a `NOCSIF_STATUS_COLOR` macro) in **ui_theme.h** returning the gold/accent/steel triad; a wifi/ble `set_status_active(lbl, …)` setter.
- **Refs.** `firmware/src/ui.c:3542`, `firmware/src/ui.c:3990`, `firmware/src/ui.c:6194`, `firmware/src/ui.c:7769`, `firmware/src/ui.c:8020`.
- **Migration.** (1) Add the helper. (2) Replace the per-screen ternaries in the WiFi/BLE/hunt ticks. (3) Verify status labels colorize identically across screens.

### [low] Hairline / rim border `0x2b2b31` (third near-dup of EDGE/EDGE2)
- **Current form.** A dim rim/border gray `0x2b2b31` hardcoded for alert rim, planet/ring border, and the peek activity pill — one shade off the existing `NOCSIF_EDGE2` (`0x28282D`).
- **Current state.** partial — EDGE/EDGE2 hairline family exists; a third value is inline.
- **Proposed global.** Reuse `NOCSIF_EDGE2`, or add `NOCSIF_HAIRLINE/EDGE3` (`0x2b2b31`) in **ui_theme.h** if the lighter value is intentional.
- **Refs.** `firmware/src/ui.c:1300`, `firmware/src/ui.c:15559`, `firmware/src/ui.c:17200`, `firmware/src/ui.c:18865`.
- **Migration.** (1) Decide EDGE2 vs a new EDGE3 token; add if needed. (2) Replace the four `0x2b2b31` borders. (3) Verify rims/pills unchanged.

### [low] Remaining off-palette grays and web-hex drift
- **Current form.** `RING_COLOR 0x3C3C44` / `DOT_COLOR 0xB6B6BD` (`ui_background.c`), flashlight hint `0x9A9AA0`, row divider `0x101013` (in `ui_theme.c` itself), `NOCSIF_VOID` re-encoded as channels `(7,7,8)` in `blend_over_void`, and captive-portal HTML `#0d0d10/#d8d8dc`.
- **Current state.** partial — palette centralized but these leaked out.
- **Proposed global.** Promote to **ui_theme.h** tokens (`NOCSIF_RING/STARDOT/ROW_DIVIDER`) or reuse STEEL/BONE for the hint; read `NOCSIF_VOID` channels rather than literal `7,7,8`; inject the theme bg/text into the portal template.
- **Refs.** `firmware/src/ui_background.c:68`, `firmware/src/ui_background.c:69`, `firmware/src/ui_background.c:100`, `firmware/src/ui_theme.c:157`, `firmware/src/ui.c:18227`, `firmware/src/wifi.c:3395`.
- **Migration.** (1) Add the missing gray tokens; replace RING/DOT/divider/hint. (2) Make `blend_over_void` read `NOCSIF_VOID`'s channels (or a `NOCSIF_VOID_R/G/B` triple). (3) Inject palette hex into the portal HTML. (4) Verify wallpaper, divider, flashlight, and served page unchanged.

---

## Layout metrics

### [high] Corner-safe horizontal content inset (`26`) re-hardcoded instead of the existing token
- **Current form.** The rounded-corner-safe content inset is written as a raw `26` (with a `/* corner-safe inset */` comment) at ~44 sites across `ui.c`; `build_voice` uses a stray `20`. `UI_LIST_INSET=26` already exists (`ui.c:154`) and duplicates `ui_nav.c` `LIST_INSET=26`.
- **Current state.** partial — the token exists and is honored by `nocsif_content_line/keypad/pin`, but direct-content screens re-hardcode `26` and one uses `20`.
- **Proposed global.** One safe-zone inset token in **ui_metrics.h** (`NOCSIF_CONTENT_INSET = 26`) consumed everywhere; ideally applied once inside `nocsif_screen_scaffold()`. Delete the `UI_LIST_INSET` / `LIST_INSET` duplication.
- **Refs.** `firmware/src/ui.c:154`, `firmware/src/ui_nav.c:36`, `firmware/src/ui.c:3416`, `firmware/src/ui.c:4506`, `firmware/src/ui.c:6258`, `firmware/src/ui.c:10811`, `firmware/src/ui.c:14574`, `firmware/src/ui.c:14794`.
- **Migration.** (1) Move `LIST_INSET` into ui_metrics.h as `NOCSIF_CONTENT_INSET`; both `ui_nav.c` and `ui.c` include it; delete `UI_LIST_INSET`. (2) `grep -n 'pad_hor(content, 26'` and the stray `20` in `build_voice`; replace with the token. (3) Consider setting `pad_hor` once in `nocsif_screen_scaffold` and dropping per-builder calls. (4) Verify no left/right clipping on a scaffold screen and that Voice now aligns with siblings.

### [high] Panel dimensions `410×502` duplicated as `BG_W/BG_H` instead of `NOCSIF_DISP_W/H`
- **Current form.** `ui_background.c` re-`#define`s `BG_W 410` / `BG_H 502` for the orrery canvas, corner masks, and star geometry, while `display.h` already defines the canonical `NOCSIF_DISP_W/NOCSIF_DISP_H` that `ui_nav.c` consumes.
- **Current state.** partial — canonical tokens exist in display.h; ui_background.c forks them.
- **Proposed global.** Use `NOCSIF_DISP_W / NOCSIF_DISP_H` from **display.h** everywhere; delete `BG_W/BG_H`.
- **Refs.** `firmware/src/display.h:21`, `firmware/src/display.h:22`, `firmware/src/ui_background.c:62`, `firmware/src/ui_background.c:63`, `firmware/src/ui_background.c:452`.
- **Migration.** (1) Include display.h in ui_background.c. (2) Replace `BG_W/BG_H` with `NOCSIF_DISP_W/H`; delete the local defines. (3) Verify wallpaper/orrery/corner-mask render unchanged on-device.

### [medium] Row / header chrome metrics duplicated between ui_nav.c macros and ui_theme.c literals
- **Current form.** `ROW_MIN_H 44` / `ROW_VPAD 15` / `ROW_ICON_GAP 16` are named macros in `ui_nav.c`, but `ui_theme.c`'s `nocsif_style_row` (which actually applies them) repeats `44/15/16` as raw literals; header chrome (`pad_bottom 16`, `min_height 64`, `pad_column 12`, badge paddings) and the row icon column width `24` are bare literals too.
- **Current state.** partial — row metrics named in one file, re-typed in the file that applies them.
- **Proposed global.** Move `ROW_MIN_H/VPAD/ICON_GAP` + `HEADER_H/PAD/GAP` + `UI_ROW_ICON_W` into **ui_metrics.h**; `ui_theme.c` and `ui_nav.c` both consume them.
- **Refs.** `firmware/src/ui_nav.c:50`, `firmware/src/ui_theme.c:151`, `firmware/src/ui_nav.c:427`, `firmware/src/ui_nav.c:430`, `firmware/src/ui.c:859`.
- **Migration.** (1) Define the metrics in ui_metrics.h; delete the ui_nav.c copies. (2) Point `nocsif_style_row` init and `build_header` at the tokens; give ad-hoc row builders `UI_ROW_ICON_W`. (3) Verify row height/padding and header footprint unchanged.

### [medium] On-screen keyboard geometry + text-entry + primary-action button hand-built per screen
- **Current form.** The keyboard-attach block (size `pct100×196`, align `BOTTOM_MID -58`, VOID bg, BONE text, pad 3), the text-entry textarea style (PIT bg, BONE mono_16, EDGE2 border, radius 6, VIOLET cursor), and the accent Apply/Save pill (VIOLET_DK fill + VIOLET border, radius 8, pad_ver 11, WHITE mono_16) are copied verbatim across ~9 text-entry and many action screens; the `-58` clears the corner arc.
- **Current state.** scattered — `ota_button/gnss_pill` are partial local helpers; keyboard/textarea/most buttons are inline.
- **Proposed global.** `nocsif_attach_keyboard(scr, ta, ready_cb, cancel_cb)`, `nocsif_make_textentry(parent, initial)`, `nocsif_primary_button(parent, label, cb, ud)` in **ui_nav / ui**, with `NOCSIF_KB_H / NOCSIF_KB_BOTTOM_OFF` tokens (offset co-located with safe-zone metrics).
- **Refs.** `firmware/src/ui.c:2125`, `firmware/src/ui.c:4827`, `firmware/src/ui.c:4868`, `firmware/src/ui.c:5208`, `firmware/src/ui.c:12082`, `firmware/src/ui.c:12106`, `firmware/src/ui.c:14336`.
- **Migration.** (1) Add the three builders + KB tokens. (2) Replace the keyboard blocks (Notes, Watch Name, beacon/AP SSID, BLE beacon name, WiFi join), textarea recipes, and the Apply/Save pills (fold `ota_button` + `gnss_pill` in). (3) Verify each text-entry screen still clears the corner arc, the textarea + button clear the keyboard, and "save failed" retext still works.

### [low] Corner-radius scale (`18/14/8/6/4/3`) typed as bare numbers
- **Current form.** A radius scale recurs as raw literals: `18` (icon cell, media card), `14` (activity pill, control btn, popup card), `8` (buttons/pills/textarea), `6` (bars/roller), `4` (small bars/keypad), `3` (grab pill).
- **Current state.** scattered.
- **Proposed global.** Radius tokens in **ui_theme.h / ui_metrics.h**: `NOCSIF_RADIUS_CARD (18)`, `_PILL (14)`, `_BTN (8)`, `_BAR (6)`, `_SM (4)`.
- **Refs.** `firmware/src/ui.c:14344`, `firmware/src/ui.c:14387`, `firmware/src/ui.c:15286`, `firmware/src/ui.c:18541`, `firmware/src/ui.c:18620`, `firmware/src/ui.c:18861`.
- **Migration.** (1) Add the radius tokens. (2) Replace `lv_obj_set_style_radius` literals grouped by tier. (3) Visual spot-check cards/pills/bars/keypad unchanged.

### [low] Button / control geometry (height `44/46/50`, `pad_ver 11/15`, widths) hardcoded
- **Current form.** Control heights and pads recur as bare numbers: `tools_btn` (132×46), OTA/voice/save buttons, the hunt control button (44), and popup card (250w/pad18).
- **Current state.** scattered.
- **Proposed global.** `UI_BTN_H (46)`, `UI_CTRL_H (44)`, `UI_BTN_PAD_VER (11)`, card metrics tokens in **ui_metrics.h**, consumed by `nocsif_primary_button`.
- **Refs.** `firmware/src/ui.c:7248`, `firmware/src/ui.c:8341`, `firmware/src/ui.c:9949`, `firmware/src/ui.c:11170`, `firmware/src/ui.c:14809`.
- **Migration.** (1) Add the metric tokens (pair with the primary-button builder). (2) Replace literal heights/pads. (3) Verify tap targets unchanged. Best landed together with the primary_button builder.

### [low] Progress / level meter bar and modal scrim+card rebuilt per screen
- **Current form.** Two composite widgets hand-built repeatedly: the dark-track+violet-fill meter (OTA/mic/voice, ~10 style calls each, differ only in height/radius) and the modal scrim+card (`lv_layer_top`, VOID + OPA_70 scrim, card chrome, dismiss-on-tap).
- **Current state.** scattered.
- **Proposed global.** `nocsif_meter_bar(parent, height)->fill` and `nocsif_modal_scrim()/nocsif_modal_card()` (with a `UI_SCRIM_OPA` token) in **ui.c / ui_nav.c**.
- **Refs.** `firmware/src/ui.c:8330`, `firmware/src/ui.c:8339`, `firmware/src/ui.c:14383`, `firmware/src/ui.c:14632`, `firmware/src/ui.c:14834`.
- **Migration.** (1) Add `nocsif_meter_bar` and the modal helpers. (2) Replace the three meter builds and the popup/overlay chrome. (3) Verify meter fill % and modal dismiss-on-scrim behavior unchanged.

---

## List & row pools

### [medium] LVGL fixed-row pool caps defined ad hoc per screen (`32 / 8 / 40 / 6 / 5`)
- **Current form.** The pooled-row count is re-declared per screen: `AP_ROWS 32` + `LIVE_ROWS 32`, `DUP/BC/SAP/PT 8`, `FILES/NOTES/UI_MACRO 40` (plus the literal `40` baked into two truncation strings), `HUNT_PICK 6/DRONE_PICK 5/NOTIF 6`, `UI_VOICE_MAX 32`, `ALERT_LOG_MAX 6`, wifi `MON_AP/HS/BCN 32` — all sized against the same LVGL object pool.
- **Current state.** partial — `LIVE_ROWS` was deliberately centralized 10→32 (PR #159), but the secondary/detail/pick pools each redefine their own literal.
- **Proposed global.** A documented pool-cap group in **ui_metrics.h**: `NOCSIF_LIST_ROWS (32, display-bound primary)`, `NOCSIF_LIST_ROWS_SM (8)`, `NOCSIF_PICK_ROWS (6)`, `NOCSIF_POOL_MAX_ROWS (40)`; the display-bound recon caps (`MON_AP/HS/BCN`) reference `NOCSIF_LIST_ROWS`; interpolate the cap into the "…first N" truncation strings.
- **Refs.** `firmware/src/ui.c:2881`, `firmware/src/ui.c:3312`, `firmware/src/ui.c:4163`, `firmware/src/ui.c:6455`, `firmware/src/ui.c:11206`, `firmware/src/ui.c:14679`, `firmware/src/wifi.c:68`, `firmware/src/wifi.c:74`.
- **Migration.** (1) Define the cap group in one place with a comment tying it to the 96 KB LVGL pool / PSRAM move. (2) Repoint `AP_ROWS/DUP/BC/SAP/PT/FILES/NOTES/UI_VOICE_MAX/MON_*` and the truncation strings. (3) Keep intentional deviations (`MON_STA/PROBE 48`) explicitly named. (4) Verify long lists still page and no pool exhaustion after a cap-bump test.

### [medium] Lock-free published status/readout double-buffer triplicated across radio modules
- **Current form.** An identical `static char[2][16]/[2][96]` double-buffer plus byte-identical `publish_status()/publish_readout()` swap routines are copy-pasted in `gnss.c`, `lora.cpp`, and `nfc.cpp` (~48 duplicated lines) — duplicated *code*, not just a value.
- **Current state.** scattered — verbatim triplication.
- **Proposed global.** A **nocsif_pubstr.h** helper (`NOCSIF_PUBSTR_STATUS_LEN 16`, `_READOUT_LEN 96`, `nocsif_pubstr_publish()`) embedded by all three workers.
- **Refs.** `firmware/src/gnss.c:48`, `firmware/src/lora.cpp:149`, `firmware/src/nfc.cpp:55`.
- **Migration.** (1) Extract the buffer widths + swap-index publish into nocsif_pubstr.h. (2) Replace the three copies. (3) Verify each module's cached getter (used by Connectivity/Signal-Hunt) still returns fresh strings LVGL-safely.

---

## Nav / scaffold

### [high] Rounded-corner clearance geometry (`HEADER_HINSET/TOP_PAD/CORNER_R`) is file-static and re-derived by hand
- **Current form.** `HEADER_HINSET 56` / `HEADER_TOP_PAD 56` are file-static in `ui_nav.c` and `CORNER_R 40` is file-static in `ui_background.c`, so `ui.c` hand-built screens re-invent corner clearance as bare literals (`pad_top 46/44`, align `54,54`, peek batt `48/44`) that already **disagree** with the scaffold value.
- **Current state.** partial — canonical insets exist but are private; non-scaffold screens copy literals that comment "cf HEADER_TOP_PAD".
- **Proposed global.** Promote `HEADER_HINSET`, `HEADER_TOP_PAD`, `HEADER_INSET`, and `CORNER_R` into a shared **ui_metrics.h** (`NOCSIF_SAFE_INSET_X/Y`, `NOCSIF_CORNER_R`); non-scaffold screens route through `nocsif_screen_scaffold` or reference the tokens and derive clearance from `CORNER_R`.
- **Refs.** `firmware/src/ui_nav.c:48`, `firmware/src/ui_nav.c:49`, `firmware/src/ui_background.c:65`, `firmware/src/ui.c:15367`, `firmware/src/ui.c:15443`, `firmware/src/ui.c:18588`, `firmware/src/ui.c:18821`, `firmware/src/ui.c:19037`.
- **Migration.** (1) Create ui_metrics.h with the four insets + CORNER_R; include from ui_nav.c, ui_background.c, ui.c. (2) Replace ui.c literals (`46/44/54/48`) with the tokens; where a screen needs the header footprint, route it through `nocsif_screen_scaffold`. (3) Verify the lock keypad, boot specimen, record dot, and peek battery all sit at the same corner clearance as scaffold screens (no clipped glyphs).

> **Note.** PLAN §1 records the proven insets as `HEADER_HINSET 48` / `TOP_PAD 42`, while the code now uses `56/56`. Centralizing forces a single reconciled value and stops the two from drifting further.

### [medium] Row / clickable-action-row construction boilerplate duplicated
- **Current form.** The flat-row incantation (`remove_style_all` + row/press styles + scrollbar-off + `SCROLLABLE`-clear + `GESTURE_BUBBLE`) and the "plain row that is a button" idiom (`menu_add_row` + add `CLICKABLE` + `CLICKED` cb) are copy-pasted ~15–23×; forgetting the `SCROLLABLE`-clear or `CLICKABLE` flag is a known LVGL tap/gesture footgun.
- **Current state.** scattered — `nocsif_menu_add_row` exists for scaffold rows, but hand-built rows re-open-code the flags.
- **Proposed global.** `nocsif_make_flat_row(parent)` and `nocsif_menu_add_action_row(list, icon, name, cb, ud)` in **ui_nav.{h,c}**.
- **Refs.** `firmware/src/ui.c:848`, `firmware/src/ui.c:884`, `firmware/src/ui.c:1779`, `firmware/src/ui.c:13751`, `firmware/src/ui.c:14930`, `firmware/src/ui.c:15125`.
- **Migration.** (1) Add the two helpers to ui_nav. (2) Replace the copied 6-line flat-row blocks and the 3-line action-row blocks. (3) Verify taps/gestures still register on pooled/live rows (the memory-noted "clear SCROLLABLE on pooled rows" path).

### [medium] Status-line label, footnote label, and "more/next page" footer builders duplicated
- **Current form.** Three near-identical recipes hand-built across ~8–12 screens each: the top-of-screen status label (mono_12 + STEEL/VIOLET + 100% + WRAP + pad), the bottom footnote (mono_11 + ASH + pad; `files_note()` already encapsulates it but is used only by Files/Notes), and the livelist "more" page footer (pad_ver 12 + centered violet mono_12 + first-tap-select flags).
- **Current state.** scattered — `files_note()` and the scaffold footer exist but are not reused.
- **Proposed global.** `nocsif_status_label(parent, text)`, `nocsif_footnote(parent, text)` (generalize `files_note`), and `nocsif_livelist_more_footer(parent, page_cb, &out_lbl)` in **ui_nav / ui**.
- **Refs.** `firmware/src/ui.c:3443`, `firmware/src/ui.c:4127`, `firmware/src/ui.c:5910`, `firmware/src/ui.c:6261`, `firmware/src/ui.c:11260`, `firmware/src/ui.c:13478`.
- **Migration.** (1) Add the three helpers (footnote = `files_note` generalized). (2) Replace the copied 6–18 line blocks in the BLE/WiFi/hunt/settings builders. (3) Verify status/footnote/footer look identical and the "more" footer's tap-select still pages. These labels should use the **role styles** from the typography item so they also reflow.

### [medium] Shared SPI3 bus pinout, CS pins, and `max_transfer_sz` redefined per device
- **Current form.** The shared SPI3 bus (MOSI34/MISO33/SCK35), the secondary CS pins (NFC CS=4, LoRa CS=36 — each also re-parked high by the other owner), and `max_transfer_sz 4096` are re-`#define`d under different prefixes in `sdcard.c`, `lora.cpp`, `nfc.cpp`; a wiring change or rename on one side silently breaks another's CS parking.
- **Current state.** scattered — board facts defined 2–3×.
- **Proposed global.** A **board_pins.h** with `NOCSIF_SPI3_MOSI/MISO/SCK/HOST`, `NOCSIF_SPI3_CS_SD/_NFC/_LORA`, `NOCSIF_SPI3_MAX_XFER`.
- **Refs.** `firmware/src/sdcard.c:26`, `firmware/src/sdcard.c:32`, `firmware/src/lora.cpp:46`, `firmware/src/lora.cpp:49`, `firmware/src/nfc.cpp:42`, `firmware/src/nfc.cpp:45`.
- **Migration.** (1) Create board_pins.h from HARDWARE.md; include in the three SPI3 owners. (2) Replace the per-device pin/CS/xfer defines; keep the "park neighbor CS high" idiom referencing the shared CS tokens. (3) Verify SD, LoRa, (NFC self-test) still bring up with correct CS parking.

---

## NVS keys

### [high] NVS namespace + key literals scattered as inline strings
- **Current form.** Persisted keys are bare string literals duplicated across each getter/setter pair in `ui.c` (`car.mode`, `tz_home`, `ui.homespin/peekspin`, `scr_timeout`, `dim_presleep`, `mic_sens`, `alm%d`…), `ble.c` (`adv_mode/adv_name/bt_master/ph_*`), `audio.c` (`sound_en/spk_vol/…`); two NVS namespaces are declared independently. A typo silently splits read from write (no compile error, 15-char NVS cap).
- **Current state.** partial — some keys **are** macros (`HOME_CAR_KEY/PEEK_CAR_KEY/DIAL_KEY_L/R/RING_KEY` in ui.c, `K_*` in settings.c/wifi.c, `K_WX_*` in weather.c), but power/mic/audio/ble/ui_theme/pm keys and the indexed formats are raw literals.
- **Proposed global.** One **nvs_keys.h** defining `NOCSIF_NVS_NS`, `NOCSIF_NVS_NS_REL`, and every key/format macro (`NVS_KEY_*` incl. slot formats `wn_s%d/ph_s%d/alm%d`), with a compile-time 15-char guard; every module includes it.
- **Refs.** `firmware/src/ui.c:10494`, `firmware/src/ui.c:12869`, `firmware/src/ui.c:13600`, `firmware/src/ble.c:864`, `firmware/src/audio.c:298`, `firmware/src/pm.c:38`, `firmware/src/settings.c:21`, `firmware/src/reliability.c:40`, `firmware/src/wifi.c:602`.
- **Migration.** (1) Create nvs_keys.h; enumerate every key found (grep `nocsif_settings_get/set` and `nvs_` opens). (2) Replace inline literals with macros, one module at a time (start `audio.c/power.c/mic.c` which have no macros yet, then `ble.c`, then `ui.c` non-carousel keys, then `pm.c`'s raw `dfs_en`). (3) Verify each setting still round-trips (get default == set path) and no key exceeds 15 chars (`static_assert`-able in the header).

---

## Task stacks

### [medium] FreeRTOS worker stack sizes, priorities, and queue depth as scattered magic numbers
- **Current form.** Worker stacks are per-call literals across the tree — `6144` (audio/ble/gnss/mic/imu/usb_gadget/ducky/nfc, with the same "deep SD/FatFs chain" comment repeated), `8192` (lora/ota/weather), `3584` (logbook), `4096` (buttons/wifi), `16384` (LVGL) — plus priority 3-vs-4 and `BLE_CMD_QLEN 6`, all as anonymous magic.
- **Current state.** scattered — memory documents "6144 for SD workers" as a rule, but it is copy-pasted per file.
- **Proposed global.** A **nocsif_task.h** with named tiers: `NOCSIF_STACK_LIGHT (4096)`, `_WORKER_SD (6144)`, `_HEAVY (8192)`, `_LVGL (16384)`; `NOCSIF_PRIO_WORKER (3)` / `_IO (4)`; `NOCSIF_WORKER_QLEN`.
- **Refs.** `firmware/src/audio.c:314`, `firmware/src/gnss.c:921`, `firmware/src/lora.cpp:1026`, `firmware/src/logbook.c:181`, `firmware/src/ble.c:2965`, `firmware/src/buttons.c:194`, `firmware/src/ui.c:19363`.
- **Migration.** (1) Add nocsif_task.h with the tier macros + one comment explaining each. (2) Replace the `xTaskCreate` stack/priority literals module-by-module; move `IMU_TASK_STACK/MIC_TASK_STACK/WX_TASK_STACK` onto tiers. (3) Verify no task-WDT/stack-overflow after a smoke test of each worker (SD write, radio bring-up, LVGL render).

---

## Heap / DMA gates

### [high] Radio internal-DMA coexistence gate (cap mask + per-radio thresholds) copy-pasted per module
- **✅ DONE (RAM remediation Phase 0).** Landed `firmware/src/coex.h` (`NOCSIF_DMA_CAPS`, `nocsif_int_dma_largest()`/`_free()`, `nocsif_log_dma_free()`, `NOCSIF_RADIO_MIN_DMA_BLE/_LORA/_FLOOR`) and routed every ad-hoc `heap_caps_get_largest_free_block(INTERNAL|DMA)` / `_get_free_size` call site in `ble.c`/`wifi.c`/`ui.c`/`main.c` through it. `BLE_MIN_DMA_BLOCK` deleted (bring_up uses `NOCSIF_RADIO_MIN_DMA_BLE`); `LORA_TASK_MIN_DMA` deleted (LoRa gate removed, Phase 1 #8). Build-time drift guard in `coex_guard.c`. *Original finding preserved below.*
- **✅ DONE (RAM remediation Phase 2) — sibling token home `radio_state.{h,c}`.** A companion shared accessor `nocsif_radio_state(nocsif_radio_state_t*)` composes the ONE live radio-state truth (`ble_logical_on` / `ble_controller_resident` / `ble_link_live` / `wifi_sta_on` / `wifi_link_live`) from `ble.c`+`wifi.c`, so the §4.13 Control-Center tiles, the Connectivity hub, and the watchface/planet status labels all read the same source (no divergent local flags). Kept OUT of `coex.h` deliberately — `coex.h` is a dependency-free leaf (heap_caps only), `radio_state` pulls in `ble.h`/`wifi.h`.
- **Current form.** The `heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA)` query and its thresholds are re-typed across modules with three different magic gates: `BLE_MIN_DMA_BLOCK 31744` (ble.c), `LORA_TASK_MIN_DMA 12288` (ui.c), and the historical `24576` (memory) — a wrong value crashes the BT-controller int-WDT.
- **Current state.** partial — each threshold is a named macro in its own module, but the cap-flag mask and log helper are re-typed ~9× in wifi.c alone, and the gate policy has no single home.
- **Proposed global.** A shared coexistence header (**coex.h**) with `NOCSIF_DMA_CAPS (INTERNAL|DMA)`, an inline `nocsif_int_dma_largest()`, a `nocsif_log_dma_free()` helper, and the per-radio thresholds `NOCSIF_RADIO_MIN_DMA_BLE/_LORA` as one table.
- **Refs.** `firmware/src/ble.c:1546`, `firmware/src/ble.c:1555`, `firmware/src/ui.c:6909`, `firmware/src/ui.c:6914`, `firmware/src/wifi.c:917`, `firmware/src/wifi.c:1055`, `firmware/src/wifi.c:1552`.
- **Migration.** (1) Add coex.h with the cap mask, accessor, log helper, and per-radio thresholds. (2) Replace the raw `heap_caps(...INTERNAL|DMA...)` call sites in wifi.c/ble.c/ui.c with the accessor; move `BLE_MIN_DMA_BLOCK/LORA_TASK_MIN_DMA` into the table. (3) Verify on-device the BLE⇄WiFi⇄LoRa Signal-Hunt switch still gates correctly (no `Malloc-failed`/int-WDT), measuring the same quantity everywhere.

> **Load-bearing.** This is the coexistence policy from `[[gotcha-ble-wifi-coexistence]]` / `[[project-ble-release-regression]]` (fragmentation gate ≥ 24576) and the `[[project-f2-signal-hunt]]` bring-up gate. Treat its branch as high-care; test radio switching carefully.

---

## Timing / intervals

### [low] LVGL per-screen refresh tick cadences as bare literals
- **Current form.** Every live-list/monitor/tool screen spawns `lv_timer_create(cb, N, ...)` with `N` an inline literal (`1000` passive lists, `500` active/AP, `300/400/200/700/800/600/100/90/150/10000`), all feeding the same "update pooled rows in place" pattern with no shared knob — relevant given the render-WDT history.
- **Current state.** scattered — `UI_HEADER_TICK_MS/RING_TICK_MS` are named; per-screen ticks are not.
- **Proposed global.** Named cadence tiers grouped with the existing tick macros (**ui_metrics.h**): `UI_TICK_FAST_MS (100)`, `UI_TICK_METER_MS (90/150)`, `UI_TICK_LIST_MS (500/1000)`, `UI_TICK_SLOW_MS (700)`, `UI_TICK_MINUTE_MS (10000)`.
- **Refs.** `firmware/src/ui.c:3297`, `firmware/src/ui.c:3607`, `firmware/src/ui.c:7588`, `firmware/src/ui.c:10633`, `firmware/src/ui.c:14097`, `firmware/src/ui.c:18279`.
- **Migration.** (1) Define the cadence tiers near `UI_HEADER_TICK_MS`. (2) Replace the `lv_timer_create` literals, mapping each to its intent tier (keep true one-offs like the 90 ms dial repaint named). (3) Verify refresh feel/battery unchanged; a power pass can now retune all "slow" screens at once.

### [low] SD lock and USB `claim_sd` timeouts chosen ad hoc per call site
- **Current form.** `nocsif_sdcard_lock()` is passed raw `500/1000/1500/2000/3000` ms at ~30 sites across ui.c/gnss/lora/nfc/ducky/audio/mic/ota with no rule for which tier, and `nocsif_usb_gadget_claim_sd(2000)` is duplicated at four callers.
- **Current state.** scattered.
- **Proposed global.** `NOCSIF_SD_LOCK_MS_SHORT (500)` / `_POLL (1000)` / `_WRITE (3000)` in **sdcard.h** and `NOCSIF_SD_CLAIM_MS (2000)` in **usb_gadget.h**.
- **Refs.** `firmware/src/ui.c:11284`, `firmware/src/ui.c:11603`, `firmware/src/gnss.c:463`, `firmware/src/lora.cpp:374`, `firmware/src/audio.c:201`, `firmware/src/ota.c:83`.
- **Migration.** (1) Add the tiers to sdcard.h/usb_gadget.h. (2) Replace the raw ms at each lock/claim, choosing the tier by intent (opportunistic read vs blocking write vs recursive delete). (3) Verify SD-contended paths (File Share active) still behave.

### [low] Worker pacing / settle / poll delays and dwell/confirm windows as inline literals
- **Current form.** Timing lives as bare `pdMS_TO_TICKS`/us literals: BLE teardown-settle `30/50`, AMS pace `25`, PCAP drain `100`; USB re-enum `150`; rail settle `8/10/50`; HID-ready `1000` (defined twice as `KBD_READY_BUDGET_MS` vs `DUCKY_HID_WAIT_MS`); ~50 Hz poll `20` (twice); user-facing dwell/confirm windows (OTA arm 4 s, snooze 2 min tied to a hint string, delete-arm 3 s, still-sleep 30 s, ring pause 2.5 s duplicated as `RING_PAUSE_US` and `PEEK_SPIN_PAUSE_US`); `USEC_PER_SEC 1e6` recurs ~7× in wifi.c.
- **Current state.** partial — a few named (`AUD_RAIL_SETTLE_MS`, `RING_TICK_MS`); most inline; several value/label duplications.
- **Proposed global.** Group per concern: `NOCSIF_HID_READY_MS`, `NOCSIF_POLL_TICK_MS` (**nocsif_task.h**); per-rail `NOCSIF_RAIL_SETTLE_*_MS` (**power.h**); `USB_REENUM_SETTLE_MS` (usb_gadget.c); `USEC_PER_SEC` (a shared **nocsif_time.h**); and derive UI dwell/confirm hint strings from their duration constants so value and label can't drift.
- **Refs.** `firmware/src/ble.c:1822`, `firmware/src/usb_gadget.c:178`, `firmware/src/hid_kbd.c:28`, `firmware/src/ducky.c:31`, `firmware/src/imu.c:58`, `firmware/src/wifi.c:1482`, `firmware/src/ui.c:16231`, `firmware/src/ui.c:17597`.
- **Migration.** (1) Add the grouped constants; collapse HID-ready and ring-pause duplicates onto one name each. (2) Replace the settle/pace/poll/reenum literals and dwell windows; derive the "2 min"/"within 2s" hint text from the constant. (3) Verify link teardown, USB re-enum, HID readiness, and confirm windows behave; carousel drift feel identical Home vs peek.

---

## Brightness / power

### [medium] Brightness / power ladder and battery sentinels scattered by feature
- **Current form.** The backlight/volume 8-bit full-scale `255` is a magic number across brightness+volume math; the brightness ladder (`CC_BR_MIN 24`, `CC_BR_DFLT 210`, `MOVIE_BRIGHT 12`, `PREDIM_LEVEL 20`, `SAVER_BRIGHT_CAP 90`) is defined far apart by feature so its ordering invariant is invisible; battery sentinels (`-1` unknown at 3 sites, `100` gauge-not-ready, `20%` auto-saver as a UI string + prose) and CPU DFS `240/80` are local.
- **Current state.** partial — some named per-feature; the ladder + full-scale + sentinels are not grouped.
- **Proposed global.** A power/display policy block: `NOCSIF_LEVEL_MAX (255)`, the brightness ladder as one commented table (`BR_MIN/DFLT/PREDIM/MOVIE/SAVER_CAP`), `NOCSIF_BATT_PCT_UNKNOWN (-1)` + `BATT_SAVER_AUTO_PCT (20)` in **power.h**, `NOCSIF_CPU_MAX/MIN_MHZ`.
- **Refs.** `firmware/src/ui.c:13665`, `firmware/src/ui.c:17885`, `firmware/src/ui.c:18233`, `firmware/src/ui.c:18848`, `firmware/src/power.c:92`, `firmware/src/pm.c:13`.
- **Migration.** (1) Group the brightness ladder + full-scale in a display/power header (keep the `min < movie < predim < saver < default` ordering visible). (2) Name the battery `-1/100/20%` sentinels in power.h and have the UI label + prose cite `BATT_SAVER_AUTO_PCT`. (3) Verify brightness policy (predim/saver/movie/wake) and the auto-saver trigger match the displayed threshold.

---

## Thresholds

### [medium] Audio format contract (`16 kHz`, 44-byte WAV, int16 full-scale) split across mic and audio
- **Current form.** `mic.c` writes WAVs that `audio.c` plays back, but the sample rate (`16000`), the 44-byte WAV header, and the int16 clamp/scale constants (`32767/-32768/32768.0`) are defined independently in each file — a change on one side silently breaks playback on the other.
- **Current state.** scattered — shared contract, two owners.
- **Proposed global.** A **nocsif_audio_fmt.h** with `NOCSIF_AUDIO_SR (16000)`, `NOCSIF_WAV_HDR_BYTES (44)`, `NOCSIF_PCM16_MAX/MIN/SCALE`; both mic.c and audio.c include it.
- **Refs.** `firmware/src/audio.c:43`, `firmware/src/audio.c:141`, `firmware/src/mic.c:43`, `firmware/src/mic.c:57`, `firmware/src/mic.c:260`.
- **Migration.** (1) Create nocsif_audio_fmt.h. (2) Replace the per-file rate/header/clamp constants. (3) Verify record→play round-trip on-device (part of the §4.13 voice-memo-too-quiet fix, which also touches the WAV gain path).

### [low] Gesture / tap / swipe thresholds re-hardcoded and inconsistent
- **Current form.** Tap dead-zone is `RING_TAP_DZ 12` (ui.c:16236) yet re-typed as bare `12` at ~5 nearby tap tests, and disagrees with `CAR_TAP_DZ 8` and `DIAL_LP_MOVE 10`; swipe distance/dominance (`dx>55`, `dy<-45`, `*3/2`) are duplicated across dual/ring/bottom peek handlers; the bottom-arc band Y (`330`), trash lift Y (`150`), and menu overscroll tuning (`DAMP/MAX/DZ/SPRING`) are local, so touch feel drifts.
- **Current state.** partial — `RING_TAP_DZ/CAR_TAP_DZ/CC_SWIPE_MIN` are tokens but re-hardcoded and not shared.
- **Proposed global.** A **ui_gesture.h** with `NOCSIF_TAP_SLOP`, `NOCSIF_LP_MOVE_SLOP`, `NOCSIF_SWIPE_H_MIN/V_MIN/DOMINANCE`, and the overscroll set; derive the bottom-arc band/trash Y from the `CAR_BOT_*` geometry.
- **Refs.** `firmware/src/ui.c:15624`, `firmware/src/ui.c:16236`, `firmware/src/ui.c:17062`, `firmware/src/ui.c:17396`, `firmware/src/ui.c:17431`, `firmware/src/ui_nav.c:589`.
- **Migration.** (1) Create ui_gesture.h; unify the tap/long-press/swipe/overscroll constants. (2) Replace the re-hardcoded `12`s, the swipe magic numbers, and derive `330/150` from `CAR_BOT_*`. (3) Verify tap-vs-drag and swipe-to-Home/menu feel identical across Ring/Dual/Bottom + CC pull.

### [low] RSSI / signal-hunt thresholds and sentinels overloaded
- **Current form.** `HUNT_RSSI_MIN/MAX (-100/-40)` double as proximity-map bounds **and** a "no signal" sentinel; LoRa's `-128` NO_READ is a macro in one place but re-typed as bare `-128` in the hunt reset, and `-120/-140` sanity floors recur across three self-tests; the Signal-Hunt EMA alpha (`7/3/10`) is buried inline and re-derived per radio.
- **Current state.** partial — some named; overloaded and re-typed.
- **Proposed global.** Separate a named sentinel (`NOCSIF_RSSI_NONE`) from the map bounds; group `LORA_RSSI_NO_READ/FLOOR_DEFAULT/SANITY_MIN`; one `NOCSIF_HUNT_RSSI_EMA_NUM/DEN` shared by BLE/WiFi/LoRa hunt so proximity "feel" is consistent.
- **Refs.** `firmware/src/ui.c:6453`, `firmware/src/ui.c:7178`, `firmware/src/ble.c:475`, `firmware/src/lora.cpp:395`, `firmware/src/lora.cpp:628`, `firmware/src/lora.cpp:791`.
- **Migration.** (1) Add a distinct `RSSI_NONE` sentinel and the grouped LoRa floors; hoist the EMA alpha to a shared signal-hunt header. (2) Replace the bare `-128/-120/-140` and the inline `7/3/10` across radios. (3) Verify Band-Survey/hunt detection and gradient smoothing unchanged.

### [low] WiFi 2.4 GHz band bounds (`1..13`, 14-slot array) and crypto field sizes re-encoded
- **Current form.** The channel band range `1..13` and its 14-slot tally array are raw numbers at ~9 spots in wifi.c (validation, wrap, clamp, UI string), and the 802.11 key-material widths (nonce 32, MIC 16, PMKID 16) are duplicated across the storage and decoded structs plus offset math.
- **Current state.** scattered.
- **Proposed global.** `WIFI_CHAN_MIN (1)/MAX (13)/COUNT (14)` in **wifi.h** reused for hop/clamp/array/Signal-Hunt; `WPA_NONCE_LEN/MIC_LEN/PMKID_LEN` in wifi.c crypto section.
- **Refs.** `firmware/src/wifi.c:186`, `firmware/src/wifi.c:376`, `firmware/src/wifi.c:1391`, `firmware/src/wifi.c:1537`, `firmware/src/wifi.c:1841`.
- **Migration.** (1) Add the channel + crypto-length macros. (2) Replace the `1/13/14` and `16/32` literals (incl. the hop UI string and the `kf+13/kf+77` offset math). (3) Verify monitor hop/clamp and hc22000/PMKID capture unchanged.

---

## Strings

### [medium] Shared text-glyph tokens (middle dot, ellipsis, dashes) inconsistently applied
- **Current form.** `NOCSIF_DOT` exists (ui_theme.h:72) yet is re-`#define`d as `BLE_DOT` (ble.c) and `RTC_DOT` (rtc.c) and open-coded as raw `\xC2\xB7` across ui.c/wifi.c; there is **no ellipsis token** (ble.c invented `BLE_ELL`, everyone else hardcodes `\xE2\x80\xA6`); em-dash/en-dash bytes are open-coded beside the existing `NOCSIF_NDASH`.
- **Current state.** partial — dot/ndash/deg tokenized but forked and bypassed; ellipsis and em-dash have no token.
- **Proposed global.** A tiny dependency-free **nocsif_glyphs.h** (no LVGL include) holding `NOCSIF_DOT/DEG/NDASH/EMDASH/ELL`; ui_theme.h re-includes it; delete `BLE_DOT/RTC_DOT/BLE_ELL`; replace raw byte sequences.
- **Refs.** `firmware/src/ui_theme.h:72`, `firmware/src/ble.c:76`, `firmware/src/ble.c:77`, `firmware/src/rtc.c:56`, `firmware/src/wifi.c:531`, `firmware/src/ui.c:4514`, `firmware/src/ui.c:13841`, `firmware/src/ui.c:14434`.
- **Migration.** (1) Create nocsif_glyphs.h with all glyph macros; have ui_theme.h include it (existing users unaffected). (2) Delete the forked `BLE_DOT/RTC_DOT/BLE_ELL` and include the glyphs header in ble.c/rtc.c (LVGL-free, so hardware drivers can use it). (3) grep the raw byte sequences (`\xC2\xB7`, `\xE2\x80\xA6`, `\xE2\x80\x94/\x93`) and replace with tokens. (4) Verify separators/ellipses render identically.

### [medium] Repeated user-facing status/message strings (no string table)
- **Current form.** Operator-facing prose is copy-pasted verbatim across screens: WiFi "stopped … suspends the WiFi link" / "authorized testing only" / "Start passive discovery", BLE "releasing WiFi for the radio…" / "BLE disabled (safe mode)", "Microphone unavailable…", "SD unavailable…" / "out of memory" / "… more (showing first 40)" / "This can't be undone.", "Nothing playing", boot line prefixes, and the `--:--`/`--%`/`-- · --` placeholders — wording drift is already visible (media vs drone vs hunt).
- **Current state.** scattered — no central strings; this is also the i18n seam.
- **Proposed global.** A **ui_strings.h** of `NOCSIF_STR_*` / `MSG_*` constants (and placeholder tokens `NOCSIF_PLACEHOLDER_CLOCK/PCT/DASH`) shared by the WiFi/BLE/mic/files/notes/power/rtc modules.
- **Refs.** `firmware/src/ui.c:3531`, `firmware/src/ui.c:6180`, `firmware/src/ui.c:11364`, `firmware/src/ui.c:14579`, `firmware/src/ui.c:18188`, `firmware/src/rtc.c:57`, `firmware/src/power.c:100`.
- **Migration.** (1) Create ui_strings.h; hoist the repeated phrases + placeholders. (2) Replace the duplicated literals (start with the verbatim pairs: SD-unavailable, OOM, truncation, mic-unavailable, radio-safe-mode, placeholders). (3) Verify wording unchanged; this becomes the single edit point if Language/i18n (backlog) lands.

### [low] SD content base paths and default AP IP open-coded
- **Current form.** `/sd` and `/sd/nocsif` (+ subdirs tracks/voice/notes/wifi/ble) are created/opened by literal in gnss/mic/ui, the soft-AP fallback IP `192.168.4.1` is hardcoded in two screens, and PCAP path templates (`/sd/nocsif/wifi|ble/…`) are duplicated as display hints that must match the real writer — while usb_gadget.c already localizes `/sd` as a private `SD_MOUNT_POINT` and ui.c has `UI_PORTAL_DIR`.
- **Current state.** partial — one private mount macro exists; base path and AP IP are inline.
- **Proposed global.** `NOCSIF_SD_MOUNT ('/sd')` + `NOCSIF_SD_BASE ('/sd/nocsif')` in **sdcard.h** (compose subdir/portal paths from it); `NOCSIF_AP_DEFAULT_IP` + `NOCSIF_WIFI_SD_DIR` shared between the AP and Portal screens.
- **Refs.** `firmware/src/usb_gadget.c:48`, `firmware/src/gnss.c:469`, `firmware/src/mic.c:209`, `firmware/src/ui.c:3803`, `firmware/src/ui.c:5017`, `firmware/src/ui.c:5360`.
- **Migration.** (1) Promote `SD_MOUNT_POINT` to sdcard.h as `NOCSIF_SD_MOUNT/BASE`; add the AP IP + wifi-dir constants. (2) Replace mkdir/fopen path literals and the AP-IP fallbacks; compose `UI_PORTAL_DIR`/PCAP hints from the base. (3) Verify SD dirs still create and the AP/Portal screens show the right IP/path.

### [low] Product brand `NocSif` and default device name re-typed per module
- **Current form.** The product name is a literal for the WiFi hostname (`NocSif-watch`), default AP SSID (`NocSif-AP`), portal title, decoy prefix (`NocSif-%02d`), and the BLE default advert name (`NocSif`, duplicated between the RAM initializer and the NVS get-default).
- **Current state.** scattered.
- **Proposed global.** `NOCSIF_BRAND ('NocSif')` + `NOCSIF_DEVICE_NAME_DEFAULT` + `NOCSIF_HOSTNAME` in a shared branding/identity header, composed for SSID/hostname/decoy/advert.
- **Refs.** `firmware/src/wifi.c:245`, `firmware/src/wifi.c:707`, `firmware/src/wifi.c:3063`, `firmware/src/ble.c:193`, `firmware/src/ble.c:867`.
- **Migration.** (1) Add the brand/name macros. (2) Compose the hostname/SSID/decoy/advert-default from them; collapse the ble.c RAM/NVS-default duplication. (3) Verify broadcast identity strings unchanged.

### [low] MAC-address format string hand-written per site
- **Current form.** The 6-byte colon-hex format `%02x:%02x:%02x:%02x:%02x:%02x` is retyped at ~6 snprintf/ESP_LOG sites in wifi.c (and elsewhere), risking wrong field count / missing colon; ESP-IDF already ships `MACSTR/MAC2STR`.
- **Current state.** scattered.
- **Proposed global.** `NOCSIF_MACSTR` / `NOCSIF_MAC2STR(m)` macros (mirroring esp-idf) in a shared header, used device-wide.
- **Refs.** `firmware/src/wifi.c:674`, `firmware/src/wifi.c:829`, `firmware/src/wifi.c:1296`, `firmware/src/wifi.c:2820`, `firmware/src/wifi.c:4252`.
- **Migration.** (1) Add the macro pair (or adopt esp-idf `MACSTR`). (2) Replace the hand-written format sites in wifi.c/ble.c. (3) Verify logged/rendered MACs unchanged.

---

## Icons / animation

There are no distinct icon-token items in this pass (glyph and font work is captured under [Strings](#strings) and [Color & Typography](#color--typography)). The one animation item lives here; several animation/opacity/motion items appear only as [Gaps](#gaps-noted-for-follow-up).

### [low] Saturating-counter idiom, u16/age sentinels, and comet cadence local
- **Current form.** The saturate-at-`0xFFFF` increment appears verbatim 3× and the `0xFFFFFFFF` "unknown age" sentinel 2× in ble.c; the shooting-star cadence (`COMET_LEN/W`, `STREAK_DUR/MIN/MAX_MS`, `PEAK_OPA`) lives behind a test flag in ui_background.c that the user wants easy to retune.
- **Current state.** partial — local, low-value but real duplications.
- **Proposed global.** A `COUNTER_SAT16()` / `NOCSIF_AGE_UNKNOWN` helper, and a documented `NOCSIF_STREAK_*` cadence block (the canonical comet cadence).
- **Refs.** `firmware/src/ble.c:469`, `firmware/src/ble.c:3499`, `firmware/src/ui_background.c:573`, `firmware/src/ui_background.c:586`.
- **Migration.** (1) Add the saturating-counter macro + age sentinel; replace the ble.c copies. (2) Group the streak cadence constants with a comment marking them the tunable prod cadence. (3) Verify counters and comet animation unchanged.

> Cross-ref: `feedback-comet-cadence` — the user likes the frequent debug cadence (~8–16 s) and may ship it over the calm default; a single named cadence block makes that a one-line change.

---

## Misc

### [low] Shared I2C bus electrical params (`400 kHz` SCL, `100 ms` timeout, 7-bit addr) redefined per driver
- **Current form.** Every device on the one shared 400 kHz I2C bus re-declares the same speed and timeout under its own prefix: `PCF_SCL_HZ/TIMEOUT` (rtc.c), `AXP2101_*` (power.c), `IMU_I2C_*` (imu.c), `TOUCH_SCL_HZ` (touch.c), `XL9555_*` (xl9555.c), with `I2C_ADDR_BIT_LEN_7` hand-set each time.
- **Current state.** scattered — `i2c_scan.h` already owns `nocsif_i2c_bus()`, the natural home.
- **Proposed global.** `NOCSIF_I2C_HZ (400000)` / `NOCSIF_I2C_TIMEOUT_MS (100)` in **i2c_scan.h**; drivers reference them in `i2c_device_config_t`.
- **Refs.** `firmware/src/rtc.c:39`, `firmware/src/power.c:20`, `firmware/src/imu.c:48`, `firmware/src/touch.c:34`, `firmware/src/xl9555.c:13`.
- **Migration.** (1) Add the two macros to i2c_scan.h. (2) Replace the per-driver defines. (3) Verify each I2C device still enumerates (bus scan + a read from PMU/RTC/IMU/touch/XL9555).

### [low] Shared geo/math constants (`DEG2RAD`, `111320` m/deg, `PI`, geofence radius) duplicated
- **Current form.** `DEG2RAD 0.0174532925…` is defined in gnss.c and weather.c and (as `MOVIE_DEG2RAD`) in ui.c; the equirectangular `111320.0` m/deg appears in gnss/weather/ui distance helpers; `PI 3.14159265f` is copied ~8× in ui_background.c; geofence radius (~150 m / ~250 m) is split across weather.c and ui.c prose. The whole `gpx_dist_m/gf_dist_m/movie_dist_m` helper is triplicated.
- **Current state.** scattered.
- **Proposed global.** A **nocsif_geo.h** with `NOCSIF_DEG2RAD`, `NOCSIF_M_PER_DEG (111320.0)`, `NOCSIF_PI`, `NOCSIF_GEOFENCE_RADIUS_M` and a shared distance helper; used by gnss/weather/ui.
- **Refs.** `firmware/src/gnss.c:113`, `firmware/src/weather.c:42`, `firmware/src/ui.c:17895`, `firmware/src/ui.c:18038`, `firmware/src/ui_background.c:120`.
- **Migration.** (1) Create nocsif_geo.h with the constants + one equirectangular distance function. (2) Replace the duplicated `DEG2RAD/111320/PI` and fold the three distance helpers into one call. (3) Verify GPX distance, weather move-threshold, and Movie-mode geofence exit still trigger correctly.

### [low] DMA/cache-line `64`-byte alignment and buffer sizes as bare literals
- **Current form.** `heap_caps_aligned_alloc(64, …)` recurs at every PSRAM/DMA canvas alloc with a bare `64` (the load-bearing SD-under-WiFi cache-line alignment), and scratch/ID buffer sizes are hand-sized per module (`BIND_MAX/DEVNAME_MAX 32` disagreeing with each other and with FILES/NOTES name maxes `96 vs 64`; log line `256`; ring persist `160` unlinked from `CAR_MAXN`; `s_clock[8]/s_date[32]`).
- **Current state.** partial — a few named; alignment and several buffer widths inline.
- **Proposed global.** `NOCSIF_DMA_ALIGN (64)` in a shared header documenting *why* 64; coalesce the 32-byte id caps to `NOCSIF_ID_MAX`, a shared `NOCSIF_LOG_LINE_MAX (256)`, and tie the ring persist buffer to `CAR_MAXN`.
- **Refs.** `firmware/src/ui_nav.c:388`, `firmware/src/ui_background.c:216`, `firmware/src/settings.c:25`, `firmware/src/logbook.c:143`, `firmware/src/ui.c:16536`.
- **Migration.** (1) Add `NOCSIF_DMA_ALIGN` + the id/log-line caps. (2) Replace the `64` alignments and the `32/256` buffer sizes; size the ring persist buffer off `CAR_MAXN`. (3) Verify canvas/framebuffer allocs, SD reads, and ring persist round-trip (raising `CAR_MAXN` must not truncate).

---

## Correlation with the plan

This catalog is deliberately aligned with work already scheduled in `PLAN.md` and lessons in `LESSONS.md`. Executing it *unblocks and de-risks* those items rather than competing with them.

| Plan / lesson item | Catalog items that support it |
|---|---|
| **PLAN §1 — rounded-corner safe zone** (`410×502` panel, `x∈[48,362] y∈[42,460]`, park unused CS HIGH) | Corner-safe inset `26` · Panel `410×502` (`BG_W/BG_H`) · Corner-clearance geometry (`HEADER_HINSET/TOP_PAD/CORNER_R`) · Keyboard `-58` corner clearance · SPI3 CS-parking pinout |
| **PLAN §4.1 — Theme** (runtime accent palette, per-wallpaper star color, surface tokens) | Default accent `0x655578` seed · Surface-elevation set · Off-palette grays · Radius scale (surface vocabulary) · GeoFence/Movie `~250 m` (geo constants) |
| **PLAN §4.13 — type scale everywhere / long names / Control-Center tiles reflect live state** | **Role-style typography migration** (the headline item) · Status/footnote/footer builders (must reflow) · Active-state status-color idiom (accent-means-live) · Brightness ladder + battery-saver `<20%` · Audio format contract (voice-memo-too-quiet fix) |
| **PLAN §4.6 / §3.12 — Connectivity Governor, Battery Saver** | Radio DMA coexistence gate · Lock-free pubstr getters (Connectivity hub) · Battery sentinels |
| **PLAN §4.1 — Language / i18n (backlog)** | User-facing string table (`ui_strings.h`) · Glyph tokens · Brand/identity macros — all i18n seams |
| **`LESSONS.md` / `[[gotcha-rounded-corner-safe-zone]]`** | Every safe-zone item above; the proposed `nocsif_align_safe()` guard (see Gaps) makes the recurring corner-clip bug un-reintroducible |
| **`LESSONS.md` / `[[gotcha-ble-wifi-coexistence]]` / `[[project-ble-release-regression]]`** | Radio internal-DMA gate (`≥24576` fragmentation policy) · DMA-align `64` · Row-pool caps (lists 10→32, LVGL objects in PSRAM) |
| **`[[project-carousel-v2-polish]]`** | Gesture/tap/swipe parity across Ring/Dual/Bottom |
| **`[[project-m11-watch-core]]` recurring gotchas** | Task-stack tiers (SD workers 6144, LVGL 16384) · Pooled-row CLICKABLE/SCROLLABLE flat-row helper |
| **`[[project-f2-signal-hunt]]` / `[[project-m9-lora]]`** | RSSI/EMA thresholds · Radio DMA gate · Signal-hunt status-color idiom |
| **`[[project-ota]]`** | SD lock timeouts · DMA-align `64` for SD read under WiFi int-DMA |
| **`feedback-*`** | `ui-color-and-size` (muted grays, type scale) · `comet-cadence` (streak block) · `usb-mode-ux` (File-Share latency timing) · `neutral-framing` (string table wording) |

---

## Proposed token homes

The guiding rule: **a value's home is the lowest layer that must not depend on a higher one.** Palette and text styles need LVGL, so they live in the theme layer; pure geometry ints must be LVGL-free so both `ui.c` and `ui_nav.c` can share them without a dependency cycle; glyphs, keys, board facts, coexistence policy, audio/geo constants are dependency-free leaf headers any driver (even a bare hardware driver) can include.

### Layering

1. **`nocsif_glyphs.h`** — *NEW, dependency-free* (no LVGL, no esp headers): `NOCSIF_DOT/DEG/NDASH/EMDASH/ELL`. `ui_theme.h` `#include`s it (existing users unchanged); `ble.c`/`rtc.c` include it directly and delete `BLE_DOT/RTC_DOT/BLE_ELL`. This resolves the biggest "token forked to dodge an include" problem.

2. **`ui_theme.h` / `ui_theme.c`** — the single source of truth for the **palette** and **text styles** (its documented role). Add here: the missing color tokens (surface-elevation set `CARD/SURFACE_*`, `NOCSIF_ALERT` red, `NOCSIF_ACCENT_DEFAULT_RGB`, the hairline third gray, `RING/STARDOT/ROW_DIVIDER`) and the new role **styles** (`nocsif_style_status/body/meta/caption/footnote/hero_num`) that `typescale_apply()` re-points. Radius tokens live here too (visual vocabulary, theme-adjacent).

3. **`ui_metrics.h`** — *NEW, LVGL-free ints*: the safe-zone + layout geometry currently trapped file-static in `ui_nav.c` — `HEADER_HINSET/TOP_PAD/HEADER_INSET`, `CORNER_R` (moved from `ui_background.c`), `CONTENT_INSET` (replacing `LIST_INSET/UI_LIST_INSET`), `ROW_MIN_H/VPAD/ICON_GAP/ICON_W`, `HEADER_H/PAD/GAP`, `KB_H/KB_BOTTOM_OFF`, the tick cadences, and the pool-cap group (`LIST_ROWS/…_SM/PICK_ROWS/POOL_MAX_ROWS`). **Both** `ui_nav.c` and `ui.c` consume it — this is what lets the scaffold own the safe zone and stops `ui.c` re-deriving corner clearance by hand. `ui_gesture.h` is a sibling (or a section) for tap/swipe/overscroll slop.

4. **Platform / config leaf headers** for non-UI cross-cutting constants: `nocsif_task.h` (stack tiers, priorities, queue depth, poll tick), `board_pins.h` (SPI3 bus + CS + max_xfer), I2C params into the existing `i2c_scan.h`, `coex.h` (DMA cap mask + accessor + per-radio gate thresholds — the load-bearing coexistence policy), `nocsif_geo.h` (DEG2RAD/M_PER_DEG/PI/geofence + distance helper), `nocsif_audio_fmt.h` (SR/WAV/PCM16), `nocsif_pubstr.h` (the triplicated status/readout double-buffer), and one `nvs_keys.h` (namespaces + every key/format macro, with a compile-time 15-char guard).

5. **String seams**: `ui_strings.h` for repeated operator-facing prose + placeholders (also the future i18n table); SD base paths + AP IP promoted into `sdcard.h` (folding usb_gadget.c's private `SD_MOUNT_POINT`); brand/identity macros in a small branding header.

**Three live duplications this exposes — delete them explicitly:** `UI_LIST_INSET` vs `LIST_INSET`; `BG_W/BG_H` vs `NOCSIF_DISP_W/H`; `BLE_DOT/RTC_DOT/BLE_ELL` vs the glyph tokens.

### Example header block

What a couple of these leaf headers would look like (illustrative — values from the catalog):

```c
/* ui_metrics.h — LVGL-free layout geometry. Included by ui.c AND ui_nav.c
 * so the scaffold and hand-built screens share one safe zone. */
#pragma once

/* Panel + rounded-corner safe zone (PLAN §1; [[gotcha-rounded-corner-safe-zone]]) */
#define NOCSIF_CORNER_R          40          /* physical rounded-rect radius   */
#define NOCSIF_SAFE_INSET_X      48          /* proven x∈[48,362]              */
#define NOCSIF_SAFE_INSET_Y      42          /* proven y∈[42,460]             */
#define NOCSIF_CONTENT_INSET     26          /* was LIST_INSET/UI_LIST_INSET   */

/* Row + header chrome (was re-typed in ui_theme.c) */
#define NOCSIF_ROW_MIN_H         44
#define NOCSIF_ROW_VPAD          15
#define NOCSIF_ROW_ICON_GAP      16
#define NOCSIF_ROW_ICON_W        24

/* On-screen keyboard */
#define NOCSIF_KB_H              196
#define NOCSIF_KB_BOTTOM_OFF     (-58)       /* clears the corner arc          */

/* LVGL fixed-row pool caps (96 KB pool; objects in PSRAM since PR #106) */
#define NOCSIF_LIST_ROWS         32          /* display-bound primary          */
#define NOCSIF_LIST_ROWS_SM      8
#define NOCSIF_PICK_ROWS         6
#define NOCSIF_POOL_MAX_ROWS     40

/* Per-screen refresh cadences */
#define UI_TICK_FAST_MS          100
#define UI_TICK_LIST_MS          1000
#define UI_TICK_SLOW_MS          700
#define UI_TICK_MINUTE_MS        10000
```

```c
/* coex.h — load-bearing radio coexistence policy.
 * One place measures the same quantity everywhere (see
 * [[gotcha-ble-wifi-coexistence]] / [[project-ble-release-regression]]). */
#pragma once
#include "esp_heap_caps.h"

#define NOCSIF_DMA_CAPS  (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)

static inline size_t nocsif_int_dma_largest(void) {
    return heap_caps_get_largest_free_block(NOCSIF_DMA_CAPS);
}

/* per-radio bring-up gates (contiguous int-DMA block required) */
#define NOCSIF_RADIO_MIN_DMA_BLE   31744
#define NOCSIF_RADIO_MIN_DMA_LORA  12288
#define NOCSIF_RADIO_MIN_DMA_FLOOR 24576   /* historical fragmentation wall   */
```

---

## Rollout order

Executed as **one branch per group**, ordered so each lands on a green build with an on-device smoke test before the next depends on it. Low-risk deletions first; the high-value type-scale sweep only after its tokens and styles exist.

1. **Zero-risk deletions of proven duplications** (small, mechanical, high confidence). Delete `BG_W/BG_H` for `NOCSIF_DISP_W/H`; unify `LIST_INSET/UI_LIST_INSET` into `ui_metrics.h` `CONTENT_INSET`; create `nocsif_glyphs.h` and delete `BLE_DOT/RTC_DOT/BLE_ELL`. Each is a compile-and-flash smoke test.

2. **Palette tokens in `ui_theme.h`** (`NOCSIF_ALERT`, `ACCENT_DEFAULT_RGB`, surface-elevation set, hairline, `RING/STARDOT/ROW_DIVIDER`) + radius scale. Additive, purely find/replace of hex, no behavior change. Verify by "renders identically" + a single-token recolor test.

3. **Promote the safe-zone geometry into `ui_metrics.h`** (`HEADER_HINSET/TOP_PAD/CORNER_R` + row/header metrics) and repoint `ui_nav.c` + `ui_theme.c` + the `ui.c` hand-built screens. High-value, medium-risk (touches corner clearance) — land **before** any large screen sweep so the scaffold owns the safe zone.

4. **The type-scale role-style migration** (the single highest-value item, PLAN §4.13). Do it **after** the role styles + safe-zone tokens exist: add the styles to `typescale_apply()`, then sweep `build_*` by role. Large but each screen is independently verifiable by toggling the type scale on-device.

5. **Shared helpers / builders** (flat-row, action-row, status/footnote/footer, keyboard-attach, primary-button, meter-bar, modal). Collapses the largest volume of duplicated LVGL boilerplate; naturally consumes the tokens from steps 2–4.

6. **NVS keys (`nvs_keys.h`) and repeated user-facing strings (`ui_strings.h`)** — mechanical but broad; do together since both are the i18n/settings seam. Verify every setting round-trips.

7. **Cross-cutting platform headers**: `coex.h` (DMA gate — **treat as load-bearing, test radio switching carefully**), `nocsif_task.h` (stacks/priorities), `board_pins.h`, I2C params, `nocsif_geo.h`, `nocsif_audio_fmt.h`, `nocsif_pubstr.h`. Each is its own branch with an on-device smoke test of the affected subsystem.

8. **Lowest-priority polish last**: gesture/tap/swipe unification, cadence tiers, RSSI/brightness/battery threshold grouping, buffer-size + DMA-align tokens, saturating-counter / comet cadence. Interleave freely; none blocks the others.

---

## Gaps noted for follow-up

Cross-cutting structural items the value-level audit surfaced that go slightly beyond single-token extraction. Recorded here so they are not lost; schedule alongside the related rollout step.

- **Z-order / layer constants.** Pages use `lv_layer_top()` and implicit child order for scrims/overlays/trash/comets with no named scheme. Add a `NOCSIF_LAYER_*` (or explicit `move_to_index`) convention so modal scrims, the flashlight overlay, edit-mode trash, and boot splash stack predictably instead of by creation order.
- **Opacity scale.** `LV_OPA_70` scrim, star/streak opacities (`STREAK_PEAK_OPA 220`), disc/pill alphas are ad hoc — no `NOCSIF_OPA_SCRIM/FAINT/…`. A small opacity token set keeps overlay dimming and star brightness consistent and theme-tunable.
- **Global spacing scale.** Pad values `8/10/11/12/14/15/16/18` recur across every builder with no shared 4/8-based scale (only row metrics are partly named). A `NOCSIF_SPACE_XS/S/M/L` set removes most remaining layout magic numbers and pairs with the radius scale.
- **Radio-state single source of truth.** PLAN §4.13 requires the Control Center tiles (WiFi/BLE/Airplane/DND) to reflect **live** subsystem state and stay mutually coherent. The finders caught the status-*color* idiom but not the missing shared state model. Add a `nocsif_radio_state` accessor layer the CC tiles + Connectivity hub + status labels all read, so no screen keeps a private `s_cc_wifi/s_cc_ble` bool that drifts.
- **Alignment / anchor safe wrapper.** Given the recurring corner-clip bug, a `nocsif_align_safe()` (or a debug assert in the align path) that refuses raw `LV_ALIGN_*_CORNER` placements outside the safe zone would make the §1 gotcha impossible to reintroduce, rather than relying on every author remembering the insets.
- **`ESP_LOG` TAG strings.** Each module hand-declares its own `static const char *TAG = "…"` with no shared convention; a per-module TAG macro convention (and a shared `nocsif_log_dma_free` helper) would standardize log formatting and the DMA-instrumentation lines.
- **Event/gesture semantic-flag discipline.** The `CLICKABLE` / `SCROLLABLE`-clear / `GESTURE_BUBBLE` combination is a semantic contract for pooled/tappable rows (a known footgun). Beyond the flat-row helper, a named `nocsif_flags_tappable_row()` intent wrapper documents *why* those flags travel together.
- **Reduce-motion / accessibility gating.** `reduce_motion` is read ad hoc; a single `nocsif_reduced_motion()` gate applied uniformly to comets, carousel drift, and slide/fade transitions would make the accessibility switch one policy instead of per-animation checks.
