# Hardware — LilyGo T-Watch Ultra (authoritative)

Source: LilyGoLib `docs/hardware/lilygo-t-watch-ultra.md` + on-device verification (2026-08).
Arduino pin variant: `arduino-esp32/variants/lilygo_twatch_ultra/pins_arduino.h`.

## Core
- **SoC:** ESP32-S3 (native USB-Serial/JTAG). This unit: rev v0.2, MAC `10:51:db:40:53:8c`,
  enumerates on **COM7** (VID 303A / PID 1001).
- **Flash:** 16 MB **QSPI** (Winbond, id 0xEF4018).
- **PSRAM:** **8 MB QSPI (QUAD, not octal)** — external QSPI PSRAM solution. `sdkconfig` MUST use
  `CONFIG_SPIRAM_MODE_QUAD=y`; octal mode leaves PSRAM unexposed. (Tell: GPIO33–37 are free
  peripheral pins, which octal flash/PSRAM would consume — so it must be quad.)
- **Two RF variants exist:** **SX1262** (our unit, 915 MHz sub-GHz LoRa) and SX1280 (2.4 GHz).

## Buses
- **I²C:** SDA = GPIO3, SCL = GPIO2. Shared by touch, PMU, expander, sensor, RTC, haptic.
- **SPI (shared):** MOSI = GPIO34, MISO = GPIO33, SCK = GPIO35. Shared by SD, NFC, LoRa.
- **Display QSPI (separate):** CS=41, SCK=40, D0=38, D1=39, D2=42, D3=45, TE=6, RESET=37.

### I²C device addresses (7-bit)
| Device | Addr |
|---|---|
| Touch **CST9217** | 0x1A |
| GPIO expander **XL9555** | 0x20 |
| Sensor **BHI260AP** | 0x28 |
| PMU **AXP2101** | 0x34 |
| RTC **PCF85063A** | 0x51 |
| Haptic **DRV2605** | 0x5A |

### XL9555 expander pin mapping (gates power/reset — sequence FIRST)
| XL9555 GPIO | Function |
|---|---|
| GPIO6 | Haptic driver enable |
| GPIO7 | **Display power supply enable** |
| GPIO10 | **Touch panel reset** |
| GPIO12 | SD insert detect |

### AXP2101 power rails
DC1 = ESP32-S3 · ALDO1 = **SD card** · ALDO2 = **Display** · ALDO3 = **LoRa** · ALDO4 = Sensor ·
BLDO1 = GNSS · BLDO2 = Speaker · DLDO1 = **NFC** · LDO1(VRTC) = GPS backup (can't off).

## Peripherals & control pins
- **Display:** CO5300 QSPI AMOLED, 2.06", **410×502**, 600 nit. Power via ALDO2 **and** XL9555
  GPIO7. Proven C driver: `kodediy/esp_lcd_co5300`. *#1 bring-up gate.*
  - **Physically rounded rectangle** — the glass clips the corners. **Measured 2026-08-26 (interactive
    on-device calibrator):** visible area = **inset ~7 px, corner radius ~115 px** (technically-perfect
    inset 5 / radius 125). Straight edges usable from ~5–7 px; a **sharp**-cornered element needs **~41 px**
    inset to clear the arc; centered content is always fine. The software `ui_background.c CORNER_R` mask is a
    smaller look-alike, not the true radius. Full detail + the content-inset helper → `docs/LESSONS.md`.
- **Touch:** CST9217 @0x1A on I²C; reset via XL9555 expander; **INT = ESP32-S3 native GPIO12**
  (active-low, needs INPUT_PULLUP; *this is the SoC's GPIO12, NOT the XL9555 IO12 SD-detect below*).
  **M2 verified (2026-08-06):** CST92xx-family protocol (16-bit BE regs, poll 0x0000→0xD000 + 0xAB
  ACK write-back), coords map **native**: X 0→410 L-to-R, Y 0→502 top-to-bottom, **no swap/rotation
  vs the display**. INT currently unused — NocSif polls at ~33 Hz (see `firmware/src/touch.{c,h}`).
  ⚠ **Reset-pin discrepancy (unresolved, non-blocking):** this doc + `xl9555.h` map XL9555 **IO10 =
  touch reset / IO12 = SD-detect**, but LilyGo's Espressif board variant (`pins_arduino.h`) has
  **EXPANDS_TOUCH_RST = IO8 / SD-detect = IO10**. Touch works regardless because the controller is
  released and ACKs at cold boot; confirm against the schematic before relying on the reset line.
- **SD card:** microSD socket, **≤32 GB, FAT only**. CS = GPIO21, shares SPI bus; power via
  ALDO1; insert-detect via XL9555 GPIO12. **Card present in this unit.** → use **SD-over-USB-MSC**
  for the M2 file-drop (already FAT, no LittleFS-translation corruption risk).
- **NFC:** ST25R3916 (13.56 MHz HF only, no 125 kHz LF). CS = GPIO4, IRQ = GPIO5, on SPI bus;
  power via DLDO1. Note: no capacitive card-presence sense — reader must be on to detect a card.
- **LoRa (sub-GHz):** SX1262. CS=36, IRQ=14, RESET=47, BUSY=48, on SPI bus; power via ALDO3.
  915 MHz matched, LoRa/(G)FSK only — **no OOK, no raw capture** (see ARCHITECTURE.md).
- **Also on board (future):** GNSS UBlox MIA-M10Q (TX43/RX44/PPS13), sensor BHI260AP (IRQ8),
  RTC PCF85063A (IRQ1), PDM mic T3902, MAX98357A speaker amp (BCLK9/WCLK10/DOUT11),
  DRV2605 haptic, AXP2101 IRQ = GPIO7... (AXP IRQ pin listed as 7 in doc; verify vs XL9555 use).
- Prior scope: **no nRF24**; mesh optional.

## Power / charging
USB-C 3.9–6 V. Charge current programmable 0–1024 mA — **cap ≤ 500 mA** (PMU thermal). Battery
3.7 V, 1100 mAh. Buttons: RST (hardware reset, not SW-controllable), BOOT/GPIO0 (custom + download
mode), PWR (1 s on / 6 s off, programmable).

## USB flashing — the hard-won gotcha
ESP32-S3 USB-Serial/JTAG **stub flasher is unstable** on this unit (long reads/writes die with
"No more data to read"). Rules:
- **Always `--no-stub`. Never force `--baud`.** (~166 kbit/s; full 16 MB read ≈ 13 min.)
- Stock app holds USB-CDC → "port busy / Access is denied". Enter **download mode first: hold
  BOOT → tap RST → release BOOT** (blank screen), then esptool's default reset connects.

### Full flash backup (before flashing anything)
```powershell
python -m esptool --chip esp32s3 --port COM7 --no-stub --after no-reset `
  read-flash 0x0 0x1000000 backups\stock_full_16MB.bin   # must be exactly 16777216 bytes
```
### Restore to stock
```powershell
# Your dump:
python -m esptool --chip esp32s3 --port COM7 --no-stub write-flash 0x0 backups\stock_full_16MB.bin
# LilyGo factory image (our 915 MHz SX1262 variant), present in the LilyGoLib clone:
python -m esptool --chip esp32s3 --port COM7 --no-stub write-flash 0x0 `
  vendor\LilyGoLib\firmware\factory.watch.ultra.sx1262.20260424.bin
```

## Bring-up order (each verified on-device before moving on)
serial boot → PMU (AXP2101) rails → XL9555 expander (display power GPIO7, touch reset GPIO10) →
**display (CO5300)** → touch (CST9217) → SD → WiFi → BLE (incl. USB/BLE HID) →
NFC (ST25R3916) → LoRa SX1262 (antenna attached!).
