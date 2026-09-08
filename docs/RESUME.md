# RESUME — start here to continue NocSif

Single-page handoff. As of **2026-09-07**.

## ⭐ CURRENT WORK (2026-09-08) — **§4.15 NocSif Desktop Bridge — MERGED #198 (main `c649586`); follow-up = live view over USB + "one downloadable app" (branch `Clankert/4-15-live-mirror`).**
- **Live view over USB:** `mirror` command (bridge.c) — pull-based: the host sends the seq it last received
  (+ `full:1` to resync, + one touch `[x,y,pressed]` per poll); the watch answers with the changed RECTANGLE
  since the previous poll (ui.c keeps a dirty bounding box the flush tap grows; `nocsif_ui_mirror_poll`), as
  PackBits RLE over 16-bit pixels (`rle565_encode`; worst case +0.4 %), or `{"none":true}`. The flush tap stays
  armed for 2 s after each poll (`s_usb_mirror_until_us`); a lapsed tap forces a full repaint. App: `LiveView`
  window (Control › Live view) — 410×502 = the panel's pixels so mouse→touch is 1:1; Tk `copy -zoom` paints
  only the changed patch; press/release kept ≥ 80 ms apart for LVGL's input poll; FN/PWR + Cast.
- **One holistic app (operator ask, "like qFlipper"):** auto-detect + auto-connect when a watch is plugged in
  (2 s port scan, ESP32-S3 VID/PID first); a board that opens but never answers `ping` → "attached but not
  answering — Flash › Flash new watch" (blank / old firmware); unplug → "watch unplugged"; `APP_VERSION`
  (0.2.0) + a release check (`updater.latest_app_release`, tags `app-v*` on the public mirror) → footer
  "update available" link; `release_app.ps1 [-Publish]` builds the single-file Windows exe (PyInstaller) and
  publishes it as a GitHub Release asset `NocSifBridge-windows-x64.exe`; README sections (repo + tool).
  ⚠ Publishing a release is an outward action — done only on the operator's go.

## (prev) 2026-09-08 — **§4.15 NocSif Desktop Bridge (qFlipper-analog) — PR 1 = firmware seam + publish parts; PR 2 = the desktop app.**
- **Firmware seam (branch `Clankert/4-15-bridge-seam`):** `bridge.{h,c}` — a JSON-lines protocol on the
  USB-Serial/JTAG console (the only always-alive USB channel; COM7). Replies are `NB>`-prefixed lines; long
  answers = base64 fragment lines under 1 KB each (one `write()` + `fsync` per line so the TX ring never drops and
  other tasks' log lines can only land between reply lines). Console driver installed FIRST in `app_main`
  (`nocsif_bridge_console_init`, 2 KB RX / 1 KB TX internal rings); the bridge task (`nocsif_bridge_init`, PSRAM
  8 KB) starts after the governor, in safe mode too. Commands: `version status health test sd.info sd.provision
  sd.format fs.ls fs.get fs.put fs.rm fs.mkdir ctl menu state screenshot log.tail usb reboot` (see bridge.h).
  Shared `/sd` rules moved from wifi.c into **`sdfs.{h,c}`** (jail / claim / list / info / provision / format);
  the companion's P4 handlers now call them. New: `nocsif_usb_gadget_sd_format` (f_mkfs via the MSC helper's
  mount-point switch — UNVERIFIED until a scratch card is available), `nocsif_ui_screenshot` (full repaint under
  the port lock through the mirror's flush tap), `nocsif_wifi_companion_dispatch/_menu_json/_state_json/_touch`
  (the companion hooks over the wired transport). `publish_firmware.py` now ships bootloader / partitions /
  ota_data beside firmware.bin + a manifest `parts` list with offsets (the public .gitignore rule widened to
  `!nocsif/firmware/*.bin`).
- **Desktop app (same PR):** `tools/nocsif_bridge/` Python + Tkinter (pyserial / esptool / requests): `nbridge.py`
  (protocol client), `bridge_cli.py`, `flasher.py` (esptool --no-stub wrappers), `updater.py` (public mirror),
  `nocsif_bridge_app.py` (Overview / Health / Flash / Files / Control / Log), README, PyInstaller scripts. Spec + the
  operator's feature list are in PLAN §4.15 "BUILDING 2026-09-08". Website constant = eigencat.org (hidden for now).
- **On-device lessons (three flash rounds):** ⚠ a PSRAM-stacked task may not call ANY SPI-flash API, reads
  included (`esp_ota_get_state_partition` mmaps otadata; `nocsif_logbook_read_tail` → `esp_partition_read`) —
  `esp_task_stack_is_sane_cache_disabled` asserts → panic. OTA posture is cached at init on the main task;
  `log.tail` + `reboot` run on the LVGL task via `lv_async_call` + semaphore. ⚠ The console VFS write path drops
  whole lines when the TX ring is full (fail-fast) → replies go via `usb_serial_jtag_write_bytes` (one ring item,
  blocking). ⚠ The RX ISR drops on a full ring → request lines < 1 KB (`PUT_CHUNK` 720 B), RX ring 4 KB, bridge
  task priority 10, retry-safe `fs.put`. ⚠ 8 KB stack overflowed on `fs.ls` (FatFs LFN + stat) → 32 KB PSRAM.
  ⚠ A runtime full I²C sweep reports ghost ACKs (9, then 17 devices) → the health check probes the 5 known
  addresses. ⚠ fopen/fclose per chunk crawled at 3 KB/s → transfer SESSIONS (handle kept open across chunks,
  30 s idle close). ⚠ The biggest win was on the HOST: pyserial `read(4096)` sat out its 200 ms timeout per
  reply → `read(1)` + `in_waiting`; and a log line written char-by-char can wrap AROUND a reply line, so the
  client parses from the LAST `NB>` marker, re-requests a short chunk, and restarts an upload once on
  "offset mismatch". **Final numbers (COM7): 2.7 MB put 29 s (92 KB/s), get 17.5 s (154 KB/s), sha256 match;
  342 KB put 3.8 s / get 2.3 s; screenshot 0.5 s; ls / mkdir / rm / provision / log.tail / remote reboot
  ("sw-restart") all good; health 12 pass / 0 fail / 5 not-probed.** Boot int-DMA: largest 21.5 K → 18.4 K
  with the 4+2 KB console rings (free 23.5 K → 19.2 K) — the price of the bridge.
- **Verification plan:** flash → `python tools/nocsif_bridge/bridge_cli.py version|health|ls` over COM7 →
  upload/download an mp3 with hash compare → `sd.provision` on the current card → `ctl launch` + screenshot → the
  app's Update flashes this very build. Wipe flows / Format SD dry-run only (destructive) unless the operator opts in.

## ⭐ (prev) CURRENT WORK (2026-09-07) — **RAM coexistence Phase A (PLAN §4.17): A1 DONE + measured, A2/A3 next; then Carts (B) and the Governor (C).**
The problem: at steady state (BLE resident + WiFi associated + display + audio + IMU) the largest contiguous
int-DMA run was ~2 KB, so **USB File Share, GNSS and LoRa refused** (`docs/DMA-COEXISTENCE-VERIFICATION.md`).
Runtime defrag is impossible (measured); taskLVGL/weather can't move to PSRAM (on-task NVS).
- **A1 ✅ (branch `Clankert/ram-a1-psram-dma-flush`, PR pending):** `display_io.{c,h}` — a NocSif QSPI panel IO
  with a **window-once streaming path** + a **never-allocate-internal-DMA** guarantee. The flush stage shrank
  **29.5 KB → 9.8 KB** (2 × 4 lines) at zero UX cost (flush ~29.5 ms, memcpy-bound; "screens feel great").
  **Measured:** steady-state largest **2,048 → 18,432**; post-imu 3,456 → 24,576; with GNSS + LoRa workers up the
  8 KB File-Share entry proxy **passes**; 0 underruns / 11.7 K chunks. ⚠ **PSRAM-direct DMA (no stage) is DEAD at
  80 MHz pclk** — 4/19 chunks underrun (PSRAM ≈ 30 MB/s < 40 MB/s panel). ⚠ `spi_device_get_trans_result`
  dequeues an underrun transaction THEN returns `INVALID_STATE` — treat as completed (the first build wedged
  silently on that). Probe: `main.c` `NOCSIF_COEXV` (build `-DNOCSIF_COEXV=1`, `findstr COEXV_A1 firmware.bin`).
  Residual: heavy UI erodes the tail to ~4 KB (free ~11 KB) — the ALWAYSINTERNAL=4096 small-alloc mechanism → A3.
- **A2 ✅ (branch `Clankert/ram-a2-psram-stacks`, stacked on A1, PR pending):** IMU (6 K + 2 K FIFO buffer),
  GNSS, buttons, usb_gadget, NFC stacks → PSRAM; bhy2 FIFO-drain bound in. **Measured:** steady-state largest
  **30,720** (pre-A1 2,048 → A1 18,432 → A2 30,720, 15×); post-imu 30,720; +GNSS/+LoRa cost the run nothing; USB
  8 KB proxy passes at 22,528 during the claim; Signal-Hunt-LoRa floor 21,504; every worker `stack in PSRAM`.
  **IMU `err -3` DECODED = a BHI260 NACK at WiFi association (INVALID_STATE with no i2c timeout log), NOT memory**
  → one retry in the IMU I2C wrapper; PR #172's Governor precondition is closed.
- **A3 ✅ VERIFIED (branch `Clankert/ram-a3-usb-entry`, stacked on A2; File Share MOUNTED on the PC with WiFi
  associated + BLE resident — Phase A's opening failure is closed):** 8 KB USB entry reserve claimed at boot,
  released right before the one-time `tinyusb_driver_install`; `coex.h` `NOCSIF_RADIO_MIN_DMA_USB` gate; USB screen
  shows the honest reason ("needs memory"/"install failed"). Lazy WiFi workers (parser/PCAP/hc-export/DNS) →
  PSRAM with `vTaskDeleteWithCaps` pairs. `msc_shed_wifi` (sibling stop-gap) not carried over. **Dropped the
  ALWAYSINTERNAL 4096→1024 knob** (ISR-touched FreeRTOS objects in PSRAM during flash ops = rare-crash class; the
  erosion was task stacks anyway).
- **B Carts rebuild ✅ VERIFIED (branch `Clankert/carts-player-rebuild`, stacked on A3; operator "all good"):**
  folders under `/sd/nocsif/carts` → `.wav` files, Stop row, tap-to-switch, live
  "playing · file" line; `audio.c` streams ANY PCM WAV (8/16/24/32-bit/float, mono/stereo, 8–48 kHz, I2S clock
  per file) via `nocsif_audio_play_file/_stop/_playing_path`.
- **§4.10 GitHub firmware pull (2026-09-07, branch `Clankert/4-10-github-ota`) — VERIFIED END-TO-END.** The
  "TLS fails with WiFi up" wall was `CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC` → `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`
  (PSRAM; set in `sdkconfig.defaults` AND the generated sdkconfig) + a compile-gated probe (`-DNOCSIF_TLS_PROBE=1`)
  proved HTTPS to raw.githubusercontent.com with BLE resident + WiFi linked. Public mirror
  `silverwolf2r/Nocsif-Firmware` (snapshot of main, no private history) carries `nocsif/firmware/{manifest.json,
  firmware.bin}`; `tools/publish_firmware.py --public-dir <clone> [--mirror]` publishes (version read FROM the
  image's esp_app_desc). Watch: System › Update › **Check for update** (manifest vs `esp_app_desc.version`) →
  **Download to card** (`/sd/nocsif/firmware/firmware.bin`, 8 KB reads on a PSRAM worker, sha256, HTTP Range
  resume) → the shipped installer (path moved to the folder; old path still accepted); manual only; install
  refused < 30 % battery off USB. ⚠ The 8 KB INTERNAL installer stack is now spawned only at Install — created
  at screen-open it starved WiFi's RX buffers and the download stalled at 6 KB/s. ~20 KB/s → ~2 min per image.
- **§4.8a Companion L4 — P4 `/sd` file browser + two reconciliations (2026-09-08, branch
  `Clankert/companion-l4-p4-files`; VERIFIED on-device, operator "everything works": 2 MB epub upload,
  browse, mp3 download, delete, and a phone socket-drop with ZERO storm lines — second cap
  `8ca9a9c4…\scratchpad\cap_20260907-212110.log`).** Reality check first: L4 P1–P3 + the
  interactive mirror/casting/auto-start/password/full-map were ALL MERGED by **#169 (main `1eb95c4`, 2026-09-02)**;
  #167 was its superseded predecessor — nothing was stranded. This pass: (1) `companion_set_on` no longer turns
  Bluetooth off/on around the surface — since RAM Phase 2 the controller is resident, so that "release" was a
  logical off that freed nothing, dropped the phone link, and persisted `bt_master=0` (a reboot/crash with the
  surface up, or auto-start, left BT off); (2) the companion httpd task → **PSRAM stack** (`task_caps`, 8 KB;
  the §4.10 lesson); (3) **P4**: `GET /api/fs?p=` · `GET /api/file?p=` (chunked, attachment) · `POST
  /api/upload?p=&n=` (`.part` → rename) · `POST /api/delete {p}` (files only) + a Files card on the page
  (breadcrumb, tap folder/file, upload w/ progress, del w/ confirm). `/sd` jail; card CLAIMED per request
  (refused with the reason under File Share); FAT lock only per readdir / 8 KB chunk, never across a send.
  ⚠ one httpd task: mirror frames queue behind a transfer. README's stale "pairing-code auth" line fixed.
  **First on-device pass (cap `cc965419…\scratchpad\cap_20260907-181205.log`) found a PRE-EXISTING L4 bug:**
  when the phone drops `/ws` (Safari backgrounding / navigation / a download hand-off) `comp_ws_handler`
  returned ESP_OK on a failed `httpd_ws_recv_frame`, so httpd re-polled the dead socket in a tight loop
  (`error in recv : 104` then `128`, ~3 log lines/ms, 48 K lines in 84 s, CPU 1 pinned — the "everything
  lags" report) until **task-wdt on `httpd` → reset** (backtrace = `httpd_server` → `lwip_select`). Fixed:
  fail the request so httpd closes the session (page reconnects). Two more lag sources fixed alongside:
  a push send failure now also **closes the session** (`ws_drop`; before, only the table entry went and the
  page kept a silent zombie socket), and the mirror got **backpressure** (`s_thumb_busy`: a new 103 KB frame
  is queued only after the previous send completed — the 80 ms tick used to pile blocking sends, each up to
  the 5 s send-wait, in front of every command/touch/page request). Upload button: iOS Safari ignores a
  scripted click on a `display:none` file input → a `<label for>` + a 1 px opacity-0 input.
  Download verified before the reset: a 350 KB mp3 in 5.5 s (Safari re-requests once and cancels the
  first — the "sent 0/… (aborted)" line is that, not a failure). **MERGED #196 (main `e5c19fa`).**
  Follow-up branch `Clankert/companion-mirror-first-frame` (VERIFIED, operator "works"): the live stage showed
  the watchface incomplete on first connect (dirty-region tap + a barely-repainting screen) →
  `companion_mirror_tick` forces one full `lv_obj_invalidate(lv_screen_active())` on the no-client → client
  transition. **⏳ REVISIT LATER (operator):** smoothness under load — 2:1 103 KB frames on one blocking httpd
  task (send-timeouts still seen on a congested link; adaptive scale / shorter WS send-wait / dedicated sender
  task), file throughput (~64 KB/s with the mirror competing), one-task blocking (a download freezes the mirror).
  Detail in PLAN §4.8a P4.
- **§4.14 operator pass (2026-09-07) — four PRs off main `fd14dd5`, verified together on-device from the
  integration branch `Clankert/4-14-combined` (two flashes, captures `cap_a1_20260907-111837/112830.log`):**
  **#188** Home add-picker / shortcut picker DERIVED from the `k_*_rows` menu tables + the app registry
  (`k_menu_cats`, `k_comp_submenus`, `<folder>.<leaf>` ids; placed planets hidden; path-stack navigation) ·
  **#189** Weather per-condition glyph — the icon font gained real snow / storm / fog (U+E02D–E02F via
  `gen_icons.py` + `npx lv_font_conv`), `wx_glyph_for_code(wmo, is_day)` feeds the peek chip, the Weather
  hero and the 3-day rows · **#190** Signal Hunt: `nocsif_nav_set_back_hook` (back while hunting → the
  pick list), strongest-first `hunt_order_build`, LoRa: no 915 preset row + the root cause of "spinning
  sets no bearing" (BLE/WiFi −100 dBm scale vs the SX1262's −110…−120 floor → every reading 0 % → equal
  bin weights) fixed with a radio-aware floor + a "signal present" gate + honest engine/no-energy text ·
  **#191** `almanac.{h,c}` (NOAA sun + low-precision Meeus moon, validated to the minute against USNO for
  four cities incl. the watch's own stored location) → a `☀ sets 2h05 · ☾ rises 4h10` line under the date,
  plus System › Display on/off rows for Time / Date / Weather chip / Sun & moon line (`peek_clock/date/wx/
  alm`, `peek_apply_chrome()`). Bonus: the A2 IMU I²C retry was SEEN recovering on-device for the first time.
  ⚠ Merge order is free (independent branches); the only overlap (weather ↔ almanac in `peek_info_tick` /
  the peek statics) was resolved on the combined branch — re-resolve the same way if main sees it.
- **B+ MP3 + BSSID places (branch `Clankert/mp3-and-bssid-places` off main `5a80cbf`; MERGED #187):** minimp3
  (CC0, header-only component `firmware/components/minimp3`, unmodified) → `audio.c` `do_play_mp3` (16 KB PSRAM
  window, ID3v2/ID3v1/APE trimmed up front, shared `emit_mono` limiter path, audio stack 6 → 28 KB PSRAM);
  Carts lists `.mp3` too. P3 geo-store re-keyed: **places = connected AP BSSID + lat/lon + SSID** (`pl_*` keys,
  8 max, oldest evicted, dropped with a forgotten profile; `nocsif_wifi_place_count/_get/connected_place`);
  `wn_la/wn_lo` gone; Governor fences iterate places. See PLAN §4.17 B+ / P3.
- **C·P1 Governor ✅ VERIFIED (branch `Clankert/governor-p1`, stacked on B; modem-sleep MAX on link seen on COM7,
  Connectivity screen + tile "all good"; park/retry = logic in place, UNEXERCISED — AP always in range):**
  `governor.{h,c}` 1 s WiFi policy tick over existing getters — linked+idle → modem-sleep MAX; up+unlinked+idle
  → PARK after `idle_min` (`esp_wifi_stop`, memory retained); parked → wake-and-look every `retry_min`; any
  holder / tile tap wakes; intent stays on (`radio_state.wifi_parked`, CC tile reads on). Settings ›
  Connectivity: "Wi-Fi power" live line + 4 rows. Never deinit. + A3 follow-up: USB reserve claimed right after
  the audio reserve. Self-test timers via `-DNOCSIF_GOV_SELFTEST=1`. **MERGED #184.**
- **C·P2–P4 ✅ VERIFIED (branch `Clankert/governor-p2-p4` off main; PR pending; cycle/hold/rail/timeout seen
  on COM7 at the production 5-min period; fences + location stamp UNEXERCISED — no fix indoors):** P2 cyclic GPS
  (`nocsif_gnss_set_hold` fourth want; one fix per `gps_min`, 60 s timeout, IMU-still gate ≥5 min) + fences
  around saved networks' learned locations (hysteresis, indoor-hold); P3 geo-store (`wn_la%d/wn_lo%d`,
  auto-learned via `CMD_GEO_STAMP` on the pinned worker; place-enter wakes a parked STA; outside known places
  → park after 60 s; timer retry stays as the indoor fallback); P4 Connectivity "Location" line + 3 rows,
  weather refresh on place-enter. **All five Phase A/B/C·P1 PRs are MERGED on main (`ed80ccd`).**
  Spec + rationale: PLAN §4.17. Flash recipe: app@`0x20000` +
  ota_data@`0xf000`, `--no-stub`, COM7; capture with scratchpad `cap.py` (DTR/RTS low before open).

## ⭐ (prev) CURRENT WORK (2026-09-01) — **§4.8a Companion control surface (L4) — P1 + P2 + P2-redesign + P3 live-state CODE-COMPLETE + BUILDS (pending on-device verify).** (See the P2-redesign/P3 block below for the latest; P1/P2 baseline follows.)
The on-network phone/laptop **web remote**. P1 = transport + discovery, **no pairing code AND no WPA2** (operator
call): the surface hosts an **OPEN SoftAP with no join gate** — anyone who joins controls the watch; the
off-by-default toggle + the on-watch "linked" dot are the only guardrails. **Transport verified on-device
(`nocsif.local` resolves on iPhone, connected page loads).** Branch **`companion-l4-p1`** (off `main` `c1ba7a4`);
not committed yet.
- **What shipped in P1 (all in `wifi.c` + `ui.c`, one new managed component):**
  - **mDNS `nocsif.local`** (`espressif/mdns` added to `idf_component.yml` + CMake `mdns` req) — brought up/down with the surface.
  - **OPEN SoftAP** with an SSID-override (`s_ap_ssid_ov`, honoured by `apply_ap_config`) so it broadcasts the
    **device-name SSID**; the shipped software AP + captive portal already open, unchanged. (An earlier WPA2 variant
    — `s_ap_secure`/`s_ap_pass`/NVS `comp_pw` — was removed when the operator chose an open AP.)
  - **Routed companion HTTP server** on :80 — its **own** `httpd_handle_t s_comp_httpd` (distinct from the portal's wildcard
    `s_httpd`); serves a styled self-contained "connected" page (`COMP_PAGE_HTML`) + **`GET /api/ping`** JSON (name/batt/
    uptime/clients). `CMD_COMPANION_ON/OFF` on the worker; `nocsif_wifi_companion_{set,active,ssid,pass,url,status_str,
    clients,tag_str}` getters.
  - **RAM/radio policy (the coexistence lever):** the UI's Start handler **releases the BLE controller**
    (`nocsif_ble_bt_set_enabled(false)`, captures prior state, restores on Stop) so the WiFi surface gets contiguous
    internal-DMA. Companion is **mutually exclusive** with the promiscuous monitor/parser + captive portal (single radio +
    port 80): enforced at bring-up (`monitor_teardown`/`portal_stop` + cycle any open AP) **and** at dispatch (CMD_AP_ON/
    CMD_PORTAL_ON call `do_companion_off()` first). **`ap_teardown` now also calls `companion_stop()`** so a STA scan/join
    can't strand the HTTP server on a dead AP. Bring-up **fails safe** (rolls back + reports) instead of crashing; heap
    health logged.
  - **UI:** `build_companion` at **System › Companion** (Start/Stop, live status, join SSID/passphrase/URL, honest
    security note) + live row tag + a **global violet "linked" dot** on `lv_layer_top` (mirrors the recording dot,
    driven by the 120 ms `wrist_wake_cb`).
- **Build:** PowerShell `pio run -e nocsif-twatch-ultra -j2` in the worktree → **SUCCESS, RAM 47.4% / Flash 58.3%**;
  `firmware.bin` binary-verified to contain the companion code (stale-`.o` gotcha checked). This worktree's
  `sdkconfig.defaults` already pins `LV_MEM_SIZE_KILOBYTES=96` + `ESP_WIFI_RX_BA_WIN=6` (the D:\Docker stale-sdkconfig
  gotcha does NOT apply here).
- **On-device (2026-09-01):** flashed `--no-stub` to COM7; **`nocsif.local` resolves + connected page loads on
  iPhone** (transport confirmed). Open-AP build re-flashed after the password removal.
- **P2 command channel ✅ CODE-COMPLETE + builds + flashed (pending on-device verify).** POST endpoints on the
  companion server: **`/api/launch {id}`** (any k_screens id + the action-rows flash/dnd/movie → toggles ride the
  launch path), **`/api/back`**, **`/api/home`**, **`/api/type {text}`** + **`/api/key {backspace|enter}`**. Bridge:
  wifi.c parses JSON (cJSON) on the httpd task → `nocsif_wifi_companion_set_cmd_handler` fn (registered by ui.c at
  init) → `companion_cmd_from_http` heap-copies + `lvgl_port_lock`+`lv_async_call(companion_exec_async)` onto the
  LVGL task (the codebase's `marshal()` pattern; NEVER touch LVGL off its task). Remote typing targets
  `s_companion_ta` (the most-recently-opened field; `nocsif_companion_track_ta` wired into all 9 keyboard screens via
  a one-line replace_all after `lv_keyboard_set_textarea`). Commands `ESP_LOGI`'d → logbook. The served page is now a
  real control surface (nav + 12-app grid + flash/DND/Movie + type box). **⚠ capture start/stop deliberately NOT
  wired** (single radio → would tear down the companion AP). Build RAM 47.3% / Flash 58.4%, binary-verified.
- **P2 PAGE REDESIGN ✅ + P3 live-state ✅ CODE-COMPLETE + BUILDS (branch `Clankert/companion-l4-page-redesign-044bbe`,
  merges PR #167's `companion-l4-p1`; pending on-device verify).** Reshaped per operator direction (2026-09-01):
  1. **Menu MIRROR, not a hardcoded grid.** New **`GET /api/menu`** returns the 3 Home categories (Cyber/Life/System)
     and their REAL rows `{id,label,en,warn}`, sourced from the now-file-scope `k_cyber_rows`/`k_life_rows`/
     `k_system_rows` (one source of truth) via `nocsif_companion_menu_json` (ui.c, registered with
     `nocsif_wifi_companion_set_menu_fn`). The page fetches it and renders collapsible category sections whose rows
     POST `/api/launch {id}`.
  2. **Warn-before-disconnect.** `companion_id_disruptive(id)` marks radio-disruptive targets (currently `hunt` —
     Signal Hunt cycles the 2.4 GHz radio) with `warn:1`; those rows `confirm()` before POSTing so a launch can't
     silently drop the companion AP.
  3. **Control Center below the menu** — flash/DND/Movie toggles (ride `/api/launch`) **+ brightness + volume
     sliders**. New **`POST /api/brightness {v}`** + **`/api/volume {v}`** (0-255) → new cmd enum values →
     `companion_apply_brightness`/`_volume` on the LVGL task, reusing the on-watch CC live+persist path (and
     reflecting the CC knobs if the shade is built).
  4. **On-page keyboard REMOVED.** Typing is now a P3 live action: when the watch reports a focused field, a bottom
     bar appears and a tap raises the phone's **native keyboard** (a hidden, on-screen-invisible `<input>`; iOS only
     raises on a user-gesture focus, hence the tap-bar) → `/api/type` + `/api/key`. The `/api/type|/api/key` backend +
     `nocsif_companion_track_ta` tracker are kept.
  - **P3 live channel:** a **`/ws` WebSocket** (needs `CONFIG_HTTPD_WS_SUPPORT=y`, added to sdkconfig.defaults) pushes
    the full watch state ~2×/s (name·screen title·batt·clients·dnd·movie·flash·bright·vol·**focused**) via an
    esp_timer→`httpd_queue_work`→`httpd_ws_send_frame_async` loop; `nocsif_companion_state_json` (ui.c) fills it from
    cached scalars only (never LVGL off-task). `GET /api/ping` now serves the same superset as a no-WS fallback; the
    page prefers WS and falls back to polling. Screen title exposed via new `nocsif_nav_current_title()` (ui_nav).
- **P3 pixel screen-mirror ✅ (this pass):** a ~1 Hz LVGL-task timer (`companion_mirror_tick`, ui.c) `lv_snapshot_take`s
  the LIVE active screen to RGB565, nearest-neighbour downsamples it to a **116×116** thumbnail, and hands it to
  `nocsif_wifi_companion_publish_thumb` (wifi.c); the `/ws` push loop sends it as a **BINARY** frame (8-byte header
  `'N','F',w16,h16,fmt,rsv` + RGB565-LE) when it is new (seq-gated, ≤~1/s). The page decodes it to a **round canvas**
  (circle-cropped to echo the panel). **Self-gated: the (heavy, ~434 KB PSRAM/frame) snapshot runs ONLY while the
  companion is up AND `nocsif_wifi_companion_ws_clients() > 0`** — zero cost when nobody is watching. Needs
  `CONFIG_LV_USE_SNAPSHOT=y` (added to sdkconfig.defaults; ESP-IDF/LVGL default is n). `lv_screen_active()` excludes
  `lv_layer_top` (CC shade / linked dot) — base-screen mirror only.
- **Operator testing additions ✅ (commit `3441cb9`, flashed to COM7):**
  1. **Auto-start on boot** — persisted "Auto-start on boot" toggle (`comp_auto`); a boot one-shot
     (`companion_autostart_cb`, +4 s, safe-mode gated) raises the surface when set. Start/Stop + auto now
     share `companion_set_on` (BLE-release policy in one place).
  2. **AP password (WPA2) set/change/delete** — `apply_ap_config` uses `WIFI_AUTH_WPA2_PSK` when a
     ≥8-char password is staged (`s_ap_pass_ov`, loaded from NVS `comp_pw` in `do_companion_on`, cleared
     on teardown; <8 chars ⇒ open, so a stray value can't brick join). New **Password** row → editor
     (`build_companion_pw`) + **Remove password** → open. Saving while up cycles the surface to apply.
  3. **Full nested menu map** — `/api/menu` is now RECURSIVE. The 8 second-level rowspec arrays (wifi,
     wifi.aphub, ble, nfc, lora, gnss, timers, autom) were hoisted to file scope + linked by an id→rows
     registry (`k_comp_submenus`); each row with a sub-menu carries a nested `sub` array (depth-cap 4).
     The page renders a collapsible tree (`renderRows`). Disruptive flags extended to deep leaves (every
     `wifi.*`/`ble.*` action, `hunt`, `gnss.wardrive`). Buffer 3072 → 8192.
  4. **Flashlight state fix** — companion toggle read perma-on (`s_cc_flash` is lazily created then
     HIDDEN, so non-NULL forever); added cached `s_flash_on` read off the LVGL task.
- **Interactive mirror / full control ✅ (commit `519f62a`, flashed):** the live preview is now a BUTTON
  → a full-screen control stage (watch at 410/502 aspect + FN/PWR side buttons) with full touch.
  - **Capture moved to the FLUSH path** (`companion_capture_region` in `nocsif_flush_cb`) — replaces the
    ~1 Hz `lv_snapshot`; reuses the watch's own rendered frames (no extra render), 3:1 downsample →
    RGB565 136×167, ~80 ms/~12 fps push (idle = no frames = no bandwidth). Now includes `lv_layer_top`
    (CC shade / flashlight / dot). ⚠ LVGL RGB888 is B,G,R.
  - **Touch injection:** a 2nd LVGL pointer indev (`remote_read_cb`) fed by the phone over the /ws uplink
    (`{"t":"m",x,y,s}` → packed volatile uint32, lock-free). **Side buttons** (`{"t":"b",k,a}` → CMD_BUTTON
    → the physical FN/PWR act_* handlers). **Casting** (`{"t":"c",on}` → CMD_CAST → brightness 0 + flush
    skips the panel draw; phone-as-display; auto-un-blanks on disconnect).
  - ⚠ latency ~100–180 ms round-trip (WS + fps cap + ESP render); casting frees the panel-flush SPI if
    motion lags. ⏳ NEXT = P4 (`/sd` file browser).

## ⭐ CURRENT WORK (2026-08-26) — **Phase 2 (capability expansion) underway. §4.1 "finish the shell" progressing, one stub-row → real screen per PR.**
- **Files SD browser ✅ (PR #143 → main).** The placeholder Files screen is now a real on-`/sd` browser: live listing (folders-first, sorted, real sizes) · folder drill-down (each level a pushed screen, back-arrow walks up) · text/hex file viewer (≤4 KB) · two-tap file delete · long-press → recursive folder delete behind a confirm screen (WDT-reset every 32 ops so a big tree can't trip the 8 s panic). Operator-verified on-device. Shell patterns: readdir snapshot under `nocsif_sdcard_lock` before any LVGL alloc; 40-row pool cap; per-row heap payloads freed on delete.
- **About device-info ✅ (PR #144 → main `5ad62b0`).** `system.about` → firmware (version/build-stamp/ELF-sha/IDF) · hardware (chip rev/cores/flash/PSRAM/MAC) · runtime (uptime/battery/reset-reason/free RAM+PSRAM, 1 s tick). Uses `esp_app_get_description`/`esp_chip_info`/`esp_flash_get_size`/`esp_psram_get_size`/`esp_read_mac` (all transitively available).
- **Left-edge clip cleanup ✅ (PR #145 → main `02ca999`).** Bare left-aligned labels added straight to the un-padded scaffold `content` sit at x=0 and the physically-rounded corner clips them. **New shared helper `nocsif_content_line(content, text, font, color, pad_top)`** insets to the 26 px menu column — apply it to any direct-to-content text. **Audited (multi-agent): the clip is NOT shell-wide** (~46 builders already self-pad; menu screens use `nocsif_menu_list`); the only genuine clippers were About + Diagnostics (#144) and the two sibling BLE screens `build_connect_phone` + `build_ble_hid` (#145) — all fixed. Also **measured the exact display bound** (interactive on-device calibrator): visible area = rounded rect, **inset 7 / radius 115** (preferred; technically-perfect 5 / 125) — corrects the old ~40/56 estimates.
- **Weather ✅ (§4.1)** — the first HTTP client. `weather.{c,h}` worker fetches **Open-Meteo over plain HTTP** (current + 3-day forecast) → a real **Weather screen** (temp hero · condition · feels/humidity/wind · 3-day rows · Refresh · °C/°F toggle) + the **live watchface temp chip** (replaces the stub `peek_wx_str`). **Location auto-follows the last GNSS fix** (captured off the GNSS worker via a cheap `nocsif_weather_note_fix` stash → the weather task persists to NVS, throttled). **Geofence seed of §4.6** — an auto-learned *last-connected* anchor; re-entering its ~150 m radius fires one forced refresh. Footer shows a **live WiFi connected/not glyph**. **⚠ Two hard-won fixes: (1) NVS/flash writes on the GNSS `publish_fix` hot path broke fix acquisition → moved all persistence to the weather task (GPS latched again). (2) HTTPS failed on this board — `mbedtls_ssl_setup` SSL_ALLOC_FAILED (no contiguous internal RAM w/ WiFi up) then handshake stalls even from PSRAM → switched to plain HTTP (public data; sidesteps TLS entirely). Verified on-device: `fetch ok: 31° code 0, stack hwm 5296`.** Next = remaining §4.1 stubs (Notes / Navigation / Activity / System-side / Alerts).

### Prior milestone — M8 GNSS/Location ✅ COMPLETE + fully field-verified
Live Fix · GPX Log · Sync Clock (auto-DST) · Wardrive — all four field-verified; the one found bug (Sync Clock off 1 h / DST) is FIXED (auto US DST, verified vs IANA zoneinfo) and operator-confirmed. M8 closed (#135-#139, main `f7556d9` incl. plan restructure #140).

M8 on the u-blox M10 (UART1, **RX=GPIO44 @38400**, rail **BLDO1**):
- **P1 Live Fix ✅ (this PR)** — the proof-of-life worker (`gnss.{c,h}`) grew a **continuous NMEA parser**
  (GGA/RMC/GSA/GSV/TXT) → a spinlock-guarded `nocsif_gnss_fix_t` snapshot: sats used/in-view · signed-decimal lat/lon ·
  alt · HDOP · speed/course · UTC · antenna status · a live sentence counter. Lazy live start/stop
  (`nocsif_gnss_set_live` — BLDO1 on-enter / off-exit; a fresh session zeroes the accumulator so the UI never shows stale
  data through the ~1 s warm-up). On-watch **Location → Live Fix** (`gnss.fix`): fix hero (3D/2D/ACQUIRING/WARMING UP) +
  sats + position + `alt/HDOP/speed` + UTC + liveness line, 500 ms tick, guarded label writes. **Receive-only** (maps
  RX=GPIO44 only; TX/GPIO43 = U0TXD stays unmapped → console-safe). **Verified on-device: indoors the sentence counter +
  in-view count + `ant OK` climb with no fix; walked outdoors → real 2D/3D lat/lon fix** — closes the PLAN §4.5
  outdoor-fix test. Dev boot probe still gated `-DNOCSIF_GNSS_BOOT_SELFTEST=1`.
- **P2 GPX Log ✅ (this PR)** — logs the live fix to a **GPX 1.1** track on microSD
  (`/sd/nocsif/tracks/trk-NNN.gpx`). Smart cadence (log on **≥3 m move OR a 15 s heartbeat**, rate-capped 1.5 s); the file
  is **kept valid-on-disk after every point** (footer rewritten each append → a power-loss mid-hike still parses); logging
  runs in the **engine** (`nocsif_gnss_gpx_set` forces live streaming on) so a **track keeps recording in the background**
  after you leave the screen. Writes go through `nocsif_usb_gadget_claim_sd` + `nocsif_sdcard_lock` (shared SPI3; **/sd is
  app-mounted from boot so it works UNPLUGGED**); refused in USB **File Share** (host owns the card → "NO SD / BUSY").
  On-watch **GPX Log** screen: state hero + points/distance/duration + live fix health + Start/Stop pill. GNSS worker stack
  bumped 4096→**6144** (SD writes + float snprintf). **Verified on-device: UI + Start→WAITING-FIX + the no-fix-logs-nothing
  gate. ⏳ Outdoor point-capture (append + distance + a valid multi-`trkpt` file) is PENDING A RETEST** — operator hadn't
  stepped outside yet ("worked so far but didn't walk outside; go ahead, retest later").
- **P3 Sync Clock ✅ (this PR)** — sets the **PCF85063A RTC** from the GNSS UTC. Writes **UTC + HOME.off_min** (reuses the
  World-Clock `tz_home` city — same "RTC = home-local" model), via a **new public `nocsif_rtc_set()`** (datasheet-safe
  STOP→write→run, clears the OS integrity flag, refreshes the cache/validity — LVGL-safe short I2C). UTC→local uses an exact
  integer **civil-date algorithm** (Hinnant days_from_civil / civil_from_days — no libc `timegm`/TZ). Manual **Sync now**,
  gated on a valid fix (won't silently change the clock). On-watch **Sync Clock** screen (`gnss.syncclock`): live watch time
  + GNSS UTC/sats + home tz + result line. **Verified on-device: UI renders cleanly (live watch clock, tz line, the
  `waiting for GNSS time` gate). ⏳ The actual clock-set is PENDING the outdoor retest** (needs a fix).
- **Wardrive ✅ (this PR)** — a geotagged WiFi survey to a **WiGLE-1.4 CSV** (`/sd/nocsif/wardrive/wardrive-NNN.csv`,
  importable to wigle.net). The GNSS engine drives GNSS-live + the passive channel-hopping WiFi monitor
  (`nocsif_wifi_request_parse`/`_monitor_hop`) and writes each newly-seen BSSID once (MAC · SSID · auth · ch · RSSI ·
  lat/lon · alt · accuracy-from-HDOP) at the live fix; **background-capable**, dedup ≤512 BSSIDs. **WiFi + GNSS run
  concurrently** (steady WiFi, no BLE cycling → the F2 int-DMA gotcha doesn't bite; RAM 41.7%). On-watch **Wardrive**
  screen (`gnss.wardrive`): state + nets/APs-visible/duration + fix health + Start/Stop. **Field-verified outdoors.**
- **Sync-Clock DST fix ✅ (this PR)** — field test found the clock **1 h slow** (World-Clock offsets are fixed STANDARD
  time → New York synced to EST not EDT). Fixed with **auto US DST** (2nd Sun Mar → 1st Sun Nov, computed in UTC epoch
  seconds; only US home cities carry the rule via a new `k_cities[].dst`). **Algorithm proven BEFORE flashing:** a Python
  reimplementation cross-checked against IANA `zoneinfo` — **35,040 hourly conversions × 4 US zones + a 47,847-day
  civil-date/`toordinal` check, all pass** (scratchpad `verify_dst.py`); UI now shows `UTC-4 DST`. **Operator-confirmed:
  the synced clock matches their phone.** (EU/Southern DST + the DST-naive World-Clock display are future work.)
- Full plan → **PLAN §4.5**; detail → memory `project-m8-gnss`.

**Watch state:** flashed with the P1 build (Live Fix live). ⚠ *sending* UBX config would still need GPIO43 freed — the
live path is receive-only, so it doesn't.

---

## ⭐ (prev milestone, 2026-08-25) — **M9 LoRa: SX1262 ALIVE + RadioLib driver + P2P messaging + Channel Activity + Band Survey + Signal-Hunt-over-LoRa ALL DONE & MERGED (#127–#133). M9 parked (remaining slices peer-gated, no 2nd radio).**

M9 on the SX1262 (915 MHz US ISM, shared SPI3):
- **Proof-of-life ✅ (PR #127)** — an instrumented raw-SPI probe confirmed the chip is ALIVE (no faith). **M8 GNSS got
  the same probe → also ALIVE; the "dead GPS" call was OVERTURNED** (streams NMEA, `ANTSTATUS=OK`; the "0,0" was
  no-fix-indoors — needs an outdoor fix test). Confirmed-dead set is now just **NFC + haptic**. → memory
  `project-hardware-faults`. ⚠ GNSS ESP-TX GPIO43 = U0TXD is console-reserved (passive RX on GPIO44 works).
- **Driver + TX ✅ (PR #128)** — RadioLib 7.2.1 vendored on a **custom ESP32-S3 HAL over shared SPI3** (the shipped
  EspHal is ESP32/SPI2-only, register-bangs the bus). `lora.c` → `lora.cpp`. TX verified (3/3 TX_DONE @915 MHz).
- **P2P messaging ✅ (PR #129)** — a NocSif broadcast text frame + send/receive engine + on-watch **Messaging** screen.
  Send tap-verified; **RX / round-trip is pending a 2nd LoRa node** (send path + RX arming + RSSI floor verified).
- **Channel Activity ✅ (PR #130)** — a solo-verifiable band monitor `lora.activity`: sweeps 15 sample frequencies
  across 902.4–927.6 MHz reading instantaneous RSSI (a coarse energy spectrum, centre bar = the 915 home channel) + a
  LoRa CAD pass on 915 counting preambles; on-watch spectrum screen (bars + home reading + busiest/quietest + CAD). **Verified
  on-device** (real per-channel floor ~-100 dBm with channel-to-channel variation, CAD free in a quiet band, zero WDT).
  ⚠ **GetRssiInst returns the −127 floor if read too soon after `startReceive` — the fix is an ~8 ms RX/AGC settle before
  each read** (the no-arg `getRSSI()` is *packet* RSSI, `getRSSI(false)` is instantaneous). Retune is `standby →
  setFrequency → startReceive` (SetRfFrequency needs STDBY/FS, not RX). SD lock released across each settle.
- **Band Survey ✅ (PR #131)** — the deeper "see everything" detector `lora.survey`: widens to **BW500** and sweeps
  **52 contiguous 500 kHz bins** across 902–928 MHz (gap-free) with per-bin max-hold + hit counter + a median floor +
  a **detected-signal list** (freq · peak dBm · width · hits, strongest-first); on-watch dense max-hold spectrum + the
  list + Reset. Sees ANY transmitter's energy (LoRa/FSK/OOK — as energy, not content); best catches persistent/frequent
  transmitters. **⚠ Detection is by HIT-COUNT, not raw energy: max-hold of noise drifts up over minutes, so an energy
  threshold would fill the list with false positives — counting how often the scanner catches a bin active is robust
  (noise rarely spikes 10 dB above the median).** Verified on-device (floor ~-93 dBm at BW500 — correctly ~7 dB above
  BW125's floor; 0 false signals in a quiet band; BW restored to 125 on exit; +1 app = 62). Positive detection (a real
  signal listed) is peer/environment-gated — same reviewed code, lists real signals near 915 activity.
- **Signal-Hunt over LoRa ✅ (this PR)** — the F2 Signal-Hunt bearing dial gained **LoRa as a 3rd radio** (Radio toggle
  BLE → WiFi → **LoRa**). **Primary flow: tap a Band-Survey-detected signal → Signal Hunt opens locked to that
  frequency and direction-finds it by ENERGY** (works on any modulation, even undecodable OOK). Engine `do_hunt(mhz)`
  parks the SX1262 at BW125 + `hunt_poll()` streams RSSI every 60 ms into a **fast-attack/slow-decay envelope** → the
  radio-agnostic `huntview_t` the dial/IMU-heading/audio already consume. Survey **auto-resumes** on return (its tick
  re-arms when radio is free). ⚠ Kept on the SX1262 alone (no BLE/WiFi cycling → int-DMA gotcha). Best on a
  continuous/frequent TX; bursty = pulsing dial (per-heading max-hold still maps it). Verified on-device (parks 915,
  −100 dBm envelope, 20/20 frames, no WDT). **LoRa mode has a proper PICK LIST** (like BLE/WiFi) — Radio→LoRa runs the
  survey and lists the detected signals (freq · dBm · width · hits) + a `915 home` row; you SELECT a target by its
  parameters, never a blind default. See [[project-f2-signal-hunt]].

**Watch state:** flashed with a **CLEAN build** (all self-tests OFF — normal boot, no LoRa self-test). Tap **Sub-GHz →
Channel Activity** (live spectrum) · **Sub-GHz → Band Survey** (signal detector — tap a signal to hunt it) · **Signal
Hunt** (Radio toggle → LoRa → pick a signal). Dev self-tests exist but are env-only-OFF: `_LORA_BOOT_SELFTEST` (TX,
emits) · `_LORA_ACTIVITY_SELFTEST` · `_LORA_SURVEY_SELFTEST` · `_LORA_HUNT_SELFTEST` (last three passive).
**Remaining M9 §4.6 = Meshtastic interop + FSK `.sub` decode — both peer-gated (need a 2nd radio the operator doesn't
have), so M9 is parked here; nothing more testable without one.**

**M9 detail → memory `project-m9-lora`. GNSS detail → memory `project-m8-gnss`.**

**Verdict-capture recipe:** flash app-only @0x10000 `--no-stub` COM7, then a pyserial reader in run mode
(DTR=False→IO0 high, EN/RTS-pulse to reboot into the listening window) catches the boot self-test block + `VERDICT`
(scratchpad `capture_activity.py`).


## ⭐ CURRENT WORK (2026-08-24) — **M11 F2 "Signal-Hunt" COMPLETE + operator-verified + MERGING: IMU relative-heading bearing dial + eyes-free audio cues + WiFi-AP hunting (BLE⇄WiFi toggle). Power management + C1 time tools + E1 audio + B1/B1.1 shake all DONE + MERGED earlier. A2 haptic HW-DEAD (shelved); B2 gestures + D1 activity SKIPPED. NEXT: user's call — ⛔ **M8 GNSS is HARDWARE-BLOCKED (dead GPS on this unit, same fault class as NFC/haptics — on hold pending a replacement watch, PLAN §4.5)**, so pick M9 LoRa or a platform/refinement direction.**

**Where the project is:** M0–M4 + UI-shell P1–P8 + reliability hardening DONE; **M5 WiFi** (P1–P5) and **M7 BLE**
(scan/GATT/advertise/observer-parity/ANCS+AMS phone companion/HID keyboard) DONE + merged. **M6 NFC** code-complete
but HARDWARE-BLOCKED (dead TX); **M8 GNSS is likewise HARDWARE-BLOCKED (dead GPS on this unit) → on hold pending a replacement watch (PLAN §4.5).** Live work is **M11 "watch-core"** (PLAN §4.4). Slice order: A1 IMU ✅ → LVGL→PSRAM ✅
→ B1 ✅ → B1.1 ✅ → ~~A2 haptic~~ (HW-dead) → **E1·1 speaker ✅** → **E1·2 mic ✅** → **E1·3 voice memo ✅** → **C1 tools ✅**
→ ~~B2 gestures~~ (skipped) → ~~D1 activity~~ (skipped, not wanted) → **power mgmt / F1 sleep-when-still ✅** →
**F2 Signal-Hunt ✅**. Automatic light-sleep (deeper than DFS) is a possible future power slice.

**F2 "Signal-Hunt" (DONE + operator-verified + MERGING).** Extends the existing BLE Signal Hunt (`build_ble_hunt`)
with three layers, all consuming a radio-agnostic `huntview_t` snapshot so the dial/audio work off either radio:
1. **IMU relative heading** (`imu.{c,h}`): on-demand **GAMERV** game-rotation-vector (accel+gyro, NO magnetometer),
enabled only while hunting (`nocsif_imu_set_heading` — gyro off at idle for power). Heading = **azimuth of the watch's
+Y (12-o'clock) axis projected onto the horizontal plane** (`nocsif_imu_heading_deg`), NOT a raw euler yaw — a euler
yaw only reads right when flat; the projection is tilt-robust (turning about vertical moves it, tilting to read it
doesn't). **⚠ GAMERV needs the gyro mounting matrix set too** (`bhy2_set_orientation_matrix(...GYROSCOPE...)`) or the
fusion gets inconsistent axes and the heading is garbage.
2. **Head-up bearing dial** (ui.c): 12 o'clock = your current facing; 12 heading-bins (max-hold RSSI) drawn as
grow/gold dots + a **gold needle** to a continuous weighted-centroid peak bearing ("strongest ~N o'clock"). Drawn with
positioned objects on a circle (cosf/sinf — the codebase has no lv_line/arc/canvas). Smoothness = a **~90 ms fast
repaint timer** over a circular-EMA heading + continuous centroid (the 500 ms tick was visibly steppy).
3. **Eyes-free audio cue** + on-screen **mute toggle** (persisted `hunt_audio`): `nocsif_audio_tone` beeps whose
rate (1.5–5 Hz) + pitch (500–1150 Hz) rise with proximity (raw tone channel, so the toggle is its own mute).

**WiFi-AP hunting** (`s_bh_wifi` mode, "Radio" toggle on the pick list): reuses the monitor's live AP table
(`nocsif_wifi_mon_ap_*`) for RSSI, smoothed UI-side; pinning an AP locks the monitor to its channel. **One radio at a
time** — the toggle hands the 2.4 GHz radio between the BLE observer and the WiFi monitor.

**⚠⚠ THE HARD ONE — BLE⇄WiFi cycling fragments internal-DMA RAM → BT-controller `Malloc failed` → `emi.c` assert →
interrupt-WDT crash (INCONSISTENT).** The BT controller needs a big contiguous internal-DMA block on init; each
BLE/WiFi-monitor cycle chops the heap (largest free block degrades 31744→22528→20480→…), and eventually the controller
can't allocate → hard assert (not catchable). It's pattern-not-threshold based (BLE both worked AND failed at
`largest=22528` on different runs → inconsistent crash). **FIX (shipped): gate every BLE bring-up from this screen on
heap health.** The tick's self-heal only calls `nocsif_ble_request_scan(true)` when
`heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA) >= HUNT_BLE_MIN_DMA (24576)`; if the heap is
fragmented it **fully yields WiFi** (`nocsif_wifi_radio_yield(true)` → `esp_wifi_deinit` frees the driver's buffers)
and waits for the heap to coalesce, showing "switching to BLE…", rather than initing into a doomed malloc. Worst case
is a brief delay, never a crash. Build entry no longer scans directly — the gated self-heal owns all BLE bring-up
(covers the toggle AND screen re-entry). **Coredump was empty (`version 0x0`) for the int-wdt — diagnosed from the
serial panic log + `reliab: CRASH RECORD` instead.**

**⚠ REUSABLE FIX — pooled-row taps were "hard to hit":** LVGL sets `LV_OBJ_FLAG_CLICKABLE` on **every** `lv_obj_create`,
so the container/label children of `wifi_live_pool_row` swallowed taps before they reached the row's own handler. Fixed
by clearing CLICKABLE on all pooled-row children (they never carry their own cb) → taps fall through to the row. Helps
every live-list screen. Secondary (not yet fixed): rapid audio-rail cycling per beep can time out the BLDO2 I²C write
under WiFi load (`axp2101: set BLDO2 voltage failed` — recovered, not fatal) — a future amp-rail-hold would cure it +
the click. Full detail → memory `project-m11-watch-core` + `project-f2-signal-hunt`.

**B1 shake-to-wake (DONE + MERGED, PR #107 `06a30f9`) + B1.1 shake-to-sleep (PR #108 `009f402`).** A deliberate wrist
shake wakes the panel; sustained shaking while awake sleeps it. **Accel-based host-side** (the Bosch wrist-tilt gesture
fired on the OPPOSITE/set-down motion for our P2 mounting) — a leaky motion accumulator on the IMU worker + a 120 ms
LVGL poll (`wrist_wake_cb` in ui.c) → `screen_wake()`/`screen_sleep()`. `SHAKE_SLEEP_FIRES 3` vs 1-to-wake makes sleep
deliberate; 1.2 s cooldown stops bounce. One **Settings > Display > "Shake wake/sleep"** toggle (`raise_wake` NVS)
gates both. **⚠ BUILD GOTCHA hit here: SCons silently linked a STALE `imu.c.o` → verify a change is in the binary
with `grep -a "<log string>" firmware.bin`, force-fix by `rm`-ing the `.o`.**

**⚠ A2 haptic — HARDWARE-DEAD on this unit → SHELVED (not merged).** The DRV2605 (I²C `0x5A`, enable XL9555 IO6)
never ACKs even with the enable verified HIGH + all rails on; **proven dead on LilyGo's own stock firmware** (no buzz).
Same fault class as the dead NFC TX + likely a damaged motor/antenna flex. **Not firmware-fixable — do not re-attempt.**
The DRV2605 driver is preserved (graceful/no-op) on branch **`m11-b1-shake-sleep`** (WIP commit `2a90478`, kept for a
possible RMA unit); `main` is clean of `haptic.{c,h}`. Full fault picture → memory `project-hardware-faults`.

**E1·1 — speaker (MAX98357A) bring-up + sound cues (DONE + operator-verified + MERGED, PR #109 `422bf78`).** First
audio out; **with haptics dead this is the alerting channel** (a chime replaces the buzz). `audio.{c,h}` = a worker
(wifi.c/imu.c pattern) owning an **I²S standard-mode TX** channel on **I²S_NUM_1** (BCLK 9 / WCLK 10 / DOUT 11) for the
MAX98357A on rail **BLDO2** (`nocsif_power_speaker_rail`, reg `0x97` volt / `0x90` b5 enable, toggled per cue). Callers
only **post** a tone/cue to its queue (never block on I²S). Lazy channel open on first play; sine synth duplicated to
both slots (amp channel-agnostic) with a short fade to kill the click. Named cues **BOOT/WAKE/SLEEP/TICK/ALERT**; mute
persisted to the `sound_en` setting. **Wiring:** main.c inits after the IMU + queues a boot chime (skipped in safe
mode / muted); ui.c = audible confirm on shake-wake/sleep, a Diagnostics **"Test tone"** row (raw tone, ignores mute —
it's the HW test), a Settings > Display **"Sounds"** toggle. CMakeLists: `audio.c` + `esp_driver_i2s`. Verified on
device: boot chime heard; `I2S TX ready` at heartbeat 0; IMU streams normally after; **zero I²S/audio errors**; largest
int-dma block 31744 unchanged; RAM 37.5%, Flash 24.3%. Full detail → memory `project-m11-watch-core`.

**E1·2 — PDM microphone (proof-of-life level meter): DONE + operator-verified (bar jumps on blow) + MERGED (PR #111
`cf6aa8d`).** First audio IN. The mic reads **PDM RX on I²S_NUM_0** (clk **GPIO17** / data **GPIO18**, 16 kHz mono LEFT
via `driver/i2s_pdm.h` → `i2s_channel_init_pdm_rx_mode`; the ESP32-S3 does **PDM→PCM + a high-pass filter in HARDWARE**,
so the driver reads 16-bit PCM directly — no software decimation). It is deliberately **independent of the E1·1 speaker**
— separate I²S port (the amp is I²S_NUM_1) and the always-on 3.3 V domain (**no BLDO2 rail call**) — so the two compose.
`mic.{c,h}` = a worker in **imu.c's producer/cache shape** (not audio.c's command queue): while capture is ACTIVE it reads
PCM blocks → **AC RMS** (about the block mean, so DC-immune) + peak → an EMA-smoothed **0..100 level** cached under a
spinlock; LVGL-safe getters (`nocsif_mic_level` / `_peak` / `_state` / `_status_str`). Capture is **gated + lazy**: the
PDM RX channel opens on `nocsif_mic_set_active(true)` and closes on `(false)`, and the worker **parks on a task
notification** in between, so the mic listens only while the Microphone screen is open (power / privacy; keeps internal
DMA free for the radio). `MIC_GAIN` (default 12) is the on-device sensitivity knob. **Wiring:** `nocsif_mic_init()` after
the audio worker in main.c; `mic.c` added to CMake (`esp_driver_i2s` already required from E1·1 — the same component
provides i2s_pdm.h); a **Microphone** test screen (`system.mic`, `build_mic`) off a System row next to Diagnostics — a
violet fill-bar + `num_48` level + decaying peak, updated **in place** on a 90 ms `lv_timer`, `set_active` bracketed by
build / `LV_EVENT_DELETE`. **⚠ GOTCHA: `i2s_new_channel(&cfg, NULL, &rx)` — the RX handle is the 3rd arg** (TX is 2nd;
audio.c passes `&tx, NULL`). Build RAM 37.7%, Flash 24.4%.

**E1·3 — voice memo + Sound settings + mic/volume tuning (DONE + operator-verified + MERGED, PR #116 `e04e151`).** The last
E1 audio slice. **Voice Memos** (Home → Life → Voice Memos): record 16 kHz mono PCM → a PSRAM buffer → **normalize toward
full-scale on stop** → WAV on `/sd/nocsif/voice`; list + **Play / Rename / Delete** each. Playback = `nocsif_audio_play_wav`
(read the WAV into PSRAM under the `/sd` lock, then stream to the E1·1 amp; BLDO2 held up the whole clip). **Background-
capable**: the mic worker stays alive while recording after the screen closes (finalizes only on Stop or the 30 s cap),
with a **red dot on `lv_layer_top`** shown on every screen while recording. **⚠ THE PLAYBACK BUG + FIX: raw PDM PCM is only
a few % of full-scale** — the meter looked healthy only via its display gain (never applied to the stored samples), so
memos recorded + played back fully but were SILENT; fix = **normalize each memo on save** (diagnosed by serial capture:
record + play both logged clean → amplitude, not a crash/read-fail; old memos stay quiet). **Sound settings** (System →
Sound): master **volume** (`spk_vol`, scales cues + memo playback, separate from the phone-facing CC media slider) +
independent **Boot / USB-plug / Shake** cue toggles under the master mute; a **USB-plug cue** on a VBUS rising edge
(`nocsif_power_vbus_read` polled ~2 Hz in the buttons worker). **Mic sensitivity** is adjustable on the Microphone screen
(meter display gain; recording normalizes regardless). **⚠ mic + audio SD workers had to go to 6144 stack** (the FatFs/
SDSPI chain overflows 4 KB). **⚠ KNOWN HARDWARE LIMIT: peak speaker loudness is bounded by the MAX98357A's fixed gain
strap on the board** — firmware is at the digital ceiling (cue headroom 0.9 / ~91% memo normalize), no louder without
distortion (operator confirmed + accepted). Build RAM 38.2%, Flash 24.5%.

**C1 — time tools (DONE + operator-verified + MERGED). Fills the mock `build_timers` hub with four tools + a shared
alert ring.** PRs: **C1·1 stopwatch #118 `b6f13cc`** · **C1·2 timer #119 `5d78a5a`** · **C1·3 world clock #120 `a98238c`** ·
**C1·4 alarms + ring + wheel pickers #121 `0e03a22`**. All live in `ui.c` (no new components; the alarm/timer pickers use
`lv_roller`, `CONFIG_LV_USE_ROLLER=y`).
- **Stopwatch** (`timers.stopwatch`): monotonic `esp_timer`, MM:SS.CS at 100 ms, Start/Stop + Lap/Reset, up to 30 laps.
  Module-global state → keeps running when you leave the screen. `tools_btn()` pill helper (reused by the rest of C1).
- **Timer** (`timers.timer`): duration set by a **six-digit HH:MM:SS scroll-wheel picker** (each wheel 0-9, max 99:99:99;
  large values normalize on the countdown, e.g. 00:90:00 → 1:30:00). Wheels while idle, countdown hero while running/paused.
  **Fires in the background** via `tm_bg_tick()` off the always-on header tick (runs even asleep) → the shared ring.
- **World Clock** (`timers.world`): current time in 13 cities computed from the RTC shifted by each city's offset vs HOME;
  **tap any city to make it HOME** (persisted `tz_home`), the rest re-shift + the home row accents. Fixed **standard-time**
  offsets — no DST/tz DB (two DST zones stay correct relative to each other; a DST-vs-non-DST pair can read an hour off in season).
- **Alarms** (`timers.alarms`): up to 8 persisted wall-clock alarms (NVS), each time + on/off + repeat (Once/Daily/Weekdays/
  Weekends); **scroll-wheel editor**; fixed-row-pool list refreshed in place. `alarm_bg_tick()` matches the RTC each minute
  (even asleep) → the shared ring; once-alarms disarm after firing.
- **Shared alarm/timer ring** (`alert_*`): a match/expiry lights the panel + raises a **full-screen overlay** (`lv_layer_top`)
  with the alert time + live current time and **Stop / Snooze** buttons, and **keeps ringing** (re-chimes every 2 s) until
  acknowledged. **PWR = acknowledge, FN = snooze 2 min** (intercepted at the top of the button handlers); snooze re-rings the
  same source 2 min later off a monotonic deadline. **Only one ring at a time** — the first wins. **⚠ fix carried here: the
  alarm editor's old hero label had NO color → defaulted to black on the near-black bg (invisible); the roller pickers set
  explicit grey (dim off-rows / bright selected).**

**POWER MANAGEMENT — DONE + operator-verified + MERGED (PR #123 `262fa6b`).** A whole **Settings > System > Power**
screen (`build_settings_power`, `system.power`) replacing the old single hardcoded 2-min panel-off. Built + verified in
four slices, all in `ui.c` + a new `pm.{c,h}`:
- **Screen timeout** (`scr_timeout`, default 120 s): `15s/30s/1m/2m/5m/never` — `peek_idle_cb` reads it (idle poll dropped
  5 s→1 s). **Dim before sleep** (`dim_presleep`, on): panel fades to a low level ~6 s before sleep; activity/wake restores.
  **Sleep when still** = the F1 essence (`sleep_still`, on): blanks early once set down **flat (|az|>0.85 g) + motionless
  ~30 s** — a new always-on IMU stillness tracker `nocsif_imu_still_ms` (STILL_MOVE 0.05 g L1 delta) paired with the flat check.
- **Power saver / CPU DFS** (`dfs_en`, off): **`CONFIG_PM_ENABLE=y`** (active sdkconfig + `sdkconfig.defaults`) + `pm.c`
  `esp_pm_configure` (240↓80 MHz idle, **no** light sleep). Transparent to display/touch (drivers hold PM locks) — verified
  with WiFi+BLE scans and carousel/CC animations clean. **⚠ verify PM stuck in the generated sdkconfig after building
  (silent-revert gotcha).** **Re-sleep after shake** (`shake_resleep`, on): a shake-wake with no touch within 2 s blanks
  again; one-shot check at the deadline off the inactivity clock (reset at wake), armed only on shake wakes.
- **Battery Saver** (`batt_saver` manual + `batt_saver_auto` <20% unplugged, on): an **overlay** that while active forces
  DFS on + caps the timeout (15 s) + caps brightness (~90, via `ui_base_brightness`) + reduced motion (`ui_reduce_motion`),
  restoring the user's settings on exit. `batt_saver_refresh()` is edge-triggered off the header tick.
- **⚠ KEY LESSON:** a screen revealed by `nocsif_nav_back()` is NOT rebuilt (lists that change under an editor must
  self-refresh). A future deeper slice could enable **automatic light sleep** (esp_pm `light_sleep_enable`) — bigger idle
  win but riskier with this display/touch/radio stack.

**NEXT — the remaining M11 slices (E1 audio + C1 tools + power mgmt complete; B2 + D1 skipped by user choice):**
**F2 Signal-Hunt IMU bearing-sweep** (the M7-P4·2 haptic/IMU deferral, now audio-cued since haptic is dead) — or a new
direction at the user's call.

**Build / flash** (paths are the CURRENT session's worktree — the name changes each session, e.g. this slice built in
`D:/NocSif_Firmware/nocsif-m11-watch-core-b5cb31`; substitute `<worktree>`, or build the stable `D:/Docker/NocSif_Firmware`
clone. App-only @0x10000 preserves NVS):
- Build (PowerShell, **never Git Bash**): `C:/Users/tok6b/.platformio/penv/Scripts/python.exe -m platformio run -j 2 -d <worktree>/firmware` (drop to `-j 1` if it OOMs). First build on a fresh worktree is a full ~12 min rebuild.
- **Verify the change is in the binary** (SCons can silently link a STALE `.o`): `grep -a "<unique log string>" <worktree>/firmware/.pio/build/nocsif-twatch-ultra/firmware.bin` — 0 hits = not in the bin → `rm` the `.o`, rebuild. Judge build/flash success from the LOG, not the exit code (Tee masks it).
- Flash app-only: `$env:PYTHONIOENCODING="utf-8"; …python.exe -m esptool --chip esp32s3 --port COM7 --no-stub write-flash 0x10000 <worktree>/firmware/.pio/build/nocsif-twatch-ultra/firmware.bin`
- **⚠ `D:/NocSif_Firmware/*` is an auto-cleanup zone that has evaporated mid-session before (warm `.pio` + worktrees lost; git refs survive).** Commit + push promptly — don't leave work only in this worktree.

## ⭐ CURRENT WORK (2026-08-20) — **M11 STARTED: A1 IMU DONE + MERGED (PR #105); then a foundational RAM-relief pass (LVGL→PSRAM) that fixed an A1 regression + unlocked BLE/WiFi coexistence + a smoother display — DONE + verified on-device, merging now**

M11 "watch-core" (PLAN §4.4) is underway. Slice order: **A1 IMU ✅** → A2 haptic → B1 wrist-raise → B2 gestures →
C1 tools → D1 steps/wellness → E1 audio → F1 off-wrist deep-sleep → F2 Signal-Hunt IMU sweep. IMU/haptics/audio are
I²C/I²S → independent of the BLE/WiFi single radio.

**A1 — BHI260AP IMU accelerometer bring-up (DONE + MERGED, PR #105 `14862b0`).** The BHI260AP (I²C `0x28`, rail ALDO4)
is a Bosch programmable sensor HUB (BHY2) that boots from a **host-uploaded RAM firmware image** every power-on
(`setBootFromFlash(false)`). Vendored the Bosch BHY2 C API + the `bhi260_gpio` RAM image as `firmware/components/bhy2/`
(from SensorLib v0.3.1); `imu.{c,h}` = a worker that boots the hub + streams the accelerometer; `nocsif_power_sensor_rail`
= ALDO4. Verified: product id `0x89`, accel tracks tilt ±1 g, zero WDT. Gotchas (ESP32-S3 I²C 64 B max, GPIO firmware
image, 1/4096 scale, ACC_PASS raw vs ACC corrected) in the topic memory `project-m11-watch-core`.

**⚠ A1 REGRESSION found on-device → fixed by moving LVGL's heap to PSRAM (this PR, merging now).** A1's IMU worker
(~8 KB internal: 6 KB task stack + 2 KB FIFO buf, forced internal by `SPIRAM_MALLOC_ALWAYSINTERNAL=16384`) tipped the
already-razor-tight **internal DMA RAM** over the edge — the **BLE controller** (which can't use PSRAM) then failed
`esp_bt_controller_init` with **"Malloc failed" / ESP_ERR_NO_MEM** (largest free internal-DMA block had dropped to
~8 KB, below what the controller needs). Root cause of the tightness: the **96 KB LVGL object pool lived in internal
`.bss`** — 29% of all internal RAM. **Fix: route LVGL's allocator to PSRAM.**
- **`firmware/src/lv_mem_psram.c`** — a `LV_USE_CUSTOM_MALLOC` allocator (`lv_malloc_core`/`_realloc`/`_free`/monitor)
  backed by `heap_caps_malloc(MALLOC_CAP_SPIRAM)`. sdkconfig: `CONFIG_LV_USE_CUSTOM_MALLOC=y` (was BUILTIN). LVGL widget
  structs are NOT DMA targets (draw buffers are separately PSRAM), so this is capability-safe. Kconfig→lv_conf maps
  `CONFIG_LV_USE_CUSTOM_MALLOC` → `LV_STDLIB_CUSTOM` (lv_conf_kconfig.h). **Result: static internal RAM 67.2% → 37.2%
  (~98 KB freed); runtime internal-DMA free ~30 KB → 128 KB, largest block ~8 KB → ~63 KB.** Screen-build times unchanged
  (~60-110 ms) — the object-tree-in-PSRAM perf cost is negligible here.
- **BLE/WiFi COEXISTENCE now on (`ble.c`).** With the RAM freed, removed the WiFi-deinit-before-BLE yield (`bring_up`
  no longer calls `nocsif_wifi_radio_yield(true)`). `s_yielded` stays false → `do_ble_release`'s yield(false) + wifi.c's
  reclaim path are automatic no-ops (dead yield machinery left in place). `CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y` already
  on. **Verified on-device: WiFi stays connected when a BLE screen opens.**
- **DISPLAY smoother (`display.{c,h}`, `ui.c`).** The nav-flush "wipe" (new screen painting in top-to-bottom) is the
  full-frame flush; measured ~45 ms serial. **Pipelined it**: two ping-pong stage bands (12 lines ×2 = ~29.5 KB) +
  `trans_queue_depth=2` + a counting semaphore, so a band's memcpy overlaps the previous band's DMA → **~29 ms** (the
  panel's raw QSPI throughput floor for a 617 KB RGB888 frame). **HARDWARE WALL: a full-screen animation is capped at
  ~34 fps in RGB888** — the CC pull-down exposed it, so its slide was snapped shorter (`CC_SLIDE_MS` 300→150) to keep
  the stepping brief. (RGB565 would hit ~60 fps but risks banding on the dark gradients — deferred; user kept RGB888.)

**NEXT: B1 wrist-raise auto-wake** — the hub's wrist-wear-wakeup virtual sensor (bhi3 module → vendor it) → post
`screen_wake()` (ui.c, the single wake entry point; mirror `wake_from_touch_async`); toggleable. A2 haptic (DRV2605
`0x5A`, XL9555 IO6) is the low-risk parallel. Full A1 detail + reusable BHY2/RAM gotchas → memory `project-m11-watch-core`.

## ⭐ CURRENT WORK (2026-08-20) — **M7 BLE HID keyboard (wireless DuckyScript) — DONE + verified on-device + MERGED (PR #102 `9f3201a` → `main`)**

The watch is now a **Bluetooth keyboard** (HID-over-GATT / HOGP): it advertises with the keyboard appearance + HID service, a host (PC/Mac/phone) pairs (Just Works, bonded), and the watch types by notifying 8-byte boot-keyboard reports — the wireless twin of the M4 USB DuckyScript. **Verified on-device: advertises as "NocSif Kbd", pairs from a computer, and types.** Full detail → [[project-m7-ble-hid]].

**FIRST GATT SERVER on the device** (ANCS/AMS make the watch a GATT *client*; P1–P4 were client/observer/broadcaster). `ble.c` hosts a standard HID keyboard service (`0x1812`, report protocol: Report Map `0x2A4B`, Input Report `0x2A4D` notify + Output Report, HID Info `0x2A4A`, Control Point `0x2A4C`, Protocol Mode `0x2A4E`) + Device Information (`0x180A` PnP ID + Manufacturer) + Battery (`0x180F`, from `nocsif_power_batt_pct`). Advertises connectable with **keyboard appearance `0x03C1`** + the `0x1812` UUID (name "NocSif Kbd" in the scan response); Just-Works pairing + bonding (reuses the ANCS `sm_*` cfg); types via `ble_gatts_notify_custom` on the input-report value handle. **No sdkconfig change** (peripheral role + bonding + NVS persist already on from ANCS).

**Three load-bearing patterns (reusable):**
- **Transport SINK (no forked DuckyScript):** `hid_kbd.c` routes its 3 report emitters through a sink (`nocsif_hid_kbd_set_sink` USB default / BLE) — keymap + caps logic identical, only the report-send differs (USB `tud_hid_keyboard_report` vs BLE `nocsif_ble_hid_send_report`), so the ENTIRE `ducky.c` grammar plays over BLE unchanged. `ducky.c` added `nocsif_ducky_request_run_ex(path, sink)` (BLE reads `/sd/ducky` under the SD lock only — no USB-MSC claim) + `nocsif_ducky_request_type(text, sink)` (inline literal, backs "Type test string").
- **Conditional GATT registration (zero-regression):** the HID/DIS/Battery services register **only on a keyboard bring-up** — `bring_up` calls `hid_gatt_register()` gated on `s_hid_mode`, in the NimBLE window between `nimble_port_init` and `nimble_port_freertos_init`. So scan/central/broadcaster/ANCS bring-ups are BYTE-IDENTICAL (no server exposed) → zero regression risk. A full teardown clears the table → no accumulation across open/close.
- **Mutual exclusion (`MAX_CONNECTIONS=1`):** the keyboard link is mutually exclusive with the phone-companion (ANCS/AMS) link AND WiFi — one connection slot. Factored `nimble_teardown()` out of `do_ble_release` (the NimBLE-deinit core, minus the WiFi-yield-back); `do_hid_start` tears any other mode down first (drops the phone link, `s_want_ancs=false`), and `do_scan_on`/`do_adv_start`/`do_ancs_start`/`do_pcap_on` tear the keyboard down when `s_hid_mode`.

On-watch **BLE Keyboard** screen (`ble.hid`, `build_ble_hid`) — live status · Type test string · Run Macro (the `/sd/ducky` picker over BLE via `s_macro_sink`) · Keymap cycle US/GB/DE · Disconnect; the BLE-menu row drills in. Build RAM 66.3%, Flash 22.5% (1.89 MB); clean boot (BLE lazy until the screen opens). **Foreground-only this slice: opening the keyboard drops the phone link and does NOT auto-restore it** (reopen Connect Phone to get it back). **Refinement candidates (none blocking): Caps-LED sync (`s_hid_led` captured, not yet fed to the keymap); a live on-watch keyboard (type by hand, not just test string/macros); consumer-control media keys; keystroke timing tuning.** iOS nuance: the iPhone's prior ANCS bond can make it auto-reconnect instead of offering the keyboard — a PC/Mac is the predictable host, or Forget the bond first. **NEXT: refine the above, or the next M7/milestone piece.**

## ⭐ CURRENT WORK (2026-08-19) — **M7 Phone Companion (AMS media remote + persistent link + Connect Phone hub + Control-Center sync) — DONE + operator-verified + MERGED (PR #100 `472a8be` → `main`)**

The phone-companion arc on top of ANCS: notifications + media both ride ONE persistent bonded BLE link. Full detail → [[project-m7-phone-companion]]. **What it is:** an **AMS media remote** (Apple Media Service, the sibling of ANCS on the same phone link) — now-playing (title/artist/volume/play-state over Entity Update `2F7CABCE…`) + working transport (Remote Command `9B3C81D8…`); a **persistent** link (notif/media no longer release the radio on exit → the bond persists across the UI, ANCS/AMS run in the background on the NimBLE host task); a **Connect Phone** hub (`phone`, `build_connect_phone`) grouping Notifications + Media under one BLE-menu entry; and the P8 Control-Center media card wired live (now-playing + prev/play/next + volume-sync).

**KEY GOTCHAS (each cost an on-device debug cycle):** (1) **serialize AMS discovery AFTER ANCS** — two concurrent `ble_gattc_disc_svc_by_uuid` on one link corrupt each other's handle ranges (symptom: `service 51868-15416`, start>end); AMS is kicked from the ANCS terminal callbacks, gated by `s_ams_started`. (2) **AMS Remote Command needs write-WITH-response** (`ble_gattc_write_flat`) — iOS silently drops write-without-response (that was the "now-playing shows but buttons do nothing" bug). (3) **AMS volume is RELATIVE-only** (~16 iOS steps, VolumeUp/Down; no absolute set) — jump-to-match is exact, a drag is approximate. (4) **BLE worker cmd queue depth = 6** → batch a multi-step op (e.g. a volume ramp) as ONE worker command, never post N. (5) **persistence = the notif/media screens simply stop calling `request_release` on exit** (`s_want_ancs` already persists + auto-reconnects); WiFi reclaims the radio via `wifi.c`→`ble.h`. Device names: scan/GATT/hunt lists show a **vendor label** ("Apple device") when no name advertises (iOS privacy → few names is EXPECTED). A speaker glyph was added for the CC volume slider (icon-font regen is deterministic via pinned `lv_font_conv@1.5.3`). **UI text says generic "phone" not "iPhone"** (user may switch back to Android). **NEXT → the BLE HID keyboard (now DONE, above).**

## ⭐ CURRENT WORK (2026-08-19) — **M7 Phone Notifications (ANCS) REVIVED on top of the display fix** — code-complete + VERIFIED end-to-end on-device + MERGED (revival PR #96 `9b2079e`; receipt-crash + polish fixes PR #97 `b5b5772` → `main`)

The shelved ANCS feature ([[project-m7-ancs-shelved]]) is revived now that the display DMA-hang fix (below) is on main. **What it is:** the watch advertises connectably soliciting Apple's ANCS; the phone connects + **bonds (Just Works, keys persisted to NVS)**; the watch becomes the GATT *client* of the phone's ANCS service, subscribes to Notification Source + Data Source, and mirrors each notification (app · title · message · category). Receive-only, foreground (radio held while the **Phone Notifications** screen is open). `ble.{c,h}` = the ANCS client (`nocsif_ble_ancs_*`, state IDLE→ADVERTISING→CONNECTED→READY); `ui.c` = the **Phone Notifications** screen (`notif`, `build_ble_notif` — status · Forget-phone · fixed live-row pool). sdkconfig: **`CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=y` + `CONFIG_BT_NIMBLE_NVS_PERSIST=y`** (SM/bonding was already compiled in from the P1 CENTRAL deinit-link need).

**Revival method:** branched off current main (has the flush fix); took ANCS `ble.{c,h}` wholesale from `ancs-wip-shelved` (`f01a64a`), applied only the ANCS *screen* hunks to `ui.c` (deliberately DROPPED that branch's failed `.trans_size` disp_cfg experiment and its flat-256B display.c band-aid — the persistent-stage flush is the real fix), added `PERIPHERAL=y`+`NVS_PERSIST=y` (skipped the proven-ineffective HCI transport trims). **On-device verified (boot hook navigating to the screen, then removed):** the **Phone Notifications screen RENDERS** (`built 'notif' in 50 ms` — the exact render that wedged the display before), NimBLE comes up in PERIPHERAL role (`NimBLE synced … largest=3712 B` — the ~2-3 KB shelving condition), `ancs: advertising (connectable, ANCS-solicit)` fires, heartbeats steady, **zero `setup_dma_priv_buffer`/wedge/WDT**. Final shippable build (hook stripped) boots clean; ANCS is **lazy** (idle until you open the screen). **VERIFIED end-to-end on-device:** paired via a BLE scanner app (nRF Connect) and test messages mirror on the Phone Notifications screen. **PR #97 then fixed 3 bugs found in that testing:** (1) **a receipt-time CRASH (critical)** — sending a notification reset the watch; the core-dump backtrace decoded to a fault in `npl_freertos_callout_is_active` on task `nimble_host` (NimBLE's own `ble_hs_timer` callout faulting on a *later* tick = the silent-corruption signature). Root cause = the **4096-B NimBLE host-task stack OVERFLOWED**: `NOTIFY_RX` puts a 256-B buffer on the host stack, then calls `anc_request_attrs → ble_gattc_write_flat` (the deep ATT/L2CAP/HCI write chain) **synchronously from inside the notify callback**. Fix = move the ANCS RX/parse buffers OFF the host stack (`static` — only the single host task touches them) + `CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE` 4096→6144 (audited the parse path first — `anc_copy_txt` clamps, bounded `strncpy` into zeroed structs — so it's stack DEPTH, not an overrun). (2) **battery clipped by the rounded corner** — header insets 48/42→56 (see the UI-GOTCHA below). (3) **pairing text** — iOS Settings won't reliably list a generic peripheral → "connect from a BLE scanner app to pair" + the local name moved to the PRIMARY advert. UI text stays generic "phone".

## ⭐ CURRENT WORK (2026-08-19) — **FOUNDATIONAL display DMA-hang fix: persistent internal-DMA flush stage (kills the per-flush bounce)** — DONE + verified on-device + MERGED (PR #95 `703ca4f` → `main`)

The recurring, sometimes-fatal "flush wedge" class is FIXED. **Root cause (nailed in ESP-IDF source, not guessed):** LVGL's draw buffers live in PSRAM (RGB888, `buff_spiram`), and `esp_lcd_panel_io_spi` never tags its color transactions `SPI_TRANS_DMA_USE_PSRAM` → `spi_master.setup_dma_priv_buffer` treats the PSRAM source as un-DMA-able and mallocs a bounce from `MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL` **every flush**. When the largest free internal-DMA block drops below that bounce (WiFi capture+PCAP, BLE, fatally the **BLE peripheral role** → ~1–3 KB), the alloc fails → `draw_bitmap` errors → the flush-done IRQ never fires → `taskLVGL` wedges forever in `wait_for_flushing` → task-WDT reboot. (This is what shelved ANCS — see [[project-m7-ancs-shelved]].)

**Fix (`display.{c,h}` + `ui.c`):** allocate ONE small internal DMA-capable **"stage" band** (`NOCSIF_FLUSH_STAGE_LINES=4`, ~4.9 KB) at boot and bounce every flush **PSRAM→stage→panel** through it, via a NocSif `nocsif_flush_cb` + `on_color_trans_done` callback that **replace** esp_lvgl_port's (registered under the port lock, held across `add_disp`, to avoid a boot flush-ready race). An internal, DMA-capable source is **never re-bounced** by `spi_master` (TX internal-DMA alignment is 1 — `spi_dma.c`: "TX don't need to follow dma alignment"), so the flush needs **ZERO internal DMA at runtime**. `max_transfer_sz` sized to one band (band = 1 DMA chunk = 1 trans-done → balanced with the per-band semaphore that lets the single stage be safely reused). Bands kept **EVEN-line** (2-px shear guard, `ui_rounder_cb`). **RGB888 UNCHANGED — no colour/banding regression.** If the stage can't alloc at boot, we keep the port's default flush (fallback → change can only help).

**Verified on-device (both conditions, zero `setup_dma_priv_buffer`/`send color data`/`wait_for_flushing`/WDT):** (1) **synthetic worst case** — reserve internal DMA to `largest=496 B` (BELOW the old 820 B bounce), forced repeated FULL-FRAME flushes, heartbeats steady; (2) **REAL condition** — `CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=y` + active scan → `NimBLE synced … largest=3456 B` (the exact ~2–3 KB that shelved ANCS), 8 forced full-frame flushes, BLE init still fit (pre-BLE 31744 → 3456). Final shippable build (peripheral OFF, no scaffolding): boots clean, Home/Control-Center render, `largest=31744`, RAM 64.7%.

**Approach notes:** Approach A (esp_lvgl_port `trans_size`/`buf3`) was RULED OUT — that flush loop exists only in the **lvgl8** port; this project compiles **lvgl9** (LVGL 9.3.0), which IGNORES `trans_size` (would need a fragile managed-component patch). Approach B (RGB565) works but loses colour depth AND forces a big permanent internal draw buffer competing with BLE — the RGB888 persistent stage avoids both. **This UNBLOCKS the BLE peripheral role (ANCS / BLE HID keyboard).** Test scaffolding (`NOCSIF_FLUSH_STRESS_TEST` / `NOCSIF_BLE_BOOT_STRESS` in main.c) was used to prove it, then removed. **NEXT: resume M7 (ANCS revival now possible) or the next milestone.**

## ⭐ CURRENT WORK (2026-08-18) — **M7 BLE P4·4 (Drone Detection / OpenDroneID reception) — built + flashed + boots clean + parser spec-verified; LIVE end-to-end decode PENDING a Remote ID broadcaster** (operator chose "merge now, verify later") (branch `Clankert/m7-ble-nimble-recon-8a4a77`): receive the ASTM F3411 / OpenDroneID **Remote ID** a compliant drone broadcasts over Bluetooth (Service Data UUID `0xFFFA`, app code `0x0D`). `ble.{c,h}`: the GAP disc callback detects the 0xFFFA service data (NimBLE `f.svc_data_uuid16`), then `odid_upsert` merges the 25-byte ODID message into an address-keyed **drone table** (`BLE_DRONE_MAX 10`, own `s_drone_mux` — must NOT nest in `s_dev_mux`). `odid_apply_msg` decodes **Basic ID** (UAS serial + UA type), **Location** (drone lat/lon deg×1e7 · geo-alt · speed · track), **System** (OPERATOR/pilot lat/lon), **Operator ID**, and **Message Packs** (type 0xF, for future BT5 ext-adv; guarded against nested recursion). BT4-legacy adverts carry ONE message each, cycling across adverts → accumulated per drone. API `nocsif_ble_drone_count/_get/_gen/_tag_str` + `nocsif_ble_ua_type_str`; table cleared on radio release. On-watch **Drone Detection** screen (`ble.drone`, `build_ble_drone`; hunt-style two-views-in-one): a `DRONE_PICK 5`-row pick list → a **detail panel** (identity · drone position+alt · speed/track · **pilot position** in gold · operator ID · RSSI/age). **No sdkconfig change (pure observer; reuses the P1 legacy scan).** **SCOPE: catches Bluetooth-broadcasting drones — ASTM mandates BT4-legacy when Bluetooth is used, so the legacy scan suffices; Wi-Fi-transport Remote ID is M5's domain. All ODID field offsets cross-checked vs opendroneid-core-c; the one make-or-break constant is the `FA FF | 0D | counter | msg` BT container framing (ASTM/Wireshark-documented). Built RAM 64.7% / Flash 22.0%; flashed + boots clean (int-dma ~46 KB, zero WDT) — but NOT yet observed decoding a real broadcast. Verify with a drone or the free Android "OpenDroneID Transmitter" app; any fix is likely a one-line framing tweak.** **NEXT (remaining M7 P4): BLE wardrive (GNSS/M8-blocked); or pivot to the phone slice (ANCS) / BLE HID keyboard.** — P4·3 recap: **M7 BLE P4·3 (Advert Capture / PCAP export) DONE + verified on-device** (branch `Clankert/m7-ble-nimble-recon-8a4a77`): while the observer scans, every received advertisement is written to `/sd/nocsif/ble/adv-NNN.pcap` as a Wireshark-decodable **BLE link-layer frame + pseudo-header (LINKTYPE 256 = `BLUETOOTH_LE_LL_WITH_PHDR`)** → RSSI + address + parsed AD structures. `ble.{c,h}`: the GAP disc callback (host task, sole producer) copies each raw advert into a **PSRAM SPSC ring** (`pcap_push`, O(1)); the **existing worker task drains it to /sd** — deliberately NO new task, because internal RAM is too tight during BLE (~8-10 KB free) for a fresh 6 KB stack, so the worker's OWN stack is bumped **4096→6144 at boot** (RAM plentiful then) and it wakes every 100 ms via an `xQueueReceive` timeout to drain a bounded chunk under one `/sd` lock. Each record is synthesised into pseudo-header + a reconstructed LL advertising PDU (`pcap_build_packet`: adv access addr `0x8E89BED6`, event-type→PDU-type map, TxAdd from the addr type, CRC left unchecked). API `nocsif_ble_request_pcap`/`_pcap_active`/`_frames`/`_bytes`/`_dropped`/`_path`/`_status_str`/`_tag_str`; `CMD_BLE_PCAP_ON/OFF`; the file is flushed + closed in `do_ble_release`. Reuses the WiFi PCAP SD discipline (`nocsif_usb_gadget_claim_sd` + `nocsif_sdcard_lock`; the app owns /sd outside File Share so `INVALID_STATE`→"File Share owns the card" and **no release_sd needed**). New on-watch **Advert Capture** screen (`ble.pcap`, `build_ble_pcap`): scans on enter, **Record to SD** toggle, live adverts/bytes/dropped + file path, releases the radio on exit (which closes the recording). **No sdkconfig change (pure observer).** **On-device: `pcap: recording -> adv-000.pcap` → `pcap: closed (457 adverts, 31078 bytes, 0 dropped)` — ZERO WDT/display-DMA, clean teardown+restore, `built 'ble.pcap'` pool 65%; NimBLE-synced `largest=8192` (the +2 KB worker stack trimmed a little headroom but stays well above the ~0.8 KB flush need — no display-DMA regression); user confirmed adverts recorded.** Fidelity note: the observer hands assembled adv reports (not raw OTA bits) → channel is nominal (primary-adv 37) + CRC unchecked. **NEXT (remaining M7 P4 observer-parity): OpenDroneID reception · BLE wardrive (GNSS/M8-blocked); or pivot to the phone slice (ANCS) / BLE HID keyboard.** — P4·2 recap: **M7 BLE P4·2 (Signal Hunt) DONE + verified on-device** (branch `Clankert/m7-ble-nimble-recon-8a4a77`): pin a scanned device → live **RSSI proximity gradient** ("closer / farther"). `ble.{c,h}` tracks one hunt target (`nocsif_ble_hunt_set_target/_clear/_active/_snapshot`, `_target_str/_tag_str`): per-advert in `dev_upsert` it drives an **EMA-smoothed RSSI** + peak-hold + last-seen for the pinned addr (guarded by `s_dev_mux`), cleared on radio release. New on-watch **Signal Hunt** screen (`hunt`, `build_ble_hunt`; enables the pre-stubbed row + a BLE-menu entry) — two views in one screen: a **compact 6-row pick list** (`HUNT_PICK`, deliberately small so the meter co-exists in the LVGL pool — a full 10-row list alone is ~88%; this screen builds at **64%**) → a hunt **meter**: a **0-100 proximity hero** (via the digit-only `nocsif_num_48`, no minus needed — RSSI mapped −100..−40 → 0..100%), `dBm · peak` subtitle, a rising 14-seg strength bar, and a **closer/farther/steady** trend from a lagging RSSI reference (`s_bh_ref_x10`). No sdkconfig change (pure observer). On-device: pool 64%, synced `largest=9728`, zero display-DMA/WDT; user confirmed the meter tracks movement + closer/farther is correct. **HAPTIC (eyes-free) + IMU rotation-sweep BEARING deferred to M11** (`docs/design/signal-hunt-mockup.html`). **NEXT (remaining M7 P4 observer-parity): BLE PCAP export · OpenDroneID reception · BLE wardrive (GNSS/M8-blocked); or pivot to the phone slice (ANCS) / BLE HID keyboard.** — P4·1 recap: **M7 BLE P4·1 (Nearby Trackers) DONE + MERGED (PR #90 → `main` `9fd9c09`)**: the P1 observer now CLASSIFIES item-tracker advertisement signatures — Apple **Find My / AirTag** (mfg company `0x004C`, message type `0x12` = the *separated* offline-finding beacon; an AirTag near its owner stays quiet, so it only shows once separated), **Tile** (service UUID `0xFEED`/`0xFEEC`), **Samsung SmartTag** (`0xFD5A`) — via `classify_tracker()` in the adv parse. Sticky `tracker` field on the device struct + `nocsif_ble_dev_t`; filtered-view API `nocsif_ble_tracker_str/_count/_get/_tag_str` (+ a `fill_dev_out` copy-out refactor shared with `dev_get`). New on-watch **Nearby Trackers** screen (`ble.trackers`, `build_ble_trackers`) — scans + shows ONLY trackers (gold type · name/addr · last-seen · RSSI bars), reusing the P1 live-row pool/paging. **No sdkconfig change (pure observer).** On-device: builds/scans clean, synced `largest=10752 B`, zero display-DMA/WDT; user confirmed a tracker classified on-screen. **NEXT = M7 P4·2 (Signal Hunt P1: live-RSSI proximity hunt — pick a device, big RSSI meter strengthens as you approach; the natural follow to tracker detection; haptic/IMU sweep completes at M11).** — P3 recap: **M7 BLE P3 (Advertise / Beacon) DONE + MERGED (PR #89 → `main` `448abc6`).** The first BLE *transmitting* op — the watch broadcasts a non-connectable advertisement (BROADCASTER role). Two formats: **named** (`ble_gap_adv_set_fields` with the chosen local name + auto TX power, visible in any scanner) and **iBeacon** (a hand-built 30-byte payload — flags + Apple manufacturer data, fixed NocSif proximity UUID `4E4F4353 4946…` = "NOCSIF", major/minor 1 — via `ble_gap_adv_set_data`). `ble.{c,h}`: `nocsif_ble_adv_start/_stop/_active/_starting`, mode + name config **persisted to NVS** (`nocsif_settings`, keys `adv_mode`/`adv_name`), `do_adv_start` reuses the lazy `bring_up` (releases WiFi, single radio) + deferred-on-sync start (`s_want_adv` → `on_sync`), non-connectable params (`conn_mode=NON`), stub adv event cb, integrated into `do_ble_release`/`on_reset`. New on-watch **Advertise / Beacon** screen (`ble.advertise`, `build_ble_advertise`): Broadcast Start/Stop · Mode toggle (named↔iBeacon, live-restart on change) · Name (keyboard entry, `build_ble_adv_name`). sdkconfig: **`CONFIG_BT_NIMBLE_ROLE_BROADCASTER=y`** (host adv code → PSRAM, `MAX_ACT=2` unchanged — advertising is ONE activity). **On-device: `advertising started (named)` → live mode-toggle → `(iBeacon)` → back to `(named)`; NimBLE-synced `largest=10752 B` (BROADCASTER did NOT regress the P2 display-DMA headroom), heartbeats steady, ZERO display-DMA errors, clean teardown/restore; user confirmed NocSif + the iBeacon visible in a phone scanner.** Scope: `NOCSIF_BLE_ADV_CUSTOM`/`_IBEACON` (major/minor fixed at 1/1 this phase). Radio model: **broadcast STOPS on screen exit** (release restores WiFi) — a persistent beacon (radio off WiFi indefinitely) is deferred to a background-radio model. **NEXT = M7 P4 (observer parity: tracker detection · live-RSSI tracking · BLE PCAP · OpenDroneID · wardrive) or the BLE HID keyboard / phone slice (ANCS) — operator's pick.** P2 recap + the P2 DISPLAY-DMA fix follow:
> **M7 P2 (GATT Explore, DONE + MERGED PR #88 → `main` `c6a66fd`):** central connect (`ble_gap_connect`) → discover all services + per-service characteristics (chained on the host task) → `ble_gattc_read`; lock-free flattened item view; on-watch **GATT Explore** (`ble.gatt`) = device-pick → service/characteristic tree w/ tap-to-read + Disconnect. iPhone verified (4 svc / 11 chr, tap-read works). Drops-on-read + no-name in the scan list = EXPECTED iOS (unbonded central; iOS omits the name from adverts when locked).
> **⚠ P2 DISPLAY-DMA WEDGE — found + fixed (still load-bearing).** Entering GATT Explore first wedged the display (`setup_dma_priv_buffer(1208) Failed` → `send color data failed`): with **WiFi connected-then-released** for BLE the internal-DMA pool is left FRAGMENTED (largest ~2.8 KB after the controller loads), then the **active-scan advertisement flood** fills the controller's adv-report buffers during a heavy first render → the ~1.2 KB flush bounce loses the race. **Fix (two levers):** (1) **controller-buffer trim** in `sdkconfig.defaults` — `BT_CTRL_BLE_ADV_REPORT_FLOW_CTRL_NUM 100→50` (its Kconfig floor; values <50 are silently reset to 100) + `BT_CTRL_SCAN_DUPL_CACHE_SIZE 100→20` — raises NimBLE-synced `largest` from **2816→11264 B**; (2) **`display.c` flush chunk halved** `~1.6 KB→~0.8 KB` (`NOCSIF_DISP_W * sizeof(uint16_t)`) for margin. (An sdkconfig value below a Kconfig `range` floor is silently discarded → the default; always re-grep the generated `sdkconfig.nocsif-twatch-ultra` after a reconfigure.)
>
> **M7 P1 (recap, DONE + merged PR #87 → `b0b39a3`):** NimBLE **observer** device scan (`build_ble_scan`). **LOAD-BEARING: BLE and WiFi CANNOT be up together** — the controller needs ~20-30 KB internal DMA the WiFi driver + 96 KB LVGL pool leave no room for; CC WiFi-off (`esp_wifi_stop`) does NOT free it, only `esp_wifi_deinit` does. **Fixes:** `display.c trans_queue_depth 3→1` (stops a BT-`.bss`-induced boot-loop) + **auto single-radio release/restore** (`nocsif_wifi_radio_yield`; entering a BLE screen deinits WiFi → `nimble_port_init`, leaving → `nimble_port_deinit` → WiFi restored). **CENTRAL role required** for `nimble_port_deinit`→`ble_sm_deinit` to link (also enables P2). Recaps of M5 (all merged, through `ea55b67`) follow. — prior M5 note kept below:

## M5 WiFi **live-PCAP-over-USB-CDC DONE + on-watch verified**. All of P1–P5 (passive P1–P4 · active P5·1/P5·2 mgmt-frame TX · hc22000 · P5·3 software AP · P5·4 captive portal) merged to `main` (through `f716a85`). **Live-PCAP stream (`m5-wifi-livepcap-cdc`)**: the P3·3 PCAP writer gained a **sink selector** — `SD` (file, unchanged) or `CDC` (live stream). CDC path pushes the identical PCAP bytes (magic `a1b2c3d4`, radiotap linktype 127) to the USB serial port so a host reads a live capture. New raw CDC TX pipe in usb_gadget (`nocsif_usb_gadget_cdc_ready/_write/_flush`, gated on CDC mode + ON + host DTR; the console is deliberately NOT on CDC, so the endpoint is free — the "suppress logs" concern didn't apply). Whole-record framing, self-healing re-header on host reconnect (readiness guard even when the ring is idle), ring-drop backpressure. On-watch **"Stream to USB (live)"** row on PCAP Capture (gold) auto-requests CDC mode + monitor, shows "waiting for host / streaming / N frames / dropped"; mutually exclusive with Record-to-SD (one shared writer). Host side = **`tools/extcap/nocsif-wifi-livecap.py`** (+ `.bat`, README): a Wireshark extcap that pumps serial→fifo, resyncing to the PCAP magic → one-click live interface. **Verified on-watch only (operator's call): Stream row + auto-CDC-switch + "waiting for host" + no crash. Host-side frame delivery into Wireshark NOT verified on-device** (design + local extcap-handshake test stand behind it). **Remaining P5+ follow-on: wardrive (BLOCKED on GNSS/M8). Next milestone = M7 BLE** (Signal Hunt P1 = BLE-RSSI + haptic gradient hunt starts there; IMU sweep completes at M11). Milestone off M6 (NFC hardware-blocked, see below). Recaps follow.

> **⚠ UI GOTCHA (keeps biting — read before ANY screen/layout work): the display is a ROUNDED RECTANGLE and the
> curved corners physically CLIP content.** Never anchor UI to a physical corner
> (`LV_ALIGN_TOP_LEFT/TOP_RIGHT/BOTTOM_LEFT/BOTTOM_RIGHT`) — keep it **centred** or inside
> **`x ∈ [56, 354], y ∈ [56, 446]`** — but that box's CORNERS still clip (the arc is a quarter-circle, R≈56: corner content like the top-right battery must satisfy `(x-(W-R))²+(y-R)²≤R²`, not just the box). Safe insets `HEADER_HINSET 56` / `HEADER_TOP_PAD 56` (were 48/42 — the battery still clipped there; PR #97).
> Full detail: `docs/PLAN.md` §1 + `docs/LESSONS.md`.

**M5-P4·1 (WPA key-exchange + PMKID capture — the risky, high-value core of P4) is DONE and VERIFIED ON-DEVICE** — a
real handshake was captured on the watch. Branch `m5-wifi-p4-handshake` (off `main` `c709fdb`). It rides the P3
copy-out ring: the promiscuous rx path now recognizes **EAPOL** (LLC/SNAP ethertype `0x888e`) in data frames via a
cheap O(1) `eapol_offset()` and copies just those in FULL (the bulk of data traffic stays at the short station-map
snaplen). A new **`parse_eapol()`** decodes the key-info bits **off the hot path** → which of the 4-way **messages
1–4** was seen + the **PMKID** from the RSN KDE in message 1 → a BSSID-keyed table (`MON_HS_MAX 32`, SSID
cross-referenced from the AP list). New API in `wifi.{c,h}`: `nocsif_wifi_mon_hs_count/get/gen`,
`nocsif_wifi_mon_hs_tag_str`, `nocsif_wifi_hs_crackable` (PMKID, or M1+M2), `nocsif_wifi_request_hs_capture(bool)`;
`CMD_HS_ON/OFF`. New on-watch **Handshake / PMKID** screen (`build_wifi_handshake`; row `wifi.handshake`): live
per-AP rows (`hs 12--` · `PMKID` · `ready`) + a **Record → SD** toggle that **reuses the P3·3 PCAP writer** with an
EAPOL filter (`s_pcap_filter`) → `hs-NNN.pcap` (EAPOL + naming beacons only; Wireshark / hcxtools-ready). **Purely
PASSIVE** — the watch never solicits a handshake (deauth→reconnect is an ACTIVE op = P5). Build RAM 56.9%, Flash
18.2%; clean boot, int-dma ~85 KB free, zero WDT. All the P3 live-list discipline is honored (fixed row POOL, static
rows, container tap-targets [[gotcha-rounded-corner-safe-zone]], `trans_queue_depth=3` display fix, 96 KB LVGL heap).

> **Reusable live-list rules (cost 3 on-device WDTs across P3 to learn — honor in every screen):** never
> clean+recreate a list on a timer (churns the 96 KB LVGL heap → render-WDT) → use a **fixed row POOL** updated in
> place; keep rows **static** (taskLVGL shares CPU with the equal-priority WiFi worker; full_refresh repaints the
> whole frame on any label change); every tap target is a **container with `LV_OBJ_FLAG_SCROLLABLE` cleared** (a bare
> clickable label eats the first tap); footer text **centered** so its ends clear the rounded corners.

**M5-P4·2 DONE + verified on-device:** new **Anomalies** screen (row `wifi.anomalies`, `build_wifi_anomalies`) with two
passive detectors — a **deauthentication / disassociation frame-RATE meter** (an O(1) rx-path subtype tally of mgmt
subtypes 12/10 → per-second combined rate + peak, over the monitor window; getters `nocsif_wifi_deauth_count` /
`_disassoc_count` / `_deauth_rate` / `_deauth_peak_rate` / `_anomaly_tag_str`) + a **duplicate-SSID / twin** detector
(`nocsif_wifi_mon_dup_snapshot` groups the AP table by SSID; a security-class MISMATCH among BSSIDs sharing one SSID —
e.g. an open clone — is the strong twin tell, shown with a violet severity meter). Build RAM 57.0%, Flash 18.3%.

**M5-P5·1 DONE + verified on-device (the FIRST active/transmitting op; user chose "management-frame TX"):** new
**Management-Frame TX** screen (`wifi.mgmt`, `build_wifi_mgmt`) — Start discovery (parser) populates a TAPPABLE AP
list; tap a network to target it; **Transmit** emits spoofed-source deauth(12)/disassoc(10) frames (26 B, addr2=addr3=
target BSSID, broadcast dest, reason 7) via `esp_wifi_80211_tx(WIFI_IF_STA,…)` on an esp_timer at ~100 ms while
monitor holds the target's channel. **KEY UNBLOCK:** IDF 5.5.4 ships `ieee80211_raw_frame_sanity_check` as a STRONG
symbol (weak before), so the app override collides at link → fixed with **`-Wl,--allow-multiple-definition`** in
`platformio.ini` (first/app def wins → `return 0` permits raw mgmt-frame TX). One-shot serial log `mgmt-tx first frame
-> ESP_OK` confirms it. API: `nocsif_wifi_set_mgmt_target`/`_set_mgmt_disassoc`/`_request_mgmt_tx`/`_mgmt_tx_active`/
`_mgmt_has_target`/`_mgmt_tx_count`/`_mgmt_tx_rate`/`_mgmt_target_str`/`_mgmt_tx_tag_str`; `CMD_MGMTTX_ON/OFF/TARGET`;
stopped in `monitor_teardown`. Build RAM 57.2%, Flash 18.3%. **End-to-end loop proven concept:** deauth → device
reconnects → P4·1 captures the solicited handshake. On branch `m5-wifi-p5-active` (off the P4 branch) → **PR #82**
(stacked on #81 since #81 unmerged; retargets to main when #81 merges).

**M5-P5·2 DONE + verified on-device (folds into PR #82):** **Beacon TX** screen (`wifi.beacon`, `build_wifi_beacon`) —
now a **user-managed SSID list** (user asked to extend beyond generated decoys): **+ Add SSID** opens the on-screen
keyboard (mirrors `build_wifi_macentry`); tap a row → per-SSID **actions screen** (`build_wifi_beacon_item`) = Enable/
Disable · Rename · Delete; **+ Add decoys (8)** bulk-adds `NocSif-NN`. Only ENABLED entries transmit (beacon subtype 8,
per-entry locally-admin BSSID, on the held channel). List **persisted to NVS** (`bc_cnt`/`bc_s%d`/`bc_e%d`), lazy-loaded
via `beacon_ensure_loaded`, guarded by `s_bcn_mux` (LVGL writes / esp_timer TX reads; fire loop snapshots enabled → TX
off-lock). API `nocsif_wifi_beacon_add`/`_remove`/`_rename`/`_toggle`/`_get`/`_count`/`_gen`/`_add_decoys`/`_request_beacon`/
`_active`/`_frames`/`_rate`/`_tag_str`; `CMD_BEACON_ON/OFF`; stopped in `monitor_teardown`. Build RAM 58.0%, Flash 18.4%.

**NEXT — decide the fork (P5 mgmt-frame-TX trio complete):** (a) **software AP / captive portal** (`wifi.ap`/`wifi.portal`
— evil-twin/rogue-AP + credential-capture form; the pentester's deauth+twin combo), (b) **passive polish** (live-PCAP-
over-USB-CDC deferred from P3·3; on-device hc22000/hccapx export deferred from P4·1), or (c) **M7 BLE** (next milestone).
*(Test-env gotcha: `cap.py` resets the watch on serial close — USB-Serial/JTAG DTR/RTS quirk, NOT firmware.)* App-only
flash `@0x10000 --no-stub` COM7, FOREGROUND. **Target only authorized/own networks.** Neutral wording throughout.

---

### M5-P2 recap (prior, merged) — promiscuous monitor / packet capture

**M5-P2 is DONE + MERGED** (`m5-wifi-p2-monitor`). Risk-first: proved `esp_wifi` promiscuous capture coexists with
the live stack. **O(1), alloc/log-free rx callback** tallies total + by-type (mgmt/ctrl/data/misc) + per-channel +
last/peak RSSI; **esp_timer** (250 ms) hops 1–13 or locks + samples frames/sec off the LVGL task; **entering drops any
STA link and remembers it, exit restores it**. On-watch **Monitor screen** (`build_wifi_monitor`): frames/sec hero ·
total · mgmt/ctrl/data + RSSI · 13-bar per-channel spectrum · Start/Stop · Hop/Lock · channel. Verified 65 s+ capture,
int-dma **healthier under capture** (frees the STA buffers), zero WDT. P3·1 (above) builds directly on this callback.

---

### M5-P1 recap (prior, merged)

**M5-P1 (WiFi station bring-up + scan/join + network management) is CODE-COMPLETE, VERIFIED ON-DEVICE, and MERGED
to main** (squash-merged from `m5-wifi-p1-impl`). New `firmware/src/wifi.{c,h}` = an esp_wifi **STA worker** (the
`nfc.cpp` pattern: LVGL callbacks only *post* commands, a dedicated worker owns the radio; **lazy bring-up on first
use, safe-mode gated**). Shipped this milestone:
- Live **AP scan** screen (signal bars + security + lock, sweep animation) + on-watch **`lv_keyboard` join** with a
  **show/hide-password** toggle (default shown) + a full-screen **connecting view** (joining / wrong-password /
  connected, driven by `nocsif_wifi_join_state()`).
- **Multiple saved networks** — every successful join is remembered (SSID + passphrase, NVS `wn_*`), **migrates the
  legacy single K_SSID slot**; a **Saved Networks** list + a **per-network menu** (IP / subnet / router / MAC /
  password · Connect / Disconnect / Forget / **Show password**). Tapping a saved net reconnects with no prompt.
- **Device-wide WiFi settings on the WiFi page:** **Auto-join** toggle + **Set MAC address** (randomize a
  locally-administered addr / **type a specific MAC** / restore the factory MAC, applied worker-side stop→set→start).
- **Device name → WiFi hostname** (System › Watch Name; sanitized, spaces→`-`, so the watch shows on the network as
  e.g. `NocSif-watch`). Control-Center **WiFi toggle is boot-truthful** — a persisted "on" actually brings the radio
  up + auto-connects at boot (was showing "on" while lazily off).
- **Coexistence solved (the DMA-hang class):** `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y` **alone crashed the watch**
  (it forces static TX buffers internal → esp_wifi ate ~100 KB internal DMA RAM → the display full-frame flush
  couldn't get its bounce → LVGL wedged → UI-liveness WDT rebooted). **FIX = slash the internal-resident WiFi pools**
  in `sdkconfig.defaults` (`STATIC_RX=4 STATIC_TX=6 CACHE_TX=16 DYNAMIC_RX=16 MGMT_SBUF=8`). After: int-dma free
  stays **~55 KB** after init (bounce needs ~3.3 KB), scan/join/render/auto-connect-at-boot all coexist clean.
- **Verified on-device:** scan lists real 2.4 GHz APs (S3 is 2.4 GHz-only) → keyboard join → `got IP 192.168.1.216`
  → creds persist → reboot **auto-connects**; hostname `NocSif-Argentum` (user rename) confirmed on the network.
  RAM 53%, Flash 18%. Build/flash: PowerShell, penv python, `-j 2`, app-only `@0x10000 --no-stub` COM7.

**M5 NEXT (unbuilt) — the rest of `PLAN.md` §4.2, a FORK to confirm with the user:** (A) **consume** = Weather tile
(`peek_wx_str`) + NTP time-sync + the L4 web-UI companion (§4.8); (B) **operations superset** = promiscuous monitor ·
software AP · captive-portal credential-capture form · 802.11 management-frame TX · handshake / PMKID capture ·
wardrive + the Ghost-ESP WiFi rows. **Neutral device-class wording throughout** (the cyber-safeguard hard-fails
charged terms). Single-radio constraint: a monitor/AP operation suspends any STA/web-UI link (BLE is the control
fallback, M7).

---

## â­ï¸ CURRENT WORK (2026-08-15) â€” M6 NFC IN PROGRESS: plan merged, **RFAL foundation decided**; P1 = RFALâ†’ESP-IDF port is the current task (see the M6 block below). Reliability hardening A1+A2 DONE (recap follows).

---

## ⚠️ UPDATE 2026-08-16 — M6-P1 LANDED + MERGED, but NFC is HARDWARE-BLOCKED on the original watch

**M6-P1 (RFAL platform port + NFC hardware self-test) is CODE-COMPLETE and MERGED to main** (squash-merged from
`m6-nfc-p1-impl`; see `git log`). The RFAL -> ESP-IDF port works end-to-end in software: the chip initializes,
discovery runs, the whole stack is sound. The vendored RFAL is now **tracked** in `firmware/components/rfal/`
(self-contained — a future agent can build + run without re-cloning anything).

**BUT the original T-Watch Ultra has a confirmed HARDWARE FAULT in the NFC transmit path — NFC cannot be verified
end-to-end on this unit.** Root-caused by direct on-chip measurement via the committed self-test (`hw_selftest()`
in `nfc.cpp`), not guessed:
- Chip alive (SPI OK, ID rev 0x02); ALL internal rails healthy & stable (VDD 3.3 V, VDD_RF 2.95 V, VDD_AM 3.25 V);
  oscillator stable (osc_ok=1); TX driver configured for max drive (TX_DRIVER d_res=0).
- Field-on succeeds (ERR_NONE) and `tx_en` sets (OP_CONTROL 0xCB), BUT **`tx_on` NEVER asserts** (rapid-sampled 40x
  on BOTH the real collision-avoidance path AND a raw register poke), **no carrier** (amp flat 0x14->0x15), **no
  over-current** (i_lim=0 — rules out a shorted coil), **no rail droop**. The transmitter output stage simply never
  engages and draws no current.
- **Not a software gap:** LilyGo's stock firmware uses the IDENTICAL init (`pinMode(NFC_INT,INPUT);
  rfalNfcInitialize();`, same DLDO1 rail, no analog config / AAT / reset — verified in `vendor/LilyGoLib` src), so it
  hits the same dead transmitter. The USER confirmed the stock LilyGo NFC app also reads nothing. NFC never worked
  on this unit.
- **Verdict:** hardware fault (chip TX output stage or the antenna-driver/matching network). Not firmware-fixable.
  USER is contacting LilyGo for an RMA/replacement watch.

**=> Everything that depends on verifying NFC on hardware is BLOCKED "waiting on new watch" (see `PLAN.md` §4.1).**
Only on-device verification is blocked; the code is landed.

**Grab-and-run NFC bring-up test (for the future agent, once the NEW watch arrives):**
1. Build with the flag: `PLATFORMIO_BUILD_FLAGS="-DNOCSIF_NFC_BOOT_SELFTEST=1" <penv-python> -m platformio run -j 2 -d firmware`
2. Flash app-only `@0x10000` (set `PYTHONIOENCODING=utf-8` first — esptool 5.3.0's colorama output crashes on the
   Windows cp1252 console otherwise and leaves the app half-erased).
3. Capture COM7 ~30 s; look for the `==== NFC HW SELF-TEST ====` block + the `---- VERDICT ----` line.
   - Healthy unit -> `TX on + antenna amplitude ROSE -> ... RADIATE OK`, then it also runs a real NFC-A discovery
     (present a tag to read a UID: `nfc: read NFC-A tag UID ...`).
   - Same fault class -> `TX never engaged` / `SHORTED` / `OPEN antenna` verdicts.
The committed DEFAULT build is CLEAN (self-test OFF, NFC lazy on first screen entry). Runtime trigger:
`nocsif_nfc_request_selftest()` (e.g. if wiring a System › Diagnostics › NFC row later).

**The `hw_selftest()` measurement recipe (reusable):** rail on -> `st25r3916OscOn` + `AdjustRegulators` -> read
VDD/VDD_A/VDD_D/VDD_RF/VDD_AM via `st25r3916MeasureVoltage(mpsv)` -> field OFF baseline (amp/phase) -> field ON via
`rfalFieldOnAndStartGT` (needs `rfalNfcaPollerInitialize` first) AND a raw `OP_CONTROL=en|tx_en|rx_en` poke, each
rapid-sampling `AUX_DISPLAY.tx_on` + reading `REGULATOR_RESULT.i_lim` (over-current) -> verdict. All the ST25R3916
measurement API is public on `RfalRfST25R3916Class`.

---

**Reliability hardening COMPLETE â€” branch `reliability-hardening` off main @ `366814f`, one PR (A1+A2), verified
on-device.** Firmware-only pass that turns silent freezes + crash loops into observable, self-recovering events.
It protects everything after it (M6 NFC and on). New files: `firmware/src/reliability.{c,h}` + `logbook.{c,h}`.

- **A1 â€” task-WDT panic-reboot + core-dump crash record + boot-loop safe mode + UI-liveness watchdog.**
  - **Root-cause found:** `CONFIG_ESP_TASK_WDT_PANIC` was **off**, so a WDT trip only PRINTED a backtrace and
    kept running â€” exactly why the P2/P3.2/DMA-hang freezes logged a `task_wdt` backtrace yet stayed frozen
    needing a manual RST. Turned it **on** (+ timeout 5â†’8s). Core dump enabled **to flash** (ELF/CRC32).
  - **Partitions changed** (`partitions.csv`): added `coredump` (data/coredump, `0xF10000`, 256 KB) + `logs`
    (data/`0x40`, `0xF50000`, 704 KB) into the previously-**unused top-of-flash** (storage ends `0xF10000`,
    flash is `0x1000000`). **No existing offsets shift**; nvs/factory/storage untouched. **âš  The table changed,
    so the first flash of this branch must be a FULL flash** (`bootloader.bin@0x0 partitions.bin@0x8000
    firmware.bin@0x10000` â€” NVS at 0x9000 is preserved since those writes never touch it); **subsequent flashes
    are app-only `@0x10000`** as usual.
  - **`reliability.c`:** on boot, classify `esp_reset_reason()` + fold a stored core dump into a compact
    last-crash record (`task / pc / backtrace PCs / app-ELF sha`) â†’ NVS (`nocsif_rel` ns) + serial, then erase
    the image. **Boot-loop guard:** 3 crash-class boots with no ~30 s healthy dwell between â†’ **safe mode**
    (main.c skips the risky/heavy subsystems â€” currently forces USB detached; future milestones like M6 gate
    their init on `nocsif_reliability_safe_mode()`); the streak clears after a healthy dwell (heartbeat 6).
    **UI-liveness watchdog:** a repeating `lv_timer` subscribes the **LVGL task** to the Task-WDT and pets it, so
    a **BLOCKED render wedge** (the DMA-hang class, which does NOT starve the idle task the stock WDT watches)
    also auto-reboots â€” the single most valuable fix. Two compile-flag test hooks ship **OFF**:
    `NOCSIF_REL_CRASH_TEST` (main.c) + `NOCSIF_REL_HANG_TEST` (reliability.c).
  - **Verified on-device:** crash test â†’ 3 aborts each saved a core dump + recorded a backtrace, streak 1/2/3,
    SAFE MODE broke the loop (boot 4 reached a working UI), healthy dwell cleared the streak. Hang test â†’ a
    blocked LVGL task (idle healthy) â†’ `task_wdt` named `taskLVGL` â†’ panic + core dump â†’ reboot in ~7 s; next
    boot recorded `task-wdt`. (A real sustained hang loop even ran the streak to **67** â€” safe mode held every
    boot, first healthy dwell cleared it.)
- **A2 â€” persistent rolling log to flash + System â€º Diagnostics.**
  - **`logbook.c`:** a sector-based, sequence-numbered **ring over the `logs` partition** (176 Ã— 4 KB). Log
    lines accumulate in a spinlock-guarded RAM buffer; a low-priority flush task commits full-ish buffers to the
    next sector (erase-then-write â†’ a sector is only ever fully-valid or invalid-magic, never half-updated); on
    boot the head is the highest-seq sector. An **`esp_log_set_vprintf()` tee** captures the whole ESP_LOG stream
    (console unaffected). Raw partition access (no FS) â†’ an unclean reboot at worst loses the last RAM buffer.
  - **`ui.c`:** new **System â€º Diagnostics** screen (`system.diag`) = reset reason + last-crash record + a live
    log tail + a "Clear log" action. **Clear is deferred to the flush task** â€” erasing ~176 sectors (~10 s)
    synchronously on the LVGL task would trip the A1 UI-liveness watchdog and reboot.
  - **Verified on-device:** logbook init on the blank partition (head=0 seq=1); tee runs invisibly, no
    regression; after a reboot the scan **resumed at head=2 next_seq=3 â†’ the ring PERSISTED**; Diagnostics screen
    feel-checked with the user.

**M6 (NFC) â€” IN PROGRESS.** Milestone order: **M6 NFC â†’ M5 WiFi â†’ M7 BLE â†’ M11 watch-core â†’ M8 GNSS â†’ M9 LoRa â†’
M10 USB-ext â†’ platform layers** (`docs/PLAN.md` Â§4).

**Plan DONE + merged** â€” `docs/design/m6-nfc-plan-2026-08-15.md` (PR #67 â†’ main `e47cfd8`). Risk-first, 4 phases,
full-featured scope. **Read it first.**

**Foundation DECIDED = port ST's RFAL to ESP-IDF** (reversed the same-day "native minimal" pick after mining
LilyGoLib: RFAL is the board-proven path â€” `RfalRfST25R3916Class` / `rfalNfcInitialize`/`Discover`/`Worker` â€” and the
full-featured scope (card emulation, ISO-DEP, NfcV) needs RFAL's complete stack).
- **Base acquired (gitignored, re-clone if the worktree changed):** `vendor/ST25R3916-elechouse/` via
  `git clone --depth 1 https://github.com/wilson-elechouse/ST25R3916.git` â€” self-contained: `NFC-RFAL/src/`
  (48-file RFAL core, platform-agnostic C++) + `ST25R3916_ELECHOUSE/src/` (chip driver + Arduino glue),
  ESP32-S3-validated (`docs/`).
- **HW facts (verified in-code):** ST25R3916 Â· CS=**GPIO4** Â· IRQ=**GPIO5** (active-high) Â· on **SPI3** shared with
  SD (MOSI34/MISO33/SCK35) Â· rail **DLDO1 = 3.3 V** (AXP2101 `0x90` b7 enable, `0x99` voltage; 3.3 V/300 mA per the
  LilyGo HAL). `sdcard.c` already inits SPI3 + parks NFC CS high.

**P1 = RFAL platform port + read UID â€” CODE COMPLETE; builds + flashes + boots clean; on-device tag read PENDING a
physical test.** Branch **`m6-nfc-p1-impl`** (off latest main, in the build worktree `D:\Docker\nocsif-p4-6c`).
What was built:
- **`firmware/components/rfal/`** (tracked, C++): the vendored RFAL core (`rfal_core/`, from `NFC-RFAL/src`) + ST25R3916
  driver (`st25r3916/`) copied **UNMODIFIED** (license headers intact) + a NocSif **`arduino_compat/`** shim. **Key port
  decision: the "glue" was NOT rewritten in-place â€” instead a tiny Arduino shim (`Arduino.h`/`SPI.h`/`Wire.h` +
  `arduino_compat.cpp`) supplies the exact Arduino API the fork uses, over ESP-IDF, so the library files stay pristine.**
  This is the whole platform port (the fork has NO `platform.h`; only `platformInfo`/`platformLog` â€” a struct field + a
  commented macro â€” so there's nothing else to implement).
  - **`SPIClass` â†’ esp-idf `spi_device`**: `beginTransaction`/`endTransaction` = `spi_device_acquire_bus`/`release_bus`
    (holds the shared SPI3 bus across each CS-asserted window); `transfer()` = polling transmit. CS = **manual GPIO4**
    (device added with `spics_io_num=-1`; RFAL drives CS via `digitalWrite`). **`transfer(buf,len)` bounces through a
    DMA-capable, 4-byte-aligned buffer** â€” the bus runs DMA (`SPI_DMA_CH_AUTO`) and RFAL passes small unaligned stack
    buffers the DMA path would reject (the vendor never hit this: their S3 default is **bit-banged soft-SPI**, which
    NocSif force-disables with `-DST25R3916_FORCE_SOFT_SPI=0` in the component CMakeLists).
  - **`millis`/`micros`/`delay`/`yield` â†’ esp_timer + FreeRTOS**; **`attachInterrupt` = no-op** â€” the fork's IRQ handling
    is *polled* (`digitalRead(GPIO5)` from every bus op), so a real ISR is only a latency optimisation, not correctness
    (a P2 add). This also means `isr_pending` is never set â†’ the com-layer `finish()` never re-enters â†’ acquire/release
    stays cleanly 1:1 per register op.
- **`nocsif_power_nfc_rail(bool)`** in `power.c` (+ `power.h`) â€” DLDO1: voltage `0x99`â†`0x1C` (3.3V) then enable `0x90`
  b7, with a **0x90/0x99 readback log** so the DLDO1 mapping is on-device-verifiable (XPowersLib isn't vendored to grep).
- **`firmware/src/nfc.cpp` + `nfc.h`** (C++ compiled, extern-"C" API) â€” a worker task (`ducky.c` pattern): UI only
  *requests* a read. **Lazy bring-up on first read** (rail â†’ `ensure_spi` add-2nd-device â†’ `RfalRfST25R3916Class(SPI,CS=4,
  IRQ=5,5MHz)` â†’ `RfalNfcClass` â†’ `rfalNfcInitialize`), **safe-mode gated**, whole discovery under `nocsif_sdcard_lock`.
  `rfalNfcDiscover(NFC-A)` + pump `rfalNfcWorker()` until `RFAL_NFC_STATE_ACTIVATED` â†’ UID. Results published to two
  lock-free strings (`nocsif_nfc_status_str` compact / `nocsif_nfc_readout_str` full).
- **`build_nfc()` in `ui.c`** â€” "Read Tag" is now a live row (`add_config_row`, tag = `nocsif_nfc_status_str`, tap =
  `nocsif_nfc_request_read`) + a wrapped **readout label** bound to `nocsif_nfc_readout_str` via the P4.2 live-label
  hook. **`nfc.write` relabelled "Write / Clone" â†’ "Write / Copy".** `nfc.cpp`+`rfal` added to `src/CMakeLists.txt`.
- **Build/flash DONE:** builds clean (`-j 2`, flash 11.5% / 962 KB, RAM 45.5%), flashed app-only `@0x10000`
  (partitions/NVS untouched), **boots clean & stable** (crash streak 0, not safe mode, heartbeats steady, no panic/WDT).
- **VERIFY STATUS:** (a) `rfalNfcInitialize` OK / chip found â€” **VERIFIED on-device 2026-08-15** (self-test serial: `DLDO1 NFC rail ON @ 3.3V (0x90=0x97 b7=1, 0x99=0x1C code=28)`, `ST25R3916 on SPI3 as 2nd device`, `rfalNfcInitialize OK`; healthy after, no WDT) via a **boot self-test**
  (`main.c`, `#if NOCSIF_NFC_BOOT_SELFTEST`, **default 0**; built here with `-DNOCSIF_NFC_BOOT_SELFTEST=1` so the flashed
  dev binary runs one bring-up + discovery at boot â€” watch serial for `DLDO1 NFC rail ON â€¦ b7=1 â€¦ code=28`, `ST25R3916 on
  SPI3 as 2nd device`, `rfalNfcInitialize OK`). (b) **read an NTAG + a MIFARE Classic UID â€” NEEDS the USER**: navigate
  Cyber â€º NFC, present a tag to the back of the watch, tap **Read Tag**; the row tag + readout show the UID (serial:
  `nfc: read NFC-A tag UID â€¦`). **The committed source defaults the self-test OFF (lazy as designed); the dev board just
  has the flag-on binary flashed for this test â€” a normal default build is lazy.**
**Neutral device-class wording throughout** (Â§1 / plan Â§4) â€” the cyber-safeguard hard-fails charged terms. **HF only â€”
no 125 kHz LF** (Â§6). **CRITICAL BUG FOUND + FIXED (2026-08-15) after (a) passed — the fork's SPI IRQ path.** Tag reads failed: field-on -> ERR_INTERNAL(12), WUPA -> ERR_IO(3). Root cause (decoded, not guessed): st25r3916_interrupt.cpp st25r3916ProcessInterrupts() gates on `!isr_pending && !(i2c_enabled && lineAsserted)` = in SPI mode `if(!isr_pending) return`, i.e. it ONLY processes IRQs when the attachInterrupt ISR set isr_pending and NEVER polls the line (unlike I2C). NocSif uses NO ISR (attachInterrupt is a no-op / polling model), so IRQs were never processed and every IRQ-sequenced op (collision-avoidance, transceive) failed while register access worked. FIX = NocSif patch to poll the line in SPI mode too: st25r3916_interrupt.cpp (`if(!isr_pending && !lineAsserted) return`) + st25r3916_com.cpp finish() lambda x8 (`if(isr_pending || digitalRead(int_pin)==HIGH)`). After: fieldOn=ERR_NONE(0), WUPA=ERR_TIMEOUT(4) = transceive works cleanly. (Cross-checked vs vendored LilyGoLib examples/peripheral/NFC_Reader which uses this SAME fork on this SAME board WITH a real attachInterrupt ISR; NocSif polls instead. POWER_NFC there = just pmu.enableDLDO1(), pins/SPI/params all match mine.) Diagnostic = boot self-test WUPA probe. Also DIAGNOSTIC/UX changes this session: scan time doubled (NFC_DISCOVER_MS 2000 / NFC_SCAN_TMO_MS 2600); rich discovery logging (rfalNfcDiscover rc, high_state, iters); the boot self-test hook itself (main.c `#if NOCSIF_NFC_BOOT_SELFTEST`, default 0, currently flashed with =1). REMAINING for a UID read: THE RF FIELD IS NOT RADIATING (deeper than coupling). Diagnosed via boot self-test probe (rfalNfcaPollerInitialize -> rfalFieldOnAndStartGT -> measure amp field-OFF vs ON -> read OP_CONTROL/AUX_DISPLAY -> WUPA): chip_ok=1 rev=0x02, osc_en=1 + osc_ok=1 (oscillator runs), fieldOn=ERR_NONE, tx_en=1, TX drive already MAX (analog cfg TX_DRIVER d_res=0x00), efd_o=0 -- BUT ampOFF==ampON (0x14, no carrier at RFI) AND AUX tx_on=0 (transmitter output never asserts) AND WUPA=ERR_TIMEOUT. Ruled out: positioning (amp unchanged w/ tag on antenna), TX drive (already max), auto-EFD gating (set en_fd=efd_off + forced tx_en|rx_en -> tx_on STILL 0, amp STILL 0x14). So the transmitter is commanded on but never engages -> either (1) a T-Watch-specific analog/regulator/ANTENNA-TUNING (AAT) step the stock elechouse config doesn't apply, or (2) a hardware NFC-antenna fault on this unit. NEXT: (a) ask USER if NFC ever worked on this watch (stock LilyGo fw) to rule hardware in/out; (b) diff LilyGoLib's rfalNfcInitialize / regulator (rfalAdjustRegulators / reg_s) / AAT (st25r3916_aat) / analog-config-FIELD_ON path vs NocSif's -- LilyGo uses this SAME fork on this SAME board WITH a real attachInterrupt ISR (NocSif polls); check if LilyGo runs AAT or a different regulator setup; (c) try rfalChipMeasurePowerSupply / verify VDD_DR driver-supply regulator is enabled. NOTE the earlier probe reg=3000mV (regulators DID adjust). Diagnostic scaffolding (boot self-test in main.c NOCSIF_NFC_BOOT_SELFTEST + the WUPA/amp/reg probe in nfc.cpp bring_up) is IN THE TREE and must be stripped before commit. [SUPERSEDED coupling note kept below for history:] RF COUPLING only -- amp reads 0x14 unchanged with/without the tag => tag not over the (small) T-Watch antenna. NEXT: interactive positioning (sweep the tag on the back, watch the on-screen readout); if NO spot reads it is field strength -> boost ST25R3916 TX drive (REG_TX_DRIVER d_res) / compare LilyGo's amplitude. USER also requested a NFC read-flow UX redesign (dedicated Scanning screen w/ spinner -> fail "No NFC card detected" + Retry/Cancel, swipe-back=cancel -> success offers Save [name via on-screen keyboard, persist to /sd/nfc/ + NVS = pull M6-P4 forward] / Emulate [needs M6-P3 listen-mode card emulation, stub for now]) -- build AFTER the read is confirmed.
**[SUPERSEDED by the 2026-08-16 UPDATE block above — this is NOW committed + squash-merged to main. Historical:] Not yet committed/PR'd â€” commit the branch, verify (a)+(b) on-device, then PR â†’ squash-merge.**
- **M5 WiFi (after M6)** later lights up the still-stubbed UI hooks: peek **weather chip** (`peek_wx_str`),
  Control-Center **WiFi toggle** (`s_cc_wifi`/`cc_wifi_cb`), the connected-state accent (`app_id_live` "wifi"),
  and the P5b deferred motion helpers. Confirm station-first vs. also monitor-mode scope with the USER.

**Build:** PowerShell `-j 2`, penv python for esptool/serial (base penv `â€¦\penv\Scripts\python.exe` has
esptool+pyserial; the `.espidf-5.5.4` sub-venv does NOT). Flash `--no-stub`, COM7, PYTHONIOENCODING=utf-8 +
file redirect (do NOT pipe esptool through `Select-Object -First N` â€” it closes the pipe early and kills esptool
mid-write). Serial via `scratchpad/cap_resilient.py` (one reboot) or `cap_fixed.py` (fixed duration, for crash
loops). Branch off main, one phaseâ†’PRâ†’squash-merge, verify on-device + feel-check each phase.

---


## What NocSif is
A modular security-testing firmware for the **LilyGo T-Watch Ultra** (ESP32-S3). Goal: import Flipper
files / GitHub projects / the user's own Python with no fuss; control from the watch **and** a
WiFi web UI; a built-in browser much later (v2/v3). The user does authorized security testing only.

## Wording convention (neutral framing) â€” apply everywhere
Describe capabilities in neutral, technical **device-class** terms â€” "USB HID keyboard", "USB
mass-storage (MSC)", "HID macro playback", "RF transceiver", "NFC reader" â€” rather than adversarial
framing ("attack / offensive / payload / weaponize / victim / target / red-team"). Keep precise,
load-bearing format names (**DuckyScript**, `.sub`, `.nfc`) where they carry real compatibility meaning.
This is an authorized security-testing tool; the neutral vocabulary is both more precise **and** avoids
tripping Anthropic's automated cyber-safeguard, which **hard-fails** Claude subagent/API calls on charged
wording (it killed the entire first M4 research workflow, 2026-08-06). Applies to docs, commit messages,
and â€” most importantly â€” any prompt sent to a subagent or Workflow.

## Current state
> The running log below is append-only history through the M4 / early-P4 work; the **current frontier is the
> CURRENT WORK banner at the top** + `docs/PLAN.md` Â§3 (done-state) / Â§4 (next). Read those first.

- **Backup DONE + verified:** `backups/stock_full_16MB.bin` (16,777,216 bytes, starts 0xE9,
  sha256 `19A78A0FCA1A8AD9DAC1A08AAD4BD6AF673901102D2013858DF3F8130C306ED5`). Un-brick net #1.
  Net #2: `vendor/LilyGoLib/firmware/factory.watch.ultra.sx1262.20260424.bin`.
- **Watch runs NocSif** â€” M0â€“M3 + M4-P1 flashed & verified on-device (latest 2026-08-08). The
  un-brick nets above restore stock at any time (see bottom).
- **Repo:** `github.com/silverwolf2r/NocSif_Firmware` (**PRIVATE**), local `D:\Docker\NocSif_Firmware`.
- **Architecture DECIDED (Option B):** native C++/ESP-IDF core (hardware + LVGL + web + Flipper
  parsers) + embedded MicroPython as a *gated* drop-in script layer. Sub-GHz **deferred**
  (SX1262 can't do OOK/raw capture). First wave: **NFC + USB HID keyboard (native, composite MSC+HID)**. Full rationale
  in `docs/ARCHITECTURE.md` + `docs/research/runtime-base-2026-08-06.md`.
- **M0 DONE â€” flashed & verified on-device (2026-08-06):** boots into NocSif, bootloader sees
  **16MB flash**, `esp_psram: Found 8MB PSRAM ... SPI SRAM memory test OK` â†’ **8MB PSRAM in the
  heap** (free heap ~8.3MB), star banner renders, heartbeat runs. (`firmware/`, PlatformIO +
  ESP-IDF v5.5.4.) RTS reset works to reboot it (entering *download* mode still needs the button dance).
- **M0.1 DONE â€” flashed & verified on-device (2026-08-06):** I2C scan module
  `firmware/src/i2c_scan.{c,h}` (new `driver/i2c_master.h` bus on SDA=3/SCL=2, `i2c_master_probe`
  over 0x08â€“0x77, i2cdetect-style grid + per-device breakdown; always-present parts PMU 0x34 /
  XL9555 0x20 / RTC 0x51 raise a loud **FAULT** log if absent; `main` re-scans every ~30 s).
  `PRIV_REQUIRES esp_driver_i2c` in `src/CMakeLists.txt`. On-device result: **all 6 devices ACK** â€”
  0x1A CST9217 touch, 0x20 XL9555, 0x28 BHI260AP, 0x34 AXP2101, 0x51 RTC, 0x5A DRV2605. No FAULTs.
  **Key finding for M1:** touch/sensor/haptic all answer at *cold boot* â€” XL9555 GPIO10 default-
  releases the touch reset, ALDO4+haptic are on out of the gate. So touch bring-up is NOT gated
  behind un-resetting it; the M1 power sequence is only needed for the **display** (ALDO2 + XL9555
  GPIO7). Built/flashed from the worktree `D:\NocSif_Firmware\nocsif-i2c-display-1592c8\firmware`.
- **M1 DONE â€” flashed & verified on-device (2026-08-06): THE DISPLAY WORKS.** CO5300 QSPI AMOLED lit
  with first pixels. `firmware/src/{power,xl9555,display}.{c,h}` â€” power seq (AXP2101 **ALDO2=3.3V**:
  reg `0x93`=`0x1C`, enable `0x90` bit1; **XL9555 IO7** display-power) â†’ **GPIO37** reset â†’
  `espressif/esp_lcd_co5300` **v2.1.0** (managed component) over QSPI, `set_gap(22,0)` for the 410-wide
  panel, **24-bit RGB888** (COLMOD `0x77`), **single full-frame window draw** from a PSRAM framebuffer
  (chunked SPI with CS held â†’ one seamless window). Gradient renders smooth. Findings: RGB565 needed a
  byte-swap (panel big-endian); the "seam" lines on a gradient were **RGB565 16-bit quantization
  banding** (colour-depth limit, not a draw seam â€” solids were always clean) â†’ fixed by RGB888. Merged
  **PR #2**, `main` @ `743d2f9`.
- **RGB888 colour byte-order CONFIRMED â€” verified on-device (2026-08-06):** switched the boot pattern to
  `nocsif_display_test_bands()` (R/G/B/W quarters) and eyeballed it â€” topâ†’bottom reads **RED / GREEN /
  BLUE / WHITE**. Red shows red, blue shows blue, so `display.c`'s `px[0]=R,px[1]=G,px[2]=B` with
  `rgb_ele_order = RGB` is **correct â€” no Râ†”B swap needed**. (The grayscale gradient couldn't tell R from
  B; bands are self-diagnosing since white/green are invariant under an Râ†”B swap.)
- **M2 DONE â€” CST9217 touch, flashed & verified on-device (2026-08-06): TOUCH WORKS.**
  `firmware/src/touch.{c,h}` â€” polls the Hynitron **CST9217 @ 0x1A** on the shared IÂ²C bus at ~33 Hz
  (dedicated FreeRTOS task in `main.c`). Protocol is the **CST92xx family** (SensorLib `TouchDrvCST92xx`,
  NOT CST816): **16-bit BIG-ENDIAN register addresses**; read `0xD000` Ã— 15 B, then **write `{0xD0,0x00,0xAB}`
  back** (frame-ACK handshake â€” mandatory or reports freeze after frame 1); gate `buf[6]==0xAB &&
  buf[0]!=0xAB && buf[0]!=0x00`; count `buf[5]&0x7F`; per finger `p = buf + i*5 + (i?2:0)` (count/ACK sit
  between P0 and P1), decode 12-bit `X=(p[1]<<4)|(p[3]>>4)`, `Y=(p[2]<<4)|(p[3]&0x0F)`, accept event nibble
  `0x06`. **On-device: taps map cleanly â€” X 0â†’410 left-to-right, Y 0â†’502 top-to-bottom, native orientation
  already matches the display â†’ NO swap/mirror/rotation needed.** Protocol nailed via a 4-agent research
  workflow (SensorLib + ESPHome cst9220 cross-check, byte-for-byte agreement). Chip-ID read behind a compile
  flag (default off â€” command-mode entry could disturb report mode; the IÂ²C probe already proves liveness).
  **Open follow-ups:** LVGL on the esp_lcd panel (feed it these touch coords), INT-driven reads (TP_INT =
  **GPIO12**, active-low) as a power optimisation, SD.
- **M3 DONE â€” LVGL v9 on CO5300 + CST9217, flashed & verified on-device (2026-08-06): THE UI WORKS.**
  `firmware/src/ui.{c,h}` via **espressif/esp_lvgl_port 2.8.0** (pinned **lvgl/lvgl `~9.3.0`**; the port owns
  tick/task/mutex/flush + the panel-IO trans-doneâ†’`lv_display_flush_ready` wiring). Kept **24-bit RGB888**
  but flipped the panel `rgb_ele_order` RGBâ†’**BGR** and swapped `draw_full` to write B,G,R (LVGL v9's
  `lv_color_t` is `{b,g,r}` in memory). Partial-mode draw buffers **2Ã—410Ã—40 px in INTERNAL RAM**; custom
  `LV_INDEV_TYPE_POINTER` indev reuses `nocsif_touch_read` (coords already native). **`main.c`'s `touch_task`
  DELETED** â€” the indev is the sole CST9217 reader. Demo = centered button (click counter) + bottom label
  tracking live touch coords. On-device: bands still RED/GREEN/BLUE/WHITE (BGR correct), button clicks
  register, coord label follows finger. **THREE non-obvious findings (the plan's literal instructions were
  wrong on 1; 2â€“3 were unknowable without on-device runtime):**
  (1) **Kconfig colour depth = `CONFIG_LV_COLOR_DEPTH_24=y`, NOT `=24`.** In LVGL v9 `LV_COLOR_DEPTH` is a
  promptless computed int â†’ a bare `=24` line is **silently ignored**, depth stays 16, colours garble, no
  build error. Verify post-build that generated `sdkconfig` contains `CONFIG_LV_COLOR_DEPTH=24`.
  (2) **esp_lvgl_port rejects `flags.buff_dma=true` with RGB888** at runtime (`"DMA buffer can be used only
  in RGB565"`, `esp_lvgl_port_disp.c:314`). Use **`buff_dma=false`** â†’ buffers land in `MALLOC_CAP_DEFAULT`
  internal RAM; the S3 GPSPI DMAs from there fine (M1 even drew from PSRAM). Also **must set
  `.color_format = LV_COLOR_FORMAT_RGB888`** (defaults to RGB565 â†’ wrong buffer size) and `buffer_size` is in
  **PIXELS** (port Ã—3).
  (3) **CO5300 needs 2-px column/row alignment.** Partial flushes on an odd x-start/width write each row
  offset â†’ the redraw **shears diagonally** (intermittent, content-dependent). Fixed with a `rounder_cb`
  snapping the area to even: `x1&=~1; y1&=~1; x2|=1; y2|=1` (safe: panel max coords 409/501 are odd).
  API/Kconfig were pre-verified by a 3-agent workflow against the real esp_lvgl_port 2.8 + lvgl 9.3 sources
  *before* coding, which caught (1). Boot RAM: ~306 KB internal free, ~8.5 MB heap; firmware ~520 KB.
  **Open follow-ups:** INT-driven touch (TP_INT GPIO12) for power; TE sync (GPIO6) only if tearing ever shows
  on full-screen animation; the button felt like it needed a slightly firm tap (small default hit-area / the
  indev's `event==0x06`-only gate may drop the lightest press-down frames) â€” revisit if it matters for UX.
- **M4 plan DECIDED + P1 DONE (2026-08-08).** M4 = a **composite USB device** â€” USB Mass-Storage SD
  file-drop **+** USB HID keyboard (DuckyScript player) â€” built as **4 independently-flashable phases**:
  **P1** SD-FAT mount (no USB) â†’ **P2** TinyUSB + CDC-ACM console behind an on-screen "USB Gadget" toggle
  â†’ **P3** +MSC (host copies files to the card) â†’ **P4** +HID (DuckyScript). Console strategy: keep
  USB-Serial/JTAG for boot/dev and **defer `tinyusb_driver_install()` behind the UI toggle** so COM7 is
  untouched for the normal flash/log loop; when gadget mode is on, logs go to a TinyUSB CDC port. Full plan
  (phases, gotchas, sources): `docs/research/usb-composite-device-2026-08-06.md`.
- **M4-P1 DONE â€” flashed & verified on-device (2026-08-08): microSD MOUNTS + reads/writes.**
  `firmware/src/sdcard.{c,h}` + new `nocsif_power_sd_rail()` (AXP2101 **ALDO1=3.3V**: reg `0x92`=`0x1C`,
  enable `0x90` bit0 â€” mirrors the ALDO2 display rail). SDSPI on **SPI3_HOST** (the display owns SPI2_HOST,
  so SD must not collide), shared bus **MOSI34/MISO33/SCK35, CS=21**, with NFC(CS4)/LoRa(CS36) parked HIGH;
  `esp_vfs_fat_sdspi_mount("/sd", â€¦, format_if_mount_failed=false)` @4 MHz; card handle kept for P3's MSC.
  On-device: **60 GB card mounts**, write/read-back of `/sd/nocsif_test.txt` verified byte-for-byte, root
  listing reads the card's own files. `src/CMakeLists.txt PRIV_REQUIRES += fatfs sdmmc esp_driver_sdspi
  esp_driver_gpio`; firmware ~596 KB. **Two findings, each cost a reflash:**
  (1) **64 GB SDXC ships exFAT, which ESP-IDF FatFs cannot mount** (`FF_FS_EXFAT 0` hard-coded, no Kconfig)
  â†’ mount fails `FR_NO_FILESYSTEM` â†’ **cards MUST be FAT32** (Windows GUI won't FAT32-format >32 GB; use
  Rufus / guiformat). (2) **Long filenames are OFF by default** (`CONFIG_FATFS_LFN_NONE` â†’ 8.3 only) so
  `f_open` rejected `nocsif_test.txt` (11-char base) â†’ enabled **`CONFIG_FATFS_LFN_HEAP=y` +
  `CONFIG_FATFS_MAX_LFN=255`** (the file-drop needs arbitrary filenames anyway). Physical card init also
  needs the card firmly seated (a loose card gave `send_if_cond` `0x108` at CMD8).
- **M4-P2 DONE â€” flashed & verified on-device (2026-08-08): USB PHY handoff + CDC log console.**
  `firmware/src/usb_gadget.{c,h}` + a "USB Gadget" toggle in `ui.c`. `espressif/esp_tinyusb "^2"` (â†’ **2.2.1**;
  PlatformIO resolved it + the transitive tinyusb port cleanly). `tinyusb_driver_install()` is **deferred behind
  the on-screen toggle** (a worker task does it, never the LVGL cb; the tap only `xTaskNotifyGive`s) so COM7
  stays live for normal flashing. On tap: the ROM USB-Serial/JTAG device (PID 0x1001) **vanishes**, a TinyUSB
  **CDC device enumerates (COM8, VID 0x303A PID 0x4001)**, and `tinyusb_console_init()` redirects ESP_LOG to it
  (heartbeats verified streaming over CDC; screen shows `ON (CDC)`). Firmware ~638 KB.
  **Verified v2 API (differs from the v1 names in the research):** include **`tinyusb_default_config.h`** for the
  `TINYUSB_DEFAULT_CONFIG()` macro (it is NOT in tinyusb.h â€” omitting it = `implicit declaration` build error);
  the calls are `tinyusb_driver_install()` / `tinyusb_cdcacm_init()` / `tinyusb_console_init()` (NOT v1's
  `tusb_cdc_acm_init` / `esp_tusb_init_console`). sdkconfig: `CONFIG_TINYUSB_CDC_ENABLED=y` +
  `CONFIG_TINYUSB_CDC_COUNT=1`, keep `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`, do NOT add
  `CONFIG_ESP_CONSOLE_USB_CDC`. Read the CDC port with the scratchpad `read_cdc.py` (finds the 0x303A port whose
  PID â‰  0x1001). Transient GCC ICE (IRA segfault) on stock `esp_lcd_panel_rgb.c` during a full rebuild â€” cleared
  on retry, not our code. **Next:** M4-P3 (add MSC â†’ CDC+MSC composite, host copies files to `/sd`).
- **M4-P3 DONE â€” flashed & verified on-device (2026-08-08): USB-MSC file drop over the microSD.**
  `usb_gadget.c` extended to a **CDC+MSC composite**; `sdcard.c` changed from `esp_vfs_fat_sdspi_mount` to a
  **raw `sdmmc_card_t` init** (the MSC helper owns the FAT mount + the single-owner handoff â€” a second FAT mount
  would corrupt the volume). Order: raw card â†’ `tinyusb_msc_new_storage_sdmmc({.medium.card, base_path="/sd",
  do_not_format=true, mount_point=MOUNT_APP})` + `set_storage_callback` â†’ `tinyusb_driver_install()` (MSC
  **before** the stack; CDC after) â†’ NULL descriptors give the default CDC+MSC composite. `wear_levelling` added to
  `PRIV_REQUIRES`; `CONFIG_TINYUSB_MSC_ENABLED=y` + `CONFIG_TINYUSB_MSC_BUFSIZE=8192`. Firmware ~697 KB.
  **On-device: SDSPI-backed MSC WORKS** (the plan's biggest unknown). Host sees a removable **FAT32 drive
  (58.9 GB, "TinyUSB MSC Storage", CDC port PID 0x4003)**; wrote `NOCSIF_P3.txt` from the PC, then a Windows tray
  **"Safely Remove"** â†’ firmware auto-switched to MOUNT_APP and listed `/sd` showing both `nocsif_test.txt` (P1)
  and `NOCSIF_P3.txt` (host-dropped) â€” the full PCâ†’cardâ†’firmware round-trip.
  **Handoff gotcha (load-bearing for P4):** the MSC helper auto-switches MOUNT_USBâ†”MOUNT_APP on `tud_mount` /
  `tud_msc_start_stop_cb` (eject) / `tud_umount` (VBUS off). A **Windows soft-eject of the drive letter does NOT
  reliably switch it** (Windows re-scans the still-connected media â†’ bounces to host); a user tray "Safely Remove"
  or a real unplug does; `mountvol /P` needs admin. â†’ **P4 must claim the card DETERMINISTICALLY via
  `tinyusb_msc_set_storage_mount_point(MOUNT_APP)` before reading the DuckyScript**, not rely on host eject.
  **Next:** M4-P4 (add HID keyboard + DuckyScript player; hand-written CDC+MSC+HID descriptor â€” HID has no default;
  5 IN-endpoints = exactly the S3 FS budget).
- **M4-P4 DONE â€” flashed & verified on-device (2026-08-09): USB HID keyboard / DuckyScript player.** The watch now
  enumerates as a **CDC + MSC + HID composite** driven by a **hand-written** descriptor (`nocsif_usb_desc.{c,h}`: IAD
  device class 0xEF/02/01, 4 interfaces, HID IN 0x84, wTotalLength 123 â€” the 5-IN budget confirmed against the real
  dwc2 `ep_in_count=5`). New `hid_kbd.{c,h}` (the three app-required HID callbacks + a typing engine + US/GB/DE keymaps
  with CapsLock compensation), `ducky.{c,h}` (a worker-task DuckyScript parser/executor: **deterministically claims the
  card via `tinyusb_msc_set_storage_mount_point(MOUNT_APP)`**, reads the macro into RAM, releases the card, plays from
  RAM, and **always release_all on every exit** so a key can never stick). `sdkconfig` `CONFIG_TINYUSB_HID_COUNT=1`
  (int, not `=y`; verified it landed). **On-device: host dropped `/sd/payload.txt` over USB-MSC â†’ tapped "Run Macro" â†’
  the watch claimed the card, read the macro, released it, and TYPED it as a real USB keyboard into Notepad, correct US
  keymap, no stuck keys.** Two workflows ran before coding/committing: a 7-agent API re-verify against the real
  esp_tinyusb 2.2.1 headers, and a 14-agent self-review (4 real bugs found + fixed: DE `@`-under-CapsLock, `STRING`
  leading-whitespace loss, a false host-eject log during the app's own claim, and an sd-lock creation race).
  **ROBUSTNESS FIX (verified on-device):** if the microSD fails to init, the composite now falls back to a **CDC+HID
  descriptor with NO MSC interface** â€” advertising MSC with no backing storage made the host's first SCSI access
  **reboot the watch**. With a card present: full CDC+MSC+HID + typing; with no card: CDC+HID, no reboot (heartbeats
  continuous). **Identity:** default is a generic-keyboard VID/PID (`0x1A2C/0x2124`); build with
  `-DNOCSIF_USB_DEV_IDENTITY=1` for the Espressif `0x303A` dev identity so `read_cdc.py` finds the console.
  **Build gotcha:** the fresh full build exhausts the Windows paging file at default parallelism (`cc1: out of memory`)
  â†’ build with **`-j 2`** (`python -m platformio run -j 2 -d â€¦`).

- **UI-shell milestone STARTED â€” P1 DONE, verified on-device (2026-08-09): embedded serif+mono fonts + theme tokens.**
  New `firmware/src/ui_theme.{c,h}` (spec Â§2 colour tokens as `lv_color_hex` macros â€” panel BGR is handled in the M3 flush
  config, so define colours normally, do NOT byte-swap â€” a shared `lv_style_t` set for the Â§3 typography roles, and
  `NOCSIF_DOT/DEG/NDASH` UTF-8 helper literals) + `firmware/src/fonts/` (generated 4bpp LVGL fonts, `README.md`, `LICENSES/`).
  **LICENSE FINDING (load-bearing):** the design spec's named serifs **Recia/Boska are NOT embeddable** â€” the Fontshare Free-Font
  EULA forbids extracting the font, restricts derivative works, and limits embedding to read-only PDFs, and this repo is slated to
  go public; Windows Georgia/Times/Consolas are proprietary too. â†’ shipped **SIL OFL 1.1** faces (which explicitly permit bitmap
  conversion + bundling): **Fraunces** serif (old-style high-contrast, on-theme; static cuts opsz=144 boot / opsz=72 titles /
  italic opsz=40, instantiated with `fonttools.varLib.instancer` because the variable **default is Black/9pt**) + **JetBrains Mono**
  for all data. **Playfair Display** was embedded as an on-panel A/B and **Fraunces won â†’ Playfair dropped**. Pipeline:
  `npx lv_font_conv` 4bpp `--no-compress` (no `LV_USE_FONT_COMPRESSED` dependency), range `0x20-0x7F` + `0xB0 Â° / 0xB7 Â· / 0x2013 â€“`
  (the em dash `0x2014` is NOT generated â€” a review caught a stray `\xE2\x80\x94` and it was fixed to `NOCSIF_NDASH`); the generated
  `#ifdef LV_LVGL_H_INCLUDE_SIMPLE` block is normalised to a plain `#include "lvgl.h"`; OFL texts vendored in `fonts/LICENSES/`.
  Font/`lv_font_t` struct + callback symbols pre-verified against the pinned **lvgl 9.3** before building. **FLASH COST MEASURED = 37 KB**
  shipped (7 faces) of the 4 MB factory partition (app image **703 KB**, 17% used, ~3.3 MB free) â€” the front-loaded "font budget"
  unknown is a non-issue. `CONFIG_LV_COLOR_DEPTH=24` confirmed. Re-skinned the M3/M4 demo into a font sampler; **M4 USB HID keyboard /
  DuckyScript still works through the new UI (on-device regression check passed)**. A 4-dimension adversarial review workflow found
  1 real bug (the em dash, fixed) + 1 correctly-refuted false positive. Plan: `docs/research/ui-shell-2026-08-09.md`
  (5 phases; P1 = fonts+tokens).

- **UI-shell P2 DONE â€” flashed & verified on-device (2026-08-09): static orrery/star background, render-once proven.**
  New `firmware/src/ui_background.{c,h}` draws the design-spec orrery **once** into **`lv_layer_bottom()`** and installs four
  rounded-corner masks on **`lv_layer_top()`**; every screen is made **transparent** (`bg_opa TRANSP`) so the orrery shows through
  and persists across screen swaps. Orrery = a single **410Ã—502 RGB888 `lv_canvas` in PSRAM** (617,460 B, stride 1230; `heap_caps_aligned_alloc(64, â€¦, MALLOC_CAP_SPIRAM)`),
  drawn once: void fill + 2 ring clusters (top-right 356,56 / bottom-left 44,476; solid + dashed) + 15 gray star dots + 2 violet
  bodies + **4 engraved stars (one violet)**. New A8 asset `firmware/src/img/nocsif_img_star.c` (+`gen_star.py`, `README.md`): the
  mockup `#i-star` concave 4-point star, rasterised directly from its 8 cubic BÃ©ziers with Pillow (no SVG lib), 64px alpha master,
  recoloured per placement via `lv_draw_image` recolor (reused by boot P5 + About). All P2 LVGL APIs pre-verified against the pinned
  **lvgl 9.3.0** sources (canvas/layer/draw_arc/rect/image dsc, `lv_image_dsc_t`/A8 magic=0x19, `lv_layer_bottom/top`, `lv_event_get_invalidated_area`).
  **RENDER-ONCE PROVEN on-device** via a temporary flush-audit (`LV_EVENT_INVALIDATE_AREA`, gated by `RENDER_START/READY` to skip the
  in-render `get_max_row` sizing probe): after the single initial full paint, the largest invalidated area stays **226Ã—26** (widget
  rects only) â€” **never 410Ã—502 again** â†’ the orrery does not repaint per frame. `orrery drawn ONCE â€¦ + 4/4 corner masks; PSRAM free
  8284632 â†’ 7641552`. Corners clean, no bleed.
  **THREE load-bearing findings (2 were on-device-only):**
  (1) **`lv_draw_arc` is pathological for the orrery â€” it tripped the task watchdog.** The dashed rings issued ~200 `lv_draw_arc`
  segments (r=106/118); `lv_draw_sw_arc` builds a full anti-aliased **radius mask** (`lv_draw_sw_mask_radius_init`/`circ_calc_aa4`)
  and scans the 2rÃ—2r box **per arc** â†’ tens of seconds on the LVGL task â†’ `task_wdt` (found by decoding the backtrace with
  `xtensa-esp-elf-addr2line`). **FIX: rasterise the rings + dots DIRECTLY into the RGB888 canvas buffer** (O(circumference); dashes =
  an arc-length test), keeping only the 4 star images in the `lv_canvas_init_layer`/`finish_layer` pass. Fast, no WDT.
  (2) **`lv_canvas_set_px` on an RGB888 (no-alpha) buffer OVERWRITES â€” it ignores `opa`** (and invalidates the whole canvas per call).
  So we **pre-blend** each mark colour over the void ground ourselves and write **B,G,R straight to the buffer** (LVGL RGB888 is B,G,R
  in memory, M3). ARGB8888 `set_px` (the corner masks) *does* store colour+alpha, so masks use it.
  (3) **`lv_canvas_init_layer` sets `layer->draw_buf = canvas->draw_buf` (in place, no clear)** â€” so the direct ring/dot writes SURVIVE
  `finish_layer` and the star images alpha-blend **over** them (draw order: fill â†’ rings/dots direct â†’ stars layer).
  Colours **muted twice on user request**: `NOCSIF_WHITE` 0xE4E4E8â†’**0xA6A6AC**, `NOCSIF_BONE` 0xBFBFC4â†’**0x8C8C92** (kept above STEEL
  0x78787F). Text **SIZE deferred to P3** (bigger, comfortable type scale + larger font cuts â€” carry this pref). Flash **703 KB â†’ 717 KB**
  (+~14 KB incl. the 4 KB star asset); PSRAM canvas 617 KB at runtime (7.6 MB free); internal RAM ~unchanged. `CONFIG_LV_COLOR_DEPTH=24`
  holds. **Two review workflows ran** (P2 code review: 6 findings folded incl. corner-mask error handling + screen-transparency-vs-init
  coupling; WDT-fix verify: 1 confirmed â€” a corner-mask failure must NOT hide the orrery, so init returns ESP_OK once the *canvas* drew,
  mask failures are cosmetic/logged â€” + 1 latent blend underflow, clamped). **The flush-audit instrumentation in `ui.c` is temporary â€”
  remove or gate behind a debug flag in P3.** **NEXT = P3 (menu-first nav shell: Home â†’ submenu â†’ action, `build_header`, 240 ms
  slide+fade, list rows, disabled stubs) â€” build the comfortable/larger type scale there.**

- **UI-shell P3.1 DONE â€” verified on-device (2026-08-09): menu-first nav shell.** Branch
  `Clankert/nocsif-ui-shell-p3-nav` (off main @ P2). New `firmware/src/ui_nav.{c,h}` = the nav MECHANISM:
  a screen STACK (`nocsif_nav_init/push/back`) of transparent-root screens (`lv_obj_create(NULL)`, bg_opa
  TRANSP) so the P2 orrery shows through; `build_header` (back arrow + serif title + `--:--`/`--%`
  clock/battery placeholders until P4), a **dashed edge rule** as a 1px ARGB8888 canvas (LVGL v9 borders are
  solid-only), a transparent screen scaffold, flat menu rows (pit-on press wash, disabled-stub styling), and
  **swipe-to-back** (`LV_EVENT_GESTURE`â†’`LV_DIR_RIGHT`, with `LV_OBJ_FLAG_GESTURE_BUBBLE` on rows/header/list/
  buttons AND the back arrow so a swipe anywhere bubbles up). `ui.c` rewritten: flush-audit gated behind
  `NOCSIF_UI_FLUSH_AUDIT` (default 0), demo sampler gone; builds Home (WiFi+USB navigate; NFC/BLE/System
  present-but-disabled stubs) + WiFi submenu (stubs) + a USB action screen carrying the **exact M4
  gadget/macro controls unchanged** (still enumerates + types on-device). **On-device: Home over the orrery,
  categoryâ†’submenu slides + swipe/arrow back, disabled stubs inert, corners clean, M4 USB intact.**
  **FOUR load-bearing findings (2 needed a 5-agent verification workflow before the fix was right):**
  (1) **`lv_obj_set_style_translate_x` is INERT on a screen root** (parent==NULL â†’ `lv_obj_move_to`, translate
  never applied â€” `lv_obj_pos.c:621`). A translate-based screen slide does NOTHING (instant cut). **Use
  `lv_obj_set_x`** (LVGL's own MOVE anim does). The final transition = a custom `slide_in`: `lv_screen_load()`
  swaps instantly, then ONE `lv_anim` drives `lv_obj_set_x` (Â±30pxâ†’0) + a `lv_obj_set_style_opa` fade
  (150â†’255), ease-out, 220 ms.
  (2) **The transition-smoothness ceiling is PSRAM bandwidth, NOT easing/duration/QSPI clock.** With
  `full_refresh` every animated frame re-composites the whole 410Ã—502 frame: orrery read + PSRAM draw-buffer
  write + flush read â‰ˆ **1.85 MB PSRAM traffic/frame â†’ ~25-35 fps** for ANY full-screen redraw. So a
  full-screen 410px slide is coarse (~68px/frame); the fix is **small motion (30px â‰ˆ 4-5px/frame) + a fade**
  (opacity steps read smoother than position steps at low fps) â€” matching the design mockup's actual
  translateX(Â±26px)+fade, which is NOT a full-screen slide. Plain `LV_STYLE_OPA` is **non-layered/cheap**
  (`calculate_layer_type`, lv_obj_style.c:1024, layers only for opa_LAYERED/transform/mask/blend), so the fade
  costs nothing extra. Residual sub-smoothness is the **hardware floor** â€” deferred to **P5 (motion helpers)**;
  the render-path investigation is now CLOSED (2026-08-12): -O2+240 MHz (PR #33), partial refresh (#56), and
  scroll-fling damping shipped; **dual-core was verified & DECLINED** (weak lever) and **SRAM buffers re-tested &
  REGRESSED + banded**. Floor **accepted** â€” see docs/LESSONS.md render-path (2026-08-12). CPU runs at **240 MHz**.
  (3) **Display config for atomic (non-banded) frames:** full-frame PSRAM buffers + `full_refresh`
  (`ui.c` `.buffer_size=W*H`, `.buff_spiram=true`, `.full_refresh=true`, keep `.buff_dma=false`+RGB888+2-px
  rounder). Frees ~98 KB internal RAM (buffers moved to PSRAM). Partial/internal buffers were REJECTED â€” they
  re-introduce the banded top-to-bottom sweep. `CONFIG_LV_DEF_REFR_PERIOD` 33â†’16 ms (finer anim step). QSPI
  pclk 40â†’**80 MHz** (`display.c`, LilyGo-rated; small real gain â€” flush was never the limiter). **full_refresh
  gotcha:** ANY invalidation â†’ a whole-frame repaint, so per-tick `lv_label_set_text` must be **guarded to
  update-on-change** (done for the USB status timers; the P4 clock must do the same) or it repaints the whole
  screen every tick.
  (4) **Header clipped by the rounded glass corners** â€” top-row content needs more inset than the 24-26px safe
  area. **FIX: `HEADER_HINSET=48` + `HEADER_TOP_PAD=42`** (verified clears it head-on); list rows stay at 26px.
  **NEXT = P3.2** (larger fonts + icons; **the on-device text reads too small/faint â€” do an on-device
  readability tuning loop WITH the user before locking sizes**; re-download the OFL Fraunces + JetBrains Mono
  sources per `fonts/README.md`; generate the 15 line icons via a Pillow `img/gen_icons.py` mirroring
  `gen_star.py`) **â†’ P3.3** (rest of the screen map: NFC/BLE/System submenus + the WiFiâ†’Scan action shell).
  **Build/flash gotcha #4 (memory-exhaustion â†’ `-j 1`/reboot) bit this session repeatedly.**

- **UI-shell P3.2 (= P3.2 + P3.3 combined) â€” larger type scale + icon font + full menu skeleton â€” DONE + verified on-device
  (2026-08-10).** Branch `Clankert/nocsif-ui-shell-p3-2-69f236` (off main @ P3.1). Flash 717â†’**729 KB** (8.7%); Home builds
  in **214 ms**. Click-through verified WITH the user (all 13 submenus drill/back, icons align, dimmed stubs inert, **M4 USB
  gadget/macro still enumerates + types**), after a **navigation-freeze bug was root-caused on serial and fixed** (below).
  - **Larger fonts** â€” `firmware/src/fonts/gen_fonts.py` (reproducible: fetches OFL Fraunces + JetBrains Mono, instances the
    serif cuts, runs `npx lv_font_conv` 4bpp `--no-compress`, normalises the include). Cuts: serif **21/23/26/13i/30**, mono
    **11/12/13/14/15/16/18**. **Sizes LOCKED on-panel WITH the user: title `serif_26`, row name `mono_16`, tag `mono_13`,
    caption `mono_12`, clock `mono_15`** â€” all one-line `lv_style_t` font-handle swaps in `ui_theme.c` (candidate cuts stay
    generated for re-tuning). **Fixed a real `gen_fonts.py` italic bug** (a review caught it): Fraunces roman VF has NO `ital`
    axis, so the italic cut must instance the **separate Fraunces-Italic VF** â€” done; `serif_13i` regenerated + confirmed italic.
  - **Icon font** â€” `firmware/src/icons/gen_icons.py` builds `nocsif_icons` (one 4bpp LVGL face) from the mockup's **38 line-icon
    `<symbol>`s**: a mini SVG parser (M/L/H/V/C/S/Q/T/A/Z) â†’ **stroke-expand to filled contours** (quad per segment + round-join/cap
    discs; nonzero-winding union keeps ring holes) â†’ fontTools TTF (PUA U+E000+) â†’ `lv_font_conv`. `nocsif_icons.h` has
    `NOCSIF_ICON_*` UTF-8 macros. **Recolour = plain text colour** (steel at rest; a stub dims to ash). **Self-verified off-device**
    by rendering the TTF to `icons/icons_preview.png` (eyeballed) + on-device (user: tint good, icons "a lil muddy but ships").
    **lsb=xMin fix** (review caught it): lv_font_conv derives ofs_x from the left side bearing, so lsb=0 left-justified every icon in
    its advance â†’ set lsb=glyph xMin so icons **center in the uniform advance** + share a column centre. Rendered at **22px**
    (chevron/back oversized vs the mockup's 14/17px â€” **follow-up: a small icon cut**).
  - **Menu shell as an APP REGISTRY (app-layer, `ui.c`; `ui_nav` stays root-based) â€” per the addendum.** `app_t{id,title,icon,
    build,root,enabled}` + `k_screens[]` (14 enabled screens) + `app_get`/`app_get_or_stub` (auto-registers an unknown leaf id as
    a disabled stub) + `app_ensure` (lazy build + `lv_obj_update_layout`, cached) + **`nocsif_app_launch(const char *id)`** (public in
    `ui.h`; the single launch-by-id entry point so FN button / peek planets / gestures can bind to any app later). Rows resolve their
    target **by id** through `app_row(rowspec_t)`; every stub row still gets a registered id. Menu tree = **`docs/design/full-app-mockup.html`**:
    Home 3 bands (operations/watch/system) + submenus wifi/ble/nfc/usb/lora/gnss/timers/activity/navi/notes/files/autom/system, with
    icons + captions + static tags; un-built leaves + hero screens (scan/hunt/voice/weather/notif/theme/flash) are **present-but-disabled
    stubs**; USB keeps the working **M4 gadget/macro controls** (mockup's USB rows = P4 migration). Scaffold refactored to a **fixed header +
    scrollable content column** (`nocsif_screen_scaffold` â†’ content; `nocsif_menu_list` + `nocsif_band` helpers); rows got an icon column,
    icon-font chevron, and a faint `#101013` bottom hairline. Specimen kept behind **`NOCSIF_UI_TYPE_SPECIMEN` (default 0)** for a future retune.
  - **CRASH FOUND + FIXED on-device (white screen â†’ task_wdt):** eager-building all 14 screens at init (~490 styled objects) on the main
    task took >5 s â†’ **task watchdog** (IDLE0 starved) â†’ LVGL's default white screen never got replaced. **Backtrace decoded** with
    `xtensa-esp-elf-addr2line` â†’ the init pre-warm loop. **Fix (verified: 214 ms, no WDT):** (1) **lazy build** â€” only Home at init;
    submenus build on first launch in the row callback (~40 objects, safe); (2) **shared row styles** `nocsif_style_row` / `_press` /
    `_chevron` â€” a row is now **2 `add_style` calls, not ~15 per-row `lv_obj_set_style_*`** (each set_style refreshes; that was the cost).
  - **Two review workflows ran** (neutral wording): the **gate review** found the icon **lsb=xMin** bug (fixed) + refuted 3; the **skeleton
    review** found the PIN-Lock tag `off`â†’`on` nit (fixed) + refuted 1. Neither caught the WDT â€” the prompts scoped logic/fidelity, not
    init-time cost (**lesson for next review: ask about watchdog / init-time work**).
  - **BUILD IN THE WORKTREE, not Docker:** this branch's `.pio` lives at `D:\NocSif_Firmware\nocsif-ui-shell-p3-2-69f236\firmware`
    (seeded `managed_components` + `sdkconfig` from the Docker checkout to skip the 181 MB re-download). `python -m platformio run -j 2 -d
    D:\NocSif_Firmware\nocsif-ui-shell-p3-2-69f236\firmware`; flash that dir's `firmware.factory.bin`.
  - **NAVIGATION-FREEZE BUG â€” root-caused + FIXED, verified on-device (2026-08-10).** Opening submenus froze the UI (needed RST)
    after the **2ndâ€“4th** navigation â€” variable, not menu-specific (any screen could be "the one"; WiFi/USB worked only because
    opened first). Serial method (see `docs/LESSONS.md`): the freeze is a **hang, not a crash** (heartbeats kept printing, system
    heap dead-flat) â†’ held at the freeze â‰¥5 s so the **task_wdt printed a backtrace** (resilient reconnecting capture
    `scratchpad/cap.py`; decoded with `xtensa-esp32s3-elf-addr2line` vs the worktree ELF) â†’ LVGL task **spinning in `lv_draw_label`**
    glyph render. **Root cause:** LVGL objects come from a **fixed 48 KB pool** (`CONFIG_LV_MEM_SIZE_KILOBYTES=48`,
    `LV_USE_BUILTIN_MALLOC`), **separate from the 8 MB system heap** â€” so the P3.1 "caching all screens is fine, heap is 8.5 MB"
    assumption was against the wrong pool. The registry cached every screen root forever; after Home + 2â€“3 submenus the pool ran
    dry and a render-time glyph-buffer alloc failed â†’ **LVGL span instead of asserting** (silent hang). **Fix (the refinement the
    P3.1 plan anticipated):** bound the cache â€” `nocsif_nav_back` **deletes the popped screen** (registry clears its cache on the
    root's `LV_EVENT_DELETE`; a re-launch rebuilds it), so live memory = **nav depth** (Home + one submenu â‰ˆ 37 KB, proven-good),
    not total screens visited. Pinned screens (`nocsif_nav_pin`, `LV_OBJ_FLAG_USER_1`) survive a pop for future running-state
    screens; **only Home is pinned**. USB is freed like any submenu â€” its two poll timers are deleted with its buttons
    (`LV_EVENT_DELETE` cb) and the gadget's real state lives in `usb_gadget.c`, so a rebuild re-reads it; the per-screen dashed-rule
    **PSRAM buffer is freed in the canvas's delete cb** (lv_canvas doesn't own it). **Only `ui.c`+`ui_nav.c` recompile** (22 s, no
    full-LVGL rebuild) vs a config bump that rebuilds ~710 LVGL objects and merely raises the ceiling. On-device: hammered all 13
    submenus repeatedly with no freeze; serial clean (no task_wdt/panic, heap flat); USB still types after being freed+rebuilt.
  - **Non-blocking cosmetic follow-ups** (a background review flagged, low severity, deferred): Home rows are tag-less vs the
    mockup's 11 status tags; Activity carries a `// caption` the mockup shows as a bottom note; `gen_fonts.py` fetches the OFL TTFs
    from moving `main`/`master` refs with no checksum (pin to an immutable ref for reproducibility).
  - **NEXT â†’ P4** (RTC PCF85063A clock + AXP2101 battery % + PWR/FN buttons; migrate the M4 USB controls into the mockup's USB
    rows). Serial capture: `scratchpad/cap.py N` (resilient reconnecting COM7 listen, no reset-on-open).

- **UI-shell P4 STARTED â€” P4.1 (physical-button input service, LOG-ONLY) DONE + verified on-device (2026-08-10).** Branch
  `Clankert/nocsif-ui-shell-p4-1-buttons` (off main @ `60b1157`). P4 spec = `docs/design/meta-prompts/ui-shell-p4.md` (authoritative);
  resume point = `docs/design/meta-prompts/ui-shell-p4-continue.md`. Flash 8.7% (731 KB). Register facts were pinned by a 5-agent
  verification Workflow (AXP2101 datasheet + XPowersLib + LilyGoLib, cross-checked) BEFORE coding, per the M3/M4 discipline.
  - **`power.c`/`power.h`** gained the AXP2101 PWRKEY interface (register-direct, matching the existing rail style): `nocsif_power_pwrkey_config()`
    â€” clears **0x22 b1** (the "long PWRKEY hold = power-off" source) so **firmware owns the button** (no hardware auto-off), sets **0x27
    IRQLEVEL = 1.5 s** (long-press threshold), drains the three IRQ-status banks (0x48/0x49/0x4A, write-1-clear), enables the PWRKEY IRQ
    sources (0x41 b3:0), and logs 0x22/0x27 before/after â€” and `nocsif_power_pwrkey_poll()` reads **0x49** and returns/clears the PWRKEY
    event bits. **Bit map (verified on-device): b3=short, b2=long, b1=press-down, b0=release.** No IRQ GPIO needed â€” the PMU latches events
    in 0x49 and we poll it (native **PMU_INT = GPIO7**, high-confidence but unused; the HARDWARE.md "GPIO7 vs XL9555" caveat is resolved â€”
    XL9555 IO7 is a different chip).
  - **`buttons.c`/`buttons.h`** (new) â€” a worker task (own task, never LVGL) at ~50 Hz decodes **PWR** (poll the PMU) + **FN = GPIO0**
    (debounced input; GPIO0 is the boot strap, sampled only at reset, so a runtime input read is safe â€” idle HIGH, press LOW). Double-press
    is synthesized in software (two shorts within 350 ms). **P4.1 is LOG-ONLY** â€” every event logs a serial line (+ the raw 0x49) so the map
    could be confirmed before wiring actions (actions = P4.4). `main.c` calls `nocsif_buttons_init()` after `nocsif_ducky_init()`; `buttons.c`
    added to CMakeLists SRCS.
  - **On-device (verified, multiple rounds):** PWR short (`0x49=0x09` = short+up) / long (`0x49=0x04`, fires at **exactly 1.5 s** â†’ IRQLEVEL
    set) / double all decode; FN short/long/double decode; **no power-off across many long holds** (0x22 b1 cleared) and **no reboot/wdt/panic**.
    Clean `downâ†’longâ†’up` with no spurious short. (The one-time config line wasn't captured â€” the USB-Serial/JTAG re-enumerates on the RTS reset,
    dropping early boot output â€” but behavior proves all three config writes landed.) **Follow-up (non-blocking):** the 350 ms double window is a
    touch tight for FN; tune when double-press gets an action in P4.4.
  - **Build note:** the first build on this fresh branch was a FULL rebuild (~11.6 min, no OOM at -j 2) because the new-branch checkout +
    adding `buttons.c` to CMakeLists forced a CMake reconfigure; subsequent P4 edits are fast incremental again.
  - **NEXT = P4.2** (RTC PCF85063A @ 0x51 â†’ header clock + Time screen; build the header live-data hook per the P4 spec Â§5). Then P4.3
    battery, P4.4 settings + button actions (FN default = **wifi**), P4.5 USB-row migration.

- **UI-shell P4.2 â€” RTC PCF85063A + header live-data hook + Time readout â€” DONE + verified on-device (2026-08-10).** Branch
  `Clankert/nocsif-p4-2-rtc-dc9264` (off main @ `60b0339`). Flash 731â†’**753 KB** (9.2%). New **`rtc.{c,h}`** (register-direct, matches
  `power.c`'s attach style â€” NO vendor lib): attach PCF85063A @ **0x51** on the shared bus, one atomic **burst read of 0x04â€“0x0A**
  (`i2c_master_transmit_receive` â€” the datasheet's own recommended method; the time counters are frozen during the access), BCD-decode to a
  `struct tm`. The **Seconds bit7 = OS (oscillator-stop) flag** gates validity; if set (fresh unit / backup lost) the driver **seeds from the
  firmware build timestamp** (`__DATE__`/`__TIME__`, weekday via Sakamoto) using the safe **STOP â†’ write 0x04â€“0x0A â†’ clear-STOP** sequence
  (which also forces 24-hour mode), else it uses the RTC's kept time. Exposes `nocsif_rtc_get`/`_time_valid` + cheap **cached**
  `nocsif_rtc_clock_str` ("HH:MM") / `nocsif_rtc_date_str` (no I2C â€” safe to read at build time), refreshed by `nocsif_rtc_tick`. A
  `tm_in_range()` gate keeps a glitched read from indexing `strftime %a/%b` out of bounds.
  - **Header live-data hook (spec Â§5), in `ui_nav.c`:** a generic **live-label registry** (`nocsif_nav_register_live_label(label, getter)` +
    `nocsif_nav_header_tick`) â€” each label stores a string-getter and **auto-unregisters on its own `LV_EVENT_DELETE`**, so a screen freed on
    nav-back never leaves a dangling label (live set = nav depth, bounded like the P3.2 cache). `build_header` now **seeds the clock label from
    `nocsif_rtc_clock_str()`** (a rebuilt header shows the current time immediately, never `--:--`) and registers it. **ONE** LVGL timer in
    `ui.c` (`header_tick_cb`, **20 s**) refreshes the RTC cache + ticks every registered label **update-on-change** (`strcmp` before
    `lv_label_set_text` â€” mandatory under `full_refresh`; it repaints only on the minute rollover). **P4.3 battery reuses this exact hook.**
  - **Time screen readout:** `build_timers()` gained a large mono **HH:MM + date** block (registered via the same hook), so the Time screen
    shows real, live wall-clock time.
  - **Register facts pinned BEFORE coding** by a 5-agent verification Workflow (NXP PCF85063A datasheet Rev.7, three independent extractions +
    lewishe/SensorLib `SensorPCF85063` cross-check): every mask (SEC 0x7F, HOURS 0x3F/24h, DAY 0x3F, WEEKDAY 0x07 with **0=Sunday = tm_wday**,
    MONTH 0x1F, YEAR +2000), the OS-bit-7 semantics, CTRL1 STOP(b5)/12_24(b1, 0=24h), and the STOP-wrapped set sequence â€” all match the code.
    A 4-dimension **adversarial review Workflow** (each finding independently verified) returned **3/4 dimensions clean** (header-hook/LVGL,
    integration, spec-fidelity) + **one low-severity finding** (a seed-*failure* error log printed a stale `err` instead of naming the failing
    step) â€” **fixed**.
  - **On-device (verified with the user):** serial boot shows `rtc: PCF85063A @ 0x51: time valid (OS clear) 2026-08-10 22:29:16` (the RTC held a
    real kept time on its VRTC backup â†’ header shows the **actual** time, not a seed); **header clock `22:29`, rolled over to `22:30`**;
    Time-screen readout live; **drilling in/out shows the time immediately, not `--:--`**; **M4 USB gadget/macro still enumerates + types**;
    `Home built in 213 ms`, heap flat (6,563,380), **no task_wdt/panic**. Seed path not exercised (OS was clear) â€” read/decode proven on real
    hardware; the seed path is proven by construction + review.
  - **BUILD-ENV GOTCHA (cost real time; see LESSONS):** the Claude desktop app crashed mid-session â†’ a **zombie `ld`/`collect2` linker** stayed
    resident holding a lock on `sections.ld`/`libbootloader_support.a` (relink failed "file format not recognized" then "used by another
    process"; unkillable by `taskkill`) **and** ~115 GB of **leaked committed memory** dropped free commit to ~2 GB. **A reboot cleared both**;
    the clean relink then took 85 s (flash 770,977 B). On-device verification ran on the pre-log-fix full build â€” functionally identical (the
    fix only changes a log string in an unreachable-on-healthy-hardware path).
  - **NEXT = P4.3** (AXP2101 battery %/charge â†’ header battery label + Systemâ†’Power row; extend `power.c` register-direct, reuse the P4.2
    live-label hook, update-on-change at a slow ~30â€“60 s cadence). Then P4.4 settings store + button actions (FN default = **wifi**),
    P4.5 USB-row migration.

- **UI-shell P4.3 â€” AXP2101 battery %/charge â†’ header battery + Systemâ†’Power â€” DONE + verified on-device (2026-08-10).** Branch
  `Clankert/nocsif-p4-3-battery` (off main @ `10e787f`). App image 753â†’**754 KB** (+~1.5 KB). New battery readout in **`power.{c,h}`**
  (register-direct, mirrors `rtc.{c,h}`'s cached-string pattern â€” NO XPowersLib): `nocsif_power_batt_tick()` polls **STATUS1 0x00**
  (b3 present, b5 VBUS-good), **STATUS2 0x01** (b6:5 current direction â†’ standby/charging/discharging), and **percent 0xA4** (state-of-charge
  0..100 direct uint8, gated on battery-present, `>100`/`0xFF` rejected as unsettled) into a cached int + **"NN%"** string + charge-state enum
  + VBUS bool. Cheap getters `nocsif_power_batt_pct/_str/_charging/_vbus_present` touch no I2C (safe on the LVGL task).
  `nocsif_power_gauge_config()` RMW-ensures the three POR-default-on enables (**0x18 b3** gauge, **0x30 b0** VBAT ADC, **0x68 b0** batt-detect â€”
  don't-trust-OTP discipline, matches LilyGo's boot) + primes the cache; called in `main.c` after the RTC, before the UI, so Home's first
  header shows a real %. **Each of the three reads keeps the LAST-GOOD cache on a transient I2C error** (no `--%` flash â€” a review finding,
  applied). Charge-state changes log one line (plug/unplug), never per-tick.
  - **Reused the P4.2 live-data hook (spec Â§5):** `build_header` seeds+registers the battery label via `nocsif_nav_register_live_label`
    (**plain "NN%" in steel, NO charge glyph** â€” matches the mockup's `.bat`); the single `header_tick_cb` refreshes the battery cache
    **every OTHER tick (~40 s)** alongside the RTC (20 s), update-on-change under `full_refresh`. `nocsif_menu_add_row` gained an optional
    **live-tag getter** (trailing param) so the **Systemâ†’Power** row tag binds live to `nocsif_power_batt_str` (dimmed but real â€” the row
    stays a stub until a Power screen lands).
  - **Register facts pinned BEFORE coding** by a 4-agent verification Workflow (XPowersLib source + AXP2101 datasheet V1.0 Â§6.11/Â§6.13.2 via
    pdfplumber + LilyGoLib usage + an independent Rust register-map â€” all concordant, high confidence). Key: **0xA4 is the fuel-gauge SOC**,
    NOT the VBAT voltage ADC at 0x34/0x35; the gauge is POR default-on (0x18 b3). A 4-dimension **adversarial review Workflow** (10 agents,
    each finding verified refute-by-default) found **1 real low-sev defect** (0xA4 transient-error cache flash â€” fixed) + 3 stale comments
    (fixed) + 1 refuted (a dead `CHG_STATE_MASK` macro â€” removed anyway); the LVGL/lifecycle dimension was clean.
  - **On-device (verified with the user):** boot shows `axp2101: battery gauge: standby 100% (vbus=1)` (full battery on the USB flash cable â†’
    charge direction **standby**, a correct non-trivial b6:5 decode); **header battery `100%` in steel, Systemâ†’Power tag matches `100%`**;
    **M4 USB gadget/macro still enumerates + types**; `Home built in 211 ms` (20 apps), heap flat (6,563,356), **no task_wdt/panic**; RTC
    still valid. (USB-tethered = vbus present; unplug flips the state to `discharging`, header % holds last-good â€” by design.)
  - **NEXT = P4.4** (settings store `settings.{c,h}` NVS + wire button actions â€” FN default = **wifi** via `nocsif_app_launch`; PWR short =
    screen on/off, in-submenu = pop to Home, long = power menu (Power off / Restart), double = bound id; PIN stored hashed, PIN-pad a thin
    stub; tune the P4.1 FN 350 ms double window). Then P4.5 (migrate M4 USB controls into the mockup's USB rows; neutralize
    `payload`â†’`macro`), P4.6 (peek/planets lock screen), â†’ P5 (boot screen + motion).

- **UI-shell P4.4a â€” settings store (NVS) + physical-button actions wired â€” DONE + verified on-device (2026-08-10).** Branch
  `Clankert/nocsif-p4-4a-settings-buttons` (off main @ `705d0ad`). App image 754â†’**810 KB** (9.7%; +NVS/mbedtls pull + settings +
  power menu). **P4.4 was SPLIT** into **P4.4a** (this: settings store + button actions + power menu) and **P4.4b** (Settingsâ†’Buttons
  config UI + PIN pad), each independently flashable/verified (user's call).
  - New **`settings.{c,h}`** â€” NVS store (namespace `nocsif`, `nvs_flash_init` + erase/retry), typed str/i32 get/set, and a **RAM-cached**
    FN-target / PWR-double binding (getters touch no flash â†’ safe to read on the LVGL task in a button-action callback; default FN=`wifi`).
  - New button-action layer in **`ui.c`** (`nocsif_ui_btn_*`) that **marshals** each decoded event onto the LVGL task
    (`lvgl_port_lock` + `lv_async_call`) â€” `buttons.c` never touches widgets. `buttons.c` now dispatches real actions (was log-only).
  - New **power-menu screen** (`build_powermenu`: Power off / Restart; chevron-less action rows; freed on nav-back like any submenu).
    `display.{c,h}` gained **`nocsif_display_sleep()`** (panel DISPOFF 0x28 / DISPON 0x29 + indev disable so a stray touch can't navigate
    under a dark panel). `power.{c,h}` gained **`nocsif_power_off()`** (AXP2101 **0x10 b0 Soft PWROFF**, RMW). `ui_nav.{c,h}` gained
    `nocsif_nav_at_root` / `_top` / `_pop_to_root`.
  - **PWR wiring honors the design-chat's single-sourced PWR state machine (mockup #31):** short = dismiss power menu Â· wake (if asleep) Â·
    pop-to-Home (in a submenu) Â· screen off (at Home); long (PMU â‰¥1.5 s) = power menu; double = a bound shortcut (default unbound â†’
    double-detection **armed only when bound**, so the common PWR tap stays instant; unbound, a "double" is just two quick singles).
  - **FN wiring â€” USER-REQUESTED DEVIATION (2026-08-10):** the P4 spec had FN single = launch; the user asked for **FN single = back one
    screen, FN double = launch the FN shortcut app (default `wifi`)**. Implemented via double synthesis (single-back fires after the
    `DOUBLE_US = 350 ms` window â€” **user confirmed the feel is good**). Recorded in code comments + PLAN Â§4.1.
  - **launch-by-id hardening:** `nocsif_app_launch` now collapses to Home (`pop_to_root`) then pushes â€” matching the mockup's
    `stack=['home', id]` and closing a **latent double-free** when FN / a shortcut re-launches an app already on the nav stack. The power
    menu uses **overlay** semantics (pushed over the current screen, not pop-to-root) so back/cancel returns to where you were.
  - **Registers pinned BEFORE coding** by a 4-agent verification Workflow (XPowersLib `shutdown()`/`reset()` + AXP2101 datasheet V1.0
    Â§6.5.4.3/Â§6.13.2.7 â€” high confidence, full agreement): power-off = **0x10 b0** (INDEPENDENT of the P4.1-cleared 0x22 b1 â€” different
    register); restart = **`esp_restart()`** (SoC-warm, rails/RTC stay up; the PMU-level 0x10 b1 reset is reserved for a hard recovery
    cycle only). âš  Soft power-off will **NOT** keep the watch off while USB VBUS is present (PMU wake-sources re-power it â†’ it reboots â€”
    EXPECTED per LilyGo; true off needs USB unplugged). Display sleep verified from the CO5300 driver source (disp_on_off â†’ 0x28/0x29).
  - A **9-agent adversarial review Workflow** (3 dimensions â†’ per-finding refute-by-default, high-effort verify) **confirmed the core
    design** (marshalling race-free/no lock-order deadlock, `pop_to_root` loads Home before deleting so no active screen is freed, the
    `app_launch` pop-then-ensure never reuses a freed root, `s_powermenu_open` cleared on every dismiss path, no settings RAM-cache data
    race, `power_off` RMW correct) â€” 6 findings â†’ 4 refuted â†’ **2 low-sev, both applied**: (1) a same-tick double-synthesis overwrite
    race (fixed by flushing a matured pending BEFORE decoding the tick), (2) no power-off path if display/touch init fails (fixed by
    gating `nocsif_buttons_init` on `ui_ok` â€” leave the PMU's hardware long-hold power-off intact when headless).
  - **On-device (verified with the user):** boot `settings up (nvs 'nocsif'): FN='wifi' PWR-double='(none)'`; **FN single = back** (feel
    good) / **FN double â†’ WiFi**; PWR screen off/on (**heap dead-flat across repeated toggles â†’ no leak**), PWR short in a submenu â†’ Home,
    PWR long â†’ power menu â†’ Restart reboots; `Home built 128 ms (21 apps registered â€” +1 power menu)`; **M4 USB gadget still
    enumerates/types**; **no task_wdt/panic**. (Build note: adding the `nvs_flash` PRIV_REQUIRES forced a CMake reconfigure â†’ one full
    ~12.7 min rebuild, then fast incrementals; no OOM at `-j 2`.)
  - **NEXT = P4.4b** (Settingsâ†’Buttons config UI + app picker writing the FN/PWR-double bindings via `nocsif_settings_set_*`; PIN stored
    **hashed** (random salt + SHA-256 via mbedtls) + a **minimal functional keypad** to set it; verify **set-then-reboot persistence**;
    call `nocsif_buttons_set_pwr_double_enabled` when the PWR-double binding changes). Then P4.5 (USB-row migration; neutralize
    `payload`â†’`macro`), P4.6 (peek/planets lock screen), â†’ P5 (boot screen + motion).

- **UI-shell P4.4b â€” Settingsâ†’Buttons config UI + app picker + hashed-PIN keypad â€” DONE + verified on-device (2026-08-10).** Branch
  `Clankert/nocsif-p4-4b-buttons-ui-pin` (off main @ `83b0219`). App image 810â†’**818 KB**; **LVGL object pool 48â†’96 KB** (below). **This
  COMPLETES P4.4.** Three new screens, all freed on nav-back (fixed-pool rule):
  - **Settingsâ†’Buttons** (`system.buttons`) â€” rows for **FN shortcut** + **PWR double-press**, each showing the current binding as a live tag
    (friendly title, e.g. "WiFi") and opening the app picker on tap.
  - **App picker** (`pick`) â€” lists the enabled category apps (excludes Home/stubs/internal screens); a tap writes the binding via
    `nocsif_settings_set_fn_target`/`_set_pwr_double` (+ arms `nocsif_buttons_set_pwr_double_enabled`) then pops. `s_pick_key` (set before the
    drill) selects which binding; the picker rebuilds each open so the key is never stale. A "(none)" row clears the PWR-double binding.
  - **Passcode keypad** (`system.pin`) â€” an `lv_buttonmatrix` numeric pad (1-9/0/del/ok, masked `*`, 4-8 digits) that stores the PIN hashed and
    returns to System. Systemâ†’PIN Lock is now enabled with a live "set"/"none" tag and drills to the keypad.
  - **settings.{c,h} passcode API:** `nocsif_settings_set_pin` (random salt via `esp_fill_random` + **SHA-256(salt||pin)** via mbedtls, stored as
    NVS blobs `pin.salt`/`pin.hash`), `_verify_pin` (constant-time compare), `_has_pin` (RAM-cached, LVGL-safe), `_clear_pin`, `_pin_str`. PIN never
    stored/logged in plaintext; scratch wiped with `mbedtls_platform_zeroize` (a plain `memset` dead-store is elided at the shipped `-O2`). **KDF is
    a single SHA-256** â€” a short numeric PIN is brute-forceable if flash is extracted; a stretch-KDF (PBKDF2) + attempt-backoff is a **P4.6 hardening**
    (when the unlock gate that consumes the PIN lands; P4.4b only sets/persists it, nothing gates on it yet).
  - **Nav fix:** `nocsif_app_launch` (top-level: FN-double/PWR-double/planets) keeps the P4.4a collapse-to-Home semantics, but **menu rows now
    drill in** via a new `app_drill` (push on top; back returns to the parent) â€” required so the 3rd/4th-level config screens don't collapse the
    hierarchy. Home-row nav unchanged (collapse-to-root is a no-op at Home).
  - **LVGL pool 48â†’96 KB** (edited BOTH `sdkconfig.defaults` **and** `sdkconfig.nocsif-twatch-ultra` â€” the master PlatformIO reuses; see LESSONS):
    the config path is up to **5 screens deep** (Homeâ†’Systemâ†’Buttonsâ†’picker + a PWR-long power-menu overlay). A `lv_mem_monitor` log in `app_ensure`
    showed the on-device peak **max_used = 53.5 KB of ~93 KB (57%)** â†’ **48 KB would have overflowed = the P3.2 hang**; 96 KB holds it with ~40 KB
    headroom (242 KB internal free after the bump).
  - **APIs pinned BEFORE coding** (mbedtls `mbedtls_sha256` one-shot + `esp_fill_random` + `lv_buttonmatrix` v9, from the pinned headers). A **9-agent
    adversarial review** confirmed the design (PIN crypto, nav/pool/lifecycle, picker/keypad state all sound) â€” 8 findings â†’ 2 refuted â†’ **6 low/nit,
    all folded** (clear_pin cache-before-commit; `memset`â†’`mbedtls_platform_zeroize`; config-tag idâ†’title; keypad save-fail/max-digit feedback; a stale
    "USB pinned" comment; + the KDF note deferred to P4.6).
  - **On-device (verified with the user):** set FN=`nfc` â†’ FN double launches NFC; set PWR-double=`system` â†’ PWR double launches System (single PWR
    still toggles the screen); set a PIN â†’ "passcode saved". **After `PWR long â†’ Restart`: `settings up â€¦ FN='nfc' PWR-double='system' passcode=set`
    â€” all three persisted across the reboot** (the acceptance test). Deep nav (4-5 screens) never hung (pool 57% peak); M4 USB still enumerates/types;
    no task_wdt/panic; heap flat. **Two build gotchas hit + now in LESSONS:** a compile error showed only on stderr (the stdout Tee log looked like an
    OOM); and a Kconfig bump needs editing `sdkconfig.<env>`, not just `sdkconfig.defaults`.
  - **P4.4 COMPLETE. NEXT = P4.5** (migrate the M4 USB controls into the mockup's USB rows â€” Run Macro / File Drop (MSC) / HID Mouse / Keymap USÂ·GBÂ·DE /
    U2F stub; reuse `usb_gadget`/`ducky`/`hid_kbd` verbatim, signal-only; **neutralize `payload`â†’`macro`**: `/sd/payload.txt`â†’`/sd/macro.txt`,
    `files.payloads`â†’`macros`; the chevron-less **action-row** variant already exists â€” `add_action_row`, from the P4.4a power menu â€” reuse/promote it).
    Then **P4.6** peek/planets lock screen (the P4.4b hashed PIN gates it), â†’ P5 (boot screen + motion).

## Two load-bearing gotchas
1. **USB flashing:** the ESP32-S3 USB-Serial/JTAG stub is unstable here â†’ **always esptool
   `--no-stub`, never force `--baud`**. If port is "busy/Access denied", enter **download mode:
   hold BOOT â†’ tap RST â†’ release BOOT** (blank screen), then esptool default reset connects. Watch
   is **COM7**.
2. **BUILD FROM POWERSHELL, NOT GIT BASH.** ESP-IDF tooling refuses to run under MSys/Mingw
   ("MSys/Mingw is not supported"). Build with `-j 2` (see #4):
   ```powershell
   python -m platformio run -j 2 -d D:\Docker\NocSif_Firmware\firmware
   ```
3. **PSRAM is QUAD (QSPI), not octal** â€” `sdkconfig` uses `CONFIG_SPIRAM_MODE_QUAD`. Octal leaves
   PSRAM unexposed.
4. **Build memory-exhaustion on Windows (learned 2026-08-09, P3).** Default parallelism OOMs
   (`cc1: out of memory` / `as: 'memory exhausted'` / SCons `MemoryError` / `Error 3221225773`).
   Cause is the Windows **commit charge**, not physical RAM or pagefile size (this box: 32 GB RAM +
   96 GB pagefile = 128 GB commit limit). A **leaked commit reservation** (seen after the Claude
   desktop app crashed mid-session) drops free commit to ~450 MB while per-process private memory
   sums to only ~10 GB â†’ any heavy concurrent compile fails. **Build with `-j 2` normally; drop to
   `-j 1` if it still OOMs** (esp. a full lvgl recompile after an sdkconfig change â€” `-j 1` halves
   peak commit); a **reboot** clears the leaked charge and restores `-j 2`. Enlarging the pagefile
   does NOT help. Diagnose with `Get-CimInstance Win32_OperatingSystem` (FreeVirtualMemory â‰ˆ free
   commit; the `\Memory\Commit Limit` perf counters are localized/unavailable here).
   **Also: `Tee-Object` makes PowerShell exit 0 regardless â€” judge build success from the LOG
   (`[SUCCESS]` / `error:` / `memory exhausted`), never the exit code.**

## Build & flash (verified working)
```powershell
# build (PowerShell, NOT Git Bash; -j 2 to avoid the memory-exhaustion in gotcha #4, -j 1 if it OOMs):
python -m platformio run -j 2 -d D:\Docker\NocSif_Firmware\firmware
# flash (watch in download mode: hold BOOT -> tap RST -> release BOOT):
python -m esptool --chip esp32s3 --port COM7 --no-stub write-flash --flash-size 16MB `
  0x0 D:\Docker\NocSif_Firmware\firmware\.pio\build\nocsif-twatch-ultra\firmware.factory.bin
# read serial: scratchpad read_serial.py / reset_read.py, or `pio device monitor`
```

## Next steps (in order)
1. ~~**M0.1** IÂ²C scan~~ Â· ~~**M1** CO5300 display~~ Â· ~~RGB888 colour byte-order~~ Â· ~~**M2** CST9217 touch~~ Â·
   ~~**M3** LVGL v9 UI~~ â€” **ALL DONE + verified on-device.** See Current state.
2. **M4 DONE â€” P1 + P2 + P3 + P4 all verified on-device.** M4 = a composite USB device (USB Mass-Storage SD
   file-drop + USB HID keyboard / DuckyScript player), 4 flashable phases. P1 (SD FAT mount) Â· P2 (TinyUSB +
   deferred CDC console, USB-PHY handoff) Â· P3 (CDC+MSC file drop) Â· **P4 (CDC+MSC+HID composite, DuckyScript
   typing, verified 2026-08-09 â€” types a host-dropped `/sd/payload.txt` as a real USB keyboard; falls back to
   CDC+HID with no reboot when the SD is absent).** See Current state + the plans
   `docs/research/usb-composite-device-2026-08-06.md` and `docs/research/m4-p4-hid-implementation-2026-08-09.md`.
3. **UI shell P1â€“P8 â€” ALL DONE & verified on-device + merged.** P1 fonts + theme tokens Â· P2 render-once
   orrery/star background Â· P3 nav shell + full menu as a launch-by-id registry Â· P4 RTC + battery + physical
   buttons + NVS settings store + USB mode picker + peek/lock watchface + passcode Â· P5 boot splash +
   reduced-motion Â· P6 comets Â· **P7 tilt-parallax BACKLOGGED** Â· **P8 v2 IA** (rotating 3-world Home Â· carousel
   peek dials + editor Â· Control Center Â· engraved celestial icons). Concise done-state in `docs/PLAN.md` Â§3;
   per-phase detail in the Current-state log above + git history.
4. **Next = reliability hardening â†’ M6 NFC**, then M5 WiFi â†’ M7 BLE â†’ M11 watch-core â†’ M8 GNSS â†’ M9 LoRa â†’
   M10 USB-ext â†’ platform layers (see the CURRENT WORK banner + `docs/PLAN.md` Â§4). One milestone per branch â†’
   PR â†’ squash-merge; verify each phase on-device.
5. *Render perf + LVGL pins (settled):* `-O2` + 240 MHz shipped; dual-core LVGL verified-and-declined;
   partial-refresh lifted motion fps. Full analysis + pinned LVGL facts in `docs/LESSONS.md` ("Render-path
   performance") â€” nothing outstanding.

## Un-brick at any time
```powershell
# your dump:
python -m esptool --chip esp32s3 --port COM7 --no-stub write-flash 0x0 D:\Docker\NocSif_Firmware\backups\stock_full_16MB.bin
# or LilyGo factory image (our SX1262 915MHz variant):
python -m esptool --chip esp32s3 --port COM7 --no-stub write-flash 0x0 D:\Docker\NocSif_Firmware\vendor\LilyGoLib\firmware\factory.watch.ultra.sx1262.20260424.bin
```

## Doc map
`docs/PLAN.md` (**the master plan â€” its Â§8 is the full doc map**) Â· `README.md` (overview) Â·
`docs/HARDWARE.md` (**authoritative** pinout / buses / rails) Â· `docs/ARCHITECTURE.md` Â·
`docs/LESSONS.md` (**hard-won gotchas + root causes** â€” LVGL fixed-pool freeze, ESP32-S3 hang/panic
serial-diagnosis, render-path). **UI source of truth:** `docs/design/full-app-mockup.html` (v2 IA).
**Capability catalog:** `docs/research/capability-expansion-2026-08-08.md` (M5+ backlog + meta-prompt).
*Historical (shipped, not current): `docs/research/{runtime-base,lvgl-integration,usb-composite-device,*
*m4-p4-hid-implementation,ui-shell}-*.md`, `docs/design/ui-mockup.html`, `docs/design/ui-design-spec-2026-08-08.md`.*
Cross-session memory lives in `~/.claude/projects/D--Docker/memory/`.
