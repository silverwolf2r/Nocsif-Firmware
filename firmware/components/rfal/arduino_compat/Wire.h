/*
 * NocSif — Arduino Wire (I2C) shim: present only so the RFAL driver's I2C code path
 * compiles.  NocSif drives the ST25R3916 over SPI (the RfalRfST25R3916Class SPI
 * constructor, i2c_enabled = false), so none of these methods is ever called at
 * runtime; they exist purely to satisfy the vendored st25r3916_com.cpp references.
 */
#ifndef NOCSIF_ARDUINO_COMPAT_WIRE_H
#define NOCSIF_ARDUINO_COMPAT_WIRE_H

#include <stdint.h>
#include <stddef.h>

class TwoWire {
  public:
    explicit TwoWire(int bus = 0) { (void)bus; }
    void   begin(int sda = -1, int scl = -1, uint32_t freq = 0) { (void)sda; (void)scl; (void)freq; }
    void   setClock(uint32_t freq) { (void)freq; }
    void   beginTransmission(uint8_t addr) { (void)addr; }
    size_t write(uint8_t value) { (void)value; return 0; }
    /* Arduino returns 0 on success; return an error so any accidental use is visible. */
    uint8_t endTransmission(bool stop = true) { (void)stop; return 4; }
    size_t requestFrom(uint8_t addr, uint8_t len) { (void)addr; (void)len; return 0; }
    int    available() { return 0; }
    int    read() { return -1; }
};

#endif /* NOCSIF_ARDUINO_COMPAT_WIRE_H */
