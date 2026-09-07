# NocSif — Independent on-device verification of the internal-DMA coexistence plan

- **Evaluates:** `docs/DMA-COEXISTENCE-PLAN.md` (and `docs/RAM-BUDGET.md`, the prior analysis).
- **Board / build:** LilyGo T-Watch Ultra, ESP32-S3, 8 MB PSRAM. Branch `Clankert/usb-msc-entry-crash-8fc5c7` + a compile-gated instrumentation harness (`NOCSIF_COEXV` / `NOCSIF_COEXV_ALL` in `firmware/src/main.c`, tag `COEXV_a7c3`). The verified `bhy2.c` IMU FIFO-drain bound was kept; `msc_shed_wifi` was left untouched.
- **Method:** per-stage int-DMA snapshots (`nocsif_int_dma_largest()` + `heap_caps_get_free_size(INTERNAL|DMA)`) after each subsystem in `app_main`, plus an all-features co-residence probe at steady state. Flashed `--no-stub` over COM7, captured with a reconnecting pyserial reader. No USB PHY switch — COM7 stayed live. **Every number reproduced byte-for-byte across two independent boots**; raw logs `scratchpad/coexv_capture.log`, `coexv_capture2.log`.
- **Revision note:** this is the **corrected** version. The first draft contained a wrong headline claim (that the display flush stage, not `esp_wifi_init`, caused the 30,720 cap). That was a methodology error — see the retraction log at the end. The doc's WiFi attribution is correct.

> ## VERDICT
> 1. **Are the plan's measurements right? Yes — independently confirmed.** Post-BLE-reserve largest = **59,392** (exact match). After `esp_wifi_init` (lean) = **30,720** (exact match; WiFi's own gauge line agrees). Steady state ≈ 2,048 (doc ~3,072; same regime). Pristine pre-BLE on this build is 86,016 vs the doc's 90,112 — a build difference, not an error.
> 2. **Does everything co-reside on the current build? No — proven on device.** At steady largest ≈ 1,984 (BLE resident + WiFi associated + display + audio + IMU), **GNSS refuses (task-create NO_MEM), LoRa refuses (avail=0), and the USB File-Share entry block refuses (5 KB → NULL).**
> 3. **The plan's relocation lever is real** (a PSRAM-stacked worker spawned at the same instant an internal-stacked one failed) — **but its single biggest reclaim, taskLVGL (16,384 B), is unsafe**, as is weather (8,192 B): both write NVS on-task, so a PSRAM stack faults during the flash write. ~24.5 KB of the claimed reclaim is unavailable.
> 4. **Whether a fully-relocated build fits is unproven.** It must be settled by the boot-order reservation test on-device, not by summing line items without a `.map`.

---

## 1. The per-stage curve — confirmed, with one important ordering fact

**Measured (identical on both boots), annotated against the raw driver log:**

| Stage | int-DMA free | **largest** | what the log shows ran in this window |
|---|---:|---:|---|
| pre-BLE | 167,307 | **86,016** | display/touch/RTC up; pool ~pristine |
| post-`nocsif_ble_boot_reserve` | 123,715 | **59,392** | BLE controller claimed first — **matches doc exactly** |
| post-`nocsif_audio_boot_reserve` | 119,559 | 55,296 | I2S TX DMA (stereo, 4 desc) |
| post-`nocsif_ui_init` | 38,355 | **30,720** | **taskLVGL (1935 ms) + flush stage (1939 ms) + `esp_wifi_init` (2174–2186 ms) — all inside `ui_init`**; WiFi's own gauge: `largest=30720` — **matches doc exactly** |
| post-`nocsif_sdcard_init` | 36,143 | 30,720 | raw card |
| post-`nocsif_usb_gadget_init` | 20,643 | **16,384** | 6,144 internal worker stack + ~8 KB MSC storage (runtime alloc) |
| post-`nocsif_imu_init` (+weather) | 5,587 | **3,456** | weather (8,192) + IMU (6,144 + FIFO) internal stacks |
| heartbeat 0 | 4,079 | 2,048 | buttons / boot chime / misc |
| WiFi **associated** (~7.1 s; IP at 8.4 s) | 3,787 | **1,984** | association costs ~64 B — pool already claimed |

**Key ordering fact (this is what the first draft got wrong):** `esp_wifi_init` runs **inside `nocsif_ui_init`**, ~2.17 s, triggered by the Control-Center WiFi-toggle restore — exactly as the `app_main` comment states. The WiFi worker is created *there* (log: `wifi worker ready` at 2169 ms), not at main.c's later `nocsif_wifi_init()` call, which is effectively idempotent. **Any snapshot taken after `ui_init` already includes WiFi.** Because there is no snapshot between the flush-stage allocation (1939 ms) and `esp_wifi_init` (2174 ms), the 55,296 → 30,720 drop cannot be decomposed into "flush" vs "WiFi" from this data — but WiFi's own gauge reading 30,720 right after init confirms the doc's attribution.

**What this adds to the doc's timeline:** the doc jumps from 30,720 (after WiFi) straight to ~3,072 (steady). The device shows what eats that 30,720: **`usb_gadget_init` (−14,336)** and the **weather + IMU internal stacks (−12,928)**. Those are precisely the plan's relocation targets (IMU and usb_gadget → PSRAM; weather must stay, see §3), so this *supports* the relocation lever and pinpoints where the post-WiFi hole goes. Minor doc drift: the MSC ~8 KB buffer is allocated at runtime in `usb_gadget_init`, not "static .bss present from boot" as the master table says.

---

## 2. Does full co-residence fit? — Not on the current build (device-proven)

All-features probe at steady state (BLE resident + WiFi associated + display + audio + IMU up), bringing up what a normal boot leaves down. Identical on both boots:

| Step | result | free | largest | shortfall |
|---|---|---:|---:|---|
| baseline | — | 3,787 | 1,984 | — |
| + GNSS (`nocsif_gnss_init`) | **`E gnss: failed to create gnss worker task` → NO_MEM** | 3,787 | 1,984 | 6,144 internal stack, ~4,160 short |
| + LoRa (`nocsif_lora_init`) | ESP_OK but **avail=0** (did not arm) | — | — | ≥12,288 gate vs ~2 KB |
| + USB File-Share entry (5,120 int-DMA proxy) | **NULL — would refuse** | — | — | ~3,100 short at 1,984 |

**GNSS, LoRa, and USB File-Share cannot be brought up alongside BLE + WiFi + display + audio + IMU on the current build.** (The probe also exercised a resident mic — that step is **struck**: the mic is not a requirement, and GNSS had already failed before it ran. It did incidentally show that driving int-DMA to ~200 B produces `wifi:mem fail` — WiFi itself degrades under pool exhaustion.)

**The relocation lever, demonstrated:** at largest = 1,984, a PSRAM-stacked worker spawned successfully while GNSS (internal 6,144 stack) failed to create — same pool, same instant. A feature with an internal stack cannot start once the pool is gone; a PSRAM-stacked one can.

---

## 3. Must-keep list — and what the plan mis-classified

Verified must-stay-internal:
- **BLE controller (31,744), claimed FIRST.** Enforced in code (`wifi` bring-up asserts `nocsif_ble_boot_reserve_ran`; `ble.c:1555/3158`); `BT_CTRL_BLE_MAX_ACT=3` (floor) confirmed live.
- **Display flush stage (29,520) — cannot move to PSRAM.** `esp_lcd_panel_io_spi` never tags transactions `SPI_TRANS_DMA_USE_PSRAM` (`ui.c:20091`, `display.h:26`); a PSRAM source forces the per-flush bounce behind the #95 DMA-hang. Only lever: shrink 12→8 lines (−9,840).
- **Cache-off-pinned stacks (audit-confirmed):** main, ble, NimBLE-host, wifi-command, logbook (+flush), coredump, OTA — each does an on-task internal-flash op.

**Plan mis-classifications (movable → must-stay):**
1. **`taskLVGL` (16,384) — the plan's largest single reclaim — is UNSAFE.** UI event callbacks run on this task and write NVS (e.g. `reduce_motion_click_cb` → `nocsif_settings_set_i32`, `ui.c:13210`; `nocsif_settings_set_*` = `nvs_set` + `nvs_commit`, `settings.c:79`). A PSRAM stack would fault on routine settings toggles, not only the rare OTA-erase case the plan flagged.
2. **weather worker (8,192) — UNSAFE.** NVS on-task in `adopt_fix` (`weather.c:382`) and `do_fetch` (`weather.c:322`).
3. Minor: wifi command worker appears in both lists; it is correctly **pinned** (`do_forget` writes NVS, `wifi.c:1207`).

**Plan open questions CLEARED (safe to move):** usb_gadget worker (MSC-mount/FAT-remount path is SD/SPI only, no internal-flash op — also confirms the File-Share entry crash was the IMU task-WDT, not a flash-cache fault), buttons worker (marshals to LVGL via `lv_async_call`), IMU, GNSS, NFC, wifipcap/wifihc/wifidns/wifiparse.

Net: **~24.5 KB the plan counted as reclaimable cannot safely move.**

---

## 4. Downsides (the ones that survive scrutiny)

1. **The plan's biggest lever is dead.** taskLVGL → PSRAM faults on ordinary settings changes. Remove it (and weather) from the reclaim before trusting any budget.
2. **The post-WiFi hole is consumed by internal stacks the plan wants to move — good — but weather can't.** After WiFi caps at 30,720, usb_gadget (6,144) + IMU (6,144) + weather (8,192) stacks carve it to ~3.4 KB. Relocating usb_gadget + IMU recovers ~12 KB of that; weather's 8,192 stays a permanent post-WiFi subtraction.
3. **WiFi degrades under exhaustion.** `wifi:mem fail` appeared once int-DMA was driven to ~200 B. Any consumer that empties the pool at runtime doesn't just refuse itself — it starts failing WiFi's own allocations.
4. **Runtime bring-up of an un-reserved feature refuses.** GNSS/LoRa brought up at steady state refused. The co-resident rule ("everything combinable, no reboots") therefore requires every feature that must be combinable to be **reserved at boot** — every optional feature becomes a permanent boot-time int-DMA cost.
5. **No `.map`.** Every margin is empirical. The plan's "~65 KB headroom" cannot be checked without a linker map, and the corrected reclaim is ~24.5 KB smaller than it assumed.
6. **QSPI contention (perf, unmeasured).** Hot IMU (50 Hz) / GNSS stacks in PSRAM share the bus with ~1.2 MB/frame draw-buffer traffic; measure sample latency, not just reclaimed bytes.
7. **Hardware gates on two shrinks:** audio stereo→mono (−1,920) needs the MAX98357A mono strap verified (audio is currently stereo, `audio.c:132`); mic PDM slot count matters only if the mic is ever made resident, which it is not.

---

## 5. What the fix would look like

- **Plan 1 — File Share entry (well-founded, standalone).** Boot-reserve a ~5 KB int-DMA block; free it just before `tinyusb_driver_install`; delete `msc_shed_wifi`. The 5 KB is proven unavailable at steady state, so reserve-and-hold is the right shape.
- **Plan 2 — co-resident redesign (direction sound, numbers need correcting).**
  - Relocate the audit-safe stacks to PSRAM: IMU, GNSS, NFC, **usb_gadget**, buttons, wifipcap/wifihc/wifidns/wifiparse + cold BSS. **Keep internal:** taskLVGL, weather, and the pinned set.
  - Boot-reserve the must-stay-DMA items biggest-first, before `esp_wifi_init` (which fires inside `ui_init`): BLE → flush bands → USB class buffers → audio → GNSS ring → LoRa bring-up → then WiFi lean. Items that today are lazy (USB, GNSS, LoRa) become boot-claimed so they never need runtime contiguity.
  - Shrinks for margin: flush 12→8, MSC 8192→4096, CDC, audio mono (iff strapped).
  - **Whether this fits is an open question that only the device can answer.** Recommended de-risk: one narrow build that adds the boot reservations as stub allocations in order + moves one stack (IMU) to PSRAM, then re-runs the co-residence probe. If GNSS spawn and the 5 KB File-Share block then succeed with everything up, the choreography is proven and the rest is mechanical.

---

## Retraction log (first draft → this version)
- **Retracted:** "the 30,720 cap is the flush stage, not `esp_wifi_init`." Wrong — `esp_wifi_init` ran inside `ui_init` before the snapshot; WiFi's own gauge reads 30,720. The doc is correct.
- **Retracted:** "WiFi comes up last / costs ~64 B / is mis-attributed." Wrong — it comes up early; ~64 B was association.
- **Retracted:** "USB + IMU/weather collapse the hole *before* WiFi." They run *after* WiFi; they eat the post-WiFi 30,720.
- **Struck:** all mic-resident framing (not a requirement).
- **Retracted:** every paper budget total (three inconsistent figures). Not reliable without a `.map`.
- **Softened:** "pristine 90,112 is wrong" → a build difference (86,016 here).

## Reproduce
- Harness: `firmware/src/main.c`, gates `NOCSIF_COEXV` + `NOCSIF_COEXV_ALL`, tag `COEXV_a7c3`. **Compile-gated diagnostic — remove before any merge.**
- Build (PowerShell): `platformio run -e nocsif-twatch-ultra -d firmware`; verify `findstr /C:"COEXV_a7c3" .pio/build/nocsif-twatch-ultra/firmware.bin`.
- Flash `--no-stub` app@0x20000 + ota_data@0xf000 (NVS@0x9000 preserved); capture with a reconnecting reader, `dtr=False`. To cleanly separate flush-stage from WiFi in a future run, add a snapshot inside `nocsif_ui_init` immediately after the flush-stage alloc and before the CC toggle restore.

---

## Phase A1 addendum — measured on the A1 build (2026-09-07, branch `Clankert/ram-a1-psram-dma-flush`, probe tag `COEXV_A1`)

**What changed.** `display_io.{c,h}` replaces esp_lcd's SPI panel IO with a NocSif IO that (a) streams a whole
flush through ONE write window (CASET/RASET/RAMWR once, then bands with CS held — `nocsif_display_window_begin`
+ `nocsif_display_io_stream_*`) and (b) never allocates internal DMA on any path (`SPI_TRANS_DMA_USE_PSRAM` on
every pixel chunk, commands/params via `USE_TXDATA`). The flush stage is now **2 × 4 lines = 9,840 B** (was
2 × 12 = 29,520 B). Probe harness: `main.c` `NOCSIF_COEXV` (compile-gated, kept), same stage order as §1.

**Two claims above are corrected by measurement:**
1. *"Display flush stage — cannot move to PSRAM"* (§3) — half right. The stage CAN be **shrunk to 4 lines at
   zero panel-time cost** once the per-band CASET/RASET/RAMWR is gone: the flush is memcpy-bound
   (PSRAM→internal ~21 MB/s effective) at **29.2–32 ms per near-full frame — identical to PR #95's 12-line
   number**. PSRAM-DIRECT DMA (no stage at all) was tried first and is **dead at the 80 MHz pixel clock**:
   `spi_master: DMA TX underflow detected` on **4 of 19 chunks** of the 617 KB boot clear (quad PSRAM through
   the cache tops out ~30 MB/s; the panel pulls 40 MB/s). It stays selectable (`NOCSIF_FLUSH_PSRAM_DIRECT=1`)
   for a 40 MHz-pclk experiment only. Side lesson: `spi_device_get_trans_result` DEQUEUES an underrun
   transaction and then returns `ESP_ERR_INVALID_STATE` — treating that as "not dequeued" desynced the first
   A1 build's in-flight count and wedged `app_main` in a `portMAX_DELAY` wait (no panic, no WDT: the WiFi
   worker kept associating while the UI never appeared).
2. *"Plan 1 — boot-reserve ~5 KB for File Share"* (§5) — still the right shape for determinism, but no longer
   load-bearing at boot: the entry proxy passes with margin (below).

**Per-stage curve (same method and probe order; pre-A1 from §1):**

| Stage | pre-A1 largest | **A1 largest** | A1 free |
|---|---:|---:|---:|
| pre-BLE | 86,016 | 86,016 | 167,147 |
| post-`nocsif_ble_boot_reserve` | 59,392 | 59,392 | 123,555 |
| post-`nocsif_audio_boot_reserve` | 55,296 | 55,296 | 119,399 |
| post-`nocsif_ui_init` (taskLVGL + stage + `esp_wifi_init`) | 30,720 | 31,744 | **57,739** (was 38,355) |
| post-`nocsif_usb_gadget_init` | 16,384 | **30,720** | 40,027 |
| post-`nocsif_imu_init` (+weather) | 3,456 | **24,576** | 24,971 |
| heartbeat 0 | 2,048 | **18,432** | 19,363 |
| WiFi **associated** | 1,984 | **18,432** | 19,071 |

The freed ~19.7 KB did not show as contiguity at `post-ui-init` (TLSF placed WiFi's buffers in the freed
holes) but did at every later stage: +14 KB at post-usb, +21 KB at post-imu, and a steady-state largest **9×**
the pre-A1 value.

**Co-residence probe at steady state (BLE resident + WiFi associated + display + audio + IMU):**

| Step | pre-A1 | **A1** |
|---|---|---|
| + GNSS (`nocsif_gnss_init` + `set_live`) | `failed to create gnss worker task` → NO_MEM | **worker created, UART1 up, `live fix ON`** (largest 18,432 → 12,288) |
| + LoRa (`nocsif_lora_init`) | avail=0 (gate refused) | worker created on its PSRAM stack (largest → 11,776); the radio itself is lazy on first command, not exercised by the probe |
| USB File-Share entry proxy | 5,120 B → NULL, would refuse | **8,192 B → OK, would enter** (largest 11,776 with GNSS + LoRa up) |
| display DMA underruns (`disp_tx_fail`) | n/a | **0** across 11,727 pixel chunks in 90 s of heavy navigation |
| near-full-frame flush | ~29 ms | 29.2–32 ms (unchanged; memcpy-bound) |

Operator eyes-on: "screens feel great" — no tearing or shearing at 4-line bands.

**New finding — runtime erosion (the next target).** After ~60 s of heavy navigation the largest run eroded
11,776 → **3,456** (free 3,619) and then recovered to free 11,179 but largest only **4,096**: a transient
~7.5 KB internal allocation during a screen build split the run, and a small long-lived internal allocation
landed in the middle. This is `SPIRAM_MALLOC_ALWAYSINTERNAL=4096` (every malloc ≤ 4 KB is internal-first)
chewing the tail — not new (the pre-A1 tail was ~2 KB from boot onward) but now visible because there is a tail
to chew. Phase A3's boot reserve-and-release for the USB entry + the ALWAYSINTERNAL knob are the answer; Phase
A2's stacks (~24.5 KB) add headroom above it. Raw log: `scratchpad/cap_a1_20260907-004633.log`.

---

## Phase A2 addendum — measured on the A2 build (2026-09-07, branch `Clankert/ram-a2-psram-stacks`, stacked on A1)

**What changed.** Five cache-off-safe worker stacks moved to PSRAM via `xTaskCreateWithCaps(MALLOC_CAP_SPIRAM)`
— **IMU 6,144 (+ its 2,048 B FIFO work buffer, now a PSRAM heap block), GNSS 6,144, buttons 4,096, usb_gadget
6,144, NFC 6,144** — each audited for on-task NVS / partition / OTA calls (none; only
`nocsif_reliability_safe_mode()` RAM reads), none ever deleted. The bhy2 FIFO-drain bound (16 iterations per
pass + a non-advancing `read_pos` guard) from `Clankert/usb-msc-entry-crash-8fc5c7` is applied. The IMU's I2C
wrappers log the real `esp_err_t` next to the int-DMA gauge and (after the decode below) retry once.

**Per-stage curve (largest contiguous int-DMA, pre-A1 → A1 → A2):**

| Stage | pre-A1 | A1 | **A2** | A2 free |
|---|---:|---:|---:|---:|
| pre-BLE | 86,016 | 86,016 | **90,112** | 169,187 |
| post-`nocsif_ble_boot_reserve` | 59,392 | 59,392 | 59,392 | 125,499 |
| post-`nocsif_audio_boot_reserve` | 55,296 | 55,296 | 55,296 | 121,343 |
| post-`nocsif_ui_init` | 30,720 | 31,744 | 31,744 | 59,679 |
| post-`nocsif_usb_gadget_init` | 16,384 | 30,720 | 30,720 | 48,095 (A1: 40,027) |
| post-`nocsif_imu_init` (+weather) | 3,456 | 24,576 | **30,720** | 39,187 |
| heartbeat 0 | 2,048 | 18,432 | **30,720** | 37,783 |
| WiFi **associated** | 1,984 | 18,432 | **30,720** | 37,491 |

Every worker's entry line reads `stack in PSRAM` (usb_gadget, imu — plus `fifo work buffer in PSRAM` —, buttons,
gnss). Pristine pre-BLE is back at the original doc's 90,112: that is the 2 KB of internal BSS that left.
**The steady-state run is now 15× the pre-A1 value** and the IMU/weather stage no longer touches it at all.

**Co-residence probe (BLE resident + WiFi associated + display + audio + IMU):** +GNSS → largest **unchanged**
at 30,720 (PSRAM stack; −516 B free) · +LoRa → 30,720 (−356 B) · 8 KB USB File-Share entry proxy **OK, with the
run still at 22,528 DURING the claim** and 30,720 after. Through 45 s of ordinary use: 30,720 flat. Signal Hunt
in LoRa mode (`wifi:state: run -> init`, WiFi monitor + parser pressure) took it to 29,696 → **21,504** — the
monitor floor the budget doc predicted, still 10× the original tail. 0 display underruns / 19,635 chunks; flush
~30 ms. The one `E` line, `spi: spi_bus_initialize(816): SPI bus already initialized`, is the pre-existing
idempotent LoRa/SD SPI3 re-init, unrelated.

**The IMU `err -3` — DECODED. It is a BHI260 NACK, not memory.** `imu: i2c read reg=0x2d len=1 ->
ESP_ERR_INVALID_STATE (#1)` at 8.27 s — 1.1 s after `wifi:state: assoc -> run`, 0.24 s before `got IP` — with
int-DMA free 37,491 / largest 30,720. In `esp_driver_i2c/i2c_master.c` a synchronous transaction returns
`ESP_ERR_INVALID_STATE` only when it ends in a status other than DONE (`s_i2c_synchronous_transaction`, "Wait
event bits"); a bus **timeout** logs `ESP_LOGE "I2C software timeout"` / `"I2C hardware timeout detected"`
(absent), whereas a **NACK** logs only `ESP_LOGD "I2C hardware NACK detected"` and sets `I2C_STATUS_ACK_ERROR`.
So the sensor hub refused the INT_STATUS (0x2D) read — the first read of every FIFO pass, hence exactly one
`-3` pass; the next 20 ms poll succeeded (the accelerometer never stopped). It coincides with association on
both boots captured (a starved poll gap during the WiFi task's burst, or a rail dip during the RF burst — not
distinguishable from this log and not needed): **the fix is one retry after a tick in the IMU's I2C wrapper**
(added, logged as "recovered by retry" the first three times). PR #172's "IMU int-DMA margin" precondition for
the §4.6 Governor is CLOSED — it was never a memory margin. Raw log: `scratchpad/cap_a1_20260907-010415.log`.

---

## Phase A3 addendum — measured on the A3 build (2026-09-07, branch `Clankert/ram-a3-usb-entry`, stacked on A2)

**What changed.** (1) The USB File-Share entry is made deterministic: `usb_gadget.c` claims
`NOCSIF_RADIO_MIN_DMA_USB` (8,192 B, `coex.h`) from the pristine pool in `nocsif_usb_gadget_init` and frees it
immediately before the one-time `tinyusb_driver_install`, so the install's ~5–7 KB of internal allocations
(esp_tinyusb's 4 KB task stack — `xTaskCreatePinnedToCore`, no caps option — plus its context, the 512+512 CDC-ACM
rings and the MSC FAT handoff) land in that hole regardless of what fragmented since boot. The gate can only
refuse when the reserve was never claimed, and the USB screen then says **why** (`nocsif_usb_gadget_fail_reason`:
"needs memory" / "install failed"). The sibling worktree's `msc_shed_wifi` stop-gap (a runtime WiFi stop, measured
to recover zero contiguity — §"Proven dead-ends") is not carried over. (2) The four lazy WiFi workers — parser
3,072, PCAP writer 6,144, handshake export 6,144, portal DNS 4,096 — move to PSRAM with `vTaskDeleteWithCaps`
pairs; they were the transient internal claims that eroded the run under Signal Hunt / capture (−9 KB in
LoRa-hunt mode on the A2 capture). (3) The planned `SPIRAM_MALLOC_ALWAYSINTERNAL` 4096→1024 knob is REJECTED:
IDF keeps small allocations internal so FreeRTOS objects created via plain `malloc` never land in PSRAM and get
touched from an ISR while the flash cache is off; the measured erosion was task stacks, not small mallocs.

**Boot curve (largest contiguous int-DMA):**

| Stage | A2 | **A3** | A3 free |
|---|---:|---:|---:|
| post-`nocsif_ui_init` | 31,744 | 31,744 | 59,563 |
| after the 8 KB USB entry reserve is claimed | — | 30,720 | 49,155 |
| post-`nocsif_usb_gadget_init` | 30,720 | 30,720 | 39,803 |
| post-`nocsif_imu_init` | 30,720 | 30,720 | 30,895 |
| heartbeat 0 / WiFi associated | 30,720 | **28,672** (+ 8,192 B banked for USB) | 29,183 |
| + GNSS, + LoRa | 30,720 | 27,648 | 28,311 |
| 8 KB USB entry proxy | OK @ 22,528 | OK @ 19,456 | — |

The ~2 KB of contiguity between A2 and A3 is the reserve's own footprint — the price of a File-Share entry that
no longer depends on the runtime's state. Every worker still reports `stack in PSRAM`; 0 display underruns.

**Eyes-on File Share (COM7 goes dark at the PHY switch, so this is the operator's verdict): ✅ "Drive mounted on
the PC"** — Cyber › USB › File Share tapped with WiFi associated and the BLE controller resident; Windows mounted
the NocSif removable drive. **The original failure this whole phase was opened for — File Share refusing under
WiFi + BLE + display — is closed.** Phase A summary (largest contiguous int-DMA at steady state): pre-A1 **~2 KB**
→ A1 18,432 → A2 30,720 → A3 28,672 + an 8 KB entry block banked for USB; GNSS, LoRa and the USB entry all
co-reside with BLE + WiFi + display + audio + IMU.

---

## Phase C·P1 addendum — Connectivity Governor, WiFi policy (2026-09-07, branch `Clankert/governor-p1`, stacked on B)

**What changed.** `governor.{h,c}`: a 1 s policy tick over the existing cached getters (no call-site sweep) that
treats radio power as ACTIVITY over a RESIDENT driver — `esp_wifi_stop`/`start` via the STA worker (driver memory
retained; `esp_wifi_deinit` has zero sites) + `esp_wifi_set_ps`. Holders: companion / portal / AP / PCAP / capture /
a weather fetch / a join / a scan. LINKED·IDLE → modem-sleep MAX; BUSY → MIN; up+unlinked+idle → PARK after
`idle_min`; PARKED → wake-and-look every `retry_min` for 45 s; any holder or a tile tap wakes; intent stays on
(`radio_state.wifi_parked`, the CC tile reads on). Settings › Connectivity: a live "Wi-Fi power" line + Auto-off /
after / look again every / Modem sleep rows. Self-test build: `-DNOCSIF_GOV_SELFTEST=1` (park 20 s, retry 40 s,
look 20 s).

**Verified on COM7 (self-test build):** `WiFi policy up: auto-off on (idle 5 min, retry on), modem-sleep on,
intent on` → `busy, unlinked` (joining) → `got IP` → **`wifi modem-sleep -> MAX (linked, idle)`** at 8.5 s → a
2-second `linked, busy` when a holder came and went at 62 s → back to MAX. Operator: the Connectivity screen's
"Wi-Fi power" line reads "linked · idle (modem sleep)", the four rows cycle, the CC Wi-Fi tile toggles normally.
**Park → retry → relink: LOGIC IN PLACE, UNEXERCISED ON-DEVICE** — the saved AP is always in range here and a
linked STA is never parked by design; the operator declined the router-off test. The path is straight-line code
over the same worker calls the tile uses; it will log `PARKED — no link for Ns` / `waking to look for the saved
network` when it first happens in the field.

**A3 follow-up + weather hoist — a placement lesson, not a gain.** Claiming the USB entry reserve right after the
audio reserve (post-audio 55,296 → 47,104, −8,192 exactly) did not restore the 30,720 tail: with that hole gone the
weather worker's 8 KB internal stack carved the tail instead (post-imu 22,528). Creating weather BEFORE `ui_init`
(post-usb-reserve 47,104 → post-weather 38,912) then let `ui_init` land elsewhere (post-ui-init 30,720) — and the
8 KB MSC storage object took the tail (post-usb-init 22,528). **Zero-sum:** total free is identical across all three
boots (~30.4 K); the "largest run" swings ±8 KB by which 8 KB boot-permanent claim lands last. The remaining
levers would be hoisting SD + the MSC object before the UI (delays the splash ~150 ms) or accepting the swing.
Accepted: the USB entry is banked regardless, and 22,528 is 10× the pre-Phase-A tail. Both reorders are kept
(boot-permanent internal claims belong before WiFi in principle). Raw logs: `cap_a1_20260907-080102.log`,
`cap_a1_20260907-080807.log`.

**Phase A/B/C status (2026-09-07):** A1 #180 · A2 #181 · A3 #182 · B #183 · C·P1 (PR pending) — a linear stack;
merge in order and rebase the next onto `main` after each squash-merge. File Share mounts with WiFi + BLE up;
GNSS/LoRa/USB co-reside; Carts plays any WAV; WiFi idles in modem-sleep and parks when away (unexercised).

---

## Phase C·P2–P4 addendum — cyclic GPS, places, policy (2026-09-07, branch `Clankert/governor-p2-p4` off main)

**What changed.** P2: `gnss.c` gains a fourth want (`nocsif_gnss_set_hold`, OR'd with Live Fix / GPX / Wardrive);
every `gps_min` the Governor holds the receiver for one fresh fix (60 s timeout), evaluates fences, releases;
skipped while the IMU reports the watch still ≥5 min; a foreground session's fixes are evaluated for free.
Fences = circles of `radius_m` around each saved network's learned location (1.3× exit hysteresis, no fix ⇒ state
held). P3: per-profile lat/lon (`wn_la%d`/`wn_lo%d`), auto-learned via `CMD_GEO_STAMP` on the pinned WiFi
worker when a fresh fix coincides with a link; place-enter wakes a parked STA at once; outside every known place
an unlinked STA parks after 60 s; the timer retry stays as the indoor fallback. P4: Connectivity "Location" line +
three rows; weather refreshed opportunistically on place-enter. Built with PRODUCTION flags (no probes, no
self-test timers) so the verified binary is the one the watch keeps.

**Verified on COM7 (7-minute capture, production 5-min period):**
`WiFi policy up: … location on (gps every 5 min, radius 200 m, no places learned yet)` → WiFi linked, modem-sleep
MAX at 8.5 s (P1 unchanged) → at **302 s** `gps: waking for a fix (period 300s, timeout 60s)` → the GNSS worker
spawned lazily (`stack in PSRAM`), `GNSS UART1 up`, `live fix ON (rail on)` → **362 s** `gps: no fix in 60s
(indoors?) — sleeping until the next period` → `live fix OFF (rail off)`. The cycle, the hold/release, the rail
duty and the timeout all behave; **fence evaluation and the location stamp are UNEXERCISED** (no fix lands
indoors — the plan's own "indoors-no-fix" caveat, and exactly why the timer retry remains the fallback). They
will log `gps: fix in Ns (sats N, hdop N.N) -> fences evaluated` and `wifi: geo: "<ssid>" learned @ lat, lon` the
first time a fix coincides with a link outdoors; `place: ENTER/EXIT` thereafter.

**Cost of the cycle, measured:** the GNSS UART ring took ~516 B of internal free (26,995 vs 27,511) and the
largest run did not move (14,336 before and after — the earlier 22,528 → 14,336 step happened at ~62 s during a
screen build, the known placement swing). The IMU-still gate did not fire in this capture (the watch was being
handled). Raw log: `cap_a1_20260907-084643.log`.

---

## B+ / P3 follow-on addendum — MP3 (minimp3) + BSSID-keyed places (2026-09-07, branch `Clankert/mp3-and-bssid-places`)

**What changed.** minimp3 (CC0, unmodified `minimp3.h`) vendored as the header-only component
`firmware/components/minimp3`; `audio.c` carries the implementation (`ONLY_MP3`, `NO_SIMD`) and dispatches
`.mp3` to a streamed decoder: 16 KB PSRAM input window refilled under short /sd locks whenever < 4 KB remain,
decoder state + the 1152×2 PCM frame in PSRAM, ID3v2 skipped by header / ID3v1 + APE trailers trimmed up front
(minimp3 validates a frame by chaining up to 10 following headers — a trailer would drop the last ~10 frames),
stereo downmixed through the shared `emit_mono` gain → soft-knee limiter → I2S path (the WAV walker now uses the
same emitter). Audio worker stack 6 → 28 KB, PSRAM (`mp3dec_decode_frame` keeps ~19 KB of scratch on the stack).
Carts lists `.mp3`. The P3 geo-store was re-keyed from per-SSID lat/lon (`wn_la/wn_lo`, dropped) to **places =
connected-AP BSSID + lat/lon + SSID** (`pl_n`, `pl_b%d`/`pl_s%d`/`pl_la%d`/`pl_lo%d`; 8 max, oldest evicted;
forgetting a profile drops its places); the Governor's fences iterate places and the Location line names the
place's SSID. Production flags.

**Verified on COM7 (180 s capture, `cap_a1_20260907-092424.log`) — build 191 s incremental, RAM 47.6 % static
(unchanged: minimp3's tables are const/flash, the decoder is heap), flash +~1 KB.** Boot curve identical to the
P2–P4 build (boot largest 22,528 → steady 21,504; the ~62 s weather-fetch dip 13,824 → 14,336 — the known
placement swing). `gov: … no places learned yet` = the empty BSSID table (`pl_n` absent) read cleanly. MP3
playback from `Cyber › Audio › Carts` (operator, four files in `/sd/nocsif/carts/rocateq/`):
`roca-armingsignal-17s.mp3` 48 kHz mono (stopped at 95,616 samples) → `roca-lock.mp3` 44.1 kHz (tap-to-switch,
**I2S clock re-followed 48 → 44.1 kHz**) → `roca-purcheck-ok.mp3` (hot file, 13,811 samples limited — the §4.13
guard) → `roca-unlock.mp3` **played to the end (`done`, 42,624 samples)** — the ID3 trailer trim held (no
"no decodable frames", no tail junk) — then replayed + stopped. A sibling WAV (`GateKeeperCartKeyLock78.wav`,
22.05 kHz 16-bit) played through the refactored emitter path (`done`). **Internal cost of an MP3 decode: none
measurable** — int-DMA free 27,371 → 27,151 during play, 27,635 after; largest 21,504 throughout (all decode
memory is PSRAM heap + the PSRAM stack). No warnings, no wdt, no resets. `GateKeeperCartKeyUnlock78.wav` was
rejected by the walker — operator: a broken file ("my own fault"), not a format gap.
**Unexercised:** the BSSID stamp + fence-by-place (no fix indoors — same caveat as P2–P4; the first outdoor fix
while linked logs `wifi: geo: place "<ssid>" (aa:bb:…) learned @ lat, lon (n/8)`); a stereo MP3 (all four test
files were mono — the downmix branch is compiled and trivially the WAV stereo path's twin); a VBR/Xing-header
file (Xing frames decode to 0 samples and are skipped by the `n <= 0` branch).

### Field test — BSSID stamp + fence (what to look for, no laptop needed)

Every Governor line is teed into the flash logbook, so **System › Diagnostics** shows the last ~2 KB of log on
the wrist; **Settings › Connectivity** has the live "Wi-Fi power" and "Location" lines. Defaults: GPS check
every 5 min, radius 200 m (exit at 1.3× = 260 m), stamp offered ≤ once/min, park-when-away after 60 s unlinked.

**Setup (2 min, indoors).** Settings › Connectivity: *Auto-connect by place* ON, *GPS check every* **2 min**,
*place radius* **100 m** (exit at 130 m — a short walk). Location line should read **"no places learned yet"**.
Optional accelerator: keep **GNSS › Live Fix** open while walking — every one of its fixes is evaluated for free
(Location line gains "· gps live"), so you don't wait for the 2-min cycle.

1. **Learn (outside, linked).** Stand outdoors within Wi-Fi range of the house (porch/yard) with the STA
   linked ("linked · idle" on the Wi-Fi line). Wait for a fix (Live Fix, or the cycle: "gps fixing… (Ns)").
   Expect, in this order: Diagnostics `gps: fix in Ns (sats N, hdop N.N) -> fences evaluated` →
   `wifi: geo: place "<ssid>" (aa:bb:cc:dd:ee:ff) learned @ lat, lon (1/8)` → on the NEXT fresh fix
   `gov: place: ENTER "<ssid>" (N m)` and the Location line flips to **"inside <ssid> · next M:SS"**.
   ✗ If it stays "no places learned yet" after a fix: the STA was not linked at fix time, or the fix was stale
   (> 5 s) — check the Wi-Fi line first.
2. **Exit + park (walk away).** Walk > 130 m (or 260 m at the default radius). On the first fix outside:
   `place: EXIT "<ssid>"`, Location **"outside known places"**. Once out of AP range the Wi-Fi line goes
   **"searching (away) · parks in 0:59"** counting down, then `wifi: PARKED — no link for 60s … (outside known
   places)` and **"parked"** (CC tile still reads on). ✗ Still "searching · parks in 4:xx" (no "(away)") =
   the fence didn't evaluate you as outside — no fresh fix yet; ✗ never unlinks = still in AP range, keep walking.
3. **Enter + wake (walk back).** Re-enter the circle (< 100 m / 200 m). First fix inside:
   `place: ENTER "<ssid>" (N m) -> waking WiFi` → `wifi: woken from parked` → link → **"linked · idle (modem
   sleep)"** + Location "inside <ssid>", and Weather refreshes opportunistically. ✗ ENTER without wake = the
   STA wasn't parked (it re-linked on its own — fine, the fence still works).
4. **Persistence (any time later).** Reboot: the Governor's boot line now says **"… places learned"** and
   `wifi: places: 1 learned`; Location reads "places known · no fix yet" until the first fix.
5. **Bonus — BSSID keying.** With a mesh / multi-AP SSID, each node you link to at a different spot becomes its
   own place (`(2/8)`, `(3/8)` …); a phone hotspot saved as a profile learns a place where you first linked and
   re-stamps only when > 100 m from it. Forgetting the profile drops its places (`places: N learned` shrinks).
Timings to expect with the 2-min setting: worst case ~3 min from arrival to ENTER (cycle + fix), seconds with
Live Fix open. The still-gate (5 min motionless) never fires while walking. Restore 5 min / 200 m afterwards.
