# NocSif RAM Budget, Contention & Coexistence Plan

**Board:** LilyGo T-Watch Ultra — ESP32-S3, 8 MB QUAD PSRAM, 16 MB QIO flash
**Scope:** What consumes RAM in each region, the contention that causes crashes / non-init, where future plan features will push next, and a coexistence-first plan to resolve it.
**Status:** *Analysis + plan only. No code has changed.* Every action in this document is a proposal; the "Prioritized actions" list is the intended sequence, not a changelog.

> The one sentence that explains the whole firmware architecture: **the scarce resource on this board is not free RAM, it is one contiguous run of internal DMA-capable SRAM ~31.7 KB wide, and almost every memory decision in the tree exists to keep that single hole reachable.**

---

## The memory map in one minute

The ESP32-S3 has **~512 KB of on-chip SRAM**, unified — IRAM (instructions) and DRAM (data) are carved from the same silicon, and ~48 KB of it is spent on cache (16 KB I-cache `0x4000` + 32 KB D-cache `0x8000`, the D-cache mandatory the moment PSRAM is enabled). What's left is roughly **300–380 KB of usable internal DRAM heap**. Off-chip there is **8 MB of QUAD PSRAM** (QSPI @80 MHz, `MALLOC_CAP_SPIRAM`), **~16 KB of RTC RAM** (unused here), and **16 MB of flash** (XIP, near-zero RAM cost).

Within the internal DRAM heap sits the pool that actually governs this firmware: the **DMA-capable subset** (`MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA`). Two rules dominate everything:

- **(a) The killer is CONTIGUOUS internal-DMA, not free bytes.** Measured on-device (main.c heartbeat gauge): the largest int-DMA block is ~80 KB at idle with the display up and no radios, but once `esp_wifi_init` runs it caps at **~27,648 B (full profile) / ~31,736 B (lean)** *no matter how much total is free*, and under steady-state BLE+WiFi it collapses as low as **~384 B**. Proof it is contiguity and not bytes: freeing ~10 KB of worker stacks moved total free 28k→38k but left the largest block at 27,648 B unchanged. *Chasing "free" is a dead end; only "largest" matters.*
- **(b) PSRAM is big but cannot hold DMA buffers or ISR/DMA task stacks, and it needs internal staging.** QSPI PSRAM cannot back SPI-DMA targets — `spi_master` bounces PSRAM→internal per transfer unless `SPI_TRANS_DMA_USE_PSRAM` is set (it never is for the LCD path), which is exactly why a persistent internal flush stage band exists. Task stacks for any task that touches DMA/ISRs must stay internal even with `SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y`.

The load-bearing configuration that implements these rules:

| Config key | Value | What it buys |
|---|---|---|
| `CONFIG_LV_USE_CUSTOM_MALLOC` | `y` → `lv_mem_psram.c` | Moves the ~96 KB LVGL object heap out of internal `.bss` into PSRAM — the single biggest lever; without it BLE+WiFi cannot coexist. |
| `SPIRAM_TRY_ALLOCATE_WIFI_LWIP` | `y` | Routes WiFi dynamic-RX + LWIP pools to PSRAM (static/cache-TX stay internal). |
| `SPIRAM_MALLOC_ALWAYSINTERNAL` | `4096` | Allocations ≤4 KB go internal, larger ones prefer PSRAM — keeps big buffers out of the scarce pool. |
| `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL` | `y` | NimBLE host heap + mbufs → PSRAM; only the ~30 KB controller stays internal. |
| `CONFIG_BT_CTRL_BLE_MAX_ACT` | `3` | Controller activity slots (3 × 828 B), folded into the ~31.7 KB contiguous block. |
| `ESP_WIFI_IRAM_OPT` / `RX_IRAM_OPT` | `n` / `n` | Returns >27 KB of would-be IRAM to the DRAM heap (a *source*, not a consumer). |

---

## Executive summary

The scarce resource is **one contiguous run of internal DMA-capable SRAM**. When WiFi is up, the largest hole is permanently capped at **~27.6 KB (full) / ~31.7 KB (lean)**; the BLE controller needs **~31.7 KB in one run** (measured gate `BLE_MIN_DMA_BLOCK=31744`). The entire architecture — LVGL→PSRAM, NimBLE host→PSRAM, WiFi dynamic/LWIP→PSRAM, `SPIRAM_MALLOC_ALWAYSINTERNAL=4096`, both `IRAM_OPT=n` flags, the lean WiFi profile, and **claiming the BLE block at boot *before* WiFi starts** — exists to keep that one hole alive. Current headroom over the failure floor is only **~4 KB** (27,648 fails "BLE_INIT: Malloc failed" → int-WDT panic; 31,736 works).

Top risks:

- **Boot-order fragility (critical).** If `nocsif_ble_boot_reserve` is skipped (safe-mode, a future init reorder, or Bluetooth OFF→ON after WiFi is up), BLE is simply unavailable until reboot. Once WiFi has fragmented the pool, the controller can never be re-claimed at runtime.
- **Runtime radio cycling (critical).** Any bring-up/teardown of a radio at runtime fragments the pool; the current design *avoids all lifecycle churn* and only toggles monitor activity. This directly limits the §4.6 Governor's power-management ambitions.
- **Thin margin + no `.map` (high).** The 31744 gate is empirical with ~4 KB of slack and no linker size report exists; an IDF bump, a `MAX_ACT` increase, or one more boot-permanent int-DMA consumer can push the post-WiFi cap below the gate in a clean build.
- **The stale-sdkconfig landmine (high).** A build from the Aug-20 generated cache would silently revert `LV_USE_CUSTOM_MALLOC`, `SPIRAM_MALLOC_ALWAYSINTERNAL`, and `BT_CTRL_BLE_MAX_ACT`, breaking coexistence or WDT-rebooting — with no CI guard.
- **SD-under-WiFi and USB-DMA paths (high).** `sdspi` bounce faults under WiFi fragmentation; the OTA workaround (`nocsif_wifi_radio_yield`) does not transfer to a feature that serves a file *over* the same WiFi.

---

## Live measured data — COM7 capture (2026-09-01, this exact build)

Three serial captures on the operator's unit (fresh boot via System→Restart, steady-state while driving Signal Hunt, and airplane). **These are the ground-truth numbers for the current build (ESP-IDF 5.5.4, app compiled Aug 27 2026); the per-region byte *estimates* below were derived without a `.map` and should be read against these.** Source: `main.c` heartbeat gauge + the wifi/ble bring-up logs.

### Boot-time int-DMA timeline (fresh boot) — why ordering is the whole game

| Stage | int-dma free | **largest contiguous** |
|---|---|---|
| PSRAM init (`8,388,608 B` exposed); `internal free heap` | **180,251** total internal | — |
| **pre-BLE** (clean, unbroken pool) | 162,123 | **90,112** |
| after NimBLE controller + host task | 125,263 | 59,392 |
| `boot reserve: NimBLE UP (controller block claimed)` | 125,175 | 59,392 |
| after `esp_wifi_init (LEAN profile)` | 44,331 | **30,720** |
| steady-state, phone connected (BLE conn + WiFi) | ~3,251 | **~3,072** |

BLE wins its ~31.7 KB block because it is claimed **first, from a pristine ~90 KB pool** — *not* from a 4-KB-margin post-WiFi hole. WiFi init is the single biggest consumer (free 125→44 KB, largest 59→30 KB). Confirmed permanent allocations, exactly as logged: **display flush stage = 2×14,760 = 29,520 B**; **orrery canvas = 617,460 B PSRAM (stride 1230)**; **2× full-frame LVGL buffers ≈ 1.23 MB PSRAM**; **LVGL pool 25 % of the 8 MB, frag 2 %** (so LVGL objects do not touch internal RAM).

### Runtime largest-contiguous by radio state — the fragmentation ceiling

| Radio state (runtime) | int-dma free | **largest** | BLE (needs 31,744) |
|---|---|---|---|
| WiFi up, **LEAN** profile | 38,371 | **15,872** | refused |
| WiFi up, **FULL** profile | 28,091 | **15,872** | refused |
| WiFi monitor + parser on | ~34,655 | **15,872** | refused |
| **WiFi fully released (deinit)** | 58,107 | **21,504** | **still refused** |
| Airplane (radios torn down at runtime) | 18,675 | **9,216** | refused |

**Two findings that refine the estimate-based narrative below:**

1. **The lean profile does NOT widen the contiguous hole.** LEAN and FULL both leave `largest = 15,872`; lean only raises *total free* (~10 KB). So "ask WiFi for its lean profile to free the radio" buys total-free headroom but **not the contiguity BLE needs** — only a full WiFi *deinit* moves `largest` (to 21,504), and even that is **< 31,744**. The runtime "turn WiFi off to enable BLE" path therefore **cannot actually re-admit BLE on this build** — it was observed failing live: `E ble: BLE bring-up REFUSED: only 15872 B contiguous internal DMA (need 31744). Turn WiFi off to free the radio.` fired on every Signal-Hunt→BLE switch, and the follow-on WiFi release reached only 21,504.
2. **Runtime teardown never defragments.** No runtime radio-off state recovers `largest` toward the boot-time 90,112 (WiFi-deinit → 21,504; airplane → 9,216). **Only a reboot restores a BLE-sized contiguous block.** Coexistence is won once, at boot, by ordering + reserve — and lost irrecoverably (until reboot) by any runtime radio lifecycle churn. This sharpens conflicts C1/C2/C13 and the §4.6 Governor predictions: the honest per-radio "power BLE down then back up" lease is **impossible without a reboot** on the current design.

Secondary live pressure: the NimBLE msys/procedure pool (`MSYS_1_BLOCK_COUNT=12`) briefly exhausted during the phone ANCS/AMS discovery burst (`GATTC proc alloc failed; op=write`), then recovered — a candidate to raise if §4.4 notify-heavy features land.

> Full capture logs and the distilled fact sheet are retained in the working notes (`scratchpad/cap_boot.txt`, `cap_passive.txt`, `cap_airplane.txt`, `measured_facts.md`).

---

## Per-region budgets

Sizes are derived from source constants + ESP-IDF driver knowledge, not read from symbols (no `.map` exists) — read them against the *Live measured data* above. See also *Measurement & uncertainty*.

### Region 1 — internal-DMA (`INTERNAL|DMA`) — **the scarce, contended pool**

Capacity here is the **sum of resident consumers when both radios + display + latched audio are up (~134 KB)**, not a hardware ceiling. The binding constraint is the largest *contiguous* run, not the total: ~80 KB idle → capped at ~27.6 KB (full) / ~31.7 KB (lean) once WiFi inits → as low as ~384 B steady-state.

| Consumer | Size (est) | Lifetime | Cap flags | Ref |
|---|---|---|---|---|
| esp_wifi driver internal buffers, **FULL** profile (static RX 4×1600 + static TX 6×1600 + cache-TX 16×~1600 + mgmt SBUF + AMPDU/PHY). Cache-TX 25.6 KB is the single largest, cannot move to PSRAM | **59,392** (lean ~37,888) | lazy @ first `esp_wifi_init` ~1.8 s; fixed at init, freed only by deinit/yield | INTERNAL\|DMA | wifi.c:913/904-912; `STATIC_RX=4`/`STATIC_TX=6`/`CACHE_TX=16`/`MGMT_SBUF=8` |
| **BT/NimBLE controller** contiguous bring-up block (incl. `BLE_MAX_ACT` 3×828, adv-report flow-ctrl 50, scan-dup 20) | **31,744** | boot-permanent when BT master ON; claimed *before* `esp_wifi_init`, held via `ble_quiesce`, never freed at runtime | INTERNAL\|DMA, **one contiguous run**; gate `BLE_MIN_DMA_BLOCK=31744` | ble.c:1546/1589/3154; `BT_CTRL_BLE_MAX_ACT=3` |
| Persistent **display flush stage band** (2 ping-pong bands, replaces per-flush bounce, PR #95) | **29,520** | boot-permanent (allocated at `nocsif_ui_init` while int-DMA plentiful) | INTERNAL\|DMA\|8BIT, 4-byte aligned; 2 × (12 lines × 410 × 3 B) | ui.c:19421-19424; display.h:41-42 |
| Audio **I2S TX** DMA buffers (MAX98357A; 6 × 240 × 2ch × 2 B) | **5,760** | lazy then effectively boot-permanent (`i2s_lazy_open` on boot chime, never `i2s_del_channel`) — the Signal-Hunt "audio NO_MEM" | INTERNAL\|DMA | audio.c:96/114 |
| OTA **SD-read DMA staging** `s_buf[OTA_CHUNK]` | **4,096** | boot-permanent (static BSS, 64-byte aligned; worker-only) | INTERNAL\|DMA, aligned 64 | ota.c:59 |
| Mic **I2S RX** DMA buffers (PDM; 6 × 240 × mono × 2 B) | **2,880** (may be 5,760 if PDM RX → 2-slot) | lazy AND released per-screen (`mic_close` → `i2s_del_channel`, well-behaved) | INTERNAL\|DMA | mic.c:116/131 |
| **GNSS UART1 RX** ring (u-blox M10, receive-only) | **2,048** | lazy then effectively boot-permanent (driver stays installed); TX buf = 0 | INTERNAL\|DMA | gnss.c:826 (`GNSS_RX_BUF=2048`) |
| **SD sdspi + spi_master (SPI3)** DMA descriptors + bounce (shared with parked LoRa/NFC CS) | **1,024** | boot-permanent | INTERNAL\|DMA, `SPI_DMA_CH_AUTO`, `max_transfer_sz=4096` | sdcard.c:99-124 |

**Subtotal (resident worst case): ~133,584 B.** **Headroom note:** this is the region that fails. The worst-case resident total *fits* in total DMA-capable bytes, but the failure is **contiguity**: once WiFi is up nothing larger than ~27.6 KB (full) / ~31.7 KB (lean) can be claimed regardless of total free. Three live gates guard this pool: **BLE 31744, LoRa 12288, historical floor 24576.** Runtime BLE re-init once WiFi is fragmented is impossible ("restart the watch to finish enabling it").

### Region 2 — internal DRAM heap (general `INTERNAL 8BIT` — task stacks + BSS tables, not DMA-tagged)

~300–380 KB usable. Bytes here aren't individually scarce, but every permanent stack/table subtracts from the same unified SRAM the int-DMA pool draws from, and their placement **manufactures the fragmentation**.

| Consumer | Size (est) | Lifetime | Cap flags | Ref |
|---|---|---|---|---|
| **Worker task-stack aggregate** (18 `xTaskCreate`: audio/ble/gnss/imu/mic 6144 ea; lora/ota/weather/httpd 8192 ea; buttons/wifi/wifidns/TinyUSB 4096; logbook 3584; wifiparse 3072; ducky/nfc/usb_gadget/wifipcap/wifihc 6144 + main 8192) | **113,152** | mixed: LVGL/main/buttons/wifi/audio/ble/imu boot-permanent; ducky/ota/mic/lora/pcap/hc/dns/httpd lazy/transient | INTERNAL, task-stack (6144 mandated for deep FatFs/SDSPI chains) | totalizer across audio.c:314, ble.c:2965, gnss.c:921, imu.c:416, lora.cpp:1026, ota.c:284, wifi.c:2183/2512/2739/3638/4166 |
| **WiFi monitor/identity BSS** (`s_mon_hs` 8704 [160 B EAPOL ×32 dominates] + `s_mon_probe` 3456 + `s_mon_sta` 2304 + `s_mon_ap` 2304 + scan/creds/beacon) | **16,800** | boot-permanent BSS (reserved even when WiFi idle) | INTERNAL, static/BSS | wifi.c:327/344/358/386 |
| **LVGL port task stack** (`taskLVGL`, raised 7168→16384 after CC composite overran) | **16,384** | boot-permanent | INTERNAL, task-stack | ui.c:19363 |
| **UI live-row pools** (~14 `live_row_t[32]` + `s_apps[128]` + alert/alarm/notif/hunt/survey; LVGL objects themselves live in PSRAM) | **15,000** | boot-permanent BSS | INTERNAL, static/BSS | ui.c:1152/3312 (est) |
| **logbook** ESP_LOG tee ring + snapshot (`s_ram` + `s_snap`, 2 × 4084) | **8,168** | boot-permanent BSS | INTERNAL, static/BSS | logbook.c:47/50 |
| **NimBLE host task stack** (raised 4096→6144 after ANCS notify-cb deep write chain overflowed & corrupted `ble_hs_timer`) | **6,144** | lazy, resident with controller while BT master ON (cannot be PSRAM) | INTERNAL, task-stack | `BT_NIMBLE_HOST_TASK_STACK_SIZE=6144`; ble.c:1611 |
| **BLE snapshot tables** (`s_dev[48]` 3072 + `s_anc[16]` 2944 + `s_chr[48]` 2304 + `s_drone[10]` 1040 + svc/ds_buf 512 + host RX/parse 424) | **6,000** | boot-permanent BSS | INTERNAL, static/BSS | ble.c:136/175/176/281/339/351/378/1382 |
| **LoRa BSS** (`s_inbox[16]` 3584 + survey ~976 + activity/hunt/published ~340) — inbox survives worker teardown | **4,900** | boot-permanent BSS (present even if LoRa never used) | INTERNAL, static/BSS | lora.cpp:123/133-135/145-150 |
| **GNSS wardrive** BSSID dedup `s_wd_seen[512][6]` | **3,072** | boot-permanent BSS (whether or not wardrive runs) | INTERNAL, static/BSS | gnss.c:153 (`WD_SEEN_MAX=512`) |
| **IMU FIFO** work buffer `s_fifo_work` | **2,048** | boot-permanent BSS | INTERNAL, static/BSS | imu.c:104 |
| Assorted small static/driver contexts (RTC/power/touch/xl9555/settings/i2c/USB desc/sdmmc/crash) | **2,500** | boot-permanent (mixed) | INTERNAL, static/BSS + small heap | rtc.c/power.c/touch.c/xl9555.c/settings.c/sdcard.c/reliability.c |
| **esp_core_dump** dedicated panic-handler stack | **1,792** | boot-permanent .bss (used only during a crash) | INTERNAL, reserved .bss | `ESP_COREDUMP_STACK_SIZE=1792` |

**Subtotal (representative resident): ~190,000 B.** **Headroom note:** the lazy-worker strategy (ducky/ota/mic/lora/pcap/hc/dns/httpd created on demand; LoRa/pcap/hc deleted after use) exists specifically so these ~113 KB of stacks do not sit permanently in the way of the BT and WiFi contiguous claims. LoRa's 8192 stack must itself be one contiguous int-DMA run (gated at 12288).

### Region 3 — PSRAM (8 MB external, QSPI @80 MHz) — the deliberate dumping ground

| Consumer | Size (est) | Lifetime | Cap flags | Ref |
|---|---|---|---|---|
| **LVGL two full-frame draw buffers** (double-buffered RGB888, full_refresh; 2 × 410×502×3) | **1,234,920** | boot-permanent | SPIRAM (port rejects DMA at RGB888); ~1.85 MB/motion-frame caps scroll ~8-9 fps | esp_lvgl_port disp_cfg; `LV_COLOR_DEPTH_24` |
| **WAV-playback** whole-clip read buffer (cap 2 MB; typical 30 s ~960 KB) | **960,000** | transient per-playback (`malloc`→free; streamed via 1 KB internal `s_chunk`) | SPIRAM | audio.c:230/50 |
| **Voice-memo capture** `s_rec_buf` (30 s × 16 kHz × 2 B) | **960,000** | transient on-demand (record→finalize free) | SPIRAM | mic.c:329 |
| **Orrery/star wallpaper canvas** (single 410×502 `lv_canvas`, render-once) — largest boot-permanent PSRAM resident | **618,464** | boot-permanent (stride 1232 × 502; repainted in place) | SPIRAM, 64-byte aligned | ui_background.c:405 |
| **draw_full/boot-splash** framebuffer | **617,460** | transient at boot only (`malloc`→free same call; RGB888 despite stale RGB565 comment) | SPIRAM | display.c:230 |
| **Screenshot** one-shot full-frame (legacy RGB565) | **411,640** | transient (`malloc`→free) | SPIRAM | display.c:230 |
| **Watchface peek pick-wheel** `s_pick_wheel_buf` (ARGB8888) | **144,400** | per-screen (freed on leave) | SPIRAM, 64-byte aligned | ui.c:13244 (`WHEEL_D=190`) |
| **Signal-Hunt compass needle** `s_bh_needle_buf` (ARGB8888, direct raster PR #162) | **123,904** | per-screen (freed on leave) | SPIRAM, 64-byte aligned | ui.c:7532 (`HUNT_NDL_SZ=176`) |
| **LVGL object heap** (all widgets/styles/labels/canvas structs) — the ~96 KB formerly in internal .bss | **100,000** (live peak) | pooled/per-screen (Home ~25 KB, submenu ~11 KB ea; bounded ~37 KB on nav-pop; exhaustion **HANGS** `lv_draw_label`) | SPIRAM (`LV_USE_CUSTOM_MALLOC`) | lv_mem_psram.c:45-72 |
| **WiFi parser copy-out ring** `s_ring` (`cap_slot_t[256]`, SPSC) | **69,632** | lazy (first parse; not freed for session) | SPIRAM (capture is *healthier* for int-DMA) | wifi.c:2164 |
| **WiFi PCAP all-frames ring** `s_pcap_ring` (`pcap_slot_t[128]`) | **53,248** | lazy (first arm; not freed on stop) | SPIRAM | wifi.c:2501 |
| **Transient PSRAM utilities** (Files rm-rf 12288 + logbook tail 8169 + captive-portal ≤8192 + dashed-rule buffers) | **40,000** | transient / per-screen | SPIRAM (aligned canvases 64-byte) | ui.c:11562; logbook.c:208; wifi.c:3496; ui_nav.c:388 |
| **Rounded-corner masks** (4 × ARGB8888 40×40 on `lv_layer_top`) | **25,600** | boot-permanent (per-mask fail cosmetic) | SPIRAM, 64-byte aligned | ui_background.c:216/457 |
| **NimBLE host heap** (msys mbuf pools + ATT/GATT/GAP reg + bond cache) | **18,972** | lazy (`nimble_port_init`→deinit) | SPIRAM (`MEM_ALLOC_MODE_EXTERNAL`) | NimBLE lib; `MAX_CONNECTIONS=1` |
| **BLE advert PCAP** SPSC ring `s_pcap_ring` (`ble_pcap_slot_t[256]`) | **16,384** | lazy on Record-Traffic arm (reused) | SPIRAM | ble.c:1987 |

**Subtotal (representative): ~1,979,000 B.** **Headroom note:** effectively unconstrained — worst-case simultaneous resident (draw 1.23 MB + orrery 0.6 MB + a 0.96 MB record/playback + capture rings + object heap ≈ 3.5–4 MB) still leaves ~4 MB free of 8 MB. **PSRAM pressure is bandwidth (motion-frame recompositing caps fps), not capacity.** Everything movable was deliberately pushed here to protect the internal-DMA pool.

### Region 4 — IRAM (instruction) — a *source*, not a pressure point

| Consumer | Size (est) | Lifetime | Cap flags | Ref |
|---|---|---|---|---|
| **WiFi IRAM optimization DELIBERATELY DISABLED** (`ESP_WIFI_IRAM_OPT=n` + `RX_IRAM_OPT=n`) — returns >27 KB of would-be IRAM to the DRAM heap | **27,648** reclaimed | boot-permanent (a RAM-relief lever; costs WiFi throughput) | IRAM carved from SRAM; disabling grows int-DMA-capable DRAM | `ESP_WIFI_IRAM_OPT=n` / `RX_IRAM_OPT=n` |

**Headroom note:** not a pressure point — it is one of the tools funding the int-DMA hole (>27 KB reclaimed to DRAM). `COMPILER_OPTIMIZATION_PERF` (-O2) enlarges code/IRAM for ~2× faster LVGL blend, but no firmware line item competes for IRAM specifically.

### Region 5 — RTC RAM

~16 KB RTC slow+fast. **No NocSif consumer places data here.** The reliability "last crash" record persists via NVS (flash), not RTC RAM. Fully available if deep-sleep state retention is ever needed.

### Region 6 — flash-rodata (context only — NOT counted against RAM)

| Consumer | Size (est) | RAM cost | Ref |
|---|---|---|---|
| **BHI260AP sensor-hub RAM firmware image** `bosch_bhi260_gpio_firmware_image[]` | 119,716 (flash) | **~0** — streamed const-ptr→I2C in ≤64 B chunks to the hub's own RAM; only RAM staging is `s_wbuf[65]` | bhi260_fw_image.h; imu.c:250 |
| Fonts (Montserrat_14), OUI table, captive-portal HTML, HID report map, USB descriptors | ~2,000 (flash) | ~146 B USB runtime descriptor copy (counted under internal DRAM) | wifi.c:1582/3393; ble.c:2304; nocsif_usb_desc.c; hid_kbd.c:31 |

**Headroom note:** flash footprint only. Partitions: `ota_0`/`ota_1` 4 MB each @`0x20000`/`0x420000`, logs `0xE0000`, coredump `0x40000`. The BHI260 image is the notable "looks like RAM but isn't" item — zero heap cost.

### Biggest consumers overall (ranked)

1. **WiFi driver int-DMA buffers** ~59 KB full / ~38 KB lean (cache-TX 25.6 KB single largest, cannot move to PSRAM) — the ~30 KB slice that starves the BT controller.
2. **BT/NimBLE controller** ~30–31.7 KB int-DMA (measured gate 31744; the one contiguous run that must be claimed before WiFi or never).
3. **Display flush stage band** 29.5 KB int-DMA (boot-permanent; the reservation that makes per-flush bounces unnecessary).
4. **FreeRTOS worker task stacks** ~113 KB internal DRAM aggregate — biggest internal-SRAM class after the radios, and the main fragmentation source.
5. **LVGL two RGB888 draw buffers** 1.23 MB PSRAM + **orrery wallpaper** 0.6 MB PSRAM — largest absolute allocations, deliberately in abundant PSRAM.
6. **Audio/voice bulk PCM** ~0.96 MB PSRAM each (transient) — and the small 5.76 KB int-DMA I2S TX channel that, once latched, denies BLE its hole.

---

## Where RAM goes / what conflicts

13 conflicts, severity-sorted. See LESSONS.md for the field history behind each mitigation.

| # | Conflict | Region | Trigger | Mechanism | Symptom | Sev | Current mitigation | Residual risk |
|---|---|---|---|---|---|---|---|---|
| 1 | BLE controller cannot claim its ~30 KB contiguous block | int-dma | Any BLE bring-up after `esp_wifi_init` (CC tile, BLE screen, missed boot-reserve) | Needs one run ≥31.7 KB; post-WiFi hole caps at 27.6/31.7 KB → "Malloc failed" → btController int-WDT panic | **crash** | **critical** | `nocsif_ble_boot_reserve` before WiFi; gate 31744; `ble_quiesce` never frees; lean profile + PSRAM moves | Boot-order load-bearing; ~4 KB margin; OFF→ON at runtime refused |
| 2 | BLE↔WiFi↔LoRa runtime cycling fragments the pool | int-dma | Runtime radio switching or a transient burst | Each bring-up/teardown carves runs at different offsets → largest collapses to ~384 B while total looks healthy | **crash** | **critical** | Signal Hunt toggles *activity only*, nothing re-inited; gates 31744/12288/24576; LoRa solo | Gate passes one moment, fails next; no sustained tri-radio path |
| 3 | Display flush per-band int-DMA bounce starved the pool | int-dma | (Historical) large int-DMA consumer coinciding with a flush | `esp_lcd_panel_io_spi` never sets `DMA_USE_PSRAM` → per-flush ~13 KB internal bounce; when hole eaten, flush wedges | **crash** | **high** | **RESOLVED (PR #95)**: persistent 29.5 KB stage band, zero runtime int-DMA | Stage band is now a boot-permanent competitor; enlarging it shrinks the radio window |
| 4 | NimBLE host-task stack overflow on notify receipt | int-dram | ANCS/GATT notify with deep synchronous callback chain | 256 B RX + write-chain frame overflowed 4096 stack, corrupted `ble_hs_timer` | **crash** | **high** | Stack 4096→6144; RX/parse scratch moved off-stack to static BSS | Empirical headroom, not a bound; deeper §4.4 chains can overflow again |
| 5 | Stale/regenerated sdkconfig reverts a load-bearing key | int-dma | Build from Aug-20 cache, or fullclean regen from incomplete defaults | Reverts `LV_USE_CUSTOM_MALLOC`/`ALWAYSINTERNAL`/`MAX_ACT`; gitignored master pinned `LV_MEM_SIZE=48` | **crash** | **high** | `sdkconfig.defaults` carries intent; verify regen + boot RAM% (54.2 vs 39.2) | Latent build-time landmine; no CI re-greps the four keys |
| 6 | SD read under WiFi fragmentation faults the sdspi bounce | int-dma | SD I/O (OTA, GPX/PCAP/memo, future /sd browser) while WiFi up | `sdspi` bounces unaligned FatFs targets internal; under fragmentation the bounce alloc faults | **crash** | **high** | OTA `nocsif_wifi_radio_yield(true)` + 64-byte-aligned `s_buf`; `OTA_WITH_SEQUENTIAL_WRITES` | Per-call/manual; §4.8 serves /sd *over* WiFi — cannot yield the transport radio |
| 7 | Audio I2S TX init NO_MEM — BLE owns the hole | int-dma | First sound after BLE controller resident | I2S TX needs ~5.7 KB contiguous; if BLE grabbed first, remaining hole < 5.7 KB → `ESP_ERR_NO_MEM` | non-init | medium | Signal-Hunt cue **deferred**; boot chime opens I2S early; PCM in PSRAM | Lost feature; §4.13 wants it back; reserving at boot costs 5.76 KB of the window |
| 8 | WiFi capture/monitor pressure under BT-on lean profile | int-dma | Monitor/PCAP/handshake capture with BT ON (lean) | Lean shrinks to rx=2/tx=2/cache=4, AMPDU off; tiny buffers drop frames; lazy 6144 writers add stack pressure | degraded | medium | Rings in PSRAM (O(1) rx path); lean = deliberate trade; workers lazy/reclaimed | Frame loss under sustained capture with BT on, real & unquantified |
| 9 | LVGL object-pool exhaustion on large live lists | psram | Big live list, or several live screens stacked | Pre-PSRAM a 10-row list ≈ 88% of the 96 KB pool; LVGL **hangs** in `lv_draw_label` on alloc fail, not error | crash | medium | `LV_USE_CUSTOM_MALLOC`→PSRAM; fixed row pool updated **in place**; nav bounded | PSRAM large not infinite; still hangs; discipline-dependent (§4.2 fusion) |
| 10 | Summed FreeRTOS task-stack pressure manufactures fragmentation | int-dram | Many workers resident at once (~113 KB stacks) | Stacks can't go to PSRAM for DMA/ISR tasks; spawn/die at different offsets → fragments the unified SRAM | degraded | medium | Lazy-worker strategy; LoRa gated 12288; stacks sized to minimum (6144) | Every new worker narrows the window; no aggregate budget |
| 11 | TinyUSB gadget install adds int-DMA pressure (DMA-hang class) | int-dma | Installing composite gadget (CDC+MSC+HID PHY + FAT mount) | USB-OTG endpoint/FIFO DMA + FAT mount claim internal-DMA; main.c flags boot-install as memory pressure | crash | medium | Default DETACHED; PHY installed only on request; safe-mode forces DETACHED; stage band resolves the specific hang | USB gadget + WiFi/BLE uncharacterized; §4.9/§4.3 never validated |
| 12 | LoRa 8 KB contiguous task stack refused under fragmentation | int-dma | `nocsif_lora_init` when largest int-DMA < 12288 | `lora_task` 8192 stack must be one run; RadioLib + survey locals need full 8 KB | non-init | medium | Gate `LORA_TASK_MIN_DMA=12288`; retry loop (transient); deinit hands 8 KB back; solo design | §4.5 wants LoRa "alongside WiFi"; retry can spin under steady-state fragmentation |
| 13 | Runtime BLE re-init impossible once WiFi has fragmented | int-dma | Freeing the controller then re-claiming while WiFi up | Post-WiFi hole permanently < 31744 gate; `nimble_port_init` can never re-satisfy the run | non-init | medium | `ble_quiesce` **never frees** the controller at runtime | Contradicts §4.6 Governor "BLE off after 5 min"; ~31.7 KB + 6 KB permanent even when "idle" |

### Critical & high conflicts — prose

**#1 — BLE controller starvation (critical).** This is the axis the architecture turns on. The controller needs a single contiguous ≥31.7 KB run of `INTERNAL|DMA`. The only reliable moment to get it is at boot, before WiFi's ~30 KB of static/cache-TX buffers carve the pool — hence `nocsif_ble_boot_reserve` runs in `app_main` while the heap is one unbroken run, and `ble_quiesce` holds the block resident forever so it never has to be re-won. The gate `BLE_MIN_DMA_BLOCK=31744` is empirical, sitting only ~4 KB above the 27,648 B failure point; a config or IDF change that adds one boot-permanent int-DMA consumer, or bumps `MAX_ACT`, can silently push the post-WiFi cap below the gate and make BLE non-startable in a clean build.

**#2 — Runtime radio cycling (critical).** Fragmentation is *made*, not found: bring-up and teardown at different heap offsets leaves total-free healthy while the largest hole collapses toward ~384 B. The current design's entire answer is to refuse to cycle — Signal Hunt's radio toggle changes monitor *activity* only, never lifecycle. That works, but it means any future feature needing sustained BLE+WiFi+LoRa concurrency has no path, and a gate that passes one moment can refuse the next.

**#3 — Display flush bounce (high, resolved).** Before PR #95, each PSRAM-resident RGB888 flush band was bounced through a per-flush ~13 KB internal buffer; when the hole was already eaten (USB PHY + mounted SD), the flush wedged into a hard UI hang. The fix — a persistent 29.5 KB two-band stage allocated once at boot — makes runtime flushes need zero int-DMA. The cost is that those 29.5 KB are now a permanent competitor for the same pool the radios need; the band is deliberately kept at 12 lines so the BT block + WiFi statics still fit, and enlarging it for smoother scroll directly steals from the radio window.

**#4 — NimBLE host-stack overflow (high).** An ANCS notify callback ran a deep synchronous chain (notify-RX → parse → `ble_gattc_write_flat`) on the host task stack; a 256 B RX buffer plus the write frame overflowed the 4096 B stack and corrupted `ble_hs_timer`. Fixed by raising to 6144 and moving RX/parse scratch off the stack into static BSS. This is empirical headroom, not a proven bound — the §4.4 NUS bridge / notify streams / mesh relay can overflow 6144 again, and each raise is a permanent internal-DRAM subtraction.

**#5 — Stale sdkconfig (high).** The generated `sdkconfig.nocsif-twatch-ultra` (Aug 20) disagrees with `sdkconfig.defaults` (Aug 31) on three RAM-critical keys. A build from the stale cache would put the 96 KB LVGL pool back in internal `.bss` (breaking coexistence) and force ≤16 KB allocs internal (draining int-DMA); a separate gitignored master pinned `LV_MEM_SIZE=48`, halving the pool → WDT reboots. The symptom (BLE Malloc-failed, heavy-app reboots) looks like a runtime bug, and no CI check re-greps the keys or asserts boot RAM% (54.2 correct vs 39.2 broken).

**#6 — SD-under-WiFi bounce fault (high).** `sdspi`/`spi_master` bounces non-DMA/unaligned FatFs targets through internal-DMA; under WiFi fragmentation the bounce alloc fails and the transfer faults. OTA dodges it by yielding the radio (`nocsif_wifi_radio_yield(true)`) and using a 64-byte-aligned buffer — but §4.8's /sd web browser reads SD *while serving the file over WiFi*, so yielding the radio would drop the connection delivering the file. The OTA workaround does not transfer cleanly.

---

## What will break next

13 predictions, grouped by plan area. Each names its early-warning signal so it can be caught on the largest-free-block heartbeat before it fails in the field.

### §4.2 Presence / detection (dual-radio, sustained)

| Scenario | Region | Why | Likelihood | Early warning |
|---|---|---|---|---|
| **Camera-glasses detector** (Ray-Ban Meta) runs WiFi monitor + BLE scan concurrently and *sustained* (not toggled) | int-dma | Both radios truly live at once — holds BLE's 31.7 KB + WiFi lean statics simultaneously, driving largest-free toward ~384 B while a hop timer + new list pool run | **high** | Heartbeat persistently < ~4 KB with both radios on; adv-report flow-ctrl spikes coinciding with monitor rx bursts |
| **Flock mode** runs background dual-radio scan even screen-off, + GNSS stamping each hit | int-dma | Adds GNSS UART RX ring (2 KB) + always-on duty on the already-tight window; background hides a gate refusing a radio | **high** | A radio silently failing to arm in background (no hits, no crash); GNSS RX ring alloc failing after WiFi up |
| **Cross-radio presence census / corroborator** fuses WiFi+BLE lists into one bigger live list | psram | Both radios up + a merged/deduped list larger than any single-radio list; BSS row-pointer pools grow, PSRAM object peak rises toward the LVGL-hang mode | **high** | `s_lv_peak` climbing past prior submenu bounds; a fused list rebuilt on a timer instead of in place |

### §4.6 Connectivity Governor

| Scenario | Region | Why | Likelihood | Early warning |
|---|---|---|---|---|
| **P1 lease/idle-timer** powers WiFi (and nominally BLE) OFF after ~5 min, ON on geofence-enter — runtime deinit/re-init | int-dma | Runtime radio lifecycle churn is exactly the fragmentation pattern the architecture avoids; re-init into a fragmented pool can no longer meet the BLE gate | **high** | Largest-free trending down across successive wake/sleep cycles; WiFi re-init NO_MEM or subsequent BLE gate refusal |
| **P2 cyclic GPS** keeps GNSS perma-on with an N-second fix-then-sleep duty | int-dma | GNSS RX ring is int-DMA + boot-permanent once installed — a fixed subtraction during every WiFi/BLE wake; indoors-no-fix burns current | **high** | GNSS RX ring install failing when GPS first wakes after WiFi up; battery telemetry GPS-dominated with no fixes |

### §4.4 BLE expansion

| Scenario | Region | Why | Likelihood | Early warning |
|---|---|---|---|---|
| **Multi-connection + richer HID** (mouse/consumer/gamepad) + NUS bridge raise `MAX_CONNECTIONS` > 1 | int-dma | Each connection multiplies controller ACL buffers + `MAX_ACT` slots (828 B ea) inside the fixed ~31.7 KB block; must grow past the gate | medium | Raising `MAX_ACT`/`MAX_CONNECTIONS` and the post-WiFi hole no longer clearing 31744 |
| **BLE mesh node + Coded-PHY** (LE Long Range) add relay buffers + longer FEC packets | int-dma | Mesh flooding + S=2/S=8 coded PHY raise controller LL RAM inside the one block already ~4 KB above its floor | medium | Enabling mesh/coded-PHY Kconfig raising footprint past the reserve; Malloc-failed on a previously-passing build |

### §4.7 Acoustic

| Scenario | Region | Why | Likelihood | Early warning |
|---|---|---|---|---|
| **data-over-sound** reconfigures PDM mic to ~40-48 kHz; **live spectrum analyzer** runs continuous FFT | int-dma | Near-3× sample rate enlarges I2S RX DMA (int-DMA); FFT wants internal RAM for speed; mic live while BLE/WiFi carry the paired transfer | medium | `mic_open` NO_MEM at higher rate with a radio up; FFT buffer forced internal |

### §4.8 / §4.8a Companion

| Scenario | Region | Why | Likelihood | Early warning |
|---|---|---|---|---|
| **Companion control surface** keeps WiFi hot: HTTP server + WebSocket push + screen-mirror + (relay) TLS | int-dram | httpd 8192 stack + control blocks + LWIP listen/accept whose pbuf headers stay INTERNAL; WS + thumbnails + mbedTLS scratch keep internal DRAM + LWIP hot | medium | Internal-DRAM free dropping + LWIP pbuf-header pressure with a session open; /sd browser SD reads faulting the bounce mid-session |

### §4.3 / §4.9 Links

| Scenario | Region | Why | Likelihood | Early warning |
|---|---|---|---|---|
| **ESP-NOW** alongside associated STA; **USB-Ethernet** (CDC-ECM/RNDIS) runs USB DMA + LWIP | int-dma | ESP-NOW shares WiFi MAC int-DMA + peer tables + can force radio on; CDC-ECM runs TinyUSB DMA + an LWIP netif — never validated against the ~31 KB window | medium | ESP-NOW init contending with STA buffers under lean; USB-Ethernet enumeration coinciding with a flush/bring-up and hanging |
| **CC tiles** wired to start/stop the *real* WiFi/BLE radios (§4.13) | int-dma | Making the tiles drive the subsystem exposes the runtime BLE bring-up path — a one-tap BLE ON after WiFi is up is the fragmented-pool Malloc-failed/int-WDT scenario | **high** | A CC BLE-tile tap after WiFi is up returning the gate refusal (tile fails to light) or, if ungated, the int-WDT panic |
| **SSH/telnet net clients + host discovery/port scan** bring mbedTLS + scan working sets onto a joined network | int-dram | SSH is heavy even on ESP32 — mbedTLS handshake buffers + key material + scan state + large transient client-task stacks fragment the pool | low | mbedTLS alloc failures during handshake under WiFi load; client task stacks failing to allocate contiguously |

### §4.5 LoRa gateway

| Scenario | Region | Why | Likelihood | Early warning |
|---|---|---|---|---|
| **LoRa-MQTT gateway + cross-band bridge** run LoRa concurrently with WiFi (and BLE for phone-typed source), sustained | int-dma | §4.5 says the gateway "runs alongside WiFi", but the LoRa 8192 contiguous stack is gated at 12288 and designed to run solo with BLE off; tri-radio + MQTT TLS/LWIP has no guaranteed contiguous budget | low | LoRa init retry loop failing to converge while WiFi associated; MQTT TLS buffers contending with the LoRa stack alloc |

---

## The coexistence plan

The heart of the document. Twelve strategies in four bands. The implementation vehicles are **PLAN §4.6 Connectivity Governor** (the runtime enforcement core) and the **`coex.h` token home named in ORGANIZATION.md** (one place where the DMA caps, the `nocsif_int_dma_largest()` reader, and the per-radio threshold table live). Each gotcha referenced is recorded in LESSONS.md.

### 1 — Coexist by construction

The default posture: arrange allocation so the scarce hole is never contended in the first place.

| Strategy | Approach | Applies to | Tradeoff |
|---|---|---|---|
| **Claim the BLE controller's ~31.7 KB block in `app_main` BEFORE `esp_wifi_init`**, while the heap is one unbroken run, and hold it resident for the BT-master session | `reserve-at-boot` | BLE controller vs WiFi statics + flush band — the load-bearing ordering rule | Controller can never be freed/re-claimed at runtime; BLE is a permanent ~31.7 KB + 6 KB cost even when "idle" — §4.6 cannot truly power BLE down/up without a reboot |
| **Route everything that does NOT need DMA/contiguity to PSRAM**: LVGL objects + RGB888 draw buffers, NimBLE host heap + mbufs, WiFi dynamic-RX + LWIP, capture rings, bulk PCM, per-screen canvases | `relocate-to-psram` | The whole system's non-DMA allocations — the single biggest lever (frees ~96 KB internal alone) | PSRAM random access slower (~8-9 fps scroll cap from draw-buffer bandwidth); QSPI PSRAM can't hold SPI-DMA — cache-TX, I2S DMA, controller, stacks, stage band MUST stay internal |
| **Keep the persistent 29.5 KB flush stage band small (12 lines) and boot-permanent** so runtime flushes need ZERO int-DMA | `reserve-at-boot` | Display flush vs BLE controller + WiFi statics | A fixed 29.5 KB int-DMA tax that permanently narrows the radio window; enlarging for smoother scroll / higher color depth steals from the hole |
| **One shared arbiter (`coex.h`)** that measures the same quantity everywhere — `NOCSIF_DMA_CAPS` + `nocsif_int_dma_largest()` + a per-radio threshold table (BLE 31744, LoRa 12288, floor 24576) — and gate every bring-up through it; make it the enforcement core of the §4.6 Governor | `budget-gate` | Every int-DMA-claiming bring-up: BLE, LoRa, WiFi re-init, audio I2S, USB gadget, SD-under-WiFi | Gating converts crashes into **refusals** ("restart the watch to finish enabling it"); thresholds empirical (~4 KB margin, no `.map`), must be re-verified on every IDF/config change |
| **Bring the audio I2S TX channel up (or reserve its ~5.7 KB) at boot, BEFORE the hunt's BLE bring-up**, and gate the cue on successful audio-init with clean degradation | `reserve-at-boot` | Signal-Hunt cue, radio-event alerter, any audio-in-a-BLE-context feature | Reserving permanently latches 5.76 KB of the window (never `i2s_del_channel`'d); the alternative (gate-and-degrade) keeps the window but silently drops the cue when BLE won the race |

### 2 — Graceful time-sharing (turn X off to let Y work)

The honest "cannot coexist" pairs, documented rather than faked.

| Strategy | Approach | Applies to | Tradeoff |
|---|---|---|---|
| **Reframe the Governor's per-radio leases as ACTIVITY gates over resident radios, not lifecycle power-cycling**: hold the BLE controller resident + connectable-advert, put WiFi into modem-sleep (stay associated ~1-2 mA) instead of `esp_wifi_deinit`, let the connection itself be the "I'm here" signal | `coexist` | Governor WiFi/BLE power management vs the fragmentation-on-re-init wall | Modem-sleep is ~1-2 mA not zero and the resident controller keeps its ~31.7 KB, so savings are smaller than true radio-off; full-off is only safe once GPS confirms you've *left* the area (a defrag-by-reboot boundary), not on a simple idle timer |
| **Time-share the truly-incompatible cases**: LoRa runs solo (BLE off, deinit hands ~8 KB back); BLE-HID stays `MAX_CONNECTIONS=1` mutually exclusive with phone+WiFi; a subsystem that must re-init into a fragmented pool triggers a guided restart ("defrag by reboot") | `time-share` | LoRa vs BLE; BLE-HID vs phone+WiFi; any runtime re-init blocked by fragmentation | User-visible interruption — turning a radio off, losing a connection, or rebooting to finish enabling a feature; honest "cannot coexist" pairs cap the tri-radio ambitions of §4.5/§4.2 |
| **For SD-under-active-WiFi paths** (OTA, §4.8 /sd browser): 64-byte-aligned DMA-capable staging + `nocsif_wifi_radio_yield(true)` where the radio can be spared; where it must stay up (serving the file over WiFi), **pre-stage the read into PSRAM and drain without a per-transfer internal bounce** | `coexist` | `sdspi`/`spi_master` bounce vs WiFi int-DMA fragmentation | Yielding is impossible when WiFi *is* the transport; those paths need a bounce-free aligned/pre-staged design; alignment is per-call and easy to forget; pre-staging costs PSRAM + a copy |

### 3 — Shrink & pool

| Strategy | Approach | Applies to | Tradeoff |
|---|---|---|---|
| **Keep the WiFi lean profile** (`static_rx=2`/`static_tx=2`/`cache_tx=4`, AMPDU off) as a runtime-selectable knob armed before WiFi bring-up whenever Bluetooth is on; full profile when WiFi runs alone | `shrink` | WiFi vs BLE coexistence; capture throughput | Frees ~18-23 KB int-DMA to fit the BLE block while the STA stays associated, but costs throughput and drops frames under sustained high-rate capture with BT on |
| **Shrink and pool the large lists**: fixed row pools updated IN PLACE (never clean+recreate on a timer), bounded nav depth, per-feature caps on fused/census lists; give each new int-DMA feature an explicit per-feature budget entry in the `coex.h` table | `shrink` | WiFi/BLE live lists, the §4.2 cross-radio census, any big live view | Fewer rows at once + more discipline; LVGL still **hangs** (not degrades) on pool exhaustion, so the bound must be conservative; per-feature budgets are estimates without a `.map` |
| **Make boot-permanent internal BSS tables that are reserved even when unused** — GNSS `s_wd_seen` (3 KB), LoRa `s_inbox` (3.5 KB), WiFi identity/monitor tables (~17 KB) — lazy or PSRAM-backed to widen the contiguous window | `relocate-to-psram` | Idle internal-DRAM/BSS that fragments the pool without being used | Adds lazy-init complexity + per-access indirection; some (identity/EAPOL tables touched from the rx path) may need to stay internal for latency — only genuinely cold tables can move |

### 4 — Observability

| Strategy | Approach | Applies to | Tradeoff |
|---|---|---|---|
| **Publish an int-DMA largest-free-block heartbeat** (main.c gauge + `coex.h` `nocsif_log_dma_free`) as continuous telemetry, and add a **build-time guard** that re-greps the four load-bearing sdkconfig keys (`LV_USE_CUSTOM_MALLOC`, `SPIRAM_MALLOC_ALWAYSINTERNAL`, `BT_CTRL_BLE_MAX_ACT`, `RX_BA_WIN`) and asserts boot RAM% (54.2 vs 39.2) | `budget-gate` | Regression detection across all int-DMA consumers + the stale-sdkconfig landmine | Observability only — catches a shrinking window or a reverted key early but frees no memory; requires discipline to watch the heartbeat + wire the CI check; heartbeat readings from different operating states aren't directly comparable |

---

## Pieces designed around RAM — and the remake plan

A cross-cutting audit found **76 distinct places** in the firmware that exist *as an accommodation to the RAM constraints* — code or config that would not exist, or would be simpler, if internal DMA-capable RAM were unlimited. Consolidated, they are **18 pieces to remake** and **16 that are already correct and should stay**. The whole set converges on one architecture:

> **The coexistence problem is an internal-DMA CONTIGUITY + ALLOCATION-ORDER problem, not a free-bytes problem, and runtime teardown never defragments** (measured: largest pins at 15,872 WiFi-up / 21,504 WiFi-released / 9,216 airplane and never returns to the pristine 90,112). The winning design is a single **Connectivity Governor (PLAN §4.6)** on top of the **`coex.h`** token/threshold home (ORGANIZATION.md) that (a) **reserves** the BLE controller's ~31,744 B block at boot into the pristine pool and **never releases it**; (b) treats **boot init order** as explicit, guarded policy; (c) is the **one place** that measures int-DMA and holds every threshold + the reserve/lease state; (d) keeps **PSRAM the default** for everything that does not literally DMA out of its buffer — *including task stacks*; and (e) where coexistence is genuinely impossible, makes the mutual-exclusion **explicit and honest** (a real "reboot to reclaim the radio, or keep BLE and degrade WiFi" choice) instead of a silent `REFUSED` or a dead "turn Bluetooth off" wall.

### Remake catalog — the 18 pieces

> **Status (Phase 0 + Phase 1 landed, build-verified):** **#1** coex.h arbiter, **#2** BLE boot-reserve,
> **#3** guarded init-order (loud assert), **#4** controller kept resident, **#5** dead radio-yield/make-room
> deleted, **#6** REFUSED/"restart" paths removed, **#8** LoRa PSRAM stack, and **#14** the CI sdkconfig
> guard are **DONE**.
>
> **Status (Phase 2 landed, on-device-verified over COM7):** **#7** worker stacks -> PSRAM is now COMPLETE
> (audio/mic/ducky moved; OTA + ble worker + NimBLE-host correctly kept internal — on-task NVS/flash);
> **#10** HID mode without a NimBLE teardown (permanent boot GATT, keyboard = advert swap; the last
> runtime teardown is gone); **#12** Signal-Hunt audio cue restored via I2S TX boot-reserve (honest
> tx_ready gate); **#13** NimBLE msys pool 12 -> 24; and **#6b** the §4.13 CC BLE/WiFi tiles now drive +
> reflect the live radios through a shared `radio_state.{h,c}` accessor. Each was proven at steady-state
> fragmentation with a compile-gated `NOCSIF_*_SELFTEST` heartbeat hook. Remaining (#9, #11, #15–#18) are
> Phase 3+. Every landed change compiles clean AND passed its COM7 self-test before merge.

| # | Piece | Approach | Pri | Effort |
|---|---|---|---|---|
| 1 | Per-radio int-DMA gates → **one `coex.h` arbiter under the §4.6 Governor** | central-arbiter | **high** | large |
| 2 | BLE controller **boot-reserve** of the ~31.7 KB block | reserve-at-boot | **high** | med |
| 3 | Boot **init-order** as explicit, guarded policy (not a bare call sequence) | reorder-init | **high** | small |
| 4 | BLE controller kept **resident** at runtime (quiesce, never teardown) | never-release-block | **high** | small |
| 5 | **Retire** the runtime WiFi "make-room / lean-for-BLE" + **delete dead radio-yield** code | rework | **high** | med |
| 6 | `BLE REFUSED` / "turn WiFi off" → **honest reboot-or-degrade** choice | explicit-exclusion | **high** | small |
| 7 | Worker task stacks (audio/mic/LoRa/PCAP/Ducky/NimBLE-host) → **PSRAM** | psram-relocate | **high** | med |
| 8 | LoRa lifecycle → **PSRAM stack** removes the `LORA_TASK_MIN_DMA` gate + "turn Bluetooth off" wall | psram-relocate | **high** | med |
| 9 | WiFi lean/full profile → **boot-time governor session mode** ("coexist" vs "capture") | explicit-exclusion | med | med |
| 10 | HID mode `nimble_teardown/re-init` → **permanent multi-service GATT gated by advertising** | rework | med | med |
| 11 | `MAX_CONNECTIONS=1` teardown-on-entry → **governor BLE-role lease** | explicit-exclusion | med | med |
| 12 | Signal-Hunt **audio cue** → reserve I2S TX descriptors at boot (or gate honestly) | reserve-at-boot | med | med |
| 13 | NimBLE **msys pool 12 → 24-32** (it lives in PSRAM — near-free) | shrink-pool | med | small |
| 14 | Consolidate int-DMA gates/logs onto `coex.h` + **CI guard** for silent sdkconfig reverts | instrument | med | med |
| 15 | Display 29.5 KB stage band → investigate **PSRAM-DMA panel-io** to shrink/eliminate it | psram-relocate | low | large |
| 16 | `BT_CTRL_BLE_MAX_ACT=3` — keep, **fix stale comment**, document the 3×828 B slot budget | keep | low | small |
| 17 | LVGL fixed-row pool caps (32) — **reframe as a UX knob**, not a RAM wall | keep | low | small |
| 18 | Voice-memo 30 s PSRAM cap — **product cap** that can be raised | keep | low | small |

### High-priority remakes — detail

**1 · One int-DMA arbiter (`coex.h`) under the §4.6 Governor.** *Today:* the thresholds (`BLE_MIN_DMA_BLOCK=31744`, `LORA_TASK_MIN_DMA=12288`, floor `24576`) are copy-pasted across `ble.c`/`wifi.c`/`ui.c`, and `heap_caps_get_largest_free_block(INTERNAL|DMA)` is called ad-hoc at every lifecycle edge with no owner of the reserve/mutual-exclusion policy. *Remake:* build the arbiter exactly as ORGANIZATION.md's `coex.h` (`NOCSIF_DMA_CAPS`, `nocsif_int_dma_largest()`, `nocsif_log_dma_free()`, the per-radio threshold table) **under** §4.6's lease + idle-timer; it owns the boot reserve token, the init-order policy, the boot-time lean/full decision, and every honest reboot-or-degrade choice. This is the umbrella every other item plugs into. *(ble.c:1546, ui.c:6909, wifi.c)*

**2 · Keep the BLE boot-reserve — it's the only guaranteed contiguity.** `nocsif_ble_boot_reserve()` brings NimBLE up into the pristine ~90 KB pool before WiFi. *Measured:* only that pristine pool yields a ≥31,744 run (after WiFi, largest is 30,720 and falling). Keep it; promote it from an implicit call-order convention to an explicit governor boot phase that emits the reserve token, and move `31744` to `coex.h NOCSIF_RADIO_MIN_DMA_BLE`. *(ble.c:3154, main.c:181)*

**3 · Make boot init-order a guarded policy.** Coexistence silently depends on `main.c` calling the BLE reserve before any WiFi-touching init. *Remake:* express the contiguity-critical claim order (BLE block → display 29,520 B stage band → I2S TX descriptors → *then* WiFi) as an ordered list the governor drives, with a **boot-time assert/panic if any WiFi init runs while the reserve is still pending**, plus a one-line log of the claim order and the resulting `largest` after each claimer. Converts a brittle convention into a checked policy. *(main.c:181-213)*

**4 · Keep the BLE controller resident (never teardown at runtime).** Leaving a BLE screen only *quiesces* (stop recon, re-assert the phone advert); it never `nimble_teardown`s. *Measured:* post-cycle largest never returns to 90,112, so a released block can't be reclaimed. Formalize as a governor **invariant**: the reserve token is released only by BT-master-OFF or safe-mode, never by navigation or radio switching. The one honest cost — WiFi stays permanently lean while BT is on (~35-40 KB vs ~58 KB) — should be surfaced, not hidden. *(ble.c:1883)*

**5 · Retire the runtime "make-room for BLE" and delete the dead radio-yield machinery.** *Today:* `ble_make_room()` flips WiFi to lean and polls ~8 s hoping the 31 KB hole forms; `do_radio_yield_on/off` fully deinits WiFi to hand ~28 KB to BLE; a `s_want_enable` "reclaim radio from phone" handshake. *Measured, this cannot work:* even fully released, WiFi only reaches **21,504 < 31,744**, and the header itself already notes these paths are "never triggered" since LVGL→PSRAM. *Remake:* **delete** the room-maker + yield + deferred-enable machinery; the 8 s blocking poll + STA drop buys nothing because the hole never forms at runtime. lean/full is a boot-time input, not a runtime re-init. Removes a fragile cross-module handshake that contradicts the resident-block model. *(ble.c:1552, wifi.c:971-1107)*

**6 · Turn `BLE REFUSED` / "turn WiFi off" into an honest choice.** Keep the gate (a failed `esp_bt_controller_init` blows the int-WDT — the refusal is real crash-prevention), but replace the silent `REFUSED` log + passive "restart the watch" hint with the governor's explicit two-option UX: **"Reboot now to enable Bluetooth (reclaims the radio block)"** or **"Save for next boot"** (`s_bt_needs_restart` already persists intent). With BLE reserved-at-boot, this path should rarely fire in normal use. *(ble.c:1579, 2232)*

**7 · Move non-DMA worker stacks to PSRAM.** audio 6144, mic 6144, BLE-PCAP 8192, Ducky 6144, LoRa 8192, NimBLE-host 6144 are all allocated from the *contended internal pool*, so a first-use spawn can fail under fragmentation (steady-state largest ~3,072 can't hold an 8 KB stack). *Remake:* allocate these stacks from PSRAM (`xTaskCreateWithCaps` + `SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY`) — a stack only needs to be DMA-capable if the task DMAs from on-stack buffers, and none of these do (SD/I2S/SPI use their own driver buffers). Returns ~34 KB of internal-DMA to the reserve budget **and** kills the "spawn fails under fragmentation" class. **⚠ Caveat: keep the OTA flash-writing worker internal** — a PSRAM stack is unusable while the flash cache is disabled during erase/write. Keep lazy spawn; only change the stack's memory source. *(audio.c:311, mic.c:68, wifi.c:2203)*

**8 · LoRa on a PSRAM stack — delete the gate and the "turn Bluetooth off" wall.** LoRa's 8 KB worker is gated on `largest ≥ 12288`; with BLE + the flush band resident, largest idles ~13 KB and **collapses to ~928 B during a phone transfer**, so the spawn is refused and Signal-Hunt shows "LoRa needs memory". *Remake:* move the LoRa stack to PSRAM (it talks to the SX1262 over SPI3 with its own DMA buffers, not the stack) — this **eliminates `LORA_TASK_MIN_DMA`, the "turn Bluetooth off to hunt LoRa" wall, and the timing-dependence** entirely. With the stack no longer contended, keep the worker *warm* instead of churning it per hunt switch, and drop the ~3 s busy-wait + 60 ms magic-settle deinit (its rationale — hand int-DMA back to WiFi — evaporates). *(ui.c:6909, lora.cpp:902)*

### Medium-priority remakes

- **9 · WiFi lean/full → two boot session modes.** The driver's int-DMA footprint is immutable after `esp_wifi_init` and does not shrink when idle; a runtime swap re-fragments the pool it just freed. Reframe as **"coexist"** (lean + BLE reserved, default) vs **"capture"** (full WiFi, BLE reserve skipped), chosen at boot; heavy-capture screens declare they need "capture" and the governor offers an honest reboot. *(wifi.c:895)*
- **10 · HID mode without a NimBLE teardown.** Entering/leaving HID currently does `nimble_teardown` + `bring_up` synchronously to swap the GATT table — a runtime re-claim of the reserved block that can strand BLE. *Remake:* register the HID/DIS/Battery service **permanently at boot** (host tables live in PSRAM, nearly free internally) and gate HID by which service is *advertised*, not by re-initializing NimBLE. Removes the only runtime BLE teardown path; non-HID behavior is byte-identical. *(ble.c:1908, 2596)*
- **11 · `MAX_CONNECTIONS=1` → a governor BLE-role lease.** Keep the `=1` default (the reserved block is sized for it), but replace the scattered teardown-on-entry with one "role lease" (phone | HID | scan) that drops the current holder cleanly and says so ("keyboard mode drops your phone link"). More connections would be a *boot* decision (enlarges the reserved block), never runtime. *(ble.c:2596, sdkconfig.defaults:229)*
- **12 · Restore the Signal-Hunt audio cue by reserving I2S TX at boot.** The lazy `i2s_new_channel` loses the int-DMA race because BLE reserves first, so the cue is silently dead though the toggle reads "on". Make audio a boot-reserve claimer (descriptors are a few hundred B–~2 KB, far cheaper than BLE) after BLE and before WiFi; if it still can't be honored, gate the toggle honestly instead of showing a dead "audio on". *(ui.c:7003, audio.c:89)*
- **13 · Raise NimBLE msys pool 12 → 24-32.** It exhausts under the ANCS/AMS discovery burst (`GATTC proc alloc failed`, why AMS is serialized after ANCS) — but it lives in **PSRAM**, so the cut was a false economy. Raising it is a near-free reliability win with zero internal-DMA impact. *(sdkconfig.defaults:253)*
- **14 · Consolidate gates/logs onto `coex.h` + a CI guard.** Route every largest-block call site through `nocsif_int_dma_largest()`, and turn the "verify post-build sdkconfig has X" human contract into a **build-time check** that fails if any load-bearing symbol drifts (`RX_BA_WIN`, `MAX_ACT`, `STATIC_RX`, `LV_USE_CUSTOM_MALLOC`, the SPIRAM knobs). *(main.c:458, ble.c:1585, wifi.c:915)*

### Low-priority remakes

- **15 · Panel-io root-fix of the 29.5 KB stage band — ✅ DONE (Phase A1, 2026-09-07), with a correction.** A NocSif panel-io (`display_io.{c,h}`) streams a whole flush through ONE write window and never allocates internal DMA; that let the stage shrink to **2 × 4 lines = 9,840 B** (−19,680 B) at zero panel-time cost (flush still ~29.5 ms/full frame, memcpy-bound). **PSRAM-direct DMA (no stage) was measured DEAD at 80 MHz pclk** — 4/19 chunks underran on the boot clear (quad PSRAM ≈ 30 MB/s vs the panel's 40 MB/s) — so the band stays; see `docs/DMA-COEXISTENCE-VERIFICATION.md` "Phase A1 addendum". Measured: steady-state largest 2,048 → 18,432; the USB File-Share 8 KB entry proxy passes with GNSS + LoRa up. *(display_io.c; ui.c nocsif_flush_cb)*
- **16 · `BT_CTRL_BLE_MAX_ACT=3` — keep + document.** Fix the stale in-file "MAX_ACT=2" comment and record the 3×828 B slot budget in `coex.h` next to the block-size math so the reserve size and `MAX_ACT` stay coupled. No behavior change. *(sdkconfig.defaults:213, 256)*
- **17 · LVGL row-pool cap (32) is now a UX knob.** Since LVGL objects moved to PSRAM (2% frag), the 32-row cap no longer costs internal RAM — keep the fixed in-place pool (correct WDT-thrash fix) but document the cap as a raiseable paging/UX choice, not a RAM ceiling. *(ui.c:2881)*
- **18 · Voice-memo 30 s cap is a product cap.** ~960 KB in PSRAM (~15% of free external RAM), no internal-DMA implication — raise to 60-120 s or make it a setting whenever longer memos are wanted. *(mic.h:79)*

### Correct as-is — keep (already the right accommodation)

These 16 are load-bearing and should **not** be changed (only re-homed onto `coex.h` where noted):

- **Persistent flush stage band** (now 2×4,920 B after Phase A1's window-once streaming IO; was 2×14,760 B) — makes flush need zero runtime int-DMA; survives largest ~3,072. The root DMA-hang fix. PSRAM-direct (no band) underruns at 80 MHz — measured, don't re-propose at this pclk.
- **LVGL heap → PSRAM** (`LV_USE_CUSTOM_MALLOC`) — the single biggest win (~96 KB internal freed); what let BLE+WiFi+IMU coexist.
- **`SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`** — keeps WiFi/LWIP dynamic buffers out of the contended pool (without it `esp_wifi_init` drove free to ~471 B).
- **NimBLE host → PSRAM** (`MEM_ALLOC_MODE_EXTERNAL`) — controller stays internal, host structs/mbufs go external, keeping the reserved block small.
- **`SPIRAM_MALLOC_ALWAYSINTERNAL=4096`** — pushes bulk allocations to idle PSRAM, reserves internal-DMA for tagged needs.
- **`ESP_COEX_SW_COEXIST_ENABLE=y`** — the enabler that lets both stacks time-share the one 2.4 GHz radio.
- **NimBLE over Bluedroid** — lightest host; S3 is BLE-only anyway.
- **`WiFi IRAM_OPT=n` / `RX_IRAM_OPT=n`** — returns ~27 KB of would-be IRAM to the DMA-capable heap; matches IDF's own default under BT+SPIRAM.
- **Full-frame framebuffer + orrery/wallpaper + mask canvases → PSRAM** — CPU compositing sources, never DMA flush targets.
- **`BT_CTRL_SCAN_DUPL_CACHE_SIZE=20`** — scans want duplicates (RSSI/last-seen), so a big controller dup cache buys nothing.
- **`BT_CTRL_DTM_ENABLE=n`** — factory RF test mode, never used in-product.
- **`BLE_ADV_REPORT_FLOW_CTRL_NUM=50`** — halves the worst-case in-flight adv-report int-DMA queue that collided with the flush bounce.
- **Reliability safe-mode + UI-liveness Task-WDT** — safe mode skips exactly the int-DMA-heavy inits after a crash streak; the liveness WDT turns the DMA-hang freeze into a recoverable reboot. Honest recovery, not a pretend fix.
- **FatFs LFN work buffer on heap** — keeps the ~512 B buffer off the internal task stack.
- **Screens-freed-on-nav-back + fixed live-label registry** — bounded-LVGL discipline; keep even though PSRAM eased the original pressure.
- **Signal-Hunt "clean switch" monitor-toggle-only invariants** — the codified proof that all three teardown strategies are fatal and that runtime teardown never defragments. Keep the invariants; only route its heap logs through `coex.h`.

---

## Prioritized actions

Ordered. Each tag names the conflict (C#), prediction, or remake item (R#) the change closes.

**Do-now hardening**

- [x] **Wire the build-time sdkconfig guard** — CI re-greps `LV_USE_CUSTOM_MALLOC`, `SPIRAM_MALLOC_ALWAYSINTERNAL`, `BT_CTRL_BLE_MAX_ACT`, `RX_BA_WIN` after fullclean and asserts boot RAM% (54.2 vs 39.2). *Closes C5 (stale sdkconfig).* **DONE (Phase 0)** — compile-time `#error` guard in `firmware/src/coex_guard.c` + a boot-time coex-config echo in `main.c`.
- [x] **Assert boot-reserve ran before WiFi** — hard-fail (or log a loud diagnostic) if `nocsif_ble_boot_reserve` did not claim the block before `esp_wifi_init`, including on the safe-mode path. *Closes C1 residual (boot-order fragility).* **DONE (Phase 1)** — loud `ESP_LOGE` init-order guard in `wifi.c bring_up` via `nocsif_ble_boot_reserve_ran()`.
- [x] **Wire the §4.13 CC BLE/WiFi tiles to drive + reflect the live radios** through the shared `radio_state.{h,c}` accessor. *Closes prediction §4.13, C2.* **DONE (Phase 2)** — the BLE tile is a LOGICAL activity toggle over the RESIDENT controller (`nocsif_ble_bt_set_enabled`), so it needs no int-DMA gate and can never crash/refuse/restart (the "refuse below gate" the original action feared only applied to the old runtime-re-claim model, now gone); the WiFi tile toggles the STA (`nocsif_wifi_request_enable`); airplane captures/restores both; the tiles + status labels read one truth. Verified with `NOCSIF_RADIO_TILE_SELFTEST`.
- [ ] **Document the honest mutually-exclusive pairs in-firmware** (LoRa-solo, BLE-HID vs phone+WiFi, defrag-by-reboot) as user-facing copy, not silent gate refusals. *Closes C2/C13 UX, R6.*
- [x] **Delete the dead runtime radio-yield / "make-room for BLE" machinery** — measured proof it cannot work (WiFi-released largest 21,504 < 31,744 gate); the header already notes it is "never triggered". Removing it deletes a fragile cross-module handshake that contradicts the resident-block model. *Closes R5, C13 (dead code).* **DONE (Phase 1)** — deleted `ble_make_room`, `do_radio_yield_on/off`, `s_yielded`/`s_want_enable`/`s_yield_prev_enabled`, the deferred-enable path, and the public yield API; OTA reworked off it (relies on the aligned `s_buf`; see C6 note). BT-master-OFF is now a logical off that keeps the controller resident.
- [x] **Raise NimBLE `MSYS_1_BLOCK_COUNT` 12 → 24-32** — it lives in PSRAM (near-free), and fixes the `GATTC proc alloc failed` burst so AMS need not serialize after ANCS. *Closes R13, the msys live finding.* **DONE (Phase 2)** — raised to 24; on-device the `GATTC proc alloc failed` / `control-point write rc=6` pair is gone, steady-state int-dma largest unchanged (+16 B internal only).

**Near-term — the Connectivity Governor as the vehicle**

- [x] **Move non-DMA worker task stacks to PSRAM** (audio/mic/LoRa/Ducky; **keep OTA's flash-writing worker internal**) — eliminates the "first-use spawn fails under fragmentation" class (steady-state largest ~3 KB can't hold a 6 KB internal stack). *Closes R7, C10.* **DONE (Phase 2)** — LoRa (Phase 1) + audio/mic/ducky (Phase 2) now use `xTaskCreateWithCaps(MALLOC_CAP_SPIRAM)`. **The "BLE-PCAP" and "NimBLE-host" candidates were CORRECTLY EXCLUDED after investigation:** there is no separate BLE-PCAP worker (pcap drains inside the single `ble` worker, which runs synchronous `nvs_commit` on-task — a PSRAM stack is unusable while the flash cache is disabled), and the NimBLE host task is IDF-created via a plain `xTaskCreatePinnedToCore` with no WithCaps path (and also commits bond-store NVS on-task). On-device (`NOCSIF_PSRAM_STACK_SELFTEST`): mic+ducky spawned at `largest=4352` (a 6 KB internal stack could not fit), all three stacks confirmed external, boot `largest` rose 3200→5120.
- [x] **Put LoRa's worker stack in PSRAM** to delete the `LORA_TASK_MIN_DMA=12288` gate and the "turn Bluetooth off to hunt LoRa" wall, then keep the worker warm instead of churning it per hunt-switch. *Closes R8, C12, §4.5 concurrency.* **DONE (Phase 1)** — `xTaskCreateWithCaps(MALLOC_CAP_SPIRAM)` + `vTaskDeleteWithCaps`, `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y`; gate + wall + deinit busy-wait removed; worker kept warm across hunt-switches.

- [x] **Land `coex.h`** as the single token home: `NOCSIF_DMA_CAPS`, `nocsif_int_dma_largest()`, the per-radio threshold table (BLE 31744, LoRa 12288, floor 24576), and a per-feature budget registry. *Closes C1/C2/C7/C11/C12 (one arbiter).* **DONE (Phase 0)** — `firmware/src/coex.h`; all ad-hoc int-DMA gates/logs routed through it. (Per-feature budget registry: deferred.)
- [x] **Shrink the display flush stage 29.5 KB → 9.8 KB** via a window-once streaming panel-io (`display_io.{c,h}`) that also never allocates internal DMA on any display path. *Closes R15, C3 residual.* **DONE (Phase A1, PR pending)** — post-imu largest 3,456 → 24,576, steady 2,048 → 18,432, USB 8 KB entry proxy OK with GNSS + LoRa up, 0 underruns, flush time unchanged. PSRAM-direct DMA measured dead at 80 MHz (kept as an experiment flag only). Remaining tail erosion under heavy UI (largest → ~4 KB with ~11 KB free after 60 s) is the ALWAYSINTERNAL small-alloc mechanism → Phase A3.
- [x] **Move the remaining cache-off-safe worker stacks to PSRAM** — IMU (+ its FIFO work buffer), GNSS, buttons, usb_gadget, NFC. *Closes the rest of R7/C10 for the safe set; taskLVGL + weather stay internal (on-task NVS).* **DONE (Phase A2, PR pending)** — steady-state largest 18,432 → **30,720** (15× the pre-A1 2,048); the IMU/weather stage no longer touches the run; GNSS/LoRa spawns cost it nothing.
- [x] **Decode the IMU `fifo process err -3` before "fixing" it.** **DONE (Phase A2)** — it is a **BHI260 NACK** (`ESP_ERR_INVALID_STATE` from `i2c_master` with no timeout log ⇒ ACK_ERROR), ~1 s after WiFi `assoc -> run`, with 37 KB int-DMA free: **not a memory margin.** One retry after a tick in the IMU I2C wrapper; the §4.6 Governor precondition recorded in PR #172 is closed.
- [x] **Make the USB File-Share entry deterministic** — an 8 KB boot reserve (`NOCSIF_RADIO_MIN_DMA_USB`, coex.h) released right before the one-time `tinyusb_driver_install`; honest "needs memory"/"install failed" on the USB screen. *Closes C11, prediction §4.9.* **DONE (Phase A3, PR pending)** — the sibling worktree's `msc_shed_wifi` (runtime WiFi stop) was measured to recover zero contiguity and is not carried over.
- [x] **Move the lazy WiFi workers to PSRAM** (parser 3 K, PCAP writer 6 K, handshake export 6 K, portal DNS 4 K, with `vTaskDeleteWithCaps` pairs) — the transient internal claims that eroded the run under Signal Hunt / capture. *Closes the rest of C10 for the safe set.* **DONE (Phase A3).** Still internal by design: `wifi` command worker (on-task NVS), captive-portal + companion httpd (IDF-created, no caps path), OTA, ble, NimBLE-host, logbook, main, taskLVGL, weather.
- [ ] ~~Lower `SPIRAM_MALLOC_ALWAYSINTERNAL` 4096 → 1024~~ **REJECTED (Phase A3):** IDF keeps small allocations internal so FreeRTOS objects created via plain `malloc` never land in PSRAM and get touched from an ISR during a flash op; the measured erosion was task stacks (now moved), not small mallocs. Don't re-propose.
- [ ] **Build §4.6 P1 leases as activity gates, not lifecycle power-cycling** — WiFi modem-sleep + resident BLE controller + connection-as-presence; reserve `esp_wifi_deinit` for the GPS-confirmed "left the area" defrag-by-reboot boundary. *Closes prediction §4.6 P1, C13.*
- [ ] **Publish the largest-free-block heartbeat continuously** and add a regression alert when it trends down across wake/sleep cycles. *Closes predictions §4.2/§4.6 early-warning.*
- [ ] **Add a per-feature RAM budget entry requirement** — no new int-DMA feature merges without a `coex.h` table row and a measured cost. *Closes C10 (no aggregate budget).*

**Future-proofing**

- [x] **Reserve the audio I2S TX channel at boot** (after the BLE reserve, before `nocsif_ui_init`'s first `esp_wifi_init`) and restore the Signal-Hunt cue with a `tx_ready` gate-and-degrade fallback. *Closes C7, §4.13/§4.7.* **DONE (Phase 2)** — I2S DMA is internal-only (no PSRAM route), so allocation ORDER is the fix. On-device: claim order BLE(59392)→audio(53248)→flush→WiFi(30720); the `ESP_ERR_NO_MEM` is gone; the honest `nocsif_audio_tx_ready()` gate drives the cue toggle. ⚠ Cost: steady-state largest ~3200→~1728 B (the 5.7 KB reserve) — safe (>384 B floor, flush needs zero runtime int-DMA) but tightens the SD-bounce/USB margin.
- [ ] **Design a bounce-free SD-over-WiFi read path** (PSRAM pre-stage + aligned drain) for the §4.8 /sd browser, since the radio cannot be yielded when it is the transport. *Closes C6, §4.8.*
- [ ] **Move genuinely-cold internal BSS tables to lazy/PSRAM** (`s_wd_seen`, LoRa `s_inbox`, cold WiFi identity tables) to widen the contiguous window; keep rx-path-latency tables internal. *Closes C10, §4.2.*
- [ ] **Bound future NimBLE callback depth** — keep notify callbacks shallow (defer deep chains to a worker) rather than raising the host stack again. *Closes C4, §4.4.*
- [x] **Register the HID GATT service permanently at boot** (host tables in PSRAM) and gate HID by which service is *advertised*, eliminating the HID-mode `nimble_teardown`/re-init — the last runtime BLE teardown path. *Closes R10, C13.* **DONE (Phase 2)** — `gatt_server_register` (GAP+GATT+HID/DIS/Battery) runs once at boot; `do_hid_start`/`hid_teardown` are pure advert swaps; `nimble_teardown` deleted (zero runtime `nimble_port_stop/deinit` sites). On-device (`NOCSIF_HID_CYCLE_SELFTEST`): enter/leave keyboard gave `max|dlargest|=0` (a teardown swings ~31 KB), `needs_restart=0`, ANCS unaffected + the phone link re-connected on release. **Bluetooth is now fully reboot-free.**
- [ ] **Validate the never-tested combinations before shipping them** — USB-gadget + WiFi/BLE, USB-Ethernet + LWIP, tri-radio LoRa-MQTT — each against the ~31 KB window with the heartbeat. *Closes predictions §4.3/§4.5/§4.9.*

---

## Measurement & uncertainty

**Measured on-device**

- Largest `INTERNAL|DMA` free block across operating states: ~80 KB display-idle, ~13,312 B idle-after-load, ~384 B steady BLE+WiFi, ~27,648 B (full) / ~31,736 B (lean) WiFi-up cap (main.c heartbeat gauge).
- The BLE gate is empirical: **27,648 B fails "BLE_INIT: Malloc failed"** then int-WDT panic; **31,736 B works** → `BLE_MIN_DMA_BLOCK=31744`.
- Contiguity ≠ bytes, proven: freeing ~10 KB of stacks moved total free 28k→38k but left the largest block at 27,648 unchanged.
- Boot RAM% as a coexistence health check: **54.2 correct vs 39.2 broken** (halved LVGL pool).
- LVGL object-heap per-screen: Home ~25 KB, each submenu ~11 KB, bounded ~37 KB on nav-pop.

**Estimated (no `.map` or linker size report exists in either tree)**

- All byte sizes are derived from source constants + ESP-IDF struct/driver knowledge, not read from symbols. WiFi per-buffer 1600 B is the assumed IDF S3 size; `wifi_ap_record_t` and several driver contexts are version-dependent.
- BT controller footprint documented range 24–30 KB vs the measured 31,744 gate; `MAX_ACT`/adv-flow-ctrl/scan-dup sub-allocations folded into the aggregate to avoid double-counting.
- WiFi MGMT static buffer: 12,800 B estimate used (config text says "small") to keep the full-profile total consistent with the code's stated ~58 KB.
- Mic PDM I2S RX: 2,880 B assumes mono 1-slot; IDF may allocate 2-slot → 5,760 B.
- LVGL object-heap PSRAM ~100 KB is a representative live peak, not a cap; the ~96 KB figure is internal RAM *freed* by the PSRAM move.
- `sdspi`/`spi_master` descriptors, TinyUSB MSC handle + device-task stack (~4096 default, not set in-repo), and NimBLE host PSRAM totals are IDF-internal estimates.
- Several boot-permanent internal BSS tables (`s_wd_seen` 3 KB, `s_inbox` 3.5 KB, WiFi identity ~17 KB) are reserved even when their feature never runs — candidates to make lazy/PSRAM, counted as resident today.
- Idle vs steady-state heartbeat readings come from different contexts and are not directly comparable; they describe operating states, not a single number.

**How to verify on-device**

- **System → Diagnostics** heap read for a point-in-time `INTERNAL|DMA` largest-free-block.
- The **boot heartbeat** log line (main.c gauge / `coex.h` `nocsif_log_dma_free`) for the largest-free trend across radio state changes — watch for it sitting persistently < ~4 KB with both radios enabled, or trending down across geofence wake/sleep cycles.
- **Confirm coexistence by boot RAM%** (54.2 correct) and by code-visible markers of the four load-bearing sdkconfig keys after any fullclean or machine change.
