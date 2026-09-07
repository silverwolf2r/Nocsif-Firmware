# Meta-prompt — RAM coexistence remake + code standardization (remediation)

> Paste this into a fresh chat to pick up and **implement** the two analysis docs already merged to `main`:
> `docs/RAM-BUDGET.md` (RAM budget + coexistence remake plan, live-measured) and `docs/ORGANIZATION.md`
> (device-wide standardization catalog). This is an implementation task, not another analysis pass.

## Your role
You are working on **NocSif**, a from-scratch ESP-IDF/LVGL firmware for the LilyGo T-Watch Ultra (ESP32-S3,
8 MB QUAD PSRAM, 16 MB flash). Authorized personal security-testing device; the operator writes/owns the firmware.
Use **neutral device-class wording** in code/commits/prompts (no "attack/target/payload/victim") — charged terms
hard-fail the automated safeguard. Keep load-bearing format names (`.sub`, DuckyScript).

## The mission
Implement the fixes catalogued in `docs/RAM-BUDGET.md` (§ "Pieces designed around RAM — and the remake plan",
18 items) and, where they share a home, the `coex.h` and token work in `docs/ORGANIZATION.md`. Do it in small,
independently flashable, on-device-verified branches — **one work-package per branch → PR → squash-merge to `main`**.

## ⛔ NON-NEGOTIABLE operator constraints (these override any convenience)
1. **NEVER require a watch restart to enable Bluetooth — ever.** Bluetooth must always be available at runtime
   without a reboot. (More generally: **no feature may ever require a restart to use.**)
2. **LoRa must be usable in Signal Hunt** without disabling Bluetooth as a hard requirement.
3. Turning **both BLE and WiFi off while LoRa is active is acceptable** *if the operator chooses it* — but it must
   be **instantly reversible at runtime**, never via a reboot.
4. It is acceptable to **degrade WiFi (cap it at the lean profile) or turn a radio off** to make room — what is
   **not** acceptable is losing Bluetooth until a reboot.

The architecture that satisfies all four (proven by the measurements below): **reserve the BLE controller block
once at boot and NEVER release it; make "BLE off" a logical activity toggle on the still-resident controller;
move LoRa's (and other non-DMA workers') task stack to PSRAM so LoRa never competes for the scarce pool.**

## Read these first (authoritative, already on `main`)
- `docs/RAM-BUDGET.md` — **the plan you are executing.** Start with "Live measured data (COM7)", the conflict
  matrix, and the 18-item remake table. Every byte number below comes from here.
- `docs/ORGANIZATION.md` — the `coex.h` token home + standardization catalog (type scale, safe-zone geometry,
  NVS keys, task-stack tiers). The RAM remake's `coex.h` is the same `coex.h` specified here.
- `docs/PLAN.md` §4.6 (Connectivity Governor — the runtime vehicle), §4.13 (CC tiles reflect live radio state),
  §8 doc map. `docs/LESSONS.md` (BLE/WiFi coexistence, display DMA-hang, stale-sdkconfig gotchas).
- `firmware/sdkconfig.defaults` — richly annotated; the load-bearing RAM config lives here.

## The measured reality you must design against (from live COM7 captures, this build)
- **Scarce resource = ONE CONTIGUOUS run of internal DMA-capable SRAM**, not free bytes. PSRAM is abundant
  (~6.4 MB free, 2 % frag) and is NOT the wall — LVGL objects, canvases, WiFi-LWIP, NimBLE host already live there.
- Boot `largest` (biggest contiguous int-DMA block) timeline: **pre-BLE 90,112 → after BLE controller 59,392 →
  after `esp_wifi_init(lean)` 30,720 → steady (phone+WiFi) ~3,072.** Internal free heap at boot ~180,251.
- **BLE controller needs 31,744 B contiguous** (`BLE_MIN_DMA_BLOCK`). It only ever gets it by being claimed
  **first, at boot, from the pristine ~90 KB pool** (`boot reserve: NimBLE UP`).
- **Runtime teardown NEVER defragments:** largest pins at 15,872 (WiFi up) / 21,504 (WiFi fully deinit'd) /
  9,216 (airplane) and never returns toward 90 KB. So a torn-down BLE can't reclaim 31,744 → `BLE bring-up
  REFUSED` until reboot. **This is the root cause of every "restart to enable Bluetooth" event.**
- **The lean WiFi profile does NOT widen the contiguous hole** (LEAN and FULL both leave largest = 15,872; lean
  only raises *total free* ~10 KB). ⇒ the current runtime "ask WiFi for lean / turn WiFi off to admit BLE" code
  is **structurally dead** (even full WiFi deinit reaches only 21,504 < 31,744). Delete it, don't fix it.
- Display flush stage band = **2×14,760 = 29,520 B permanent internal-DMA** — correct and load-bearing (flush
  then needs zero runtime int-DMA). Keep it.

## Target architecture (implement this)
A single **Connectivity Governor (PLAN §4.6)** sitting on the **`coex.h`** token home (ORGANIZATION.md), owning:
- **(a) Boot reserve, held forever.** Claim the BLE controller's 31,744 B block at boot into the pristine pool and
  never release it for the session. BLE is always available at runtime — this is what makes constraint #1 true.
- **(b) Boot init-order as guarded policy.** Contiguity-critical claimers in order: **BLE block → display 29,520 B
  stage band → I2S TX descriptors → then WiFi.** Add a boot-time assert/panic if any WiFi-touching init runs
  while the BLE reserve is still pending, and log the claim order + resulting `largest` after each claimer.
- **(c) One measurement point.** All `heap_caps_get_largest_free_block(INTERNAL|DMA)` calls route through
  `coex.h nocsif_int_dma_largest()`; all thresholds (BLE 31744, LoRa gate removed, floor 24576) live in one table.
- **(d) PSRAM-first, including task stacks.** Every worker task whose stack neither DMAs from on-stack buffers nor
  runs while the flash cache is disabled gets a PSRAM stack (`xTaskCreateWithCaps` + `MALLOC_CAP_SPIRAM`; enable
  `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY` if not set). **⚠ KEEP the OTA flash-writing worker internal** (PSRAM
  stack is unusable during flash-cache-disable). Validate each moved stack on-device.
- **(e) Honest, restart-free time-sharing.** "BLE off", "WiFi off", "LoRa on" are logical activity toggles over
  resident/PSRAM-backed resources, all instantly reversible. Never a silent `REFUSED`, never a reboot to re-enable.

## Ordered work plan (small branches, verify each on-device before the next)
**Phase 0 — foundation**
1. Land `coex.h` (from ORGANIZATION.md): `NOCSIF_DMA_CAPS`, `nocsif_int_dma_largest()`, `nocsif_log_dma_free()`,
   the per-radio threshold table. Route the existing ad-hoc gates/logs through it. *(Remake #1, #14.)*
2. Add the build-time CI guard that re-greps the load-bearing sdkconfig keys (`LV_USE_CUSTOM_MALLOC`,
   `SPIRAM_MALLOC_ALWAYSINTERNAL`, `BT_CTRL_BLE_MAX_ACT`, `RX_BA_WIN`) so a regen can't silently revert them. *(#14, C5.)*

**Phase 1 — the operator's two hard requirements (highest priority)**
3. **Move LoRa's worker stack to PSRAM** → deletes the `LORA_TASK_MIN_DMA` gate and the "turn Bluetooth off to
   hunt LoRa" wall; then keep the LoRa worker **warm** instead of churning it per hunt-switch, and drop the ~3 s
   busy-wait + 60 ms magic-settle deinit. **This resolves the LoRa↔Bluetooth Signal-Hunt conflict.** *(#8, C12.)*
4. **Guarantee BLE never needs a restart:** enforce the governor invariant that the BLE controller block is
   released only by an explicit BT-master-OFF that KEEPS the block reserved (logical off), never by navigation,
   radio switching, or HID mode. Assert the boot reserve ran before WiFi. *(#2, #3, #4, C1.)*
5. **Delete the dead runtime "make-room-for-BLE" / radio-yield / deferred-enable machinery** (proven incapable:
   21,504 < 31,744). *(#5.)*
6. **Turn the `BLE bring-up REFUSED` / "restart the watch" paths into a no-op** — with the always-resident block
   they should never fire; if a legacy path can still hit it, replace the reboot hint with an instant logical
   re-enable (never a restart). *(#6.)*

**Phase 2 — widen the pool + restore features**
7. Move the other safe non-DMA worker stacks (audio/mic/BLE-PCAP/Ducky/NimBLE-host) to PSRAM; keep OTA internal.
   Returns ~34 KB of int-DMA to the reserve budget and kills the "first-use spawn fails under fragmentation"
   class. *(#7, C10.)*
8. **Restore the Signal-Hunt audio cue** by reserving the small I2S TX descriptors at boot (after BLE, before
   WiFi) — or route I2S DMA to PSRAM if supported; if still impossible, gate the toggle honestly. *(#12, C7.)*
9. **HID mode without a NimBLE teardown:** register HID/DIS/Battery GATT permanently at boot (host tables in
   PSRAM) and gate HID by which service is advertised — removes the last runtime BLE teardown path. *(#10, C13.)*
10. **Raise NimBLE `MSYS_1_BLOCK_COUNT` 12 → 24-32** (lives in PSRAM, near-free) to fix the `GATTC proc alloc
    failed` burst so AMS need not serialize after ANCS. *(#13.)*
11. Wire the §4.13 Control-Center BLE/WiFi tiles to reflect **live** radio state and drive it through the governor
    (a tile tap is a logical toggle, never a crash/refuse/restart). *(PLAN §4.13, #6.)*

**Phase 3 — standardization (ORGANIZATION.md), can interleave**
12. Follow ORGANIZATION.md's rollout order (deletions → palette tokens → safe-zone `ui_metrics.h` → the type-scale
    role-style sweep so "font size everywhere" works → shared builders → NVS keys/strings → platform headers).
    The `coex.h` from Phase 0 is one of its platform headers.

## Verification protocol (do this after every RAM change — non-negotiable)
- Build from **PowerShell** (not Git Bash): `python -m platformio run -e nocsif-twatch-ultra -j 2` (`-j 1` if OOM),
  `PYTHONIOENCODING=utf-8`. Flash `esptool --no-stub` @ `0x20000`.
- Watch **COM7 @115200** (USB-Serial-JTAG — survives a chip reset / System→Restart, drops only on power-cycle).
  `main.c` logs `heartbeat N free heap=… int-dma free=… largest=…` every 5 s. A reconnect-robust pyserial capture
  pattern is in the session scratchpad history; baud is nominal for USB-Serial-JTAG.
- **Acceptance checks:**
  - Enter Signal Hunt, switch radio BLE → WiFi → **LoRa** → BLE repeatedly. **No `LoRa needs memory` / `turn
    Bluetooth off` message; no `BLE bring-up REFUSED`; no reboot ever needed.** BLE remains usable throughout.
  - Confirm `largest` no longer collapses below the LoRa need during a hunt (LoRa stack now in PSRAM).
  - Toggle BLE off then on from the Control Center → **instant**, no restart.
  - Confirm the boot log shows the ordered claim sequence and BLE reserved before WiFi.
  - Watch the heartbeat `largest` across a full session — it must never force a restart to restore a radio.

## Conventions & guardrails
- Rounded-corner display safe zone (recurs): keep UI content centered or within `x∈[48,362] y∈[42,460]`; never
  anchor to a physical corner. (PLAN §1 / `docs/LESSONS.md`.)
- LVGL is single-threaded behind `esp_lvgl_port`; UI callbacks only signal workers.
- GitHub flow: branch → PR → squash-merge to `main`; docs/plan edits are small docs-only PRs.
- Keep `docs/RAM-BUDGET.md` and `docs/ORGANIZATION.md` in sync as items land (tick them off / note "DONE + PR#").

## Definition of done
LoRa is usable in Signal Hunt with Bluetooth on; no operation anywhere in the firmware ever requires a watch
restart to (re)enable a radio or feature; the dead runtime-reclaim code is gone; the int-DMA policy lives in one
governor + `coex.h`; and the standardization rollout has at least reached the type-scale sweep (font size affects
all screens). Every RAM change is verified against the COM7 heartbeat before merge.
