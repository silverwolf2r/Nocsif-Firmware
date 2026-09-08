# NocSif Desktop Bridge

The computer-side companion for the NocSif watch (PLAN §4.15) — a qFlipper analog. Plug the watch in over
USB-C and the app connects by itself: flash / provision / wipe it, run the hardware-defect check, manage the
microSD, and drive the watch from the computer with a **live view** of its screen. Windows, macOS and Linux.

## Download

Windows: `NocSifBridge-windows-x64.exe` from the
[releases page](https://github.com/silverwolf2r/Nocsif-Firmware/releases) — a single file, nothing to
install (Windows SmartScreen may ask once; the exe is unsigned). The app checks the same page for a newer
build and shows an "update available" link in its footer. macOS / Linux: run from source (below) or build
with `build_exe.sh` on that machine.

A board with no NocSif firmware (blank, or an older build without the bridge) shows up as *attached but not
answering* — use **Flash › Flash new watch**; everything else needs the firmware's bridge to answer.

## Run from source

```bash
pip install -r requirements.txt          # pyserial, esptool, requests (Tk ships with Python)
python nocsif_bridge_app.py              # the app
python bridge_cli.py version             # the same actions from the command line
```

The watch's console is the ESP32-S3 native USB-Serial/JTAG port — `COM7` on Windows, `/dev/ttyACM0` on
Linux (add yourself to the `dialout` group), `/dev/cu.usbmodem…` on macOS. No driver is needed on
Windows 10+ / macOS; the app lists ESP32-S3 ports first.

## What each tab does

- **Overview** — device name, running firmware (version · build · slot · boot reason · last crash),
  battery, the published version from the public mirror, and **Update watch** (esptool, app slot only —
  settings, credentials and bonds are kept).
- **Health** — the hardware check: one pass / fail / not-probed line per subsystem (I²C, PMU, display,
  IMU, RTC, microSD, audio, mic, GNSS, LoRa, NFC, WiFi, BLE, USB, memory, reliability), plus the active
  self-tests (speaker tone, NFC front-end, LoRa RSSI probe, GNSS) whose verdicts print in the Log tab.
- **Flash** — **Flash new watch** (bootloader + partition table + otadata + app from the public mirror,
  then a microSD check and the folder setup; tick *erase the whole flash first* for a used board),
  **Update**, **Flash a local firmware.bin**, **Wipe & reflash (keep settings)** — erases every region
  except `nvs`, then reflashes — and **Full wipe** — `erase-flash`, everything gone, then reflash.
  Destructive actions confirm twice and list what is erased.
- **Files** — the microSD: browse, download, upload, delete, new folder, **Set up folders** (the
  canonical `nocsif/…` layout + README), **Format SD** (fresh FAT, everything erased, folders recreated).
- **Control** — the watch's own menu tree (double-click launches), Home / Back, typing into the focused
  field, brightness / volume, FN / PWR short and long, **Screenshot** (save as PNG), and **Live view**: the
  watch's screen at 2× over USB, updated as it changes (only the changed rectangle travels, run-length
  packed — a ticking clock costs its digits, not a frame), with mouse tap / drag mapped to the touchscreen,
  FN / PWR, and **Cast** (blank the watch panel while the computer shows it). The WiFi live-control web
  page remains a shortcut for phones.
- **Log** — the live serial log and the stored log ring from the watch's flash.

## Command line

```
bridge_cli.py [--port COM7] ping | version | status | health | test tone|nfc|lora|gnss
bridge_cli.py ls [/sd/path] | get <remote> <local> | put <local> <remote> | rm <p> | mkdir <p>
bridge_cli.py sd info | sd provision | sd format --yes
bridge_cli.py ctl launch <id> | back | home | type "<text>" | key enter|backspace | bright <v> | vol <v>
              | button fn|pwr [--long] | touch x y s
bridge_cli.py menu | state | screenshot out.png | log [n] | usb detached|cdc|hid|msc | reboot | tail
bridge_cli.py mirror-bench [seconds]        (live-view poll loop: frames/s and bytes/frame, no window)
```

## Standalone binaries

`build_exe.ps1` (Windows) / `build_exe.sh` (macOS, Linux) wrap PyInstaller:

```bash
pip install pyinstaller
./build_exe.sh            # -> dist/NocSifBridge
```

Only the Windows build is exercised by the maintainer; the sources run unchanged on macOS and Linux.

## How it talks to the watch

One JSON object per line over the console; the watch answers with `NB>`-prefixed JSON lines (see
`firmware/src/bridge.h`). Long answers (files, screenshots, the menu) arrive as base64 fragment lines so
they never collide with the ordinary log stream. Flashing goes through `esptool --no-stub` on the same
port (the watch is reset into the ROM loader; the app reconnects afterwards). Published firmware comes
from `https://github.com/silverwolf2r/Nocsif-Firmware` (`nocsif/firmware/manifest.json` lists every image
with its flash offset).
