/*
 * NocSif — minimal Arduino-compatibility shim for the vendored ST25R3916 / RFAL
 * library (elechouse fork).  See arduino_compat.cpp + README.md.
 *
 * The RFAL sources were written against the Arduino core.  Rather than edit ~68
 * vendored files (and fork them away from upstream), NocSif provides just the tiny
 * slice of the Arduino API those sources actually call, implemented over ESP-IDF:
 *   - GPIO      (pinMode / digitalWrite / digitalRead)      -> driver/gpio
 *   - timing    (millis / micros / delay / delayMicroseconds / yield)
 *   - interrupt (attachInterrupt / detachInterrupt)         -> no-op (see below)
 * SPI + I2C live in SPI.h / Wire.h.  This is the entire "platform port" — the fork
 * folded ST's classic platform.h layer into these Arduino primitives, so there is no
 * platformSpiTxRx / platformProtectST25RComm / platformDelay to implement.
 *
 * attachInterrupt is intentionally a no-op: this fork's IRQ handling is *polled*
 * (st25r3916ProcessInterrupts reads digitalRead(int_pin) from every bus op and from
 * st25r3916WaitForInterruptsTimed), so a real ISR is only a latency optimisation, not
 * correctness.  NocSif's nfc worker pumps rfalNfcWorker() in a loop, which is exactly
 * how the fork's own examples drive it.  (A GPIO5 ISR -> worker notify is a P2 add.)
 */
#ifndef NOCSIF_ARDUINO_COMPAT_H
#define NOCSIF_ARDUINO_COMPAT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>   /* abs() — used by st25r3916_aat.cpp */
#include <string.h>   /* mem* — used throughout the RFAL core */
#include <math.h>

/* ---- pin / logic levels + modes ------------------------------------------------ */
#ifndef HIGH
#define HIGH 0x1
#endif
#ifndef LOW
#define LOW 0x0
#endif

#define INPUT           0x01
#define OUTPUT          0x03
#define INPUT_PULLUP    0x05
#define INPUT_PULLDOWN  0x09

/* attachInterrupt trigger modes (values are cosmetic — attach is a no-op here). */
#define DISABLED  0x00
#define RISING    0x01
#define FALLING   0x02
#define CHANGE    0x03
#define ONLOW     0x04
#define ONHIGH    0x05

typedef uint8_t  byte;
typedef bool     boolean;

#ifdef __cplusplus
extern "C" {
#endif

/* GPIO — pin numbers are SoC GPIO numbers (the RFAL driver is given CS=4, IRQ=5). */
void     pinMode(int pin, int mode);
void     digitalWrite(int pin, int level);
int      digitalRead(int pin);

/* Timing — millis/micros are monotonic since boot (esp_timer). */
uint32_t millis(void);
uint32_t micros(void);
void     delay(uint32_t ms);
void     delayMicroseconds(uint32_t us);
void     yield(void);

/* Interrupts — no-op (this fork polls the IRQ line; see file header). */
void     attachInterrupt(int pin, void (*isr)(void), int mode);
void     detachInterrupt(int pin);

#ifdef __cplusplus
}
#endif

#endif /* NOCSIF_ARDUINO_COMPAT_H */
