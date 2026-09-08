idf_component_register(SRCS "main.c" "i2c_scan.c" "xl9555.c" "power.c" "display.c" "touch.c" "ui.c" "sdcard.c"
                            # RAM remediation Phase A1 — NocSif QSPI panel IO: DMAs the LVGL PSRAM draw
                            # buffers straight to the panel (SPI_TRANS_DMA_USE_PSRAM), retiring the
                            # ~29.5 KB internal flush stage (docs/RAM-BUDGET.md remake #15).
                            "display_io.c"
                            # §4.6 Connectivity Governor P1 — WiFi power policy as activity leases over
                            # the RESIDENT driver (stop/start + modem-sleep, never deinit).
                            "governor.c"
                            # §4.14 — sun / moon almanac (pure math) for the watch-peek countdowns.
                            "almanac.c"
                            # RAM remediation Phase 0 — build-time guard: #error if a load-bearing
                            # coexistence sdkconfig key silently reverts (docs/RAM-BUDGET.md C5).
                            "coex_guard.c"
                            # RAM remediation Phase 2 (§4.13 / remake #6) — shared read-only radio-state
                            # accessor: ONE truth the CC tiles + Connectivity hub + status labels read.
                            "radio_state.c"
                            # Reliability hardening (Phase A) — task-WDT panic + core-dump crash record
                            # + boot-loop safe mode + UI-liveness watchdog (A1); persistent rolling
                            # log to the 'logs' flash partition + ESP_LOG tee (A2)
                            "reliability.c"
                            "logbook.c"
                            "usb_gadget.c" "nocsif_usb_desc.c" "hid_kbd.c" "ducky.c"
                            # M-OTA — A/B over-the-air update worker (esp_ota_* to the inactive slot,
                            # source = firmware.bin on microSD). Rollback-confirm wired to the main
                            # heartbeat's healthy dwell. Reads /sd via the same claim_sd path as ducky.
                            "ota.c"
                            # §4.8a P4 + §4.15 — shared /sd file-service rules (jail / claim / short
                            # locks / provision / format) for the companion browser AND the desktop bridge.
                            "sdfs.c"
                            # §4.15 — desktop bridge: JSON-lines seam on the USB-Serial/JTAG console for
                            # the computer-side tool (tools/nocsif_bridge): version, health, files, control.
                            "bridge.c"
                            # M6-P1 — NFC (ST25R3916 / RFAL) worker + UI glue. C++ (drives the
                            # `rfal` component's classes); its public API is extern "C".
                            "nfc.cpp"
                            # M5-P1 — WiFi (on-SoC 2.4 GHz, STA) worker + UI glue. esp_wifi on a
                            # worker task; lazy safe-mode-gated bring-up; NVS-persisted creds.
                            "wifi.c"
                            # M7-P1 — BLE (on-SoC 2.4 GHz, NimBLE observer) worker + UI glue. NimBLE
                            # host on its own task; lazy safe-mode-gated bring-up; device scan.
                            "ble.c"
                            # M9 — LoRa (SX1262) worker via RadioLib (components/RadioLib) through a
                            # custom ESP32-S3 HAL (nocsif_esp_hal.h) on the shared SPI3 bus. C++.
                            # Lazy safe-mode-gated bring-up; boot self-test = radio bring-up + TX test.
                            "lora.cpp"
                            # M8 — GNSS (u-blox MIA-M10Q / LS550G) worker. Dedicated UART; lazy
                            # safe-mode-gated bring-up. Proof-of-life = BLDO1 + baud/pin sweep for
                            # NMEA/UBX framing (this unit's GPS was reported dead — instrumented recheck).
                            "gnss.c"
                            # M11-A1 — BHI260AP inertial sensor hub worker (accelerometer bring-up).
                            # Drives the vendored Bosch BHY2 API (firmware/components/bhy2) over I2C;
                            # lazy safe-mode-gated bring-up on a worker task. Independent of the radio.
                            "imu.c"
                            # M11-E1 — audio output (MAX98357A I2S speaker on BLDO2). Worker task
                            # owns the I2S TX channel + amp rail; synthesizes tones / named cues.
                            "audio.c"
                            # M11-E1·2 — PDM microphone (I2S_NUM_0 PDM RX, GPIO17/18). Worker task
                            # reads PCM + publishes a cached level for the Microphone meter screen.
                            "mic.c"
                            # §4.1 — Weather: the first HTTP *client*. Worker task fetches Open-Meteo
                            # (HTTPS) for a GNSS-captured home location + publishes a cached record for
                            # the Weather screen + the watchface peek chip.
                            "weather.c"
                            # M11 power — CPU dynamic frequency scaling (esp_pm); gated by "dfs_en".
                            "pm.c"
                            # M11 — route LVGL's heap to PSRAM (CONFIG_LV_USE_CUSTOM_MALLOC) to free
                            # ~96 KB internal RAM so the IMU coexists with the BLE controller / WiFi.
                            "lv_mem_psram.c"
                            # UI-shell P4.1 — physical-button input service (PWR/AXP2101 PWRKEY + FN/GPIO0)
                            "buttons.c"
                            # UI-shell P4.2 — PCF85063A real-time clock (header clock + Time readout)
                            "rtc.c"
                            # UI-shell P4.4 — persistent settings store (NVS) + button-action bindings
                            "settings.c"
                            "ui_theme.c"
                            # UI-shell P2 — orrery/star background (bottom layer) + corner masks
                            "ui_background.c"
                            # UI-shell P3 — menu-first navigation shell (screen stack + widgets)
                            "ui_nav.c"
                            # UI-shell P1/P3.2 — embedded LVGL fonts (generated 4bpp, see fonts/README.md)
                            "fonts/nocsif_serif_30.c" "fonts/nocsif_serif_21.c"
                            "fonts/nocsif_serif_23.c" "fonts/nocsif_serif_26.c"
                            # §4.13 x-large / XXL type-scale steps: full-charset serif 28 / 30 + mono 20 / 22.
                            "fonts/nocsif_serif_28.c" "fonts/nocsif_serif_30f.c"
                            "fonts/nocsif_mono_20.c" "fonts/nocsif_mono_22.c"
                            "fonts/nocsif_serif_13i.c" "fonts/nocsif_serif_16i.c"
                            "fonts/nocsif_serif_20i.c"
                            "fonts/nocsif_mono_11.c" "fonts/nocsif_mono_12.c" "fonts/nocsif_mono_13.c"
                            "fonts/nocsif_mono_14.c" "fonts/nocsif_mono_15.c" "fonts/nocsif_mono_16.c"
                            "fonts/nocsif_mono_18.c"
                            # UI-shell P4.6 — watchface hero: big numeric time + larger italic date
                            # UI-shell P8 v2.2 — smaller peek hero time (num_48) + bigger peek date (serif_20i)
                            "fonts/nocsif_num_64.c" "fonts/nocsif_num_48.c"
                            # UI-shell P2 — generated A8 image assets (see img/README.md)
                            "img/nocsif_img_star.c"
                            # P8 v2.6 — engraved celestial planet icons (A8, resvg-traced; Home ring)
                            "img/nocsif_cel_sun.c" "img/nocsif_cel_star.c" "img/nocsif_cel_moon.c"
                            "img/nocsif_cel_sun_flame.c" "img/nocsif_cel_sun_ray.c"
                            "img/nocsif_cel_moon_full.c" "img/nocsif_cel_moon_crescent.c" "img/nocsif_cel_moon_stars.c"
                            "img/nocsif_cel_star_spark.c" "img/nocsif_cel_star_burst.c"
                            # UI-shell P3.2 — menu icon font (generated 4bpp, see icons/README.md)
                            # P8 v2.2 — large celestial-motif cut for the Home-hub circles
                            # P8 v2.4 — lighter 48px cut for the Control-Center toggles
                            "icons/nocsif_icons.c" "icons/nocsif_icons_lg.c" "icons/nocsif_icons_cc.c" "icons/nocsif_icons_xl.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_driver_i2c esp_driver_spi esp_driver_gpio esp_lcd
                                     # M8 — GNSS receiver on a dedicated UART (gnss.c)
                                     esp_driver_uart
                                     # M9 — LoRa SX1262 via vendored RadioLib (components/RadioLib),
                                     # driven by lora.cpp through the custom S3 HAL (nocsif_esp_hal.h)
                                     RadioLib
                                     # M11-E1 — I2S standard-mode TX for the MAX98357A speaker (audio.c)
                                     esp_driver_i2s
                                     fatfs sdmmc esp_driver_sdspi esp_tinyusb wear_levelling
                                     # §4.15 — the USB-Serial/JTAG console driver (bridge.c reads host lines)
                                     esp_driver_usb_serial_jtag
                                     nvs_flash mbedtls esp_hw_support
                                     # Reliability (Phase A): esp_task_wdt + esp_reset_reason live in
                                     # esp_system; esp_core_dump_* live in espcoredump; the persistent
                                     # logbook's raw ring uses esp_partition.
                                     esp_system espcoredump esp_partition
                                     # M-OTA — esp_ota_* (A/B slot write + rollback) live in app_update.
                                     app_update
                                     # M6-P1 — the vendored RFAL / ST25R3916 driver (C++
                                     # component in firmware/components/rfal); nfc.cpp calls it.
                                     rfal
                                     # M11-A1 — the vendored Bosch BHY2 sensor-hub API (C component
                                     # in firmware/components/bhy2); imu.c calls it.
                                     bhy2
                                     # Phase B — minimp3 (header-only, CC0) for the Carts MP3 path (audio.c).
                                     minimp3
                                     # M5-P1 — WiFi station: esp_wifi driver + esp_netif + the
                                     # default event loop (wifi.c). NVS is already required above.
                                     # M5-P5·4 — captive portal: esp_http_server (landing page) +
                                     # lwip sockets (the UDP:53 DNS redirector, via lwip below).
                                     esp_wifi esp_netif esp_event esp_http_server
                                     # §4.8a Companion (L4) — mDNS responder (espressif/mdns,
                                     # managed) so the companion web UI resolves at nocsif.local.
                                     mdns
                                     # §4.1 — Weather: the first HTTP client (weather.c). esp_http_client
                                     # pulls esp-tls transitively; the mbedTLS CA bundle
                                     # (esp_crt_bundle_attach) ships in the already-required mbedtls
                                     # component; json = cJSON for the small forecast document.
                                     esp_http_client json
                                     # M7-P1 — Bluetooth LE: the NimBLE host + BLE controller live in
                                     # the 'bt' component (ble.c). NVS is already required above.
                                     bt)
# esp_lcd_co5300 and esp_lvgl_port (which pulls lvgl) come from idf_component.yml
# (managed components); the component manager adds them to this component's
# requires automatically, so ui.c can #include "esp_lvgl_port.h" / "lvgl.h".
