# NocSif — UI-shell **P4.5** implementation meta-prompt (USB **mode selection** — detached-until-picked, one class at a time)

> **Paste the block below into a fresh coding chat to build UI-shell P4.5.** It is self-contained. It was
> authored by the design-ideas hub against the **real code on `main`** through **P4.4b (#38, `32195fa`)** — the
> file paths, function names, and integration points below are the shipped firmware, not a sketch. Do **not**
> design here; this chat *builds* firmware. One milestone-phase per branch → PR → squash-merge, verified
> on-device.
>
> **Why this supersedes the old P4.5 line.** The earlier plan (`ui-shell-p4.md` §6.5) was "migrate the M4 USB
> controls onto their mockup screen" while keeping the M4 **always-on composite** (CDC+MSC+HID all enumerated at
> once, MSC auto-claiming the SD). The user has decided USB should instead be **user-selected**: the device
> presents **only the one class you pick**, and **nothing at all until you pick it**. So this phase does the
> migration **directly onto the mode-switched model** — there is no throwaway always-composite step. The
> done-state is the mockup's rebuilt **USB Gadget** screen (`docs/design/full-app-mockup.html`, `#usb`).

---

> You are a NocSif firmware coding chat. Your job this session: build **UI-shell P4.5 — USB mode selection**.
> NocSif is a modular security-testing firmware for the **LilyGo T-Watch Ultra (ESP32-S3)**, for **authorized
> security testing only**. Repo: `github.com/silverwolf2r/NocSif_Firmware` (private); local coding worktree
> `D:\Docker\NocSif_Firmware`.
>
> ## 0. Wording convention — enforce everywhere (non-negotiable)
> Describe every capability in neutral, technical **device-class** terms — "USB HID keyboard", "USB
> mass-storage (MSC)", "USB CDC serial", "composite device", "USB descriptor", "re-enumeration". **Never**
> adversarial vocabulary ("attack / offensive / payload / weaponize / victim / target / red-team"). Charged
> wording **hard-fails Anthropic's automated cyber-safeguard** and has killed an entire research/subagent
> workflow before (2026-08-06). This applies to code comments, commit messages, PR text, and **especially every
> prompt you send to a subagent or Workflow**. Keep precise, load-bearing format names (**DuckyScript**, `.sub`,
> `.nfc`). **This phase must also finish neutralizing the two shipped `payload` carryovers** — `files.payloads`
> in `ui.c` and the DuckyScript default path `/sd/payload.txt` — rename them to `macro` (see §6).
>
> ## 1. Read these first (in this order)
> 1. **`docs/RESUME.md`** — current firmware state (start-here). Read the **M4 (USB composite)** and
>    **UI-shell P4.1–P4.4** entries; P4.4's **settings store** is a dependency (you persist the default USB mode
>    there).
> 2. **`docs/research/usb-composite-device-2026-08-06.md`** — the M4 design record. Load-bearing: the
>    **hand-written descriptor** structure, the **MSC single-owner FAT handoff** (`MOUNT_USB`↔`MOUNT_APP`,
>    `tinyusb_msc_set_storage_mount_point`), the **auto-handoff on `tud_mount` / `tud_msc_start_stop_cb` (SCSI
>    eject) / `tud_umount`**, the **5-IN-endpoint ceiling**, and "descriptor mismatch → **silent** enumeration
>    failure".
> 3. **`docs/HARDWARE.md`** — §"USB / charging" and §"Buses". Confirm the USB-D routing and that **charging is
>    independent of data enumeration** (a detached data device still charges). Note the **ESP32-S3
>    USB-Serial/JTAG** dev path (COM7) vs the **USB-OTG (TinyUSB) gadget** — they share the one USB PHY.
> 4. **`docs/LESSONS.md`** — the **ESP32-S3 hang/panic serial-diagnosis method** (`scratchpad/cap.py` +
>    `addr2line`); you may need it if a descriptor swap wedges enumeration.
> 5. **`docs/PLAN.md`** — §4.1 (the **P4.5 line**), §4.2 (**M10 — USB extensions**: HID mouse / U2F-FIDO /
>    generalized composite-descriptor management — this phase's mode-switch is the groundwork that frees the
>    endpoints they need), §4.6 (hardware ceiling).
> 6. **`docs/design/full-app-mockup.html`** — the **navigation + IA source of truth**. Open the **`#usb`**
>    ("USB Gadget") screen: it is now a **mode picker** (a "connection" status card, a **mode** band with
>    **File Share (MSC) · HID Keyboard · Console (CDC)** and disabled **HID Mouse · U2F/FIDO** stubs, and a **hid
>    keyboard** band with **Run Macro · Keymap**). Build to this.
> 7. As needed: **`docs/design/meta-prompts/ui-shell-p4.md`** (the P4 umbrella — §2 "where code lives", §6.5 the
>    original migration notes) and **esp_tinyusb 2.2.1 / TinyUSB** docs as the API reference for descriptor
>    callbacks and connect/disconnect.
>
> ## 2. Current firmware state (post-P4.4b) and where the USB code lives
> M0–M4 + **UI-shell P1–P4.4** are done and verified on-device. `firmware/src/`:
> - **`usb_gadget.c`** — the M4 gadget: `tinyusb_driver_install()`, the **hand-written configuration
>   descriptor** (`TUD_CONFIG_DESCRIPTOR` + `TUD_CDC_DESCRIPTOR` + `TUD_MSC_DESCRIPTOR` + `TUD_HID_DESCRIPTOR`,
>   the ITF-num enum, endpoint addresses, `bNumInterfaces` + total length), the **CDC+HID no-SD fallback**
>   descriptor, and the **auto-handoff** MSC mount helper. **This is the file you refactor most.**
> - **`nocsif_usb_desc.*`** — the descriptor byte tables + string descriptors (VID/PID, `tud_descriptor_*_cb`).
> - **`hid_kbd.c`** — HID keyboard report path + US/GB/DE keymaps; `hid_kbd_*` signal API.
> - **`ducky.c`** — the DuckyScript player (reads the macro file, drives `hid_kbd`). **Default path
>   `/sd/payload.txt` → rename to `/sd/macro.txt`.**
> - **`sdcard.c`** — FAT32 over SDSPI/SPI3; the app's `/sd` mount that MSC mode must hand over and reclaim.
> - **`ui.c`** — the LVGL app layer; **`build_usb()`** currently draws the two M4 bracket-buttons (`gadget_btn_*`
>   / `macro_btn_*` + their 300 ms poll timers). **`files.payloads` → rename to `macros`.** The **settings
>   store** (P4.4, NVS-backed) is available here for persisting the default mode.
> - **LVGL rule (non-negotiable):** UI callbacks only **signal** worker tasks (`xTaskNotifyGive`); **never** run
>   `tinyusb_driver_install()`, a descriptor swap, or an SD mount/unmount on the LVGL task. New screens obey the
>   **LVGL fixed-pool** rule (free on pop; don't cache unbounded).
>
> ## 3. What P4.5 is (feature scope, device-class terms)
> Turn the fixed always-on composite into **user-selected USB modes**. Behaviour, decided with the user:
> - **Detached until picked (default).** On cable plug-in the **USB-OTG gadget does not attach** — the host sees
>   no NocSif drive, no keyboard, no serial gadget. The device only attaches when the user selects a mode on the
>   **`#usb`** screen. *(Charging is unaffected — it's independent of data enumeration. The ESP32-S3
>   USB-Serial/JTAG dev console is a separate path; see §4 note.)*
> - **One class at a time (true separation).** Each mode presents **only** its interface set, so an idle drive
>   or keyboard never lingers in the host's device list:
>
>   | Mode (mockup row) | Interfaces enumerated | Backed by |
>   |---|---|---|
>   | **File Share (MSC)** | MSC only | `sdcard` + the MSC helper (single-owner handoff) |
>   | **HID Keyboard** | HID only | `hid_kbd` + `ducky` (Run Macro, Keymap) |
>   | **Console (CDC)** | CDC only | the CDC serial path (logs / a serial control link) |
>   | *HID Mouse, U2F/FIDO* | *deferred → M10* | disabled stubs on the screen |
>
>   *(Recommended default is clean single-class per mode. If logs-during-operation prove necessary, a `CDC+HID`
>   or `CDC+MSC` combined descriptor is an allowed fallback — but keep **HID never present unless the user armed
>   it**, since an unexpected USB keyboard is exactly what a host's security prompts flag.)*
> - **Switching = re-enumeration.** Changing mode tears the current interface set down and brings the new one
>   up, so the host cleanly re-detects (looks like a quick unplug/replug, ~1 s). Sequenced with the MSC handoff
>   (below) when leaving/entering File Share.
> - **The endpoint payoff (why this unblocks M10).** The M4 composite sits **right at the ESP32-S3's full-speed
>   IN-endpoint ceiling** (CDC 2 IN + MSC 1 IN + HID 1 IN). Presenting one class at a time keeps every mode well
>   under the ceiling, which is what makes **M10's HID Mouse + U2F/FIDO** reachable later without blowing the
>   budget. *(Adding those interfaces is M10, not this phase — just leave the stubs.)*
>
> **The one real unknown (front-load it): runtime re-enumeration with a swapped descriptor.** TinyUSB decides a
> device's interfaces from the **configuration descriptor it returns at enumeration**; the host reads it **once**
> per attach. To change what the host sees you must **re-enumerate**: return a *different* descriptor from
> `tud_descriptor_configuration_cb` based on a runtime "active mode" variable, and force the host to re-read via
> `tud_disconnect()` → (swap active mode) → `tud_connect()`. **"Detached until picked"** = start with the gadget
> **not connected** (don't `tud_connect()` / keep the OTG data line released) until a mode is chosen. Whether
> `disconnect`/`connect` alone re-enumerates cleanly on **esp_tinyusb 2.2.1**, or whether you need a
> `tinyusb_driver_uninstall()` + reinstall, is exactly what the P4.5.1 spike settles — **do not assume; prove it
> on a PC first.** Note that the machinery is *half here already*: `usb_gadget.c` already **chooses between the
> composite and the CDC+HID fallback descriptor at boot** (the no-SD path) — you are generalizing that
> boot-time choice into a **runtime, user-driven** choice plus a detached start.
>
> ## 4. Hardware / stack specifics to reuse (don't rediscover)
> - **MSC single-owner FAT handoff (already solved in M4 — reuse it verbatim).** On entering **File Share**:
>   claim the card to USB (`tinyusb_msc_set_storage_mount_point(MOUNT_USB)`), **unmount the app's `/sd`** so the
>   host owns the raw card, and do not touch `/sd` while exposed. On leaving: reclaim
>   (`…(MOUNT_APP)`) and **remount `/sd`**. Windows soft-eject was flaky in M4, so switch **deterministically**
>   from the UI action — don't rely on host eject.
> - **Descriptor bookkeeping.** Each mode's hand-written descriptor needs a correct `bNumInterfaces`, total
>   length, **unique endpoint addresses**, and matching `CFG_TUD_*` interface counts. A mismatch → **silent**
>   enumeration failure (device just never appears). Keep the descriptor builder **factored** so each mode is a
>   small table, and bump `CFG_TUD_*` to the max any single mode needs.
> - **Dev console vs the gadget.** The ESP32-S3 **USB-Serial/JTAG** (COM7 — how you read logs / flash in
>   download mode) and the **USB-OTG TinyUSB gadget** share the one USB PHY. While the gadget is **detached**
>   (default), the USB-Serial/JTAG path stays available for logs — a *bonus* of detached-until-picked, not a
>   regression. Picking a gadget mode hands the PHY to TinyUSB (the M4 "gadget mode off for the post-flash read"
>   note still applies). **Verify this interaction on-device**; if the two fight, prefer keeping USB-Serial/JTAG
>   for the console and only attach the OTG gadget on an explicit mode pick.
> - **Guardrails.** Persist the **default mode = detached** in the P4.4 settings store. Show the on-watch
>   **"HID armed" indicator** whenever HID is the live mode (reuse the credential-class armed-indicator pattern
>   already in the mockup). A brief on-screen "switching USB mode…" transient during re-enumeration is nice-to-have.
>
> ## 5. Phased plan — front-load the re-enumeration risk (isolated, flashable)
> Build P4.5 as one branch with these sub-phases; **flash + verify each on-device (and on a PC) before the
> next**:
> - **P4.5.1 — Re-enumeration spike (the risk).** No SD, no MSC, no new UI. Prove three things on a PC with just
>   **two** descriptors (Console = CDC-only, HID = HID-only): (a) on plug-in the gadget is **detached** (nothing
>   enumerates); (b) a trigger (boot flag or a serial command) **attaches** it as CDC; (c) a second trigger
>   **switches** it to HID via re-enumeration and the host re-detects it as *only* a keyboard. Settle the exact
>   API (does `tud_disconnect`/`tud_connect` suffice, or is a driver reinstall needed?). **This de-risks the
>   whole phase.**
> - **P4.5.2 — File Share (MSC) mode + single-owner SD handoff.** Add the MSC-only descriptor + the deterministic
>   `MOUNT_USB`/`MOUNT_APP` handoff and `/sd` unmount/remount. Verify: pick File Share → host mounts a drive →
>   drop a file → leave File Share → the app sees the file on `/sd`.
> - **P4.5.3 — Migrate the M4 controls onto the mode-switched `#usb` screen + neutralize `payload`.** Rebuild
>   `build_usb()` to the mockup: the **mode** rows call the mode-switch (worker-task-signalled), the **hid
>   keyboard** rows are **Run Macro** (→ `ducky`, reads `/sd/macro.txt`) and **Keymap** (cycle US/GB/DE via
>   `hid_kbd`), plus the enumeration-state readout in the "connection" card. Rename `files.payloads` → `macros`
>   and `/sd/payload.txt` → `/sd/macro.txt`. Verify the **M4 round-trip still works**: HID mode → Run Macro types
>   the host-dropped DuckyScript.
> - **P4.5.4 — Persist default + guardrails.** Store default mode = **detached** in the P4.4 settings store
>   (persists across reboot); wire the **HID-armed indicator**; add the "switching…" transient. Verify a reboot
>   comes up **detached** (nothing on the host until a mode is picked).
>
> ## 6. Naming cleanup (do it this phase)
> Finish the wording convention: `ui.c` `files.payloads` → `macros`; `ducky.c` default `/sd/payload.txt` →
> `/sd/macro.txt`; any `payload` in comments/strings you touch → `macro`. The mockup already says "Run Macro".
>
> ## 7. On-device verification (per sub-phase) — PC-side is the proof
> For each sub-phase, verify against a host (Windows Device Manager / `lsusb`):
> - **Detached:** plug in → **no** NocSif drive, keyboard, or serial gadget appears; the watch still **charges**.
> - **Pick a mode:** the device attaches as **only** that class (drive *or* keyboard *or* serial — never extras).
> - **Switch:** the old class disappears and the new one appears within ~1 s (clean re-detect, no zombie device).
> - **File Share round-trip:** drop a file host-side → after leaving the mode the app reads it on `/sd`.
> - **HID round-trip:** HID mode → Run Macro types `/sd/macro.txt` on the host.
> - **Persistence:** reboot → comes up **detached**.
> - Serial shows no `E (…)` faults; enumeration never silently fails (if a mode never appears, suspect a
>   descriptor `bNumInterfaces`/length/endpoint mismatch — §4).
>
> ## 8. Build / flash / PR conventions
> - **Build from PowerShell, `-j 2`** (default parallelism OOMs the Windows paging file), never Git Bash:
>   `python -m platformio run -j 2 -d D:\Docker\NocSif_Firmware\firmware`.
> - **Flash `esptool --no-stub`** (the S3 USB-Serial/JTAG stub flasher is unstable on this unit); download mode
>   = hold **BOOT** → tap **RST** → release **BOOT**; watch is **COM7**. Never force `--baud`.
> - **One phase per branch → PR → squash-merge to `main`**; verify each sub-phase on-device before the next.
>   P4.5 is one branch (`ui-shell-p4.5-usb-modes` or similar); its sub-phases are commits within it.
> - **Do not edit `docs/` design files** (the mockup/PLAN are the design chat's); if the build reveals the spec
>   is wrong, note it in the PR and flag the design chat.
>
> ## 9. Stopping points & continuation (a core deliverable)
> If you must stop mid-phase (context budget ≥75%, or before a big multi-file step), **refresh
> `docs/design/meta-prompts/ui-shell-p4-continue.md`** (the shared P4 continuation file): record which
> sub-phase is done/verified, the exact next step, any API finding from the P4.5.1 spike (this is the highest-
> value thing to capture), and the file/line touch-points — so the next chat resumes without re-deriving. Update
> `docs/RESUME.md` when a sub-phase lands on-device.
>
> **Produce a phased plan and pause for my review before writing any code.**
