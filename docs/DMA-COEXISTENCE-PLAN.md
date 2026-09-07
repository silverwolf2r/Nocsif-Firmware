# NocSif — Internal-DMA Coexistence: Plan, Inventory & Open Verification

- **Repo:** silverwolf2r/NocSif_Firmware · **Branch:** `Clankert/usb-msc-entry-crash-8fc5c7`
- **Board:** LilyGo T-Watch Ultra — ESP32-S3, 8 MB QUAD PSRAM, 16 MB flash
- **Interactive inventory (sortable, all 75 consumers):** https://claude.ai/code/artifact/52328b16-725c-4db3-a6ee-1fe1fb727ebe
- **Companion doc (authoritative prior analysis):** `docs/RAM-BUDGET.md`

> ## ⚠ CONFIDENCE CAVEAT — READ FIRST
> Every byte figure below is **empirical**, read from the on-device heartbeat gauge (`nocsif_int_dma_largest()` = `heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA)`). **There is NO linker `.map`.** The operator believes these measurements may be **wrong**, and specifically doubts that **every feature can actually co-reside** (suspects the real demand is "too much" to fit). **This plan is NOT confirmed. It must be independently re-measured on-device before any of it is trusted or built.** Treat the numbers as hypotheses to falsify, not facts.

## The physics (the wall this all fights)

The scarce resource is **ONE contiguous run of internal-DMA-capable SRAM**, not total free bytes. Measured: pristine pre-BLE largest run = **90,112 B**; total DMA-capable pool ≈ **180 KB**. Once `esp_wifi_init` runs, the largest run caps ~27,648 (full) / ~31,736 (lean); steady-state BLE+WiFi collapses it to **~384 B–2 KB**.

**Proven dead-ends (do not re-propose):** runtime WiFi `stop` recovers *zero* contiguity (measured `largest` unchanged at 1984 this session); WiFi `deinit` reaches only 21,504 (< BLE's 31,744 gate); airplane → 9,216. **Runtime teardown NEVER defragments — only a reboot restores a big contiguous run.** Coexistence is won *once, at boot*, by claim-order + reserve.

**Operator design rules (load-bearing):**
1. **No reboots to make a feature work** — a future app must be able to combine *every* feature at once, so time-multiplexing / reboot-into-mode is off the table.
2. **Connectivity Governor (§4.6) is about battery, not memory** — RF leases save current, they do not free RAM. Keep it decoupled from this problem.
3. **Priority:** HIGH (keep in fast int-DMA) = BLE, WiFi, GNSS, display. LOW (PSRAM-able / deprioritize) = USB, LoRa, NFC. PSRAM's only downside is being slower.

---

## PLAN 1 — The USB File Share (MSC) entry fix (the original task)

**Symptom:** entering Cyber › USB Gadget › File Share rebooted the watch. **Root cause (confirmed on-device):** it was a `task-wdt` on task `nocsif_imu` — the BHY2 FIFO-drain loops are unbounded and, under the int-DMA pressure of the USB install (which slows the shared I2C), a single drain exceeded the 8 s watchdog. **Fixed** by bounding the drain (`firmware/components/bhy2/bhy2_core/bhy2.c`, `NOCSIF_BHY2_FIFO_DRAIN_MAX_ITERS 16` + a non-advancing-`read_pos` guard) — verified: File Share no longer crashes under WiFi-on.

**Residual (the reason PLAN 2 exists):** with the IMU fix in, File Share still *fails to switch* while WiFi is up, because the TinyUSB bring-up needs a small contiguous int-DMA block (~5 KB — the ~4 KB lazy TinyUSB task stack; the S3 OTG is FIFO/slave, so USB is a LOW fixed consumer) and there is only ~2 KB free.

The File-Share fix options that were evaluated:
- **A — Reserve-and-hold (RECOMMENDED):** boot-reserve USB's ~5 KB block from the pristine pool, free it just-in-time before `tinyusb_driver_install`, **delete the `msc_shed_wifi` workaround** (a runtime WiFi `stop` — it recovered nothing, confirmed). File Share then enters with WiFi still connected, COM7 stays live, no reboot.
- **B — Reboot-into-mode:** REJECTED by operator rule #1 (no reboots).
- **C — Boot-install TinyUSB:** REJECTED — darkens the COM7 console from every boot.
- **D — Free-then-reserve:** = Plan A + PLAN 2's freeing levers for real margin (the superset).

A stop-gap `msc_shed_wifi` (release WiFi on File-Share entry) was implemented and **on-device proven NOT to work** (WiFi `stop` doesn't defrag). It must be **deleted** — it is a dead-end, kept only as evidence.

---

## PLAN 2 — Co-resident redesign (the RAM plan — the operator's real ask)

Make **every feature co-resident at once** (no reboots, no time-mux), via three levers together — the co-resident budget (below) claims this fits, **but that claim is exactly what needs independent on-device confirmation**:

1. **Relocation (the big lever, ~124.6 KB):** move movable worker **task stacks** (`xTaskCreateWithCaps(..., MALLOC_CAP_SPIRAM)`) and cold BSS (`EXT_RAM_BSS_ATTR` / lazy PSRAM) off fast int-DMA into slow PSRAM. Most int-DMA usage is *stacks + cold tables*, not DMA buffers — even high-priority workers (IMU, GNSS, buttons) can move their *stack* because the stack isn't the DMA path (the driver ring buffers are).
2. **Shrink (~30 KB):** trim the true-DMA-source items that can't move (display flush stage 12→8 lines = −9,840; WiFi cache-TX full 16→8 = −12,800; USB MSC BUFSIZE 8192→4096 = −4,096; audio stereo→mono = −1,920; mic desc 6→4 = −960; CDC RX = −448).
3. **Boot-order reservation:** claim BLE (31,744, FIRST) + both flush bands + WiFi-lean + the other must-stay items from the pristine pool before fragmentation.

**The claimed result:** raw co-resident internal demand ≈ 247.7 KB overflows the 180 KB pool → relocation is mandatory; after moving ~124.6 KB to PSRAM and ~30 KB of shrinks, must-stay internal ≈ 114.7 KB, which fits with ~65 KB headroom **as total bytes**, and the contiguity test passes **only** because BLE is claimed first from the pristine 90 KB. Margin over the 31,744 gate stays ~4 KB. See the full STEP 1–3 arithmetic and the master table below.

---

## DOWNSIDES OF PLAN 2 (already identified — the new chat should confirm, extend, or refute these)

1. **PSRAM cache-off correctness trap (worst).** A task whose *stack* is in PSRAM **hard-faults if it runs while the flash cache is disabled** (OTA erase/write, some NVS/`spi_flash` ops). A single mis-classified stack move (e.g. one that does an on-task `nvs_commit`) becomes a **rare crash that only fires during a flash op** — passes normal testing. Pinned-internal for this reason: main, ble, wifi, logbook, coredump, OTA. **Unproven / must audit:** usb_gadget MSC path, wifi command worker, buttons long-press.
2. **Hot stacks in slow PSRAM (perf).** `taskLVGL` → PSRAM (the biggest 16 KB reclaim) risks visible UI jank; IMU (50 Hz) / GNSS parse loops slow per-call; PSRAM QSPI-bus contention with the 1.2 MB/frame framebuffer traffic can degrade everything on PSRAM at once.
3. **Does NOT fix the real fragility.** BLE still needs 31,744 in one run from the *fixed* 90 KB pristine pool — the ~4 KB margin stays ~4 KB, no `.map`. Fits *by construction* (boot order), so any future boot-permanent int-DMA consumer / IDF bump / `MAX_ACT` increase re-breaks it.
4. **The co-resident rule itself costs int-DMA.** Making today's lazy/freed consumers permanent (mic +~2,880, OTA +4,096) removes the lazy-free savings that currently help BLE fit. "Always-on mic" is the sharpest tension with the ~4 KB margin — confirm it's actually required.
5. **WiFi must run LEAN whenever BT is on.** Full-profile WiFi + BLE cannot co-reside → heavy promiscuous WiFi capture runs degraded (may drop frames) while BLE is up.
6. **Shrinks degrade their own features.** UI wipe fps (flush 12→8), USB transfer ~half (MSC 8192→4096), audio silence unless the DAC is strapped mono, rare mic drops.
7. **Broad refactor + heavy verification.** Touches nearly every subsystem; each stack move needs a cache-off proof and the largest-free heartbeat watched (4 KB BLE margin).

**Mitigation already noted:** the two worst (1 & 2) are avoidable — keep `taskLVGL` and all flash-touching stacks internal, move only the *cold / infrequent* stacks. Trades a little reclaimed space for a lot less risk (a "safe movers only" tier vs the aggressive full relocation).

---

# ─────────────────────────────────────────────
# INVENTORY, CO-RESIDENT BUDGET & RESERVATION MAP (auto-generated from the code audit)
# ─────────────────────────────────────────────

# MASTER TABLE
## NocSif internal-memory master inventory (every consumer, sorted by current bytes desc)

Legend — Class: `INT|DMA` = internal DMA-capable (the scarce contiguous pool) · `INT` = internal DRAM, not DMA-tagged (fragments the same unified pool) · `PSRAM` = already off-chip (zero int-DMA cost). "Must stay?" = must remain internal-DMA. WiFi buffers shown at the **lean** (BT-on default) value with full in parens.

| Feature | Current bytes | Class | Must stay int-DMA? | Priority | Best shrink / move | Bytes saved (internal) |
|---|---:|---|---|---|---|---:|
| LVGL 2× full-frame RGB888 draw buffers | 1,234,920 | PSRAM | no | low | partial draw buffers (~1/4 frame) | 0 int (926k PSRAM) |
| Orrery/star wallpaper canvas | 618,464 | PSRAM | no | low | RGB565 canvas | 0 int (309k PSRAM) |
| Boot-splash/screenshot draw_full fb (transient) | 617,460 | PSRAM | no | low | band-stream through flush stage | 0 int |
| Voice-memo WAV playback buffer (transient) | ≤2,097,152 | PSRAM | no | low | cap 2 MB→1 MB | 0 int |
| Voice-memo record buffer (transient) | 960,000 | PSRAM | no | low | 30 s→15 s | 0 int |
| Companion HTTP screen-mirror thumbnail | 118,808 | PSRAM | no | low | cap dims | 0 int |
| Watchface peek pick-wheel canvas (per-screen) | 144,400 | PSRAM | no | low | smaller WHEEL_D | 0 int |
| Signal-Hunt compass needle canvas (per-screen) | 123,904 | PSRAM | no | low | A8 raster | 0 int |
| Companion screen-mirror fb s_mirror_fb (lazy) | 102,910 | PSRAM | no | low | free on cast-stop | 0 int |
| LVGL object heap (widgets/styles) | ~100,000 | PSRAM | no | low | keep (PR #106 — do not revert) | 0 |
| WiFi capture ring s_ring | 69,632 | PSRAM | no | low | CAP_SLOTS 256→128 | 0 int |
| WiFi PCAP ring s_pcap_ring | 53,248 | PSRAM | no | low | PCAP_SLOTS 128→64 | 0 int |
| **BLE/NimBLE controller contiguous block** | **31,744** | **INT\|DMA** | **YES** | **critical** | at shrink floor (MAX_ACT=3, dup=20) | **0** |
| **Display flush stage band (2× ping-pong)** | **29,520** | **INT\|DMA** | **YES** | **critical** | LINES 12→8 | **9,840** |
| WiFi dynamic-RX buffers | 25,600 (16×) | PSRAM | no | low | DYNAMIC_RX 16→8 | 0 int |
| Rounded-corner masks (4× ARGB) | 25,600 | PSRAM | no | low | smaller CORNER_R | 0 int |
| NimBLE host heap (msys/ACL/ATT/bond) | 18,972 | PSRAM | no | high | **RAISE** 24→32 (reliability, not shrink) | −4,096 |
| **WiFi cache-TX (PSRAM→MAC copy cache)** | **6,400** (full 25,600) | **INT\|DMA** | **YES** | **critical** | CACHE_TX 16→8 (full) | **12,800** |
| **taskLVGL render/flush stack** | 16,384 | INT | no (movable) | low | port_cfg.task_stack_caps=SPIRAM | 16,384 (move) |
| WiFi monitor/identity BSS (hs/probe/sta/ap) | 16,768 | INT | no (parser-task) | low | lazy PSRAM on monitor arm | 16,768 (move) |
| BLE advert-PCAP ring s_pcap_ring (lazy) | 16,384 | PSRAM | no | low | SLOTS 256→128 | 0 int |
| UI live-row pools + s_apps registry | ~11,700 | INT | no (LVGL-task) | low | move pools to PSRAM | 11,700 (move) |
| **WiFi static-TX (TX DMA source)** | **3,200** (full 9,600) | **INT\|DMA** | **YES** | **high** | STATIC_TX 6→4 (full) | 3,200 |
| **WiFi mgmt-SBUF** | **3,200** (full ~12,800) | **INT\|DMA** | **YES** | **high** | MGMT_SBUF 8→4 (full) | ~6,400 |
| **WiFi static-RX (RX DMA landing)** | **3,200** (full 6,400) | **INT\|DMA** | **YES** | **high** | STATIC_RX 4→3 + RX_BA_WIN lockstep | 1,600 |
| MSC endpoint buffer (DMA-mode) | 8,192 | INT\|DMA | YES | low | BUFSIZE 8192→4096 | 4,096 |
| main task / heartbeat stack | 8,192 | INT | no (ota_confirm on-task) | critical | trim 8192→6144 after watermark | 2,048 |
| weather worker stack | 8,192 | INT | no (movable) | low | xTaskCreateWithCaps SPIRAM | 8,192 (move) |
| OTA worker stack (lazy) | 8,192 | INT | YES (flash cache off) | low | keep; delete after failed session | 8,192 (lazy) |
| LoRa worker stack | 8,192 | PSRAM | no | low | already moved (Phase 1) | 0 |
| Captive-portal httpd stack (lazy) | 8,192 | INT | no (component-created) | high | none safe (FatFs depth) | 0 |
| logbook s_ram accumulator | 4,084 | INT | YES (tee under spinlock, cache-off) | low | 4084→2048 (2× commit freq) | 2,048 |
| logbook s_snap commit scratch | 4,084 | INT | no (flush-task; but flash-src) | low | move s_snap to PSRAM | 4,084 (move) |
| WiFi PHY/RF cal + connection/AMPDU-TX | ~5,000 | INT\|DMA | YES | high | none (IDF-internal) | 0 |
| NimBLE host task stack | 6,144 | INT | YES (bond NVS; IDF-created) | high | keep; bound callback depth | 0 |
| BLE 'ble' worker stack | 6,144 | INT | YES (nvs_commit + FATFS on-task) | high | keep (raised for PCAP drain) | 0 |
| IMU worker stack | 6,144 | INT | no (movable) | high | xTaskCreateWithCaps SPIRAM | 6,144 (move) |
| GNSS worker stack | 6,144 | INT | no (movable) | high | xTaskCreateWithCaps SPIRAM | 6,144 (move) |
| NFC worker stack | 6,144 | INT | no (movable) | low | xTaskCreateWithCaps SPIRAM | 6,144 (move) |
| usb_gadget worker stack | 6,144 | INT | no (movable, validate MSC path) | low | xTaskCreateWithCaps SPIRAM | 6,144 (move) |
| wifipcap writer stack (lazy) | 6,144 | INT | no (SD-SPI, movable) | low | xTaskCreateWithCaps SPIRAM | 6,144 (move) |
| wifihc export stack (transient) | 6,144 | INT | no (SD-SPI, movable) | low | xTaskCreateWithCaps SPIRAM | 6,144 (move) |
| companion httpd stack (lazy) | 6,144 | INT | no (component-created) | high | none (no WithCaps path) | 0 |
| audio worker stack | 6,144 | PSRAM | no | low | already moved (Phase 2) | 0 |
| mic worker stack | 6,144 | PSRAM | no | low | already moved (Phase 2) | 0 |
| ducky worker stack | 6,144 | PSRAM | no | low | already moved (Phase 2) | 0 |
| BLE snapshot BSS (dev/anc/chr/svc/drone) | 6,000 | INT | no (host-task, cold tables) | low | move cold tables to PSRAM | 4,192 (move) |
| LoRa receive inbox s_inbox | 3,584 | INT | no (movable) | low | EXT_RAM_BSS_ATTR → PSRAM | 3,584 (move) |
| logbook flush task stack | 3,584 | INT | YES (esp_partition erase/write) | low | 3584→2560 | 1,024 |
| GNSS wardrive dedup s_wd_seen | 3,072 | INT | no (worker-only, cold) | low | lazy PSRAM on wardrive start | 3,072 (move) |
| wifiparse task stack (lazy) | 3,072 | INT | no (movable) | low | xTaskCreateWithCaps SPIRAM | 3,072 (move) |
| WiFi scan snapshot s_recs+s_ap | ~3,600 | INT | no (cold) | low | WIFI_MAX_AP 20→12 / PSRAM | ~3,600 (move) |
| Audio I2S TX DMA buffers | 3,840 | INT\|DMA | YES | critical | stereo→mono slot | 1,920 |
| Mic PDM I2S RX DMA (lazy; 0 steady) | 2,880 | INT\|DMA | YES | critical | dma_desc_num→4 | 960 |
| wifi command worker stack | 4,096 | INT | YES (conservative — on-task NVS?) | high | keep (radio control) | 0 |
| buttons worker stack | 4,096 | INT | no (movable) | high | xTaskCreateWithCaps SPIRAM | 4,096 (move) |
| wifidns portal stack (lazy) | 4,096 | INT | no (movable) | low | xTaskCreateWithCaps SPIRAM | 4,096 (move) |
| TinyUSB device task stack (lazy, detached-boot) | 4,096 | INT | YES (USB-OTG DMA; no knob) | low | boot-reserve, don't shrink | 0 |
| OTA s_buf SD-read/flash-write staging | 4,096 | INT\|DMA | YES (dual DMA+flash-src) | low | make lazy (frees when idle) | 4,096 (lazy) |
| CDC-ACM FIFO buffers (DMA-mode) | 2,056 | INT\|DMA | YES | low | RX 512→64 (+TX/EP 512→256) | 448 (–960) |
| GNSS UART1 RX ring | 2,048 | INT\|DMA | YES (ISR-fed, no PSRAM knob) | high | 2048→1536 | 512 |
| IMU FIFO work buffer s_fifo_work | 2,048 | INT | no (worker-only) | high | move to PSRAM | 2,048 (move) |
| esp_core_dump panic stack (.bss) | 1,792 | INT | YES (panic, cache off) | critical | keep (IDF default) | 0 |
| coredump summary temp (transient) | ~400 | INT | no | low | leave | 0 |
| audio synth scratch s_chunk | 1,024 | INT | no (CPU-copy src) | low | move to PSRAM | 1,024 (move) |
| DWC2 OTG DFIFO (on-chip peripheral) | 1,024 | peripheral | n/a | low | none (not main heap) | 0 |
| SD SPI3 GDMA desc + host ctx | ~1,024 | INT\|DMA | YES (GDMA structures) | low | near-minimal | 0 |
| LoRa survey/act/hunt BSS | ~1,320 | INT | no (worker-only, cold) | low | EXT_RAM_BSS_ATTR → PSRAM | 1,320 (move) |
| LoRa command queue | ~1,100 | PSRAM | no | low | already moved | 0 |
| audio command queue s_q | ~600 | INT | no (movable) | low | union path[] / QLEN 6→4 | ~200 |
| mic PCM scratch s_buf | 512 | INT | no (CPU-copy) | low | move to PSRAM | 512 (move) |
| sdmmc_card_t | ~256 | INT | no | low | heap_caps SPIRAM | 256 (move) |
| reliability last-crash record | 200 | INT | no | low | leave | 0 |
| HID endpoint buffers (DMA-mode) | 192 | INT\|DMA | YES | low | at FS minimum | 0 |
| USB active descriptor copies | ~146 | INT | no | low | leave (pointers stored) | 0 |

**Totals (co-resident, WiFi lean):**
- Boot-permanent **INT|DMA (must-stay, cannot move)**: BLE 31,744 + display 29,520 + WiFi-lean statics ~21,000 + audio 3,840 + mic 2,880 + OTA s_buf 4,096 + SD 1,024 + USB(MSC+CDC+HID) 10,440 + GNSS ring 2,048 ≈ **106,592 B**
- Boot-permanent **INT (must-stay, can't relocate)** stacks/BSS: main 8,192 + NimBLE-host 6,144 + ble 6,144 + wifi 4,096 + logbook 8,168 + logflush 3,584 + coredump 1,792 ≈ **38,120 B**
- **Movable INT → PSRAM** (relocation reclaim): taskLVGL 16,384 + monitor tables 16,768 + UI pools 11,700 + weather 8,192 + IMU 6,144 + IMU-FIFO 2,048 + GNSS 6,144 + NFC 6,144 + usb_gadget 6,144 + buttons 4,096 + BLE-snapshot 4,192 + LoRa BSS 4,904 + GNSS-wardrive 3,072 + s_snap 4,084 + audio/mic scratch 1,536 + scan 3,600 + lazy wifi stacks 19,456 ≈ **124,652 B reclaimable**
- **Realistic SHRINK savings** (bytes eliminated, not moved) ≈ **30,000 B** (see total_shrink_savings)
- Already-PSRAM (zero int-DMA): LVGL buffers/heap/canvases + WiFi rings + NimBLE heap + audio/mic/lora/ducky stacks + WAV/record buffers ≈ **>4.5 MB PSRAM**

# HIGH-PRIORITY INT-DMA SUM
HIGH-priority MUST-stay fast int-DMA (operator's keep list — BLE + WiFi + GNSS + display), at the BT-on lean WiFi profile that is the only valid co-resident profile:

- BLE controller contiguous block: 31,744 B (one contiguous run, at floor — MAX_ACT=3, dup-cache 20, adv-flow-ctrl 50)
- Display flush stage (2× 14,760 ping-pong bands): 29,520 B
- WiFi lean statics (cache-TX 6,400 + static-TX 3,200 + static-RX 3,200 + mgmt-SBUF 3,200 + PHY/cal ~5,000; AMPDU-RX 0 in lean): ~21,000 B
- GNSS UART1 RX ring: 2,048 B

SUM = ~84,312 B (drops to ~74,472 B if the flush stage is trimmed 12→8 lines).

Does it fit the 90,112 pristine run? The 90,112 is the largest *single contiguous* run, NOT a budget the sum must sit under — total internal-DMA pool is ~180,251 B. The only consumer with a large contiguous demand is BLE (31,744 in one run), and it fits ONLY because it is claimed FIRST from the pristine 90,112 pool before esp_wifi_init (after WiFi the largest run is 30,720 and falling — a runtime BLE claim is impossible). Each flush band (14,760) and the WiFi/GNSS buffers are small enough to be satisfied from the remaining pool if reserved at boot before fragmentation. So: YES, all four HIGH consumers co-reside — conditional on (a) BLE reserved first at boot, (b) WiFi forced lean whenever BT is on, (c) both flush bands + the BLE block claimed while the pool is still pristine. The margin over the 31,744 failure gate is only ~4 KB, with no .map — thin but real.

# PSRAM OFFLOAD CANDIDATES
- taskLVGL render/flush stack — 16,384 B → PSRAM via port_cfg.task_stack_caps=MALLOC_CAP_SPIRAM (SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y already set). CAVEAT: render hot-path stack in slow QSPI (fps risk) + UNSAFE if scheduled during an OTA flash-erase cache-off window — needs on-device validation. Single largest int-DRAM reclaim available.
- WiFi monitor/identity BSS tables (s_mon_hs/probe/sta/ap) — 16,768 B → lazy PSRAM on monitor arm. Parser-task-owned (rx cb only fills the PSRAM ring), so NOT rx-ISR/DMA-critical despite the doc's cautious note. Clean reclaim.
- UI live-row pointer pools + s_apps registry — ~11,700 B → PSRAM (LVGL-task-only; referenced objects already in PSRAM).
- weather worker stack — 8,192 B → xTaskCreateWithCaps(SPIRAM). HTTP+JSON over LWIP, no on-stack DMA, no on-task flash. LOW priority.
- OTA worker stack — 8,192 B (lazy): CANNOT move (esp_ota flash-erase cache-off) but is non-resident; delete-after-failed-session frees it when the Update screen is closed.
- IMU worker stack — 6,144 B → PSRAM (I2C only, no on-task flash). Doc remake #7 names it. Priority high but the STACK is not the fast path (the UART/I2C driver buffers are).
- GNSS worker stack — 6,144 B → PSRAM (UART ring is driver-owned; SD writes keep flash cache on). GNSS is operator-HIGH but 'high' protects the 2,048 RX ring, not this text-parse stack.
- NFC worker stack — 6,144 B → PSRAM (RFAL over SPI3, driver DMA). NFC is LOW-priority AND hardware-dead on this unit — still budget it for a healthy unit per the co-resident rule; relocate rather than delete.
- usb_gadget worker stack — 6,144 B → PSRAM. USB is LOW-priority. VALIDATE the MSC-mount/FAT-remount path does no on-task internal-flash op first (it is the active crash branch).
- wifipcap writer stack — 6,144 B (lazy) → PSRAM (SD-SPI keeps cache on). Also removes the 'capture fails to spawn under fragmentation' failure mode.
- wifihc handshake-export stack — 6,144 B (transient) → PSRAM (SD-SPI).
- BLE snapshot cold tables (s_chr/s_svc/s_drone/s_anc_ds_buf) — ~4,192 B → lazy PSRAM (keep s_dev internal — hot adv-report upsert).
- logbook s_snap commit scratch — 4,084 B → PSRAM (flush-task-only). Keep s_ram internal (tee under spinlock, cache-off).
- buttons worker stack — 4,096 B → PSRAM (I2C poll). Verify no long-press NVS write on-task.
- wifidns captive-portal DNS stack — 4,096 B (lazy) → PSRAM (UDP sockets only).
- WiFi scan snapshot s_recs+s_ap — ~3,600 B → lazy PSRAM (cold; scan not latency-critical).
- LoRa receive inbox s_inbox — 3,584 B → EXT_RAM_BSS_ATTR/PSRAM (spinlock-guarded copy-out, no ISR). LoRa is LOW-priority.
- GNSS wardrive dedup s_wd_seen — 3,072 B → lazy PSRAM on wardrive start (worker-only, cold, doc open action).
- wifiparse task stack — 3,072 B (lazy) → PSRAM (reads PSRAM ring).
- IMU FIFO work buffer s_fifo_work — 2,048 B → PSRAM (bhy2 I2C CPU-copy, not DMA target).
- LoRa survey/act/hunt BSS — ~1,320 B → PSRAM (worker-only, cold).
- audio s_chunk (1,024) + mic s_buf (512) scratch — 1,536 B → PSRAM (CPU-copy sources into the real DMA buffers).
- sdmmc_card_t (~256 B) + LoRa cmd queue + audio queue — small movables → PSRAM.
- NOTE — LoRa worker stack (8,192) + cmd queue and audio/mic/ducky worker stacks (4×6,144) are ALREADY in PSRAM (Phase 1/2). The doc's Region-2 tables still charge ~35 KB of these to internal — stale; fix the doc.

# CANNOT MOVE — MUST SHRINK
- BLE/NimBLE controller block — 31,744 B (INT|DMA). True controller link-layer/DMA working memory; the one contiguous run the whole architecture protects. ALREADY AT SHRINK FLOOR: MAX_ACT=3 (2 breaks concurrent advert+scan, hci 0x207, and is pinned by coex_guard.c:51 #error), adv-report flow-ctrl at Kconfig min 50, scan-dup 100→20, STATIC_ACL_TX_BUF_NB=0. Shrink target: NONE further without losing co-resident advert+scan. Must be reserved FIRST at boot — this is the axis everything turns on.
- Display flush stage band — 29,520 B (INT|DMA), true DMA source (esp_lcd DMAs from it every flush; PSRAM source forces a per-flush internal bounce that wedges taskLVGL — the DMA-hang PR #95). Shrink target: 19,680 B (12→8 lines, −9,840) as the safe cut; 14,760 B (single band, no ping-pong, −14,760) sacrifices pipelining; the only elimination is a custom SPI_TRANS_DMA_USE_PSRAM panel-io (large effort, high risk).
- WiFi cache-TX — full 25,600 / lean 6,400 B (INT|DMA). The PSRAM→MAC copy cache; it exists PRECISELY to be internal (staging PSRAM TX into DMA-capable RAM) so relocating is a contradiction. Single largest WiFi slice starving BLE. Shrink target: lean 4 desc = 6,400 (already default with BT on); full-profile 16→8 = 12,800 saved.
- WiFi static-RX — full 6,400 / lean 3,200 B (INT|DMA), MAC DMAs frames INTO them. Shrink target: lean 2 (default); further only 4→3 (−1,600) and must drop RX_BA_WIN in lockstep (compile #error at RX_BA_WIN > 2×static_rx).
- WiFi static-TX — full 9,600 / lean 3,200 B (INT|DMA), TX DMA source (SPIRAM_TRY_ALLOCATE forces static TX). Shrink target: lean 2 (default); full 6→4 = −3,200.
- WiFi mgmt-SBUF — full ~12,800 / lean ~3,200 B (INT|DMA). Shrink target: lean 2 (default); full 8→4 = −6,400 (degrades capture, safe for STA).
- WiFi PHY/RF cal + AMPDU-TX — ~5,000 B (INT|DMA), PHY cal must be internal for the RF path. Shrink target: NONE (IDF-internal, irreducible driver core).
- Audio I2S TX DMA — 3,840 B (INT|DMA). IDF hard-codes I2S_DMA_ALLOC_CAPS=INTERNAL|DMA|8BIT; GDMA reads it directly; no PSRAM route. Boot-reserved before WiFi. Shrink target: 1,920 B via stereo→mono slot (verify MAX98357A channel strap) OR 2,880 via desc 4→3. Sample-rate does NOT shrink it (rate-independent formula).
- Mic PDM I2S RX DMA — 2,880 B (INT|DMA), same I2S pin. Currently lazy/released (0 at steady state) but that is the time-multiplex the operator wants gone; making it co-resident costs +2,880 permanent. Shrink target FIRST: 1,920 B via dma_desc_num 6→4 so the permanent tax is ~1.9 KB.
- USB MSC endpoint buffer — 8,192 B (INT|DMA, DMA-mode DRAM_ATTR, static .bss present even when USB detached). Shrink target: 4,096 (−4,096, ~half bulk throughput) or 512 (−7,680, ~0.24 MB/s).
- USB CDC-ACM FIFOs — ~2,056 B (INT|DMA). Shrink target: RX 512→64 (−448, near-free) + TX/EP 512→256 (−512, throughput cost on live-PCAP feed).
- OTA s_buf staging — 4,096 B (INT|DMA). Dual-role: SD-read DMA target AND esp_ota_write flash source (cache off) — pinned on both sides. Shrink target: make LAZY (frees 4,096 when not installing = ~always) rather than shrink; shrinking below the 4096 FatFs sector reintroduces a bounce.
- SD SPI3 GDMA descriptors + host ctx — ~1,024 B (INT|DMA). Literal DMA-engine structures. Shrink target: near-minimal (~24-48 B from max_transfer_sz 4096→512, trivial). The real SD hazard is the TRANSIENT 4096 B FatFs bounce (fix via CONFIG_FATFS_SECTOR_512), not this resident cost.
- GNSS UART1 RX ring — 2,048 B (INT|DMA pool occupancy; ISR-fed, standard uart driver has no PSRAM knob). Operator-HIGH. Shrink target: 1,536 (−512, low risk); 1024 risks dropped NMEA under GPX/wardrive SD-lock stalls.
- HID endpoint buffers — 192 B (INT|DMA, DMA-mode). Shrink target: NONE (already FS-minimum 64 B/EP; negligible).
- Must-stay INT (not DMA but flash-cache-off / component-created, cannot relocate): main stack 8,192 (ota_confirm on-task; app_main never returns), NimBLE-host 6,144 (bond NVS + IDF-created), ble worker 6,144 (nvs_commit + FATFS drain), wifi worker 4,096 (conservative — driver-control on-task), logbook s_ram 4,084 + logflush 3,584 (esp_partition erase/write), coredump 1,792 (panic, cache off), TinyUSB-device 4,096 (USB-OTG DMA), OTA worker 8,192 (flash cache off). Shrink target for these is call-depth discipline / watermark-verified trims (main 8192→6144, logflush 3584→2560), NOT relocation — each was sized up after a real overflow.

# TOTAL SHRINK SAVINGS
Realistic SHRINK savings (bytes ELIMINATED from the scarce pool, distinct from PSRAM relocation) ≈ 30,000–32,000 B:

- Display flush stage 12→8 lines: 9,840 (cost: slower nav wipe / CC drag fps)
- WiFi cache-TX 16→8 (full profile): 12,800 (cost: TX back-pressure under bursty uplink; lean already applies 4)
- USB MSC BUFSIZE 8192→4096: 4,096 (cost: ~half File-Share bulk throughput — USB is low-pri)
- Audio I2S TX stereo→mono slot: 1,920 (cost: MUST verify MAX98357A channel strap on-device)
- Mic I2S RX dma_desc_num 6→4: 960 (cost: 96→64 ms read span; rare dropped block under load)
- USB CDC RX FIFO 512→64: 448 (near-free; device is TX-mostly)

Subtotal ≈ 30,064 B. Optional additional if margin demands (higher risk / UX cost): WiFi mgmt-SBUF 8→4 (+6,400 full), WiFi static-TX 6→4 (+3,200 full), logbook s_ram 4084→2048 (+2,048), main stack 8192→6144 (+2,048 after watermark), GNSS ring 2048→1536 (+512). Pushing the aggressive full-profile levers takes it to ~44,000 B but degrades exactly the capture/throughput use-cases the full profile exists for.

NOTE: the far larger lever is RELOCATION (~124,650 B of movable stacks + cold BSS → PSRAM), which is what actually makes co-residence possible; shrink alone (~30 KB) is insufficient — see co_resident_budget. Do not conflate the two: relocation moves bytes to abundant PSRAM; shrink removes them entirely.

# CO-RESIDENT BUDGET
HONEST ARITHMETIC against the measured pool (total internal-DMA ≈ 180,251 B; largest pristine contiguous run = 90,112 B):

STEP 1 — Sum EVERY must-stay-internal consumer if NOTHING is relocated and everything is co-resident (WiFi lean):
  Must-stay INT|DMA (true DMA / flash-src): BLE 31,744 + display 29,520 + WiFi-lean ~21,000 + audio 3,840 + mic 2,880 + USB(MSC 8,192+CDC 2,056+HID 192)=10,440 + OTA s_buf 4,096 + SD 1,024 + GNSS ring 2,048 = 106,592
  Must-stay INT stacks/BSS (cache-off / component-created, can't move): main 8,192 + NimBLE-host 6,144 + ble 6,144 + wifi 4,096 + logbook 8,168 + logflush 3,584 + coredump 1,792 = 38,120
  Movable-but-not-yet-moved INT (stacks + cold BSS): taskLVGL 16,384 + monitor 16,768 + UI pools 11,700 + weather 8,192 + IMU 6,144 + GNSS 6,144 + NFC 6,144 + usb_gadget 6,144 + buttons 4,096 + BLE-snapshot 6,000 + LoRa BSS 4,904 + GNSS-wardrive 3,072 + IMU-FIFO 2,048 + s_snap 4,084 + audio/mic scratch 1,536 + scan 3,600 ≈ 103,000 (excl. lazy wifi stacks)
  RAW TOTAL ≈ 106,592 + 38,120 + 103,000 = ~247,700 B.

  247,700 > 180,251 → EVERYTHING co-resident WITHOUT relocation does NOT fit. Impossible. Relocation is mandatory, not optional.

STEP 2 — Apply the strategy (move all movable INT → PSRAM, shrink the rest):
  Move ~124,650 B of stacks/cold-BSS to PSRAM (PSRAM has ~4 MB free — trivially absorbs it).
  Remaining must-stay internal = 106,592 (INT|DMA) + 38,120 (unmovable INT) = 144,712 B.
  Apply realistic shrinks (~30,000 B: flush 12→8 = 9,840; cache-TX 16→8 = 12,800; USB MSC = 4,096; audio mono = 1,920; mic desc = 960; CDC = 448) → ~114,700 B must-stay internal.
  114,700 < 180,251 → FITS as total bytes, with ~65 KB headroom.

STEP 3 — The contiguity test (the real killer, not total bytes):
  After BLE claims 31,744 first from the pristine 90,112, largest drops to ~59,392. Every other must-stay consumer's LARGEST single allocation is a 14,760 flush band (or 9,840 if trimmed); nothing else needs >6,400 contiguous. All are reservable at boot while the pool is ≥30 KB. So no single post-BLE allocation exceeds the available run — provided the boot-order + boot-reserve discipline holds.

VERDICT: EVERY feature CAN be co-resident in fast int-DMA at once — but ONLY with (1) all movable stacks/BSS pushed to PSRAM (mandatory: raw internal demand ~248 KB overflows the 180 KB pool by ~68 KB), (2) WiFi pinned lean whenever BT is on, (3) the ~30 KB of shrinks, and (4) boot-order reservation so BLE's 31,744 run and both flush bands are claimed from the pristine pool before WiFi fragments it. The one irreducible risk that remains after all this: the BLE 31,744 gate has only ~4 KB of measured slack and no .map — a single new boot-permanent int-DMA consumer, an IDF bump, or a MAX_ACT increase re-breaks coexistence. It fits, but it fits tightly and by construction (order), never by slack.

# RESERVATION PLAN
- BLE/NimBLE controller (nocsif_ble_boot_reserve, FIRST — before esp_wifi_init): 31,744 @ int-DMA (from pristine ~90,112 pool)
- Display flush stage 2× ping-pong bands (at UI init, before WiFi): 29,520 (19,680 if trimmed 12→8) @ int-DMA (heap_caps_aligned_alloc DMA|INTERNAL|8BIT)
- Audio I2S TX (nocsif_audio_boot_reserve, before WiFi): 3,840 (1,920 if mono) @ int-DMA (I2S_DMA_ALLOC_CAPS)
- Mic PDM I2S RX (NEW boot reserve to end the lazy time-multiplex): 2,880 (1,920 if desc 6→4) @ int-DMA
- WiFi lean statics (esp_wifi_init, AFTER the above are claimed): cache-TX 6,400 + static-TX 3,200 + static-RX 3,200 + mgmt 3,200 + PHY ~5,000: ~21,000 @ int-DMA (pin lean whenever BT on)
- GNSS UART1 RX ring: 2,048 @ int-DMA (ISR-fed; no PSRAM knob)
- USB class EP buffers (MSC + CDC + HID, static .bss — link-time, present from boot): 10,440 (6,300 if MSC 8192→4096 + CDC trims) @ int-DMA (DMA-mode DRAM_ATTR)
- OTA s_buf (make lazy — reserve only during install): 4,096 @ int-DMA (dual SD-read + flash-write)
- SD SPI3 GDMA desc + host ctx (boot, once): ~1,024 @ int-DMA
- Unmovable INT stacks/BSS (main 8,192 + NimBLE-host 6,144 + ble 6,144 + wifi 4,096 + logbook 8,168 + logflush 3,584 + coredump 1,792): 38,120 @ int-DMA (cache-off/component-created — cannot relocate)
- All movable worker stacks (taskLVGL 16,384 + weather 8,192 + IMU 6,144 + GNSS 6,144 + NFC 6,144 + usb_gadget 6,144 + buttons 4,096 + lazy wifi stacks 19,456): ~72,704 @ PSRAM (xTaskCreateWithCaps MALLOC_CAP_SPIRAM)
- All cold INT BSS (WiFi monitor 16,768 + UI pools 11,700 + BLE snapshot 4,192 + LoRa 4,904 + GNSS-wardrive 3,072 + IMU-FIFO 2,048 + s_snap 4,084 + scan 3,600 + audio/mic scratch 1,536): ~51,900 @ PSRAM (EXT_RAM_BSS_ATTR / lazy heap_caps SPIRAM)
- NimBLE host heap (already external; RAISE msys 24→32 for reliability): 18,972 (+4,096) @ PSRAM (BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL)
- LVGL draw buffers + object heap + canvases + WiFi rings + WAV/record buffers (already external): >4,500,000 @ PSRAM

# OPEN QUESTIONS
- MAX98357A channel strap: is the speaker DAC strapped L, R, or (L+R)/2? Determines whether stereo→mono I2S TX (−1,920 B) outputs audio or silence. Needs on-device verification before taking the single biggest audio int-DMA lever.
- Mic PDM RX slot count on the S3: doc flags it 'may be 5,760 if PDM RX allocates 2 slots' vs the assumed 2,880 (mono). Confirm with heap_caps_get_largest_free_block before/after mic_open — doubles the value and the shrink leverage if 2-slot.
- taskLVGL → PSRAM safety: is the LVGL render task ever scheduled while the flash cache is disabled during an OTA erase? If yes, a PSRAM stack faults. This 16,384 B reclaim is the largest single lever and is gated entirely on this on-device check.
- usb_gadget worker → PSRAM: does the MSC-mount / FAT-remount path (the active File-Share entry-crash branch) run any on-task internal-flash op? Must audit before relocating, and ideally after the entry-crash root cause is known to avoid confounding the investigation.
- wifi command worker (4,096) → PSRAM: does any on-task handler (do_enable/apply_config/esp_wifi_set_config) trigger an NVS/internal-flash write with cache off? Currently kept internal conservatively; a clean audit could reclaim 4,096 B.
- No .map exists: every full/lean WiFi byte figure and the 12,800 B mgmt-SBUF assume IDF's 1,600 B/buffer. The 31,744 BLE gate has only ~4 KB measured slack. A linker .map + the continuous largest-free heartbeat regression alert (doc open action) are needed to make the co-resident budget auditable rather than empirical.
- buttons worker → PSRAM: does any button handler persist a setting via synchronous on-task NVS (e.g. long-press save)? If so, defer to a worker before relocating the stack.
- Making the mic genuinely co-resident (boot-reserved, not lazy/released) adds +1,920–2,880 B permanent to the scarce pool — is always-on mic capture actually required by the future all-features-at-once app, or is lazy acceptable for this one consumer? It is the clearest tension between the operator's co-resident rule and int-DMA scarcity.
- Doc drift to fix (housekeeping, not a question): RAM-BUDGET.md Region tables still charge LoRa stack+queue (~9.2 KB) and audio/mic/ducky stacks (~18 KB) to internal though they are already in PSRAM; flush-stage cite is ui.c:20290-93 not 19421-24; audio I2S is 3,840/4-desc at audio.c:122 not 5,760/6-desc; WiFi buffer-count cite points at on_ip_evt not the config; NOCSIF_RADIO_MIN_DMA_LORA=12288 is dead code.