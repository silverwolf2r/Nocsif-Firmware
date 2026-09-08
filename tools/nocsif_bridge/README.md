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

## Any T-Watch Ultra, stock or NocSif

The app works with whatever is on the watch:

- **NocSif** — the bridge answers: everything below.
- **Stock (LilyGo firmware)** or a **blank board** — nothing answers on the console, so the app asks the
  chip's ROM loader instead (esptool): chip, flash size, MAC, and the app descriptor at LilyGo's Arduino
  offset, so it shows "*factory firmware · version · built …*". From there: **Back up this watch** (the whole
  16 MB, verified, into `~/.nocsif_bridge/backups/`), **Flash NocSif** (erase-first, backup offered first),
  **LilyGo factory firmware** (LilyGo's own merged image from their LilyGoLib repo, SX1262 or SX1280 radio
  variant — the way the watch ships), **Restore a backup** (the exact original comes back), and the
  ROM-level rows of Health. Files, Control, the live view and the full hardware board need NocSif (the
  hardware tests for a stock watch — a diagnostic that runs from RAM without touching the flash — are next).
- **NocSif not answering** (an older build without the bridge, or the watch is in a USB mode) — the app
  says so and offers Update.

## The look

The app wears the firmware's own clothes: the near-black ground, Fraunces serif over JetBrains Mono (the
watch's fonts, bundled), the muted greys, the engraved star / orrery motif, a left-hand menu — and the
**accent colour the owner picked on the watch** (System › Theme): the app takes it from the watch when it
connects; with no watch it wears the NocSif purple. The window has rounded corners and an accent-coloured
edge that follows the same theme (Windows 11 through DWM, Windows 10 through the app's own frameless chrome
— drag the title strip, double-click it to maximise, the corner grip resizes).

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
  battery, the published version from GitHub, and **Update watch** (esptool, app slot only —
  settings, credentials and bonds are kept).
- **Health** — the hardware check: one pass / fail / not-probed line per subsystem (I²C, PMU, display,
  IMU, RTC, microSD, audio, mic, GNSS, LoRa, NFC, WiFi, BLE, USB, memory, reliability), plus the active
  self-tests (speaker tone, NFC front-end, LoRa RSSI probe, GNSS) whose verdicts print in the Log tab.
- **Flash** — **Flash new watch** (bootloader + partition table + otadata + app from GitHub,
  then a microSD check and the folder setup; *erase the whole flash first* for a used or stock board),
  **Update**, **Flash a local firmware.bin**, **LilyGo factory firmware** (radio variant picker), **Flash a
  local 16 MB image**, **Back up watch (flash + microSD)** — every file on the card into
  `~/.nocsif_bridge/backups/<watch>_<date>/sd/` plus the whole flash as `flash.bin` and a `backup.json`
  manifest — or **Back up flash only**, **Restore a backup** (pick a backup's `backup.json` to restore the
  flash, the card files, or both; or a flash-only `.bin`), **Wipe & reflash (keep settings)**
  — erases every region except `nvs`, then reflashes — and **Full wipe** — `erase-flash`, everything gone,
  then reflash. Destructive actions confirm twice and list what is erased. 16 MB images go through the
  ROM loader (~4–5 min); backups read in 256 KB chunks because the stub loader's stream is flaky over the
  native USB port.
- **Files** — the microSD: browse, download, upload, delete, new folder, **Set up folders** (the
  canonical `nocsif/…` layout + README), **Format SD** (fresh FAT, everything erased, folders recreated).
- **Control** — the watch's own menu tree (double-click launches), Home / Back, typing into the focused
  field, brightness / volume, FN / PWR short and long, **Screenshot** (the panel's own 410×502, save as
  PNG), and **Live view**: the watch's screen pixel-for-pixel over USB, updated as it changes (only the
  changed rectangle travels, run-length packed — a ticking clock costs its digits, not a frame; tick
  *half res* on a slow link), with mouse tap / drag mapped to the touchscreen 1:1, FN / PWR, and **Cast**
  (blank the watch panel while the computer shows it). The WiFi live-control web page remains a shortcut
  for phones.
- **Log** — the live serial log and the stored log ring from the watch's flash.

## Command line

```
bridge_cli.py [--port COM7] ping | version | status | health | test tone|nfc|lora|gnss
bridge_cli.py ls [/sd/path] | get <remote> <local> | put <local> <remote> | rm <p> | mkdir <p>
bridge_cli.py sd info | sd provision | sd format --yes
bridge_cli.py ctl launch <id> | back | home | type "<text>" | key enter|backspace | bright <v> | vol <v>
              | button fn|pwr [--long] | touch x y s
bridge_cli.py menu | state | screenshot out.png | log [n] | usb detached|cdc|hid|msc | reboot | tail
bridge_cli.py mirror-bench [seconds] [half] (live-view poll loop: frames/s and bytes/frame, no window)
bridge_cli.py sd-backup [dir] | sd-restore <dir>   (every file on the card ↔ a folder, over the bridge)
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
