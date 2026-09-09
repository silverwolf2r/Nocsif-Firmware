idf_component_register(SRCS "main.c" "i2c_scan.c" "xl9555.c" "power.c" "display.c" "touch.c" "ui.c" "sdcard.c"
                            # QSPI panel IO: DMAs LVGL's PSRAM draw buffers straight to the panel
                            # instead of bouncing through an internal-RAM flush stage.
                            "display_io.c"
                            # WiFi power policy: activity leases over the resident driver
                            # (stop/start + modem-sleep, never a full deinit).
                            "governor.c"
                            # Sun / moon almanac (pure math) for the watch-peek countdowns.
                            "almanac.c"
                            # Build-time guard: #error if a load-bearing coexistence sdkconfig
                            # key silently reverts.
                            "coex_guard.c"
                            # Shared read-only radio-state accessor used by the CC tiles,
                            # Connectivity hub, and status labels.
                            "radio_state.c"
                            # Reliability hardening: task-WDT panic + core-dump crash record +
                            # boot-loop safe mode + UI-liveness watchdog, plus a persistent
                            # rolling log to the 'logs' flash partition with ESP_LOG tee.
                            "reliability.c"
                            "logbook.c"
                            "usb_gadget.c" "nocsif_usb_desc.c" "hid_kbd.c" "ducky.c"
                            # A/B over-the-air update worker (esp_ota_* to the inactive slot,
                            # source = firmware.bin on microSD). Rollback-confirm is wired to
                            # the main heartbeat's healthy dwell; reads /sd via the same
                            # claim_sd path as ducky.
                            "ota.c"
                            # Shared /sd file-service rules (jail / claim / short locks /
                            # provision / format) used by both the companion browser and the
                            # desktop bridge.
                            "sdfs.c"
                            # Desktop bridge: JSON-lines seam on the USB-Serial/JTAG console for
                            # the computer-side tool (tools/nocsif_bridge): version, health,
                            # files, control.
                            "bridge.c"
                            # NFC (ST25R3916 / RFAL) worker + UI glue. C++ (drives the 'rfal'
                            # component's classes); its public API is extern "C".
                            "nfc.cpp"
                            # WiFi (on-SoC 2.4 GHz, STA) worker + UI glue. esp_wifi on a worker
                            # task; lazy safe-mode-gated bring-up; NVS-persisted creds.
                            "wifi.c"
                            # BLE (on-SoC 2.4 GHz, NimBLE observer) worker + UI glue. NimBLE
                            # host on its own task; lazy safe-mode-gated bring-up; device scan.
                            "ble.c"
                            # LoRa (SX1262) worker via RadioLib (components/RadioLib) through a
                            # custom ESP32-S3 HAL (nocsif_esp_hal.h) on the shared SPI3 bus. C++.
                            # Lazy safe-mode-gated bring-up; boot self-test = radio bring-up + TX test.
                            "lora.cpp"
                            # GNSS (u-blox MIA-M10Q / LS550G) worker. Dedicated UART; lazy
                            # safe-mode-gated bring-up; baud/pin sweep to acquire NMEA/UBX framing.
                            "gnss.c"
                            # BHI260AP inertial sensor hub worker (accelerometer bring-up).
                            # Drives the vendored Bosch BHY2 API (firmware/components/bhy2) over
                            # I2C; lazy safe-mode-gated bring-up on a worker task.
                            "imu.c"
                            # Audio output (MAX98357A I2S speaker on BLDO2). Worker task owns
                            # the I2S TX channel + amp rail; synthesizes tones / named cues.
                            "audio.c"
                            # PDM microphone (I2S_NUM_0 PDM RX, GPIO17/18). Worker task reads PCM
                            # and publishes a cached level for the Microphone meter screen.
                            "mic.c"
                            # Weather: HTTP client that fetches Open-Meteo (HTTPS) for a
                            # GNSS-captured home location and publishes a cached record for the
                            # Weather screen and the watchface peek chip.
                            "weather.c"
                            # CPU dynamic frequency scaling (esp_pm), gated by "dfs_en".
                            "pm.c"
                            # Routes LVGL's heap to PSRAM (CONFIG_LV_USE_CUSTOM_MALLOC) to free
                            # internal RAM so the IMU coexists with the BLE controller / WiFi.
                            "lv_mem_psram.c"
                            # Physical-button input service (PWR/AXP2101 PWRKEY + FN/GPIO0).
                            "buttons.c"
                            # PCF85063A real-time clock (header clock + Time readout).
                            "rtc.c"
                            # Persistent settings store (NVS) + button-action bindings.
                            "settings.c"
                            "ui_theme.c"
                            # Orrery/star background (bottom layer) + corner masks.
                            "ui_background.c"
                            # Menu-first navigation shell (screen stack + widgets).
                            "ui_nav.c"
                            # Embedded LVGL fonts, generated 4bpp bitmaps (see fonts/README.md).
                            "fonts/nocsif_serif_30.c" "fonts/nocsif_serif_21.c"
                            "fonts/nocsif_serif_23.c" "fonts/nocsif_serif_26.c"
                            # Larger type-scale steps: full-charset serif 28 / 30 + mono 20 / 22.
                            "fonts/nocsif_serif_28.c" "fonts/nocsif_serif_30f.c"
                            "fonts/nocsif_mono_20.c" "fonts/nocsif_mono_22.c"
                            "fonts/nocsif_serif_13i.c" "fonts/nocsif_serif_16i.c"
                            "fonts/nocsif_serif_20i.c"
                            "fonts/nocsif_mono_11.c" "fonts/nocsif_mono_12.c" "fonts/nocsif_mono_13.c"
                            "fonts/nocsif_mono_14.c" "fonts/nocsif_mono_15.c" "fonts/nocsif_mono_16.c"
                            "fonts/nocsif_mono_18.c"
                            # Watchface hero: big numeric time (num_64) + larger italic date;
                            # peek variant: smaller numeric time (num_48) + bigger peek date (serif_20i).
                            "fonts/nocsif_num_64.c" "fonts/nocsif_num_48.c"
                            # Generated A8 image assets (see img/README.md).
                            "img/nocsif_img_star.c"
                            # Engraved celestial planet icons (A8, resvg-traced; Home ring).
                            "img/nocsif_cel_sun.c" "img/nocsif_cel_star.c" "img/nocsif_cel_moon.c"
                            "img/nocsif_cel_sun_flame.c" "img/nocsif_cel_sun_ray.c"
                            "img/nocsif_cel_moon_full.c" "img/nocsif_cel_moon_crescent.c" "img/nocsif_cel_moon_stars.c"
                            "img/nocsif_cel_star_spark.c" "img/nocsif_cel_star_burst.c"
                            # Menu icon font (generated 4bpp, see icons/README.md), plus a large
                            # celestial-motif cut for the Home-hub circles and a lighter 48px cut
                            # for the Control-Center toggles.
                            "icons/nocsif_icons.c" "icons/nocsif_icons_lg.c" "icons/nocsif_icons_cc.c" "icons/nocsif_icons_xl.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_driver_i2c esp_driver_spi esp_driver_gpio esp_lcd
                                     # GNSS receiver on a dedicated UART (gnss.c).
                                     esp_driver_uart
                                     # LoRa SX1262 via vendored RadioLib (components/RadioLib),
                                     # driven by lora.cpp through the custom S3 HAL (nocsif_esp_hal.h).
                                     RadioLib
                                     # I2S standard-mode TX for the MAX98357A speaker (audio.c).
                                     esp_driver_i2s
                                     fatfs sdmmc esp_driver_sdspi esp_tinyusb wear_levelling
                                     # USB-Serial/JTAG console driver (bridge.c reads host lines).
                                     esp_driver_usb_serial_jtag
                                     nvs_flash mbedtls esp_hw_support
                                     # esp_task_wdt + esp_reset_reason live in esp_system;
                                     # esp_core_dump_* live in espcoredump; the persistent
                                     # logbook's raw ring uses esp_partition.
                                     esp_system espcoredump esp_partition
                                     # esp_ota_* (A/B slot write + rollback) live in app_update.
                                     app_update
                                     # The vendored RFAL / ST25R3916 driver (C++ component in
                                     # firmware/components/rfal); nfc.cpp calls it.
                                     rfal
                                     # The vendored Bosch BHY2 sensor-hub API (C component in
                                     # firmware/components/bhy2); imu.c calls it.
                                     bhy2
                                     # minimp3 (header-only, CC0) for the Carts MP3 path (audio.c).
                                     minimp3
                                     # WiFi station: esp_wifi driver + esp_netif + the default
                                     # event loop (wifi.c). NVS is already required above.
                                     # Captive portal: esp_http_server (landing page) + lwip
                                     # sockets (the UDP:53 DNS redirector, via lwip below).
                                     esp_wifi esp_netif esp_event esp_http_server
                                     # mDNS responder (espressif/mdns, managed) so the companion
                                     # web UI resolves at nocsif.local.
                                     mdns
                                     # Weather: the first HTTP client (weather.c). esp_http_client
                                     # pulls esp-tls transitively; the mbedTLS CA bundle
                                     # (esp_crt_bundle_attach) ships in the already-required mbedtls
                                     # component; json = cJSON for the small forecast document.
                                     esp_http_client json
                                     # Bluetooth LE: the NimBLE host + BLE controller live in the
                                     # 'bt' component (ble.c). NVS is already required above.
                                     bt)
# esp_lcd_co5300 and esp_lvgl_port (which pulls lvgl) come from idf_component.yml
# (managed components); the component manager adds them to this component's
# requires automatically, so ui.c can #include "esp_lvgl_port.h" / "lvgl.h".
