# NocSif — UI-shell **P4** implementation meta-prompt (real bindings: RTC · battery · buttons · settings store · USB migration)

> **Paste the block below into a fresh coding chat to build UI-shell P4.** It is self-contained. It was
> authored by the design-ideas hub against the **real P3.2 code on `main`** (`60b1157`, PR #21) — the file
> paths, function names, and integration points below are the shipped shell, not a sketch. Do **not** design
> here; this chat *builds* firmware. One milestone-phase per branch → PR → squash-merge, verified on-device.

---

> You are a NocSif firmware coding chat. Your job this session: build **UI-shell P4 — real bindings**. NocSif
> is a modular security-testing firmware for the **LilyGo T-Watch Ultra (ESP32-S3)**, for **authorized
> security testing only**. Repo: `github.com/silverwolf2r/NocSif_Firmware` (private); local coding worktree
> `D:\Docker\NocSif_Firmware`.
>
> ## 0. Wording convention — enforce everywhere (non-negotiable)
> Describe every capability in neutral, technical **device-class** terms — "USB HID keyboard", "USB
> mass-storage (MSC)", "RF transceiver", "NFC reader", "802.11 management-frame transmission",
> "power-management IC (PMU)", "real-time clock". **Never** adversarial vocabulary
> ("attack / offensive / payload / weaponize / victim / target / red-team"). Charged wording **hard-fails
> Anthropic's automated cyber-safeguard** and has killed an entire research/subagent workflow before
> (2026-08-06). This applies to code comments, commit messages, PR text, and **especially every prompt you
> send to a subagent or Workflow**. Keep precise, load-bearing format names (**DuckyScript**, `.sub`, `.nfc`).
> *(Note: the shipped code still carries two `payload` carryovers — `files.payloads` in `ui.c` and the ducky
> default path `/sd/payload.txt` — see §6.5; neutralize them to `macro` when you touch the USB screen.)*
>
> ## 1. Read these first (in this order)
> 1. **`docs/RESUME.md`** — current firmware state (start-here). The **UI-shell P3.2 entry** and its
>    "Two load-bearing gotchas" + build-memory sections are load-bearing for P4.
> 2. **`docs/HARDWARE.md`** — authoritative pinout / buses / rails / I²C addresses. P4 lives almost entirely
>    in here (RTC, PMU, GPIO0). Read §"Buses", §"I²C device addresses", §"AXP2101 power rails",
>    §"Peripherals & control pins", §"Power / charging".
> 3. **`docs/LESSONS.md`** — the LVGL fixed-pool freeze + **the ESP32-S3 hang/panic serial-diagnosis method**
>    (`scratchpad/cap.py` + `addr2line`). You will use the serial method to bring up the PMU IRQ.
> 4. **`docs/PLAN.md`** — §4.1 (UI-shell phases, incl. the **P4 line** and the two cross-phase foundations:
>    the app/launch-target registry and the settings store pulled forward), §4.3 (missing-driver table:
>    RTC / PMU fuel-gauge / PWRKEY IRQ / GPIO0 rows), §7.4 ("Physical buttons", "PIN lock", "Personalization").
> 5. **`docs/design/full-app-mockup.html`** — the **navigation + IA source of truth**. P4's USB rows, the
>    header clock/battery, and the eventual FN/planet bindings all track this. (The mockup's assignable-planet
>    registry has **19 launch targets that already match the firmware registry 1:1** — see §4.)
> 6. As needed: **`docs/research/ui-shell-2026-08-09.md`** (the 5-phase UI-shell plan),
>    **`docs/ARCHITECTURE.md`**, and **`vendor/LilyGoLib/`** +
>    **XPowersLib / SensorLib** as *register references* for the AXP2101 and PCF85063A (see §6).
>
> ## 2. Current firmware state (post-P3.2) and where code lives
> M0–M4 + **UI-shell P1–P3.2** are done and verified on-device. Everything wireless/sensing (WiFi, BLE, NFC,
> GNSS, LoRa, IMU, audio, haptics — **and, until P4, the RTC and battery fuel gauge**) is still unused.
> `firmware/src/`:
> - **`ui.c` / `ui.h`** — the LVGL v9 app layer: the **app/launch-target registry** (`app_t`, `k_screens[]`,
>   `app_get` / `app_get_or_stub` / `app_ensure`) and **`nocsif_app_launch(const char *id)`** — the single
>   launch-by-id entry point (public in `ui.h`) that FN / peek-planets / gestures bind to. Home + 13 category
>   submenus build lazily; hero/leaf actions are present-but-disabled stubs with registered ids. The **M4 USB
>   controls live in `build_usb()`** as two bracket-buttons (`gadget_btn_click_cb` / `macro_btn_click_cb` +
>   their 300 ms poll timers).
> - **`ui_nav.c` / `ui_nav.h`** — the nav MECHANISM (root-based; app-agnostic): screen **stack**
>   (`nocsif_nav_init/push/back`, `nocsif_nav_pin`), the custom `lv_obj_set_x`+opacity `slide_in`, the
>   scaffold/row/band/list builders, and **`build_header()`** — which today creates the **`--:--` clock and
>   `--%` battery placeholder labels** (see §5, the key P4 integration point).
> - **`ui_theme.c` / `ui_theme.h`** — colour tokens (`NOCSIF_VOID/PIT/ASH/STEEL/BONE/WHITE/VIOLET…`), the
>   embedded fonts, and the shared `lv_style_t` set incl. **`nocsif_style_clock`** (mono 15, bone) and
>   **`nocsif_style_battery`** (mono 13, steel). Swap font/size in one place here.
> - **`power.c` / `power.h`** — the AXP2101 (0x34) driver **so far**: `nocsif_power_init()` (attach to the
>   shared I²C bus), `nocsif_power_display_rail()`, `nocsif_power_sd_rail()`. **Register-direct, no XPowersLib**
>   (writes regs 0x90/0x92/0x93). **P4 extends this file** for the fuel gauge + PWRKEY IRQ.
> - **`i2c_scan.c`** — the shared I²C bus is already up (SDA=3/SCL=2); the RTC (0x51) and PMU (0x34) both ACK at
>   boot (M0.1 confirmed all 6 devices). **`usb_gadget.c` / `hid_kbd.c` / `ducky.c`** — the working M4
>   composite you migrate in §6.5. **`display.c`** — panel + backlight/rail control for PWR screen on/off.
> - Icons: **`icons/nocsif_icons.h`** (`NOCSIF_ICON_*`, incl. `NOCSIF_ICON_BATT`, `_CLOCK`, `_LOCK`, `_USB`,
>   `_DRIVE`, `_KEY`, `_GLOBE`). Fonts: `fonts/` (regen via `gen_fonts.py`).
>
> ## 3. What P4 is (feature scope, device-class terms)
> P4 makes the shell **real** by binding four on-board parts the UI has been faking, standing up the shared
> **settings store**, and migrating the M4 USB controls onto their mockup screen:
>
> | # | Deliverable | On-board part · bus / pins / rail |
> |---|---|---|
> | A | **Real-time clock** driving the header clock + Time screen | **PCF85063A** · I²C **0x51** · IRQ **GPIO1** · backup rail LDO1(VRTC) |
> | B | **Battery %** / charge state driving the header battery + System→Power | **AXP2101** fuel gauge · I²C **0x34** |
> | C | **Physical-button input service** | **PWR** via AXP2101 **PWRKEY IRQ** (decoded over I²C 0x34 status regs) · **FN = native GPIO0** |
> | D | **Settings / persistence store** (NVS-backed; optional SD mirror) | on-SoC NVS (flash) + `/sd` |
> | E | **Migrate the M4 USB controls** into the mockup's USB action rows | existing `usb_gadget`/`ducky`/`hid_kbd` |
>
> **Hardware notes that will bite you (resolve before coding the driver):**
> - ⚠ **The AXP2101 IRQ line's GPIO is ambiguous in our docs.** `HARDWARE.md` says "AXP2101 IRQ = GPIO7 …
>   verify vs XL9555 use", but **XL9555 IO7 is the display-power enable** (a different device on a different
>   bus) — so that "7" is suspect. The AXP2101 physical **IRQB** is an open-drain line into a **native SoC
>   GPIO**; confirm the exact pin against `vendor/LilyGoLib` (the board pins header / `LilyGoLib.cpp`) and the
>   schematic **before** wiring the ISR. **You do not strictly need the IRQ line to decode PWRKEY** — the
>   AXP2101 latches short/long-press events in its IRQ **status registers**, which you can read over I²C; the
>   GPIO IRQ is a low-power *notify* so you don't have to poll. Start by polling the status registers if the
>   pin is unconfirmed, then add the IRQ line once verified.
> - ⚠ **GPIO0 is the download-mode strap.** Held LOW at power-on it enters USB download mode (that is how you
>   flash). The **runtime** read must simply tolerate that: read it as a normal input **after** boot; never
>   reconfigure it in a way that interferes with the strap. RST is a hardware-only reset (not SW-readable) —
>   not a UI button.
> - The AXP2101 **PWRKEY is programmable** (HARDWARE.md: "PWR 1 s on / 6 s off, programmable"). Configure its
>   power-on/off timing deliberately so a short press does **not** hardware-poweroff the watch out from under
>   the firmware.
> - **Register addresses:** take the AXP2101 fuel-gauge (battery-percent / gauge) and IRQ-enable/status
>   registers, and the PCF85063A time/alarm registers, from the **AXP2101 & PCF85063A datasheets**, cross-checked
>   against **XPowersLib** / **SensorLib** / `vendor/LilyGoLib`. **Stay register-direct** to match `power.c`'s
>   existing style (no XPowersLib dependency) unless you make and document a deliberate call to vendor it.
>   **Verify the exact registers with a short research pass/Workflow before coding** — this is the M3/M4
>   pattern (API pre-verified against real sources before writing code); it caught real bugs each time.
>
> ## 4. Registry reconciliation (already done — informational)
> The design hub verified the mockup's **19 assignable-planet launch targets** against the firmware registry:
> they **match 1:1, no drift** — `activity · autom · ble · files · flash · gnss · hunt · lora · navi · nfc ·
> notes · notif · system · theme · timers · usb · voice · weather · wifi` (13 are enabled category screens;
> `hunt · notif · voice · weather · flash · theme` are present-but-disabled stubs whose hero screens land with
> later milestones). So when P4 wires **FN** (and, later, peek-planets/gestures) it must bind **through
> `nocsif_app_launch(const char *id)` by string id** — never a hardcoded screen pointer. A launch of a
> disabled-stub id is already a logged no-op, so a sensible **default FN target must be an *enabled* id**
> (e.g. `usb`, `files`, `wifi`) — not `hunt`/`flash` (still stubs). Make the default explicit and overridable.
>
> ## 5. The header clock/battery integration point (read before Phase B/A)
> `build_header()` in `ui_nav.c` currently creates, **per screen**, a clock label (`"--:--"`,
> `nocsif_style_clock`) and a battery label (`"--%"`, `nocsif_style_battery`), and **does not expose them**.
> Two consequences you must design around:
> - **Every screen has its own header labels**, and non-pinned screens are **freed on nav-back and rebuilt**
>   (the LVGL fixed-pool bound — see LESSONS). So a global "update the clock" tick must reach the **currently
>   active** screen's header, and a freshly built header must render the **current** time/% immediately (not
>   `--:--`). Recommended shape: keep the latest time-string and battery-% in the rtc/power modules; have
>   `build_header()` read them at build time; and add a small nav/ui hook (e.g. an updater that resolves the
>   active screen's header labels, or have the header register its labels) that a **single** LVGL timer calls.
>   Pick one approach and note the trade-off.
> - **`full_refresh` repaints the whole 410×502 frame on ANY invalidation.** So the clock/battery label writes
>   **must be guarded update-on-change** (the M4 status timers already do `strcmp` before `lv_label_set_text`
>   — copy that). Tick the **clock once per minute** (not per second) and the **battery every ~30–60 s**; a
>   per-second repaint would burn the panel/PSRAM budget for nothing. (RESUME's P3.1 entry calls this out: "the
>   P4 clock must [guard update-on-change].")
>
> ## 6. Suggested phased shape — **front-load the biggest unknown, isolated + flashable**
> Produce your **own** plan and pause for review (§9), but this is the intended shape. Each phase is
> **independently flashable and verified on-device before the next** (the M4 / UI-shell rule), and each is a
> **stopping point** (§7).
>
> - **P4.1 — Physical-button input service (the riskiest driver; isolate it).** Bring up **PWRKEY decode** and
>   the **FN=GPIO0** read and *only log the decoded events over serial* — no UI actions yet. Prove: (1) which
>   native GPIO is the AXP2101 IRQ (or confirm status-register polling works without it); (2) the PMU status
>   registers decode **short / long / double** PWR press; (3) GPIO0 reads cleanly at runtime and its boot-strap
>   use is untouched; (4) a short PWR press does not hardware-poweroff the watch. Runs in a **worker task / ISR-
>   deferred**, never on the LVGL task (PLAN §1). This de-risks the scary unknown first, flashable alone.
> - **P4.2 — RTC PCF85063A + header clock.** New `rtc.{c,h}` (PCF85063A @ 0x51: read/set BCD time, optionally
>   alarm). Add the header-update mechanism from §5; drive the header clock + the **Time** screen. Seed the
>   clock from a build/settings value for now (**GNSS/NTP sync are later milestones — M8/M5**, do not pull them
>   in). Guard update-on-change.
> - **P4.3 — AXP2101 battery %/fuel gauge + header battery + System→Power.** Extend `power.c` to read
>   battery-percent + charging state (register-direct). Bind the header battery label and the **System → Power**
>   row (today a static `"--%"`). Same update-on-change + slow cadence.
> - **P4.4 — Settings store + wire the buttons to real actions.** Stand up the **NVS-backed key-value store**
>   (`settings.{c,h}`; namespace e.g. `nocsif`; typed get/set str/int/blob; optional `/sd` mirror deferred).
>   First real users (PLAN §4.1 / §7.4): **FN target** (persisted app id, bound via `nocsif_app_launch`),
>   **PWR behaviors** (short = screen on/off; in a submenu = pop toward Home via `nocsif_nav_back`/to-Home; long
>   = a minimal **power menu** screen you add — Power off / Restart; double = a bound shortcut id), and the
>   **PIN-hash** slot (store the passcode **hashed**, per §7.4 — the unlock PIN-pad screen itself can be a thin
>   stub this phase). Add a **Settings → Buttons** screen (register it as an app id) that writes the FN/PWR-double
>   bindings. ⚠ Any new screen must obey the **LVGL fixed-pool** rule (don't cache unbounded; let it free on pop).
>   **Watchface dependency (be honest):** "PWR in a menu = back-to-watchface" — there is **no watchface/peek
>   screen yet** (it's a later phase), so scope PWR-home to "pop to Home (stack[0])" now and note the watchface
>   follow-up.
> - **P4.5 — USB mode selection.** **Expanded 2026-08-10 → use the dedicated, self-contained meta-prompt
>   `docs/design/meta-prompts/ui-shell-p4.5-usb-modes.md`, which supersedes this bullet.** P4.5 now migrates the
>   M4 controls **onto a user-selected mode-switch** (detached-until-picked · one class at a time — File Share /
>   HID / Console · re-enumerate on switch), **not** the always-on composite; the rebuilt mockup `#usb` is the
>   done-state. The migration/naming notes below still apply. Replace the two bracket-buttons in
>   `build_usb()` with the mockup's **USB Gadget** rows — **Run Macro** (`macro.txt`), **File Share
>   (MSC)**, **HID Keyboard**, **Console (CDC)**, **Keymap** (cycle US/GB/DE via `hid_kbd`), **HID Mouse** +
>   **U2F/FIDO** (disabled stubs) — plus the **interface / enumeration-state readout**. Reuse the working `usb_gadget`/`ducky` calls verbatim
>   (they only *signal* worker tasks — never run `tinyusb_driver_install()` or SD claims on the LVGL task).
>   These are chevron-less **action rows** (the mockup's `data-act` pattern) — `nocsif_menu_add_row` always
>   draws a chevron today, so add a small action-row variant or flag. **Neutralize the `payload` naming here**
>   (`/sd/payload.txt` → `/sd/macro.txt`; `files.payloads` → `macros`) to match the wording convention and the
>   mockup's "Run Macro". This is the **final P4 stopping point** → hand off to **P5** (boot screen + motion).
>
> *(P4.4 is the natural place the settings store first earns its keep; P4.1 only logs, so it needs no
> persistence and can safely come first. If you re-order, keep the risky PWRKEY/IRQ bring-up as the isolated
> first flash.)*
>
> ## 7. Stopping points & continuation meta-prompts (**do this — it's a core deliverable**)
> This work spans several flashable phases and will outlast one context window. **At every good stopping
> point, before you run low on context, you must author a *continuation meta-prompt* so the user can clear the
> context and start a fresh chat that picks the plan up cleanly.** Treat this as part of "done," not optional.
>
> **A "good stopping point" is:** (a) a phase is **verified on-device and its PR opened/merged**; or (b) you
> are about to open a **large multi-file phase**; or (c) context use is running high (**≈ 75%**, PLAN §1).
> **Never** push past a context wall mid-phase and leave the work half-described.
>
> **At a stopping point, do all of this:**
> 1. **Update `docs/RESUME.md`** — add the phase's on-device result + findings (the established per-milestone
>    style), and update its "Next steps". Update **`docs/PLAN.md` §4.1** only if a phase's status flips
>    (🔵→✅). Update **`docs/LESSONS.md`** if you hit a new root-caused gotcha. *(These doc edits ride the
>    phase's own PR — RESUME/LESSONS/PLAN status live on `main`.)*
> 2. **Write the continuation meta-prompt** — a short, **paste-ready block** the user drops into a new chat.
>    It must: name what's **done / in-flight / next** with **exact file paths**; point at **this file**
>    (`docs/design/meta-prompts/ui-shell-p4.md`) as the authoritative P4 spec plus `docs/RESUME.md`; name the
>    **exact next phase** and its first task; **restate the conventions** (neutral device-class wording; build
>    from **PowerShell `-j 2`** never Git Bash, drop to `-j 1` if it OOMs; flash `esptool --no-stub`, watch is
>    **COM7**, download mode = hold BOOT→tap RST→release BOOT; **verify on-device before the next phase**; the
>    **locked design language** — grayscale + one soft antique accent, no hot colours, no scanlines); and note
>    the **LVGL fixed-pool** + **`full_refresh` update-on-change** rules so the next chat doesn't relearn them.
>    Save it to **`docs/design/meta-prompts/ui-shell-p4-continue.md`** (overwrite each stopping point so it's
>    always the *latest* resume point), and also paste it into the chat.
> 3. **Tell the user plainly:** "Good stopping point — **P4.x is verified on-device and merged**. Clear the
>    context and start a fresh chat with the block above (saved to `…/ui-shell-p4-continue.md`) to continue with
>    **P4.(x+1)**." If P4 is fully done, the continuation meta-prompt hands off to **P5** (boot screen + motion
>    helpers) instead, and says so.
>
> The continuation meta-prompt is itself a small **docs-only change** — commit it with (or alongside) the
> phase's PR so the resume point is on `main`, never stranded locally.
>
> ## 8. Hardware / LVGL specifics to reuse (don't rediscover)
> - **Display/flush invariants (M3, load-bearing):** 24-bit **RGB888**, panel `rgb_ele_order = BGR` handled in
>   the flush config (define colours normally in `ui_theme.h`, do **not** byte-swap), the **2-px `rounder_cb`**
>   (CO5300 shears on odd columns), `CONFIG_LV_COLOR_DEPTH=24` (`=y`, not `=24`), full-frame **PSRAM** buffers
>   + **`full_refresh`** (⇒ the §5 update-on-change rule).
> - **LVGL threading:** single-threaded behind `esp_lvgl_port`; touch UI objects only under
>   `lvgl_port_lock()`. Worker tasks (button service, any I²C polling loop) must **marshal onto the LVGL task**
>   (e.g. `lv_async_call` or a notify + an LVGL timer) before touching widgets — mirror M4's "clicks only
>   signal workers" discipline in reverse.
> - **LVGL fixed 48 KB object pool** (separate from the 8 MB system heap): **do not cache screens/objects
>   unbounded** — new P4 screens (power menu, Settings→Buttons, PIN pad) must free on pop like the others
>   (LESSONS 2026-08-09). `esp_get_free_heap_size()` will **not** show LVGL pressure.
> - **Shared I²C bus** (SDA=3/SCL=2) already up; RTC 0x51 + PMU 0x34 ACK at boot. **Rails** are gated by the
>   AXP2101 (`power.c`); the RTC backup rail LDO1(VRTC) can't be turned off.
> - **UI tokens/nav to reuse:** `nocsif_screen_scaffold` / `nocsif_menu_list` / `nocsif_band` /
>   `nocsif_menu_add_row` for any new screen; `nocsif_app_launch(id)` for launch-by-id; `nocsif_nav_pin` for a
>   screen that owns a live timer/state; `nocsif_style_clock` / `nocsif_style_battery` for the header; the
>   `NOCSIF_*` colour tokens + `NOCSIF_ICON_*` glyphs. Screens/rows track **`full-app-mockup.html`** for
>   placement — fill existing stubs, don't invent nav.
> - **Serial bring-up:** for the PWRKEY/IRQ work use the LESSONS serial method — `scratchpad/cap.py [sec]`
>   (resilient reconnecting COM7 capture, no reset-on-open) + `xtensa-esp32s3-elf-addr2line` against the
>   worktree ELF. Hold at any freeze ≥ 5 s to catch a task-wdt backtrace.
>
> ## 9. On-device verification (per phase — nothing merges unflashed)
> Every phase ends **flashed to the watch (COM7) and verified with the user**, serial clean (no `task_wdt` /
> panic, heap flat). Concretely: **P4.1** — press PWR short/long/double and FN, watch the decoded events log
> correctly over serial; confirm no unintended power-off. **P4.2** — the header clock shows real minutes and
> advances; rebuilding a screen (drill in/out) shows the current time immediately, not `--:--`; serial shows no
> per-second whole-frame repaints. **P4.3** — header battery + System→Power show a plausible % that tracks
> charge/discharge. **P4.4** — set an FN target in Settings → press FN → the bound app launches; PWR
> long-press opens the power menu; set/verify a PIN persists across a reboot. **P4.5** — the migrated USB rows
> start the gadget and run a host-dropped `/sd/macro.txt` as a USB HID keyboard (the M4 round-trip still works
> through the new rows). Re-run the M4 USB regression each phase that could disturb it.
>
> ## 10. Build / flash / PR conventions
> - **Build from PowerShell, NOT Git Bash** (ESP-IDF refuses MSys), with **`-j 2`** (default parallelism OOMs
>   the Windows commit charge; drop to **`-j 1`** if it still OOMs, or reboot to clear a leaked reservation):
>   `python -m platformio run -j 2 -d D:\Docker\NocSif_Firmware\firmware`. Judge success from the **log**
>   (`[SUCCESS]` / `error:` / `memory exhausted`), never the exit code (`Tee-Object` masks it).
> - **Flash `--no-stub`, never force `--baud`** (the S3 USB-Serial/JTAG stub is unstable here). Download mode:
>   **hold BOOT → tap RST → release BOOT**, then flash `firmware.factory.bin`. Watch is **COM7**.
> - **One phase per branch → PR → squash-merge to `main`**, verified on-device first. Neutral wording in every
>   commit/PR/subagent prompt. Use the **register-verification Workflow** pattern (M3/M4) before coding the RTC
>   and PMU registers.
> - The **`gh` CLI** is authorized (`silverwolf2r`). Keep PRs focused; land the RESUME/PLAN/LESSONS doc updates
>   and the continuation meta-prompt (§7) with the phase they belong to.
>
> ## 11. Design language (LOCKED — PLAN §7.4) — hold the line
> Grayscale default · **one soft antique accent** (violet `void` default; the header/clock/tags already use
> `NOCSIF_VIOLET` for live state) · **no hot / neon colours ever** · **NO SCANLINES anywhere** · serif titles
> are the fixed identity; data font + wallpaper + palette are user-selectable (incl. **typewriter**). Any new
> P4 screen (power menu, Settings → Buttons, PIN pad) uses the existing scaffold/tokens so it looks native.
>
> ---
>
> **Produce a phased plan and pause for my review before writing any code.**
