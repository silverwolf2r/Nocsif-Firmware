# `rfal` — ST25R3916 / RFAL driver (ESP-IDF C++ component)

The 13.56 MHz HF NFC front-end (ST ST25R3916) driver for NocSif (M6). It is a **port of
ST's RF Abstraction Layer (RFAL)** to ESP-IDF, chosen over a hand-rolled native driver
because RFAL is the board-proven ST25R3916 path and the full-featured M6 scope
(read/write, card emulation, ISO-DEP, NfcV, NDEF) needs its complete stack.

## Provenance

Vendored base (gitignored under `vendor/ST25R3916-elechouse/`), re-clone with:

```
git clone --depth 1 https://github.com/wilson-elechouse/ST25R3916.git vendor/ST25R3916-elechouse
```

Copied **unmodified** into this tracked component:

| Upstream | Here | License |
|---|---|---|
| `NFC-RFAL/src/*.{cpp,h}` (48 files: RFAL core, NDEF, ISO-DEP) | `rfal_core/` | BSD-3 (`LICENSE-RFAL.txt`) |
| `ST25R3916_ELECHOUSE/src/*.{cpp,h}` (20 files: chip driver + SPI/timer/interrupt) | `st25r3916/` | ST MyLiberty (`LICENSE-ST25R3916.txt`) |

`examples/`, `.ino`, `library.properties`, `keywords.txt`, and the upstream `CMakeLists`
are **not** copied. The library sources are unchanged so upstream can be re-diffed.

## The port surface — `arduino_compat/`

The fork was written against the Arduino core. Instead of editing the vendored files,
NocSif supplies the small slice of the Arduino API they actually use, implemented over
ESP-IDF. **This shim is the entire platform port** (the fork folded ST's classic
`platform.h` — `platformSpiTxRx`, `platformProtectST25RComm`, `platformDelay`, … — into
these Arduino primitives, so there is nothing else to implement):

- **`SPI.h` / `arduino_compat.cpp`** — `SPIClass` over an esp-idf `spi_device`. The
  ST25R3916 is a **2nd device on SPI3** (shared with the microSD): manual CS on **GPIO4**
  (device added with `spics_io_num = -1`), and `beginTransaction`/`endTransaction` map to
  `spi_device_acquire_bus`/`release_bus` so the whole CS-asserted window holds the bus and
  an SD access can't interleave mid-register-op. `nfc.cpp` creates the device and binds it
  via `setHandle()`.
- **`Arduino.h`** — `pinMode`/`digitalWrite`/`digitalRead` (GPIO), `millis`/`micros`/
  `delay`/`delayMicroseconds`/`yield` (esp_timer + FreeRTOS). `attachInterrupt` is a
  **no-op**: this fork's IRQ handling is polled (`digitalRead(int_pin)` from every bus op),
  so a real ISR is a latency optimisation, not correctness — a P2 add.
- **`Wire.h`** — inert `TwoWire` stub (NocSif uses the SPI path; the I2C code just needs
  to compile).

`ST25R3916_FORCE_SOFT_SPI=0` is forced in `CMakeLists.txt` — the fork's ESP32-S3 default
is a bit-banged soft-SPI on fixed pins that would bypass esp-idf and the shared bus.

## Hardware (T-Watch Ultra)

ST25R3916 · CS = GPIO4 · IRQ = GPIO5 (active-high) · SPI3 (MOSI34 / MISO33 / SCK35,
shared with microSD + LoRa) · rail = AXP2101 **DLDO1 @ 3.3 V**. Consumed by
`firmware/src/nfc.cpp`.
