# Planned Features

Features planned for future releases of NocSif.

<!-- Fill in the planned feature list below. -->
## Future state (planned)

Every risk-first milestone is shipped; what follows is capability expansion. This is a priority-ordered
menu, not a hard sequence — items are built one work-package per branch. Backlogged and long-horizon
ideas are intentionally excluded (see below).

- **WiFi recon & detection** — finish rogue-AP / evil-twin, add responder / management-frame-flood /
  deauth-flood detectors, flag nearby tools (Flipper-BLE / pwnagotchi / deauther / Pineapple), a
  cross-radio (WiFi+BLE) presence census, an audible/visual radio-event alerter, camera-glasses
  (Ray-Ban Meta) detection, and Flock ALPR-camera detect-and-direction-find (now ungated by BLE +
  Signal Hunt + GNSS).
- **WiFi active & network tools** — ESP-NOW device-to-device link, DNS-sinkhole / walled-garden on the
  own AP, **authorized network host & service discovery** (enumerate live hosts and open ports / services
  on a network you're connected to and cleared to assess — standard, lawful recon, nmap-on-the-wrist),
  DIAL / Chromecast control, a wireless-HID-over-WiFi console with an auditable keystroke witness log, and
  USB-Ethernet emulation.
- **BLE expansion** — GATT write / subscribe-notify / characteristic testing; HID mouse / media / gamepad;
  Nordic-UART serial bridge; persistent + coded-PHY (long-range) beacon/scan; custom GATT-server
  emulation; BLE mesh node; a bond-manager UI; and decoders for Continuity / Handoff / AirDrop and Fast
  Pair / Swift Pair, plus card-skimmer detection and an anti-stalking "a tracker is following me" alert;
  and a **BLE Advertisement Resilience Test** — controlled adverts (volume + malformed) against a device
  you own or are authorized to assess, a controlled-scope counterpart to advertisement-flood detection
  (a target under test, never saturation of bystander devices).
- **Off-grid & LoRa** — mesh telemetry / store-and-forward / traceroute, MQTT internet gateway, MeshCore
  and Reticulum/RNode stacks, repeater/router node, a narrowband in-band FSK `.sub` subset, cross-band
  BLE/web→LoRa bridge, and GNSS-time-synced collision-avoiding mesh slots; plus a **Sub-GHz RF Carrier /
  Resilience Test** — a controlled continuous-carrier (CW) output for antenna & matching characterization
  (VSWR / tuning) and interference-resilience testing of a receiver you own, ideally in a shielded /
  controlled environment (a test primitive for your own equipment, not a jammer).
- **Connectivity Governor** — geofenced, lease-driven radio power management (radios on only when
  useful): a per-radio lease + idle-timer, cyclic GPS + geofence engine, an auto-learned coords↔network
  geo-store for geofenced auto-connect, and a policy/UI layer. Phased P1–P4.
- **Companion control surface** — an on-network (same-LAN) phone/browser remote over the shipped SoftAP:
  mDNS `nocsif.local`, an open AP by default with an optional WPA2 password (no pairing code — the
  off-by-default toggle and the on-watch "linked" dot are the guardrails), a phone→watch command channel
  into the app registry, a watch→phone WebSocket live-push + interactive screen mirror (touch, side
  buttons, casting), a wireless `/sd` file browser (download / upload / delete), and a first-class
  Approvals lane (approve/deny/confirm) that sidesteps the iOS notification ceiling.
- **Watch utilities & audio apps** — image / GIF viewer, QR display, bubble level / inclinometer, and
  audio tools (theremin, data-over-sound acoustic modem, dB/SPL meter, live spectrum analyzer + tuner).
- **USB host mode & platform** — USB host with peripheral class drivers, U2F/FIDO CTAP over HID,
  MicroPython drop-in script layer, Flipper-format parsers + an app/script manager, and a self-hosted
  OTA path.

- **NFC & haptics** — the NFC driver is complete and haptic support is a small add; both are blocked
  only by the faulty hardware on my personal test watch and **will ship once I receive a replacement
  watch from LilyGo.** NFC brings read / write / emulate / NDEF and tap-to-pair/-type combos; haptics
  restores the tactile alerting channel.

**Excluded (backlogged / long-horizon, by operator decision):** step-count / workout / sleep tracking,
GPX route navigation & track-back, localization / i18n, an on-watch web browser, and the long-horizon
concept set (head-coupled 3D idle scene, cloud TTS voice assistant, the USB-C "Backpack" co-processor,
and the home-server thin-client architecture).
