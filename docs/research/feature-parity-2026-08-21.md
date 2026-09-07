# NocSif — full capability catalog & harmful-feature boundary (2026-08-21)

> **What this is.** A *huge* reference catalog of what the well-known multi-tools do — cross-checked against the
> **T-Watch Ultra (ESP32-S3 + fitted silicon)** — with the genuinely **harmful** capabilities (the ones that hurt
> people who never consented) **marked ☠ and declined**. It is a **catalog, not a plan**; most rows will never be
> built. Anything actually planned lives in `docs/PLAN.md` §4.
>
> **Design of this doc (per the decisions that shaped it):** (1) it's *big* — features are listed granularly, not
> collapsed; (2) there is **no "silicon can't ever do it" list** — permanently-blocked items (wideband SDR, BT
> Classic) are just omitted, and module-based gaps are tagged 🎒 for the Backpack co-processor; (3) the **primary
> job is marking the harmful features** so the scope boundary is unmistakable. Descriptions stay device-class
> neutral (PLAN §1); the harm is named plainly because **naming it is the marking.** This catalog **labels and
> declines** harmful techniques — it is not a how-to for any of them.

## Legend
| Tag | Meaning |
|---|---|
| ✅ | built & verified on-device |
| 🔵 | planned (a milestone in PLAN §4) |
| 🕳️ | buildable — silicon supports it, no milestone yet |
| 🎒 | needs the **NocSif Backpack** co-processor (OOK / 125 kHz / IR / iButton / nRF24) |
| ☠ | **HARMFUL — will NOT build.** Primary effect is harm to **non-consenting third parties.** See the focus table. |
| † | silicon supports it, but **impractical on this worn watch** (moving-body / RAM / battery / mic-bandwidth / no-GPU limits). |

---

## ☠ HARMFUL-FEATURE FOCUS — the capabilities NocSif refuses
The point of the whole doc. These ship in the reference tools; **NocSif will not build any of them.** Each is
named at device-class level with the concrete harm, the tool(s) that carry it, and the **defensive twin** NocSif
*does* pursue (detecting the thing is fine — doing it to people is not).

| ☠# | Capability (neutral name) | Who it harms / how | Ships in | ✅ Defensive twin NocSif builds |
|---|---|---|---|---|
| ☠1 | **BLE pairing-popup advertisement flood** ("BLE spam" / "Sour Apple") | Every nearby phone — floods pairing dialogs; the iOS-17 variant **crashes the device.** | Ghost ESP, Bruce, Marauder, Flipper apps | Detect an advertisement flood |
| ☠2 | **Covert person-tracking via stealth Find-My tag** (key-rotating "Find You") | A person — **bypasses anti-stalking alerts** to track them without consent. | esp32-airtag, FindMyFlipper (misuse) | "A tracker is following me" anti-stalking alert |
| ☠3 | **Continuous indiscriminate WiFi disconnect flood** | Every nearby network — mass knock-offline (DoS). | Ghost ESP, Bruce, Marauder | Detect a management-frame flood |
| ☠4 | **Auto-impersonate-any-requested-SSID** ("Karma" / PineAP) | Devices/people — lures them onto a look-alike of a trusted network for interception. | WiFi Pineapple, Bruce | Karma-responder / evil-twin detection |
| ☠5 | **Credential-capture portal vs non-consenting users** ("evil portal" for real theft) | People — harvests real logins / 2FA. | Marauder, Ghost ESP, Wifiphisher | Detect a look-alike captive portal |
| ☠6 | **On-path MITM** (ARP/DNS/DHCP spoof + SSL-strip + credential sniffer) | People on the network — intercepts/rewrites their traffic. | Bettercap | Detect ARP/DNS spoofing on a joined net |
| ☠7 | **Wireless-HID keystroke injection into others' devices** ("MouseJack") | A bystander's PC — hijacks their wireless keyboard/mouse. | Bettercap, Flipper + nRF24 | — (receive-side only) |
| ☠8 | **Malformed / anomalous frame injection to crash devices** | Nearby stations — driver crashes / lockups. | Marauder | Detect malformed-frame anomalies |
| ☠9 | **Rolling-code capture-and-replay** ("RollJam") | Vehicle / garage owners — steals access. | EvilCrow RF | — |
| ☠10 | **Continuous-transmit RF / NAV jamming** (sub-GHz, 2.4 GHz, NAV/duration) | Everyone in range — denies the medium. | Bruce, PortaPack | Detect interference / a NAV-reservation storm |
| ☠11 | **GPS / GNSS spoofing** | Anyone relying on GNSS — falsifies location; safety hazard. | HackRF / PortaPack | Detect implausible/spoofed GNSS on our receiver |
| ☠12 | **Fully-autonomous indiscriminate handshake farming** (auto-deauth *everything*) | Every nearby network — hands-off mass deauth. | Pwnagotchi | Passive-only, opt-in-scope farming |
| ☠13 | **Covert badge/credential clone without consent** | The badge owner — impersonates their access. | Flipper, Proxmark, Chameleon | Authorized clone of *your own* badge only |

**The line, once:** NocSif does **targeted, authorized** testing — you point it at something you're allowed to
test. The ☠ set is out because each hits **bystanders indiscriminately**, **tracks/steals from a person
covertly**, or **damages** someone who never consented. Every ☠ row's *detect* twin is welcome.

> **Dual-use siblings NocSif *does* ship** (the authorized form of a ☠ primitive): a **targeted** management-frame
> disconnect for handshake capture (✅ M5-P5·1, cf. ☠3), and an **own-network** captive portal for testing your
> own setup (✅ M5-P5·3/4, cf. ☠5). The ☠ marking is precisely the *indiscriminate / against-non-consenting*
> version.

---

## Reference-tool roster (★ = you may not know it)
| Tool | Class | What NocSif harvests from it |
|---|---|---|
| **Flipper Zero** | multitool | NFC-HF, sub-GHz OOK, 125 kHz, IR, iButton, BadUSB, BLE. |
| **ESP32 Marauder** ★ | ESP32 WiFi/BLE | Deep recon + **detect-other-tools**, wardrive+GPX, web GUI. |
| **Ghost ESP** | ESP32 WiFi/BLE | Recon/AP/portal, BLE, GPS wardrive, **DIAL/Chromecast**, printer/TP-Link mgmt, games/RGB. |
| **Bruce** | ESP32 multitool | WiFi/BLE/USB + CC1101/PN532/nRF24/IR modules + **JS interpreter**. |
| **Nemo (n0xa)** ★ | M5Stick/Cardputer | Compact WiFi/BLE + IR-TV. |
| **WiFiDuck / Super / Ultra** ★ | ESP32-S2/S3 | **Wireless keystroke-injection console.** |
| **CapibaraZero / Willy / HackyPi 2.0** ★ | ESP32-S3 | Flipper-like decks; Flipper-file-compatible; AI-assisted BadUSB. |
| **Pwnagotchi** ★ | Pi / ESP32 ports | Autonomous handshake farming + A2C "personality" + **PwnGRID** peer mesh. |
| **OpenHaystack / FindMyFlipper** ★ | BLE | Turn a device into an Apple **Find-My tag** (own-asset use). |
| **Meshtastic / MeshCore ★ / Reticulum·RNode ★** | LoRa mesh | Off-grid messaging (Meshtastic/MeshCore) → full **networking stack** (Reticulum). |
| **EvilCrow RF** ★ | RF board | Dual-CC1101 sub-GHz (incl. rolljam). |
| **Bettercap** ★ | PC framework | The 802.11/BLE/HID/Ethernet recon+MITM engine (pwnagotchi's core). |
| **Kismet / Wifiphisher / Aircrack** ★ | PC tools | Passive multi-protocol recon (Kismet); rogue-AP phishing (Wifiphisher); WPA crack (Aircrack). |
| **spacehuhn Deauther** | ESP8266/32 | OG management-frame tool + **detect-deauth**. |
| **Hak5** (Ducky/Bunny/Pineapple/Key Croc) | gear | HID injection, USB attacks, WiFi auditing, USB-host keylog. |
| **Chameleon Ultra / Proxmark3 Iceman** ★ | NFC/RFID | Multi-slot card emulation; RFID gold standard. |

---

## NocSif today — already shipped (✅) vs. planned-but-incomplete (🔵)
The per-domain tables mark ✅/🔵 on each feature; this is the **native picture** — the watch-platform work that
isn't a reference-tool parity item but *is* the firmware you're running right now.

### ✅ Shipped & verified on-device
- **Core / HAL** — boot; 16 MB flash + 8 MB PSRAM; I²C scan; **AXP2101 PMU** rail-gating; **battery %/charge fuel
  gauge**; **PCF85063A RTC**; physical-button service (PWR IRQ + FN); XL9555 expander.
- **Storage** — microSD FAT32 over SDSPI.
- **USB (M4)** — composite CDC+MSC+HID; HID keyboard (US/GB/DE) + DuckyScript player; runtime USB mode picker.
- **Watch shell (P1–P8)** — menu-tree launch-by-id app registry; NVS settings; **passcode lock** + unlock keypad;
  **peek/lock watchface**; input safety (palm/cover rejection); deliberate-wake; reduced-motion; the **v2 IA**
  (3-circle celestial hub · carousel peek dials · Control Center).
- **Reliability** — task-WDT + UI-liveness watchdog; coredump; **safe-mode**; logbook ring; System→Diagnostics.
- **WiFi (M5)** — the entire ✅ column of §1 below (recon → monitor → capture → detect → targeted mgmt-frame →
  own-network AP/portal → live-PCAP).
- **BLE (M7)** — the entire ✅ column of §2 below (scan · classify trackers/drones · Signal Hunt · advert-PCAP ·
  GATT-read · BLE HID keyboard · iBeacon/named beacon · ANCS/AMS phone companion · bonding).
- **M11 watch-core (in progress)** — **IMU** accel bring-up (BHI260AP); **shake-to-wake** + **shake-to-sleep**;
  **speaker audio** (MAX98357A — boot chime + alert cues); **PDM mic** + level-meter screen.

### 🔵 In the plan, not complete
- **M11 remaining** — voice memo (record → WAV → SD → playback); motion gestures. **⚠ haptic (DRV2605) is
  hardware-dead on this unit** — shelved (RMA).
- **M6 NFC** — RFAL→ESP-IDF driver ported & merged; the transmitter is **hardware-dead on the current unit**, but
  a **fully-working replacement watch is planned** → NFC is a first-class 🔵 milestone (full HF parity in §3 + the
  NFC-combination tools in §J).
- **M8 GNSS · M9 LoRa · M10 USB-ext · platform layers** (MicroPython + app/plugin manager + Flipper-file import).

---

## THE BIG LIST — every reachable feature, by domain (☠ marks the harmful ones inline)

### 1. WiFi (2.4 GHz) — deepest area; M5 essentially complete
| # | Feature | NocSif |
|---|---|---|
| 1 | AP scan (ESSID/BSSID/channel/RSSI/security) | ✅ |
| 2 | Station / client scan | ✅ |
| 3 | Probe-request harvest (who's looking for what) | ✅ |
| 4 | Live "who's here" AP/client/probe lists | ✅ |
| 5 | Promiscuous monitor / raw packet capture | ✅ |
| 6 | Air-activity meter + per-channel spectrum + frames/sec | ✅ |
| 7 | Channel hop / lock | ✅ |
| 8 | WPA/WPA2 EAPOL handshake capture | ✅ |
| 9 | Clientless PMKID capture | ✅ |
| 10 | hc22000 / hashcat export | ✅ |
| 11 | Live-PCAP over USB-CDC (Wireshark extcap) | ✅ |
| 12 | Beacon transmit / custom SSID broadcast | ✅ |
| 13 | Novelty-SSID / "rickroll" beacon list | ✅ (nuisance-mild) |
| 14 | **Targeted** deauth/disassoc for handshake capture | ✅ |
| 15 | Software / open AP | ✅ |
| 16 | **Own-network** captive portal (credential form) | ✅ |
| 17 | Rogue-AP / evil-twin **detection** | ✅ / 🕳️ |
| 18 | Twin-detector / anomaly alerts | ✅ |
| 19 | Karma-responder / PineScan (Pineapple) **detection** | 🕳️ |
| 20 | Management-frame-flood **detection** | 🕳️ |
| 21 | **Detect-other-tools** (Flipper-BLE / pwnagotchi / deauther / Pineapple / ESP32) | 🕳️ |
| 22 | Autonomous **passive** handshake farm (opt-in scope) + a "face" | 🕳️† (battery-windowed, not all-day) |
| 23 | **WiFi CSI** motion / presence sensing (no camera) | 🕳️† (set-down only; ESP32 CSI low-fidelity) |
| 24 | **ESP-NOW** device-to-device link (NocSif↔NocSif) | 🕳️ |
| 25 | Wireless-payload **console** (connect over WiFi, push HID) | 🕳️ |
| 26 | DNS-sinkhole / walled-garden on **own** AP | 🕳️ |
| 27 | Network host discovery / port scan on joined net | 🕳️ |
| 28 | SSH / telnet / net clients | 🕳️ |
| 29 | Deauth-flood **detector** ("is this network under load") | 🕳️ |
| 30 | DIAL / Chromecast cast control | 🕳️ |
| 31 | Network-printer / TP-Link device mgmt (Ghost ESP) | 🕳️ |
| 32 | WPS scan / info | 🕳️ |
| 33 | WiFi wardriving (WiFi/BLE + GPS + GPX/WiGLE) | 🔵 (M8) |
| 34 | Web-UI companion / over-internet remote control | 🔵 / 🕳️ |
| — | ☠3 continuous indiscriminate deauth flood · ☠4 Karma auto-SSID · ☠5 phishing portal · ☠6 MITM · ☠8 malformed-frame crash · ☠12 auto-deauth farm | ☠ |

### 2. Bluetooth LE (5.0) — other deep area; M7 largely complete
| # | Feature | NocSif |
|---|---|---|
| 1 | Observer scan (all advertisers) | ✅ |
| 2 | Classify trackers (AirTag / Tile / SmartTag) | ✅ |
| 3 | Decode drones / OpenDroneID Remote-ID (+ pilot pos) | ✅ |
| 4 | Detect camera-glasses (Ray-Ban Meta) | 🔵 |
| 5 | Detect card-skimmer BLE signatures | 🕳️ |
| 6 | Signal Hunt — RSSI proximity direction-find | ✅ |
| 7 | Advertisement capture → PCAP to SD | ✅ |
| 8 | GATT explore — connect / enumerate / **read** | ✅ |
| 9 | GATT **write** | 🕳️ |
| 10 | GATT **subscribe / notify** (live streams) | 🕳️ |
| 11 | GATT characteristic **write-testing / fuzz** (authorized device) | 🕳️ |
| 12 | Pairing-security assessment (Just-Works / weak) — own device | 🕳️ |
| 13 | BLE HID keyboard (DuckyScript over BLE) | ✅ |
| 14 | BLE HID mouse | 🕳️ |
| 15 | BLE HID consumer / media keys | 🕳️ |
| 16 | BLE HID gamepad | 🕳️ |
| 17 | iBeacon broadcaster | ✅ |
| 18 | Named / Eddystone broadcaster | ✅ |
| 19 | Persistent / background beacon | 🕳️ |
| 20 | Phone companion — ANCS notifications | ✅ |
| 21 | Phone companion — AMS media remote | ✅ |
| 22 | Bonding / persistent link / auto-reconnect | ✅ |
| 23 | Nordic-UART (NUS) serial bridge | 🕳️ |
| 24 | Continuity / Handoff / AirDrop **decode** | 🕳️ |
| 25 | Fast Pair / Swift Pair **decode** | 🕳️ |
| 26 | **"A tracker is following me"** anti-stalking alert | 🕳️ (twin of ☠2) |
| 27 | **Find-My locator tag** for your *own* watch/asset | 🕳️ |
| 28 | Detect a BLE advertisement flood | 🕳️ (twin of ☠1) |
| 29 | Coded-PHY (BLE 5) long-range scan / beacon | 🕳️ |
| 30 | Standard sensor clients — HRM strap / cycling / battery / env | 🕳️ |
| 31 | Device impersonation / custom GATT-server emulation (authorized) | 🕳️ |
| 32 | BLE mesh node | 🕳️ |
| 33 | Bond-manager UI (list / remove) | 🕳️ |
| — | ☠1 pairing-popup flood/Sour Apple · ☠2 covert Find-My tracking | ☠ |

### 3. NFC / RFID-HF — 🔵 planned (M6), unlocked by the incoming fully-working watch
> The NFC transmitter is **hardware-dead on the *current* unit**, but a **fully-working replacement watch is
> planned** — so NFC is a first-class **🔵 planned** capability, not a dead-end. The **ST25R3916** does read +
> write + **card emulation** across ISO14443-A/B, ISO15693/NFC-V, and FeliCa. Full Flipper / Proxmark / Chameleon
> HF parity below; **dual-use rows are 🔵 for your OWN cards, ☠ against a non-consenting holder.**
| # | Capability | From | NocSif |
|---|---|---|---|
| 1 | Multi-protocol tag read & identify (NFC-A/B/V/F: UID, ATQA/SAK/ATS, family) | Flipper/Proxmark | 🔵 |
| 2 | MIFARE Classic read / write / dump (CRYPTO1 auth vs key dictionary) | Proxmark/Flipper | 🔵 |
| 3 | MIFARE Ultralight / NTAG read / write (T2T, GET_VERSION, READ_SIG, dump/restore) | Flipper | 🔵 |
| 4 | DESFire / ISO-DEP APDU explorer (enumerate apps/files, AES/3DES auth, APDU console) | Proxmark | 🔵 |
| 5 | ISO15693 / NFC-V read / write (iCLASS · PicoPass HF groundwork) | Proxmark | 🔵 |
| 6 | FeliCa (NFC-F) read (IDm/PMm, systems/services) | Proxmark/Flipper | 🔵 |
| 7 | MIFARE Classic key recovery (dictionary / nested / hardnested / mfkey32) | Proxmark | 🔵 |
| 8 | NDEF read / write / compose (URI, Text, WiFi/BT handover, contact) | Flipper | 🔵 |
| 9 | Card emulation (ISO14443-A listen/target: set ATQA/UID/SAK, T2T/NDEF) | Chameleon | 🔵 |
| 10 | Multi-slot / multi-card emulation (a wallet of stored profiles) | Chameleon | 🔵 |
| 11 | Amiibo read / emulate (NTAG215 + derived keys) | Flipper | 🔵 |
| 12 | Contactless reader response fuzzing (malformed frames → your own reader) | Proxmark | 🔵 |
| 13 | Active peer-to-peer (NFCIP-1 / NFC-DEP, LLCP/SNEP) | device-native | 🔵 |
| 14 | `.nfc` import / export (Flipper format + NocSif wallet bundle) | Flipper | ✅ parser / 🔵 |
| 15 | UID / card duplication to writable "magic" cards (gen1a/gen2 backdoor) | Proxmark/Flipper | 🔵 own-card · ☠ non-consented |
| 16 | EMV / payment application-data read (offline read, never a transaction) | Flipper/Proxmark | 🔵 own-card · ☠ non-consented |
| 17 | Contactless session trace capture (frame-by-frame → SD; feeds key recovery) | Proxmark | 🔵 own-card · ☠ non-consented |
| 18 | 125 kHz LF (EM4100 / HID Prox / Indala) read/emulate/write | Backpack | 🎒 |
| — | **NFC combination tools** (tap-to-type, card wallet, relay, audit trail, …) | | → **§J** |
| — | ☠13 covert badge clone without consent | | ☠ |

### 4. Sub-GHz · IR · iButton · nRF24 — Backpack co-processor
| # | Feature | NocSif |
|---|---|---|
| 1 | Sub-GHz OOK read / save / replay (garage / gate / fixed-code fob) | 🎒 |
| 2 | Sub-GHz FSK in-band subset of `.sub` (SX1262 modem, narrowband) | 🕳️-partial |
| 3 | Sub-GHz frequency analyzer / spectrum | 🎒 |
| 4 | `.sub` file **parsing** | 🔵 (parser) |
| 5 | TPMS / weather-station / sensor decode | 🎒 |
| 6 | IR universal remote / learn / replay | 🎒 |
| 7 | IR "turn any TV off" sweep (nuisance) | 🎒 |
| 8 | iButton / 1-Wire read / emulate / write | 🎒 |
| 9 | nRF24 scan / sniff | 🎒 |
| — | ☠9 rolljam · ☠10 RF jammer · ☠7 MouseJack keystroke-inject | ☠ (also 🎒) |

### 5. GNSS — planned M8
| # | Feature | NocSif |
|---|---|---|
| 1 | Location + accurate time | 🔵 |
| 2 | Wardrive GPS tagging | 🔵 |
| 3 | GPX / WiGLE export | 🔵 |
| 4 | Track logging / breadcrumb | 🕳️ |
| 5 | Geofence / region enter-exit | 🕳️ |
| — | ☠11 GNSS spoofing | ☠ |

### 6. LoRa / mesh — planned M9 (SX1262)
| # | Feature | NocSif |
|---|---|---|
| 1 | LoRa point-to-point messaging | 🔵 |
| 2 | Meshtastic-compatible mesh | 🔵 |
| 3 | Encrypted channels (AES-256 + key exchange) | 🔵 |
| 4 | Position sharing over mesh | 🔵 |
| 5 | Telemetry / sensor relay | 🕳️ |
| 6 | Traceroute / canned messages / store-and-forward | 🕳️ |
| 7 | MQTT internet gateway (mesh ↔ internet) | 🕳️ |
| 8 | MeshCore stack | 🕳️ |
| 9 | Reticulum / RNode stack (multi-transport) | 🕳️ |
| 10 | Repeater / router node | 🕳️ |
| 11 | Emergency beacon / SOS | 🕳️ |

### 7. USB / HID
| # | Feature | NocSif |
|---|---|---|
| 1 | BadUSB HID keyboard (wired) + DuckyScript player | ✅ |
| 2 | US / GB / DE keymaps | ✅ |
| 3 | HID over BLE (wireless DuckyScript) | ✅ |
| 4 | Composite MSC + HID + CDC, runtime mode pick | ✅ |
| 5 | Mass-storage file drop / exfil container | ✅ |
| 6 | USB-Ethernet emulation (CDC-ECM / RNDIS) | 🕳️ |
| 7 | U2F / FIDO token | 🕳️ |
| 8 | USB-host: read foreign drives | 🔵 (M10) |
| 9 | USB-host: inline keylogger (authorized machine) | 🔵 (M10) |
| 10 | Wireless-payload console (WiFiDuck-style) | 🕳️ |

### 8. Platform / scripting / novelty
| # | Feature | NocSif |
|---|---|---|
| 1 | On-device MicroPython interpreter | 🔵 |
| 2 | Flipper-file import (`.sub` / `.nfc` / `.ir`) | 🔵 |
| 3 | App / plugin manager, no-reflash drop-in | 🔵 |
| 4 | Web-UI companion | 🔵 |
| 5 | Over-internet remote relay control (§7) | 🕳️ |
| 6 | **On-wrist dev-agent approvals** (§7) | 🕳️ |
| 7 | QR display / scan; notes; utilities | 🕳️ |
| 8 | Games / music visualizer / RGB | 🕳️ |

### 9. Display / screen — the T-Watch's standout hardware
The panel is a **CO5300 QSPI AMOLED, 24-bit RGB888, 410×502 rounded-rect** — far richer than a Flipper's mono LCD,
so this is a whole category the classic multitools barely have. **⚠ the rounded corners physically clip UI —
keep content in the safe zone (PLAN §1).**
| # | Feature | NocSif |
|---|---|---|
| 1 | 24-bit RGB888 AMOLED + capacitive touch (CST9217) | ✅ |
| 2 | LVGL v9 UI + colour-token theme + embedded serif/mono fonts | ✅ |
| 3 | Pipelined 2-buffer partial-refresh flush (~29 ms) + persistent-DMA stage | ✅ |
| 4 | Render-once orrery / star background + corner masks | ✅ |
| 5 | Shooting-star comet animation | ✅ |
| 6 | Boot splash (+ white-flash fix) | ✅ |
| 7 | Rotating 3-circle celestial hub + engraved sun/moon/star icons | ✅ |
| 8 | Peek watchface (weather · time · date · battery · BT glyph · activity chip · carousel dials) | ✅ |
| 9 | Control Center overlay (swipe-down · live brightness · toggles · media card) | ✅ |
| 10 | Passcode lock + unlock keypad screens | ✅ |
| 11 | Live-capture screens (AP/client/probe lists · Signal Hunt meter · monitor spectrum) | ✅ |
| 12 | Live brightness control + reduced-motion flag | ✅ |
| 13 | Double-tap-wake · shake-to-wake · shake-to-sleep | ✅ |
| 14 | Theme / wallpaper / font switching | ✅ mockup / 🔵 |
| 15 | Always-on display (AOD) — off by default (power) | 🔵 |
| 16 | Image / photo viewer | 🕳️ (Flipper, Bruce) |
| 17 | GIF / animation player | 🕳️ |
| 18 | On-screen games (snake / dolphin-style) | 🕳️ (Flipper, Ghost ESP) |
| 19 | A **celestial mascot / pet face** (à la Flipper dolphin / pwnagotchi face) | 🕳️ |
| 20 | Audio / music visualizer, RGB "rave" | 🕳️ (Ghost ESP) |
| 21 | QR-code display; hex / text viewer for captures | 🕳️ |
| 22 | Head-coupled 3D "window into a room" (IMU tilt) | 🕳️† no GPU → baked frames / low fps (PLAN §7 parked) |

---

## The "dig-deep" payoff — fresh buildable candidates the wide sweep surfaced
All 🕳️ buildable, all benign/defensive:
- **Detect-other-tools** — flag a nearby Flipper (BLE), pwnagotchi, deauther, Pineapple, or ESP32 rig (Marauder).
- **WiFi CSI presence/motion radar †** — Espressif's `esp-csi`: sense people via WiFi reflections, no camera. *(† set-down only — a worn/moving watch swamps the signal; ESP32 CSI is low-fidelity.)*
- **ESP-NOW NocSif↔NocSif link** — connectionless device-to-device channel, no AP.
- **Find-My locator tag (own asset)** — the benign side of OpenHaystack (covert-tracking misuse is ☠2).
- **Anti-stalking "tracker follows me" alert** — defensive twin of the stealth-tag threat.
- **Autonomous passive handshake farm + a face** — pwnagotchi *personality* without the indiscriminate auto-deauth.
- **Wireless-payload console** — you already have HID + AP + web UI; this is the glue (WiFiDuck-style).
- **MeshCore / Reticulum on the SX1262** — alt mesh stacks beyond Meshtastic (M9).
- **Detect-a-flood / detect-a-spoof / detect-a-rogue-AP** — the defensive twin of every ☠ row.

---

## Combined / fusion tools — capabilities that emerge from COMBINING silicon
The T-Watch's deep sensor stack means the most interesting tools live in the *combinations*, not any one radio.
Generated by a fusion sweep across 8 silicon-combination axes and deduped from ~100 candidates. Every entry names
the subsystems it fuses and genuinely needs 2+ of them. **★ standout · † silicon-present but marginal on this
worn watch · ☠ harmful (the *combination* is what makes it harmful).**

### A. Location & mapping — radio + GNSS + RTC + SD
| Tool | Fuses | What | NocSif |
|---|---|---|---|
| ★ Geo-tagged RF site survey (wardrive) | WiFi+BLE+GNSS+SD+RTC | one coord+time-stamped record per sighting → WiGLE-style coverage map | 🔵 |
| Live survey map over web portal | WiFi+GNSS+webUI | a phone browser watches the map build live + CSV, no app | 🔵 |
| Location-provenance audit capture | WiFi+GNSS+SD+RTC | stamps each own-network capture with where/when (chain-of-custody) | 🔵 |
| ★ GNSS-disciplined multi-node timeline | GNSS+RTC+WiFi+BLE+LoRa | PPS-disciplined clock → sub-second frame stamps; multi-watch shared timeline | 🔵 |

### B. Presence · direction-finding · counter-surveillance — WiFi + BLE + IMU + audio
| Tool | Fuses | What | NocSif |
|---|---|---|---|
| ★ Cross-radio presence census | WiFi+BLE | dedupe a device seen on both radios → true "who's here" count | 🕳️ |
| Dual-radio signature corroborator | WiFi+BLE | confirm/flag infrastructure only when WiFi+BLE signatures agree | 🕳️ |
| ★ Companion-device follow detector | BLE+WiFi+IMU+GNSS | IMU+GNSS confirm you're travelling → warn when a device co-moves with you (anti-stalking) | 🕳️ |
| Off-wrist theft / loss guardian | IMU+BLE+audio+GNSS | removed-from-wrist + phone-tether lost → chirp + log/beacon position | 🔵 |
| ★ Eyes-free RF direction finder | BLE/WiFi-RSSI+IMU+audio | sweep the wrist; RSSI + bearing + rising speaker pitch walk you in (haptic dead → audio) | 🕳️ |

### C. Off-grid comms — LoRa / ESP-NOW + GNSS + display
| Tool | Fuses | What | NocSif |
|---|---|---|---|
| ★ Off-grid position beacon / group locator | GNSS+LoRa+RTC+Display | share your GNSS fix over LoRa; peers plotted bearing/distance (hiking / SAR) | 🔵 |
| ★ Motion-triggered distress beacon | IMU+GNSS+LoRa+audio+Display | fall/gesture → repeating LoRa distress frame w/ position + locator tone | 🔵 |
| ★ Two-radio squad link | ESP-NOW+BLE | BLE = zero-pair discovery, ESP-NOW = the data pipe; no AP/internet | 🕳️ |
| Cross-band message bridge | LoRa+WiFi+BLE+Display | phone types over BLE / web-UI → relayed out over LoRa; replies come back | 🔵 |
| Time-synced store-and-forward LoRa mesh | LoRa+GNSS+RTC | GNSS time → collision-avoiding slots + hold-and-forward for out-of-range peers | 🔵 |
| Off-grid RF-environment relay node | WiFi+BLE+LoRa | monitor local RF, forward a summary over LoRa past WiFi range | 🔵 |
| Return-to-waypoint homing | GNSS+BLE/LoRa+IMU | GNSS gets you close, dropped-beacon RSSI + IMU dead-reckoning for the last metres | 🔵 |

### D. Navigation & time — GNSS + IMU + RTC
| Tool | Fuses | What | NocSif |
|---|---|---|---|
| Pedestrian dead-reckoning breadcrumb | GNSS+IMU+RTC+Display | IMU steps carry the track through GNSS dropouts; back-trackable | 🔵 |
| Satellite clock + auto-timezone | GNSS+RTC+Display | GNSS UTC + lat/lon → correct local time & zone, no phone/WiFi | 🔵 |

### E. Sensing — occupancy & acoustic (mic / speaker / CSI)
| Tool | Fuses | What | NocSif |
|---|---|---|---|
| ★† CSI device-free occupancy sensor | CSI+IMU+audio+BLE | set-down+still (IMU gate) → CSI senses room motion → chime / push | 🕳️† |
| † Acoustic + RF room monitor | CSI+mic+IMU+BLE | set-down; cross-confirm presence from CSI motion + loud sound | 🕳️† |
| ★† Acoustic co-presence / OOB pairing | speaker+mic+BLE+ESP-NOW | two watches trade a chirp → proof they're in the same room (16 kHz mic → coarse, not true ranging) | 🕳️† |
| Context-stamped audio capture | mic+GNSS+RTC+WiFi+SD | voice memo w/ embedded position, time, RF-fingerprint → findable later | 🔵 |
| Geo-time soundscape / noise logger | mic+GNSS+RTC+SD | log sound level vs position/time → walkable noise-exposure map | 🔵 |
| Clap / whistle hands-free capture trigger | mic+WiFi+RTC | a clap/whistle bookmarks or starts/stops a WiFi capture | 🕳️ |

### F. Alerting & context automation — sensors gate the radios/UI
| Tool | Fuses | What | NocSif |
|---|---|---|---|
| ★ Radio-event audible & visual alerter | WiFi+BLE+audio+Display | watched device appears/leaves or an anomaly fires → chime + full-screen card | 🕳️ |
| Context/power-aware radio governor | PMU+RTC+IMU+WiFi+BLE | worn/charge/time jointly decide when background scans run & how deep | 🕳️ |
| RF-fingerprint geofence automation | IMU+WiFi+BLE+RTC | learn a place's RF fingerprint → GPS-free "home/desk" automations | 🕳️ |
| GNSS-geofenced capture autostart | GNSS+WiFi+PMU+RTC | enter a site geofence → auto-arm+power capture; leave → disarm+power down | 🔵 |
| Fused-presence proximity lock | BLE+WiFi+IMU+Display | stay unlocked only while phone-link + known-SSID + worn all agree | 🕳️ |

### G. Motion UI & interaction — IMU + display + touch
| Tool | Fuses | What | NocSif |
|---|---|---|---|
| Motion-gated display + privacy blank | IMU+Display | raise/shake wake, shake blank, face-down instantly blanks+locks (shoulder-surf shutter) | ✅ |
| Motion & tap alt-input (crown substitute) | IMU+Display+Touch | roll-to-scroll, flick-to-page, knock-as-button when the screen's unusable | 🕳️ |
| Parallax depth watchface | IMU+Display | layered star scene shifts with tilt → a window into depth (2.5D layers) | 🕳️ |
| Bubble level & inclinometer | IMU+Display | gravity vector → live spirit-level + tilt angle | 🕳️ |
| Motion-pattern unlock | IMU+Display+Touch | an air-drawn tilt/shake sequence as unlock secret or 2nd factor | 🕳️ |
| ★ Fall / impact alert w/ audio escalation | IMU+Display+Touch+audio | free-fall+impact → cancel-countdown → loud alarm if no response | 🕳️ |
| Bump-to-pair handshake | IMU+WiFi+BLE+Display | two watches knocked together → co-timed impact unlocks a key exchange | 🕳️ |

### H. Input bridge — motion/radio → HID (on a host you control)
| Tool | Fuses | What | NocSif |
|---|---|---|---|
| ★ Gesture-fired HID macro & presenter | IMU+BLE/USB HID+Display+audio | a wrist gesture emits a DuckyScript macro / advances slides hands-free | 🕳️ |
| ★ Wireless HID macro console | USB/BLE HID+WiFi web UI+SD+Display | pick/launch a microSD DuckyScript (or free-type) from a phone; display echoes what's sent | 🕳️ |
| ★ Worn credential typer (HW password manager) | SD vault+Display+Touch+USB/BLE HID | unlock on-watch → type a stored secret into a host, never on-screen / on-network | 🕳️ |
| Presence-driven host automation | BLE+IMU+HID+GNSS+RTC | desk-beacon seen → auto-login macro; walked-away + fading link → auto-lock | 🕳️ |
| Long-range remote host control | LoRa+USB/BLE HID+Display | a LoRa message tells the watch to lock / run a macro on its host, off-grid | 🔵 |
| ★ Auditable HID witness console | USB/BLE HID+Display+WiFi+SD | every keystroke shown + logged + live-streamed to a browser → witnessed HID | 🕳️ |

### J. NFC combinations — tap-driven tools (needs the working watch · M6)
With a live ST25R3916, a tap becomes a trigger and the watch becomes a card. All 🔵 (NFC = M6).
| Tool | Fuses | What | NocSif |
|---|---|---|---|
| ★ On-wrist card wallet & .nfc vault | NFC+display+touch+SD | browse a carousel of *your own* enrolled cards, tap to arm → emulate the picked UID/NDEF (Chameleon-on-wrist) | 🔵 |
| ★ Tap-to-type credential / login token | NFC+USB/BLE HID | a tag's UID picks a stored credential the watch types into the host (user/Tab/pass/Enter); presence-interlock unlock | 🔵 |
| ★ Tap-to-fire DuckyScript macro deck | NFC+DuckyScript+HID+SD | bind a `.txt` macro to a tag UID → tap plays it over USB/BLE; a deck of tags = a physical macro pad | 🔵 |
| ★ NFC out-of-band pairing & handover | NFC+BLE+WiFi | a tap carries the BLE/WiFi params → link bootstraps with no typed password; touch defeats an over-the-air listener | 🔵 |
| ★ Geo+time-stamped access-audit trail | NFC+GNSS+RTC+SD+PMU | every reader/tag interaction stamped where+when → exportable evidence trail for a commissioned audit | 🔵 |
| ★ Motion-gated / gesture-presented emulation | NFC+IMU+speaker | emulation stays dark until a deliberate wrist gesture (never silently presentable in a pocket) + audio cue when live | 🔵 |
| ★ Reader-field & relay awareness monitor | NFC+speaker+display+BLE | sense a nearby energized reader / abnormal 2nd field → tone + strength meter + phone alert (defensive) | 🔵 |
| ★ NFC web console & enrollment station | NFC+WiFi+SD | drive the whole read/write/emulate surface + card library from a phone browser; batch-enroll tags | 🔵 |
| Live tag inspector | NFC+display+touch | render a present tag's structure live (tech/UID/SAK/page-map/NDEF), tap to expand | 🔵 |
| On-wrist tag authoring & verify | NFC+display+touch+speaker | compose an NDEF record on the keyboard, write your own blank, read back to verify (eyes-free pass/fail) | 🔵 |
| NFC-seeded one-time code | NFC+RTC+HID | a tapped tag + the RTC time window → the watch computes a TOTP/HOTP and types it (2FA) | 🔵 |
| NFC / passcode interlock | NFC+passcode+display | tap a known tag to unlock (or 2FA); the card store + emulate sits behind the passcode (lost-watch safe) | 🔵 |
| Tap-to-launch on-watch routine | NFC+SD+WiFi+BLE | a known NDEF tag drives an on-device action (jump to a screen, arm WiFi monitor, toggle a radio) | 🔵 |
| Context-gated / self-expiring credentials | NFC+GNSS+RTC+PMU+SD | a card presents only inside a geofence / time window / on dock power; auto-deletes when the window closes | 🔵 |
| Eyes-free NFC confirmation tones | NFC+speaker | distinct chirps for read / no-tag / emulating (the tap lands against the watch back; haptic dead) | 🔵 |
| NFC antenna / field-power diagnostic | NFC+PMU | drive the carrier while the battery gauge reads current draw → flag a shorted/open front-end (extends the M6 self-test) | 🔵 |

### I. ☠ Harmful fusion combos — WILL NOT build (the *combination* is the harm)
The fusion forms of the ☠ boundary at the top — each pairs otherwise-benign blocks into a covert-tracking,
indiscriminate-disruption, or credential/exfil capability. Listed so the boundary stays explicit.
| ☠ Tool | Fuses | The harm | ✅ Defensive twin |
|---|---|---|---|
| Cross-radio persistent re-identifier | WiFi+BLE | defeats MAC/RPA randomization → covertly tracks a specific person across both radios | point it at your OWN devices to audit how linkable they are |
| Persistent presence recorder | BLE+WiFi+GNSS+LoRa+SD | reconstructs where a person goes over time + ships the trail off-device | the follow-detector (warns the WEARER); consented own-tag locating |
| Covert location beacon | GNSS+LoRa | a planted device silently reports its own position long-range | consented, always-indicated asset finder |
| Captive-portal-to-HID text relay | WiFi portal+USB HID+SD | captures another person's portal login + replays it into a host | self-service onboarding kiosk (user types their OWN creds, consented) |
| HID-triggered out-of-band return loop | USB HID+WiFi web server+SD | moves data off a host, around its owner's network monitoring | authorized offline asset-inventory over an isolated link |
| Motion/location-gated management-frame trigger | WiFi mgmt-TX+IMU+GNSS | gesture/geofence fires broadcast disconnects at networks you don't own | allowlisted own-AP roaming test + the receiver-side alerter |
| Scheduled multi-band connectivity-denial sweep | WiFi+BLE+RTC+GNSS | timed, indiscriminate WiFi+BLE disruption for everyone in range | own-device-only resilience test w/ stop-on-strangers |
| Geofenced auto-arming duplicate-SSID capture portal | GNSS+WiFi+RTC | auto-presents a look-alike + capture page to bystanders at a place/time | geofenced OWN-AP auditor, allowlisted + logged |
| Contactless relay bridge (NFC) | NFC+BLE/WiFi/LoRa | tunnels a genuine card's live responses to a distant reader → used far from a non-consenting holder | measure your OWN system's relay resistance; the reader-field awareness monitor |
| HF presence survey / UID wardrive (NFC) | NFC+GNSS+WiFi/LoRa | reads + geolocates UIDs of cards on passers-by → tracks/profiles people by their credentials | map only your OWN tags / a commissioned inventory; own-card leak-range meter |
| NFC quick-capture clone (non-consented) | NFC+display+SD | reads a non-consenting card into an emulable profile → a working duplicate | own-cards only: commit only after you confirm + label it as yours |
| NFC read-to-host logbook (non-consented) | NFC+RTC+HID | reads a bystander's card + types the data into a host log | own-card inventory only, or a consented awareness demo |

## Escape hatch
The **🎒 NocSif Backpack** (PLAN §7) — USB-C co-processor with OOK / 125 kHz / IR / iButton / nRF24 silicon —
later converts the 🎒 rows into buildable ones. Their *harmful* uses (rolljam, jammer, MouseJack) stay ☠ anyway.
