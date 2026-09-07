# LESSONS — hard-won technical gotchas (running log)

Concrete errors we hit and what actually fixed them, so we don't relearn them. Newest first.
Per-milestone findings also live in `docs/RESUME.md`; this file is the cross-cutting, searchable
index of *failures and their root causes*. See also the "load-bearing gotchas" list at the bottom of
`docs/RESUME.md`.

---

## 2026-08-28 — ⚠ IF THE WATCH FEELS SLOW, READ THIS FIRST: Bluetooth-on puts Wi-Fi in a reduced-speed mode
**Read this before debugging any "Wi-Fi got slower" / "capture is dropping frames" / "the UI feels
different" report.** Two deliberate, shipped trade-offs make the firmware slower in exchange for letting
**BLE and Wi-Fi run at the same time**. Neither is a bug; both are reversible.

**1) While Bluetooth is ON, Wi-Fi runs a LEAN buffer profile (slower, especially RX).**
The BLE controller needs ~30 KB of *contiguous* internal DMA. To leave room, `nocsif_wifi_set_lean(true)`
re-initialises the Wi-Fi driver with far smaller queues: `static_rx 4→2`, `static_tx 6→2`,
`dynamic_rx 16→4`, `cache_tx 16→4`, and **AMPDU RX disabled** (`rx_ba_win 0`). The station stays
connected — it is just slower. **Monitor mode, packet capture, PCAP-to-SD and Wardrive feel this most.**
*To get full speed back: turn Bluetooth OFF (System ▸ Connect Phone ▸ Bluetooth). The full profile is
restored automatically.* Buffer counts are runtime fields of `wifi_init_config_t`, so a future change
could let the capture screens request the full profile on entry.

**2) `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` is 4096 (was 16384) — this affects the WHOLE firmware.**
Every `malloc()` larger than 4 KB now prefers **PSRAM** instead of internal RAM. PSRAM is slower than
internal SRAM, so this is the first thing to suspect for a **general, non-radio slowdown or new jitter in
any subsystem** (audio, SD, LoRa, GNSS, file browsing, UI). It was necessary because at 16384 essentially
every allocation hoarded internal RAM. Meshtastic ships 2048 on comparable hardware, so 4096 is
conservative — but it is a system-wide policy change and deserves suspicion before exotic theories.
Explicit `heap_caps_malloc(..., MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL)` callers are unaffected.

Also shipped with these: `CONFIG_ESP_WIFI_IRAM_OPT=n` + `CONFIG_ESP_WIFI_RX_IRAM_OPT=n` (frees >27 KB of
IRAM, which on the S3's unified SRAM becomes DRAM heap — at the cost of Wi-Fi throughput; **ESP-IDF
itself defaults both to `n` when `BT_ENABLED && SPIRAM`**), and the `ota`/`ducky`/`mic` workers are now
created lazily by the screens that use them rather than at boot.

**Root cause worth remembering: the wall was FRAGMENTATION, not free memory.** With Wi-Fi up, the largest
free internal-DMA hole is capped (~27.6 KB full profile, ~31.7 KB lean) *no matter how much total memory
is free* — freeing 10 KB of task stacks moved `free` 28k→38k and left `largest` at 27,648 unchanged. The
fix was **allocation order**: `nocsif_ble_boot_reserve()` claims the controller's block in `main.c`
**before `nocsif_ui_init()`**, while the heap is still one unbroken run (`free=171155 largest=98304`).
It must precede the UI because the Control Center restores its persisted Wi-Fi toggle during UI bring-up
and enables the radio ~1.8 s in — any later hook loses the race. **Chasing `free` is a dead end; only
`largest` matters.**

**⚠ A failed BLE bring-up does not fail gracefully:** `esp_bt_controller_init` logs
`BLE_INIT: Malloc failed` and then the `btController` task blows the interrupt watchdog
(`CRASH RECORD: int-wdt task=btController`) and panics the watch. Hence the measured gate
`BLE_MIN_DMA_BLOCK = 31744` in `ble.c` (27,648 fails, 31,736 works) — never attempt below it. Turning
Bluetooth on *at runtime* while Wi-Fi is up cannot claim a block, so the UI says
"restart the watch to finish enabling it" rather than "failed".

---

## 2026-08-19 — The display physically clips its rounded corners; keep corner content clear of the arc
**Symptom:** the battery indicator in the **top-right of the header** was visibly cut off by the curve.
Not a software mask — the T-Watch Ultra panel is a **physically rounded rectangle**, so pixels inside the
corner arc are simply not there. The software corner masks (`ui_background.c CORNER_R=40`) only *match*
the look; the glass clips a **~56px** radius, a touch MORE than the 40px mask.

**Root cause:** corner UI must clear the arc, and "near the edge" is not enough. A point `(x,y)` in the
top-right corner square is clipped when `(x-(W-R))² + (y-R)² > R²` (R≈56). The header's corner elements
(top-left back arrow, top-right battery) sat at `HEADER_HINSET=48 / HEADER_TOP_PAD=42` — **less than the
corner radius** — so they were nipped even though a *centered* element at the same y would be fine.

**Fix:** header insets bumped to a full corner radius (`HEADER_HINSET=56`, `HEADER_TOP_PAD=56`, ui_nav.c),
so the corner cluster clears the arc on every screen. **General rule (also in the memory
[[gotcha-rounded-corner-safe-zone]]):** never anchor content to a physical corner; keep it centered, or
inside the corner-safe box **x∈[56, 354], y∈[56, 446]** when it must live near a corner (the earlier
x∈[48,362]/y∈[42,460] was too tight for the very corners — fine for straight edges, not the arc). A
full-width row in the vertical MIDDLE is fine at the usual 26px inset; only true corner content needs the
radius of clearance. This RECURS — check every header/peek/overlay that puts something in a corner.

**2026-08-26 — measured the exact bound with an interactive on-device calibrator** (a live rounded-rect
outline tuned with on-screen inset± / radius± buttons, temporarily wired to System > About; §4.1). Dialing it
to trace the visible screen gives the true edge — and the glass is **much rounder than earlier estimates**:
- **Technically-perfect fit: inset 5 px, corner radius 125 px.**
- **Preferred (slightly conservative) working bound: inset 7 px, corner radius 115 px** — use this.
So the ~56 px figure above was a conservative *safe-inset*, not the geometric radius; the real corner radius is
~115–125 px. Clearances that fall out of it, all self-consistent: **straight edges** usable from ~5–7 px; a
**sharp-cornered** element needs **~41 px** inset to clear the arc (matches the old sharp-box ~40 finding);
content that follows the curve can go tighter. **Shared content-text** (`nocsif_content_line`, ui.c) insets to
the 26 px menu column — below-header text rides the straight edge, so 26 clears it with margin. Corner clusters
still want ≥ ~41 px (the header's 56 stays safe). Applied to About + Diagnostics; roll the helper out shell-wide
later.

---

## 2026-08-16 — NFC never reads: proving it's a HARDWARE transmit-path fault (not firmware), tag-free
**Symptom:** ST25R3916 `rfalNfcInitialize` succeeds and discovery runs, but NO tag ever reads (any tag, any
position); LilyGo's own stock NFC app also reads nothing on the same unit.
**Decide hardware vs firmware without a tag** — drive the chip's OWN measurement hardware and log a verdict over
serial (see `nfc.cpp` `hw_selftest()`; the whole ST25R3916 measurement API is public on `RfalRfST25R3916Class`):
- `st25r3916CheckChipID` (digital/SPI alive) + `st25r3916OscOn` + `st25r3916AdjustRegulators` (LDO) +
  `st25r3916MeasureVoltage(mpsv_vdd/_a/_d/_rf/_am)` — proves the chip + every internal rail is healthy.
- Turn the carrier on TWO ways — the real read path (`rfalNfcaPollerInitialize` → `rfalFieldOnAndStartGT`, which is
  Initial-RF-Collision-Avoidance, NOT merely setting `tx_en`) AND a raw `OP_CONTROL=en|tx_en|rx_en` (en_fd=off)
  poke — and for each, **rapid-sample `AUX_DISPLAY.tx_on` in a tight ~40× loop** (catches a momentary assert that
  trips instantly), read `REGULATOR_RESULT.i_lim` (over-current), and `MeasureAmplitude` (RFI carrier).
- **Verdict matrix:** tx_on rises + amp jumps → RADIATE OK (hardware fine; a no-read is positioning/tag). tx_on
  momentary + i_lim set → SHORTED antenna/match. tx_on set + amp flat → OPEN coil/broken match. **tx_on NEVER
  asserts + i_lim=0 + no rail droop → TX output stage never starts → chip TX fault** (this unit's failure).
**This unit:** chip alive (rev 0x02), all rails healthy (VDD 3.3 / VDD_RF 2.95 / VDD_AM 3.25 V), driver at max drive
(TX_DRIVER d_res=0), fieldOn=ERR_NONE, tx_en sets — but tx_on never asserts, no carrier, no over-current. **Proof
it isn't our port:** LilyGo's firmware does the byte-identical init (`pinMode(NFC_INT,INPUT); rfalNfcInitialize()`,
same DLDO1 rail, no analog config / AAT / reset — checked `vendor/LilyGoLib`), so it hits the same dead transmitter
→ hardware RMA. **Lesson:** the ST25R3916's built-in VDD/amplitude/phase/i_lim measurements localize an RF fault to
rails / driver-enable / antenna / output-stage entirely in firmware, no scope needed.

## 2026-08-16 — Flashing + serial tooling gotchas (hit while diagnosing the NFC fault)
- **esptool 5.3.0 crashes with `UnicodeEncodeError` (colorama → cp1252) mid-write** on the Windows console, leaving
  the app partition half-erased (chip then boot-loops `invalid header: 0xffffffff`). FIX: `export
  PYTHONIOENCODING=utf-8` (+ `PYTHONUTF8=1`) before esptool; recovery is just to re-flash with the fix set.
- **COM7 wedges to "A device attached to the system is not functioning" (Win err 31)** — the port still enumerates
  and `Get-PnpDevice` says `OK / CM_PROB_NONE`, yet every open fails; happens when the watch sleeps or the stock
  firmware hangs. esptool retries + `pnputil /restart-device` (needs admin we don't have) can't fix it. FIX =
  physically unplug/replug the cable (and wake the watch).
- **NEVER run two `platformio run` against the same `.pio` dir concurrently** — the two ninja invocations race and
  corrupt objects (`ar.exe: <x>.c.o: No such file or directory` at archive time); the build "completes" but the
  image is garbage. One build at a time; if you backgrounded one, confirm it's truly finished before starting another.

## 2026-08-11 — Tapping any USB mode hard-hung the S3: the display flush's internal DMA bounce OOM'd → LVGL wedged
**Symptom:** After the P4.5.3b HID work, tapping ANY USB mode (File Share / CDC / HID) froze the watch —
last frame stuck, PWR/FN dead, needed a reboot — but the `main` heartbeat kept printing on CPU0 and
`task_wdt` fired repeatedly on IDLE1 with `taskLVGL` running (a **HANG, not a panic**; no reboot). The host
still enumerated the device.
**Root cause:** the CO5300 renders full-frame from a **PSRAM** framebuffer; esp_lcd copies each SPI chunk
(`max_transfer_sz`) through an **internal DMA-capable bounce buffer allocated per flush**. The one-time USB
PHY install (`usb_new_phy`/`tinyusb_driver_install`, first mode pick) plus the mounted 60GB SD consumed
enough internal DMA RAM that the 16-row (~13KB) bounce alloc failed (`setup_dma_priv_buffer: Failed to
allocate priv TX buffer`) → `esp_lcd_panel_draw_bitmap` errored → **no DMA transaction was queued → the
flush-done IRQ never fired → `lv_display_flush_ready` was never called → `disp->flushing` stayed set → LVGL
spun forever in `wait_for_flushing` (lv_refr.c:1429)** holding the port lock (so buttons, marshalled onto
that task, died too). It fit at boot but failed post-install; hit every mode, not just the new HID submenu.
Pinned by decoding the wdt backtrace with `xtensa-esp32s3-elf-addr2line`.
**Fix:** `display.c` `max_transfer_sz` **16→4 rows (~13KB→~3.3KB)** so the bounce survives the pressure;
CS-keep-active means more/smaller chunks add no seams and the per-chunk CPU copy total is unchanged. `main.c`
heartbeat now logs `int-dma free=/largest=` (heap_caps `INTERNAL|DMA`) as an ongoing gauge (idle `largest` ≈
80KB; the install eats most of it). **A DEAD END tried first:** serializing the install against the flush via
`lvgl_port_lock` did nothing — the trigger is DMA-RAM **exhaustion**, not flush **overlap**. **Latent:** a
failed flush STILL wedges LVGL; the 96KB LVGL builtin pool (`CONFIG_LV_MEM_SIZE_KILOBYTES=96`, static
`work_mem_int[]`) sits in INTERNAL RAM — move it to PSRAM and/or make a failed flush non-fatal (call
`lv_display_flush_ready` on a `draw_bitmap` error) to fully harden.
**Also (flash-verification discipline):** the boot banner `Compile time` is **CACHED** — it did NOT change
when only main.c/display.c changed (the app_desc unit didn't recompile), so it is NOT a reliable "which build
is running" check. Verify with a **code-visible marker** (a changed log-line format, e.g. the new `int-dma
free=` heartbeat) or `grep -a` the binary for a unique source string. A `git stash <files>` silently failed
to hold ui.c once here → a "pristine" test build was actually the dirty one; confirm binary CONTENT, not just
"flash succeeded / hash verified".

## 2026-08-10 — A build fails but the Tee log looks clean: read the task `.output` (stderr)
**Symptom:** `python -m platformio run ... | Tee-Object -FilePath $log | Out-Null` exited non-zero and the
`$log` just STOPPED mid-compile with **no `error:` line** — it looked exactly like an OOM/process-kill (and
free commit was 115 GB, so it clearly wasn't OOM). **Root cause:** the Tee log captured only **stdout**;
GCC/clang write compile errors to **stderr**, which a bare `... | Tee` does not capture. The real error
(`implicit declaration of 'nocsif_buttons_set_pwr_double_enabled'` — a missing `#include`) was in the
harness's own task `.output` file (which captures both streams), not in `$log`. **Fix / rule:** to judge a
build, grep the **task `.output` file** (or pipe with `*>&1 | Tee-Object`), not just the stdout Tee log;
an abrupt stop with no `error:` in a stdout-only log is usually a real compile error on stderr, not OOM.
Confirm OOM only from actual free-commit (`Get-CimInstance Win32_OperatingSystem` FreeVirtualMemory).

## 2026-08-10 — Changing a Kconfig value: edit `sdkconfig.<env>`, not just `sdkconfig.defaults`
**Symptom:** bumped `CONFIG_LV_MEM_SIZE_KILOBYTES` in `firmware/sdkconfig.defaults` (48→72) and rebuilt —
but LVGL did **not** recompile and the value stayed 48 (`.pio/build/<env>/config/sdkconfig.cmake` still
`"48"`). **Root cause:** PlatformIO applies `sdkconfig.defaults` only when generating the per-env config
**`firmware/sdkconfig.nocsif-twatch-ultra`** the first time; once that master file exists it is REUSED and
`sdkconfig.defaults` is not re-applied. So a defaults-only edit is silently ignored on an existing checkout.
**Fix:** edit the value directly in **`firmware/sdkconfig.nocsif-twatch-ultra`** (the master PlatformIO
reads) — changing it invalidates `lv_conf`/the LVGL objects and forces the recompile — and also update
`sdkconfig.defaults` so a fresh generation matches. (Deleting the master to force regen works too but
regenerates the WHOLE config from defaults+Kconfig, which can drop any un-captured menuconfig tweaks.)

## 2026-08-10 — Build lock + OOM after the desktop app crashes (zombie linker + leaked commit)

**Symptom.** After the coding desktop app crashed mid-session, every incremental build failed at LINK: first
`libbootloader_support.a: error adding symbols: file format not recognized`, then on retry `sections.ld: The
process cannot access the file because it is being used by another process`. Separately, `Get-CimInstance
Win32_OperatingSystem` showed free commit collapsed to ~2 GB (of a ~128 GB limit) while physical RAM was ~60% free.

**Root cause (two independent crash artifacts).** (1) A **zombie `ld` + `collect2` linker** (plus its
`xtensa-*-gcc` wrappers) from the in-flight build stayed resident when the app died — parent gone, `taskkill /F`
reporting "no running instance" **yet `tasklist` still listing it** holding ~91 MB and an exclusive handle on the
build outputs. (2) ~115 GB of **committed (reserved) memory leaked** — not attributable to any live process
(per-process private bytes summed to ~10 GB) but still counted against the Windows commit limit, so any heavy
compile would OOM (extends build gotcha #4). Neither clears on its own: the `ld` is wedged non-terminable and the
commit stays reserved.

**Fix.** **Reboot.** It reclaims the leaked commit (free commit → ~125 GB) **and** kills the zombie linker (the
build-output lock releases). The clean relink then completed in 85 s. Trying `Stop-Process`/`taskkill /F /T` on
the stale PIDs first is worth it but did **not** work here — the `ld` was in a non-terminable state.

**Carry-forward.** A build that fails at link with "file format not recognized" or "used by another process" —
especially right after an app/IDE crash — is almost never source/archive corruption (verify the `.a`: correct
`!<arch>` magic, non-zero size, siblings fine, no zero-byte archives). Look for a **stale toolchain process
holding the lock** (`Get-Process ninja,cc1,as,ld,collect2,xtensa-esp-elf-gcc`); if it won't die, **reboot** —
which clears the leaked commit charge in the same step. A finished `firmware.factory.bin` from an earlier
successful build is **not** locked by the zombie (it holds build *inputs*), so it stays flashable while you
resolve the lock.

---

## 2026-08-09 — UI freezes after opening a few menus (LVGL object-pool exhaustion)

**Symptom.** On the P3.2 menu shell, opening submenus worked for the first few, then the **2nd–4th**
navigation **froze the UI** (needed a physical RST). Not menu-specific — *any* screen could be "the one"
that froze; the count varied with which screens you had opened. WiFi/USB worked because they were opened
first (before the pool filled).

**What it was NOT.** Not a bad icon glyph (all 38 present, sane box dims), not the nav stack
(`NAV_STACK_MAX=8`, oscillates 0↔1), not a system-heap leak (heartbeat `free heap` stayed dead-flat at
6,568,112 through the freeze), not a crash (main task kept logging heartbeats — a **hang**, not a reboot).

**Root cause.** LVGL's object memory is a **fixed pool** (`CONFIG_LV_MEM_SIZE_KILOBYTES=48`,
`LV_USE_BUILTIN_MALLOC`), **separate from the ESP-IDF system heap**. The shell cached *every* screen root
forever (`app_t.root`, never freed) on the assumption — stated in the P3.1 plan — that "caching all screens
is fine because boot heap is ~8.5 MB." **That heap is the wrong pool.** Screen widgets come from the 48 KB
LVGL pool. After Home (~25 KB) + 2 submenus (~11 KB each) the pool ran dry; the next frame's render tried to
allocate a glyph draw-buffer, the alloc failed, and **LVGL spun instead of aborting** → the LVGL task hung in
`lv_draw_label` while everything else kept running.

**Why the flat heap fooled us.** Because LVGL uses its own pool, building screens does **not** move the
system `free heap` number. A flat heap during navigation was the tell that LVGL was on a separate allocator.

**Fix (this session).** Stop caching unbounded — **free a submenu when it is popped** (`nocsif_nav_back`
deletes the leaving screen; the app registry clears its cached pointer on the root's `LV_EVENT_DELETE`, so a
re-launch rebuilds it). Live memory is now bounded by **nav depth** (Home + one submenu ≈ 37 KB, the config
we proved works), not by how many screens you have ever visited. Screens that own timers/live state can be
**pinned** (`nocsif_nav_pin`, `LV_OBJ_FLAG_USER_1`) to survive a pop; USB is *not* pinned (its poll timers are
deleted with its buttons via an `LV_EVENT_DELETE` cb, and the gadget's real state lives in `usb_gadget.c`, so
a rebuild re-reads it). The per-screen dashed-rule PSRAM buffer — which `lv_canvas` does **not** own — is
freed in the canvas's `LV_EVENT_DELETE` cb so freeing screens does not leak it.

**Why this fix over "just give LVGL more memory."** Bumping `LV_MEM_SIZE` or switching to
`LV_USE_CLIB_MALLOC` (system heap) only *raises* the ceiling and still accumulates; it also forces a **full
LVGL recompile (~710 objects → slow + OOM-prone on this box)**. Bounding the cache is the fix the P3.1 plan
already anticipated ("rebuild leaf screens on entry if memory pressure shows"), scales to unlimited screens
(the capability backlog is 50+), and recompiles **only `ui.c` + `ui_nav.c`** (fast, no full-LVGL rebuild).

**Carry-forward rules.**
- **LVGL widgets do NOT come from the system heap.** They come from the fixed `LV_MEM_SIZE` pool (48 KB).
  Watching `esp_get_free_heap_size()` will not show LVGL pressure — use `lv_mem_monitor()` (only with
  `LV_USE_BUILTIN_MALLOC`) if you need to see it.
- **Do not cache LVGL screens/objects unbounded.** Bound the live set (free on pop) or the 48 KB pool wins.
- **LVGL OOM can present as a HANG, not a clean assert/abort** (a render-path alloc failure spun in
  `lv_draw_label` rather than tripping `LV_ASSERT_MALLOC`). A silent UI freeze with the rest of the system
  alive ⇒ suspect the LVGL task / its pool first.

---

## 2026-08-09 — Diagnosing an ESP32-S3 hang/panic over USB-Serial/JTAG (method)

Transferable method that cracked the freeze above:

1. **A hang vs a crash:** if the app's periodic logs (heartbeats) keep printing but the UI is dead, it is a
   **task hang**, not a panic/reboot. With `CONFIG_ESP_TASK_WDT_PANIC` **unset**, the task watchdog (5 s,
   watching the idle tasks) **prints a backtrace of the stuck task every 5 s without rebooting** — so **hold
   at the freeze ≥ 5 s** to capture it. Hitting RST too early captures nothing.
2. **Capture must survive a reset.** On USB-Serial/JTAG (COM7 here, ROM device VID 0x303A **PID 0x1001**), a
   chip reset/re-enumeration **kills the COM handle** (`ClearCommError ... PermissionError(13)`), and panic
   text can be **lost** as the port drops. Use a **resilient reconnecting capture** (reopen on drop; do NOT
   toggle DTR/RTS on open, or you reset the chip). Script: `scratchpad/cap.py [seconds] [outfile]`.
3. **Decode the backtrace against the *matching* ELF.** `firmware/.pio/build/nocsif-twatch-ultra/firmware.elf`
   (verify its mtime is newer than the sources = it matches the flashed image), with
   `…/toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf-addr2line.exe -pfiaC -e <elf> <PC0> <PC1> …`.
   Identical repeating backtraces across watchdog firings = a real spin at a fixed location.

---

## Render-path performance: the full-frame + orrery + PSRAM floor, and why dual-core is a weak lever (2026-08-10)

Scroll and menu transitions felt laggy/jumpy. Root of the *workload*: the UI renders **full-frame**
(`full_refresh=true` + two 410×502 RGB888 draw buffers in PSRAM) and every screen root is **transparent**, so
the persistent full-screen orrery on `lv_layer_bottom()` is **re-composited into the frame buffer on every
motion frame** — ~1.85 MB of PSRAM traffic per frame (the P3.1 "smoothness ceiling"). Idle screens don't
repaint, so it only bites during scroll/transition.

**What actually helped (cheap, zero-code, merged):** the build had been at ESP-IDF's stock **`-Og`** (optimize
for debugging) with the core at **160 MHz** — both just untouched defaults. LVGL's software compositor is
CPU-bound per-pixel C loops (no NEON on Xtensa, `LV_DRAW_SW_ASM_NONE`), so those defaults left it running well
below its ceiling. Switching to **`-O2` (`CONFIG_COMPILER_OPTIMIZATION_PERF=y`) + 240 MHz
(`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y`)** — in `sdkconfig.defaults` — cut **Home build 211 → 132 ms (~1.6×)**;
on-device it felt "smoother but still jumpy." Assertions stay enabled (separate symbol). This is a **full
rebuild** (opt-level change invalidates every object; ~11 min at `-j 2`, no OOM). App image grew ~15 KB.

**Why dual-core software rendering (`LV_DRAW_SW_DRAW_UNIT_CNT=2` + `LV_OS_FREERTOS`) is a WEAK lever for THIS
layout** (verified by a 3-source workflow against the pinned lvgl 9.3 source, high confidence — so we did NOT
enable it):
- LVGL parallelizes at **draw-TASK** granularity (one primitive = one task); it **never splits one primitive**
  (a fill/blit) across cores. `lv_draw_sw.c` spawns N `swdraw` threads that each take one *independent* task.
- **The killer:** `lv_draw_get_next_available_task` (`lv_draw.c:~337`) returns NULL while the layer's **first
  task is screen-sized and in-progress**. The orrery **is** that first full-screen blit, so the 2nd unit
  **idles until the orrery finishes** each frame. Parallelism only helps the smaller non-overlapping foreground
  widgets afterward — and both cores then contend for the **same PSRAM** (bandwidth-capped). Realistic gain:
  **~1.2–1.5× on menu-dense screens, ~1× on the orrery-dominated slice** — not 2×. Cost: ~16 KB internal RAM
  (2× 8 KB draw-thread stacks). Deferred as not worth it here.
- (If ever revisited: it's **low-risk** — draw threads are unpinned prio 3, the esp_lvgl_port task is prio 4,
  TinyUSB prio 5, touch is read inside the port task, and `task_wdt` is **warn-only** here (`PANIC` off). The
  gate is *value*, not safety.)

**The real lever for the residual jumpiness (a P5-motion-class change, not a config flip):** shrink or eliminate
the **per-frame full-screen orrery re-composite** — it is both the un-splittable dominant draw task *and* the
PSRAM hog. Options to explore later: avoid re-blitting the full-screen orrery every frame; cut per-frame PSRAM
traffic; or, only if going dual-core, **tile the orrery into horizontal bands** so it stops being one
un-splittable screen-sized first task (LVGL won't tile a single primitive for you).

**Update (2026-08-12, after #56 partial refresh) — the scroll bottleneck MOVED; the floor is now accepted, not chased.**
The section above was written under `full_refresh=true` (whole orrery re-composited every frame). #56 switched to
**partial refresh** — comet/header/most motion went 8 → 40–50 fps, but large-area frames (menu scroll, a full-screen
slide/fade) stay at the ~8–9 fps floor. Re-measured on-device (the `NOCSIF_UI_FRAME_TIMING` instrument) with the
orrery removed and with alternative buffers:
- **The orrery is no longer the scroll bottleneck.** Fill-only (orrery hidden, solid void fill): scroll 8–9 → only
  **~11 fps** (composite ~125 → ~85 ms). The full-screen orrery re-blit is only ~25–30 ms of a scroll frame now; the
  dominant **~85 ms is single-threaded RGB888 glyph rasterisation**. Partial refresh already banked the orrery win
  that `full_refresh` used to pay every frame — so "shrink the orrery re-composite" is now a **weak** scroll lever
  (only +2 fps), though it still matters for orrery-dominated idle frames (comet/watchface).
- **Partial SRAM band buffers were RE-TESTED on the current UI** (the P3.1 banding verdict predated most of the UI):
  **regressed to ~7–8 fps AND still banded** on load/scroll (on-device, user-confirmed). Band-flush overhead
  (~16 bands/frame) + the still-PSRAM orrery *source* outweigh faster SRAM draw-buffer writes → **full-frame PSRAM
  re-validated** with current data.
- **Dual-core still declined** (PR #33 reasoning re-derived from the pinned source): the orrery is one un-splittable
  screen-sized first task that idles the 2nd unit, and PSRAM bandwidth caps the rest → ~1.2–1.5×.
**Resolution (shipped):** the scroll rate is an **architectural floor** (full-viewport RGB888 software text at
240 MHz), not a bug — every config/buffer lever is spent or disproven. Accepted; the shapeable part (the
post-release fling coast that read as "jumpy") is damped via `lv_indev_set_scroll_throw(30)` (was 10) so it settles
quickly. During-DRAG steppiness IS the frame rate and is unchanged. **P7 tilt-parallax is backlogged for the same
reason** — moving the whole background every tilt frame is the same large-area floor (~10–20 fps even sprite-based);
it needs a fundamentally cheaper full-screen-motion path or a discrete tilt-**settle** effect, not live parallax.

---

## Older gotchas

The big earlier findings (many verifiable only on-device) are recorded inline in `docs/RESUME.md`'s
per-milestone entries and its "Two load-bearing gotchas" / build-memory-exhaustion section — e.g.
`CONFIG_LV_COLOR_DEPTH_24=y` (not `=24`), the CO5300 2-px rounder (odd-column shear), `lv_draw_arc` tripping
the watchdog on the orrery, `translate_x` inert on a screen root (use `lv_obj_set_x`), full-frame PSRAM +
`full_refresh` for atomic frames, and the Windows commit-charge build OOM (`-j 2`, drop to `-j 1`).
