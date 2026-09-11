<!-- ★ NocSif ★ -->
# NocSif

**A modular security-testing firmware for the LilyGo T-Watch Ultra (ESP32-S3).**

> ✦ ˚ · ｡ NocSif — a wireless-testing multitool on the wrist ｡ · ˚ ✦

Hi! I coded this firmware for my T-watch Ultra just to kind of take advantage of the idea of having a wearable device that can do a bunch of penetration testing and signal monitoring things. 
Unfortunately my T-watch Ultra came with broken NFC and Haptics so I am currently unable to code features for those parts right now. Fortunately LilyGo was really nice and gave me a refund. 
As soon as they have more of the watch back in stock I will buy again and make the firmware capable for those 2 features as well. (highkey the NFC is the feature I was most excited for so this was a blow for me)

I want to be very upfront this whole firmware was a kind of fever dream vibe coded app that I made in about a month and a half because there frankly wasn't any firmware out there i liked. I have done dev work on the Flipper
as well as some other devices and work in Cyber Security so this was a kind of fun little project for me to have a chunky looking watch on my wrist.

Full Disclosure this is a hobby project so maintenance can be spotty at times but if you submit issues I will do my best. Feature requests are more likely to get my attention though cause those are more exciting.

-------------------------------------------------------------------------------------------------

NocSif is a from-scratch firmware for the T-Watch Ultra that turns the watch into a wearable
wireless-and-sensor testbench: WiFi, BLE, LoRa, GNSS, USB-HID, audio and motion tooling, all driven
from a clean on-watch touch UI. It is built for **authorized security testing and research only**.

The design goal is a *platform that runs things*, not a fixed toolkit — a launch-by-id app shell where
every capability is a real, hardware-backed module.


## Install NocSif on your watch

### The desktop app does everything:

1. **Download the app** — **[NocSif Desktop Bridge for Windows](https://github.com/silverwolf2r/Nocsif-Firmware/releases/latest/download/NocSifBridge-windows-x64.exe)** (one file, no installer; Windows SmartScreen may warn once — the build is unsigned). On macOS or Linux, [you gotta build it from source](#desktop-app).
2. **Plug the watch into your computer** over USB-C. The app finds it on its own and tells you what it is — a stock LilyGo watch, a blank board, or a watch already running NocSif.
3. **Click _Flash new watch_.** It writes NocSif, creates the microSD folders NocSif expects, and reboots into the firmware. On a stock or used watch pick _erase first_; the app offers to **back the watch up** beforehand, so you can put it back exactly as it was — LilyGo's own factory firmware included — whenever you like.

The same app also runs a hardware self-check, manages the microSD, mirrors the watch's screen to your computer (mouse acts as touch), backs the watch up and restores it, and keeps NocSif up to date. More in [Desktop app](#desktop-app).

**Already running NocSif?** Update straight from the wrist — **System › Update → Check → Download → Install** — no cable needed.

### Web Flasher
You can also flash Nocsif Firmware to the watch with the click of a button using the Web Flasher [here](https://eigencat.org/nocsif/) 
To bypass the memory door enter in "nightdog" 

#### Rather build it yourself? See [Build from source](#build-from-source).

---

## Current capabilities

Signal Hunt - Hunt Bluetooth signals, Wifi signals, and Lora (902-915) Signals to their source

WiFi Handshake & PMKID Capture — Monitor-mode sniffing that grabs WPA handshakes/PMKID and writes .hc22000 straight to SD, ready for offline cracking.

Evil Portal / Captive Portal — Stand up a rogue AP with a custom landing page (drop your own HTML in nocsif/wifi/portals/), with live hit-logging.

Deauth / Disassoc — Emits management deauth/disassoc frames spoofing a target AP, plus a live deauth-rate analyzer that doubles as an attack detector.

Beacon Flood / Beacon TX — Broadcast a managed list of fake SSIDs.

Wardriving — WiFi + GPS logged to WiGLE-format CSV

BadUSB / DuckyScript — Plug into a computer and inject keystrokes from .txt DuckyScript macros in /sd/ducky/. Works over USB and BLE HID.

BLE Recon — Scan and sniff nearby BLE advertisements to .pcap, connect and explore GATT services, and broadcast your own adverts.

Sub-GHz (915 MHz ISM) — SX1262 band monitor that sweeps 902–928 MHz reading signal energy, plus TX self-test. (Note: this is monitor + test-TX today — not a Flipper-style capture-and-replay of arbitrary remotes. Spectrum monitor, not "clone your garage remote.")

Unlock Shopping carts with audio cart unlock

Trackers - locate trackers around you that may be following you

Customize the UI accent colors, choose from different icons, and choose from different ring menu types for personalized UI feel on both the watchface and the home screen

Bluetooth connection with phone for notifications, media control, and volume control

Bluetooth Phone - See your bluetooth connected phone alerts, pause and play music/change volume

Weather - off GPS location, sunset, and sunrise

Smart watch - Notes, Voice Memos, DND and location based modes

You can control the watch from your phone through a little webUI companion thing that lets you connect to the watches spawned wifi network and control it from there

Performance and Battery Life, NocSif over time associates GPS and Saved Wifi Data along with movement data to selectively turn off and on radios to lengthen battery life and increase performance

For a full list of Current Capabilities there is a current features document. Located Here: [docs/CURRENT-FEATURES.md](docs/CURRENT-FEATURES.md)

---

## Future state (planned)

NFC Read / Clone / Emulate — Read NFC-A tags (UID + type), clone access cards, and emulate saved cards from the wrist. (My Unit is dead and once LilyGo has it in stock I will add this in)

Camera Glasses detection

Flock Hunter

Card-skimmer detection

BLE Spam

Port Scan and other Nmap type items

Wifi sharing and disguising through Cloning Mac + hostname of another device on the network

pihole and wireguard vpn configuration

For a full list of planned Features there is a planned features document. Located Here: [docs/PLANNED-FEATURES.md](docs/PLANNED-FEATURES.md)

---

## Build from source

_Most people don't need this — the [desktop app](#install-nocsif-on-your-watch) flashes prebuilt NocSif in a
click._ To build the firmware yourself, from **PowerShell** (ESP-IDF refuses MSYS), with `-j 2` (default
parallelism OOMs the Windows paging file):

```bash
python -m platformio run -e nocsif-twatch-ultra -j 2
```

Flash with `esptool --no-stub`, or point the desktop app at your own `firmware.bin`. Full pinout, rails, and
build/flash notes are in `docs/`.

### Performance trade-offs (not bugs)
Two deliberate settings trade a little speed to run **BLE and WiFi at the same time**: while Bluetooth
is on, WiFi uses a reduced-speed "lean" buffer profile (turn Bluetooth off to restore full throughput),
and most allocations are routed to slower PSRAM firmware-wide.

## Desktop app

**NocSif Desktop Bridge** (`tools/nocsif_bridge/`) is the computer-side companion — plug the watch in over
USB-C and it connects on its own: flash / update / provision a watch (a blank board included), run the
hardware-defect check, manage the microSD, back the watch up and restore it, and drive the watch from the
computer with a live view of its screen. It works with any T-Watch Ultra — stock LilyGo firmware, a blank
board, or NocSif — and can put LilyGo's factory firmware back.

Windows: download **[NocSifBridge-windows-x64.exe](https://github.com/silverwolf2r/Nocsif-Firmware/releases/latest/download/NocSifBridge-windows-x64.exe)**
from the [releases page](https://github.com/silverwolf2r/Nocsif-Firmware/releases) — a single file, nothing to
install. macOS and Linux: run from source:

```bash
pip install -r tools/nocsif_bridge/requirements.txt
python tools/nocsif_bridge/nocsif_bridge_app.py
```

The app talks to the watch over its USB-Serial/JTAG console (a small JSON protocol, `firmware/src/bridge.h`)
and flashes through `esptool`. Details in `tools/nocsif_bridge/README.md`.
