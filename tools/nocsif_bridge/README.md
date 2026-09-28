# NocSif Desktop Bridge

The computer-side companion for the NocSif watch (PLAN §4.15) — a qFlipper analog. Plug the watch in over
USB-C and the app connects by itself: flash / provision / wipe it, run the hardware-defect check, manage the
microSD, and drive the watch from the computer with a **live view** of its screen. Windows, macOS and Linux.

## Download

Windows: `NocSifBridge-windows-x64.exe` from the
[releases page](https://github.com/silverwolf2r/Nocsif-Firmware/releases) — a single file, nothing to
install (Windows SmartScreen may ask once; the exe is unsigned). The app checks the same page for a newer
build; when there is one, an **update to vX.Y.Z ↓** link appears in its footer — click it and the app
downloads the new build, closes, swaps itself in, and reopens (no manual re-download). macOS / Linux: run
from source (below, and update with `git pull`) or build with `build_exe.sh` on that machine.

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
pip install -r requirements.txt          # pyserial, esptool 5, requests, Pillow (Tk ships with Python)
python nocsif_bridge_app.py              # the app  (--live opens straight onto Control once a watch connects)
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
- **Flash** — four groups. **Backup**: Flash + SD + NVS (everything: every file on the card into
  `~/.nocsif_bridge/backups/<watch>_<date>/sd/`, the whole flash as `flash.bin`, the settings carved out as
  `nvs.bin`, plus a `backup.json` manifest), Flash only, SD only, NVS only. **Restore**: the same four
  from a backup folder. **Erase**: Flash + SD + NVS, Flash only, SD only (a format), NVS settings only.
  **Flash**: **Flash NocSif to Watch** (updates the app slot on a NocSif watch, settings kept; provisions
  bootloader + partition table + otadata + app on a stock or blank board — nothing is erased) and
  **Flash LilyGo Firmware** (their factory image, radio variant picker). Destructive actions confirm twice
  and list what is erased. 16 MB images are WRITTEN through the ROM loader (~4–5 min); a full-flash
  BACKUP is READ in one continuous ROM pass (~30–35 min) — the stub loader's streaming read stalls after
  a few MB on the native USB port and the reset needed to retry it won't re-enter download mode, so a
  single uninterrupted read is the reliable way to get all 16 MB.
- **Files** — the microSD: browse, download, upload, delete, new folder, **Format SD** (fresh FAT,
  everything erased). The watch creates each `nocsif/…` folder on demand the first time a feature writes
  to it, so there is no "set up folders" step.
- **Control** — the **live mirror**, the same surface the phone Companion shows, and nothing else: the
  watch's screen over USB updated as it changes (only the changed rectangle travels, run-length packed,
  and as an XOR delta against what the app already shows on a firmware that supports it — a ticking
  clock costs its digits, an animated backdrop a few KB, never a softer frame), mouse tap / drag / swipe
  mapped to the touchscreen, the **scroll wheel** scrolls (a short drag under the pointer), the
  computer's **keyboard types into a focused watch field** (Enter / Backspace edit it), **☰** for
  **Blank watch screen** (the panel goes dark while the computer shows it), **Wake watch**, **Reset
  watch** and **Save screenshot…** (one pixel-exact PNG), and **FN / PWR** under the screen (hold for a
  long press). It scales to the window; maximise for the panel's own 410×502 pixels. The WiFi
  Companion page remains the way in from a phone.
- **Log** — the live serial log and the stored log ring from the watch's flash.

## Command line

```
bridge_cli.py [--port COM7] ping | version | status | health | test tone|nfc|lora|gnss
bridge_cli.py ls [/sd/path] | get <remote> <local> | put <local> <remote> | rm <p> | mkdir <p>
bridge_cli.py sd info | sd format --yes
bridge_cli.py ctl launch <id> | back | home | type "<text>" | key enter|backspace | bright <v> | vol <v>
              | button fn|pwr [--long] | touch x y s | cast 0|1 | wake | reset
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
they never collide with the ordinary log stream — or, since protocol 2, RAW: the app asks for binary
blobs, the watch announces the byte count on a normal line and streams it in 1 KB chunks each stamped
with a magic header (log text can only land between chunks and is picked out), a third less on the
wire than base64. One reader thread owns the port: replies go to the command waiting on their id,
everything else is the log tail, so the log can never hold up the live mirror. The mirror polls `mirror` as fast as the round trip allows; each poll carries every queued
touch point (a whole swipe), and each reply carries the changed rectangle (or `none` / `busy`) plus the
flags asleep / blank / focused. Flashing goes through `esptool --no-stub` on the same
port (the watch is reset into the ROM loader; the app reconnects afterwards). Published firmware comes
from `https://github.com/silverwolf2r/Nocsif-Firmware` (`nocsif/firmware/manifest.json` lists every image
with its flash offset).
