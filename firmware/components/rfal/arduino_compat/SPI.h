/*
 * NocSif — Arduino SPI shim backed by an ESP-IDF spi_device (see arduino_compat.cpp).
 *
 * This IS the "port st25r3916_com's SPI transceive -> esp-idf spi_device" work: the
 * RFAL driver's register/FIFO transport calls SPIClass::beginTransaction / transfer /
 * endTransaction, and each maps onto the esp-idf SPI master:
 *   beginTransaction -> spi_device_acquire_bus   (hold the shared SPI3 bus for the
 *                                                  whole CS-low..CS-high window, so an
 *                                                  SD access on the same bus can't
 *                                                  interleave mid-register-op)
 *   transfer(byte)   -> one full-duplex polling transaction (4-byte in-place path)
 *   transfer(buf,len)-> one full-duplex in-place polling transaction
 *   endTransaction   -> spi_device_release_bus
 * CS is driven MANUALLY by the RFAL driver via digitalWrite(cs_pin,...) (the device is
 * added with spics_io_num = -1), which is why the driver can hold CS asserted across a
 * multi-transfer sequence.  nfc.cpp creates the spi_device and binds it via setHandle().
 */
#ifndef NOCSIF_ARDUINO_COMPAT_SPI_H
#define NOCSIF_ARDUINO_COMPAT_SPI_H

#include <stdint.h>
#include <stddef.h>
#include "driver/spi_master.h"

/* Bit order + SPI modes (the ST25R3916 uses MSBFIRST + SPI_MODE1). */
#define LSBFIRST 0
#define MSBFIRST 1

#define SPI_MODE0 0x00
#define SPI_MODE1 0x01
#define SPI_MODE2 0x02
#define SPI_MODE3 0x03

/* Arduino SPISettings — only the fields the RFAL driver reads. The clock/mode are set
 * once on the esp-idf device (nfc.cpp), so these are informational here. */
class SPISettings {
  public:
    SPISettings(uint32_t clock = 1000000, uint8_t bitOrder = MSBFIRST, uint8_t dataMode = SPI_MODE1)
      : _clock(clock), _bitOrder(bitOrder), _dataMode(dataMode) {}
    uint32_t _clock;
    uint8_t  _bitOrder;
    uint8_t  _dataMode;
};

class SPIClass {
  public:
    /* The RFAL fork constructs SPIClass(busId); NocSif ignores the bus id and binds a
     * pre-configured esp-idf device via setHandle() instead. */
    explicit SPIClass(int bus = 0) : _dev(nullptr) { (void)bus; }

    /* Arduino lifecycle calls the RFAL fork may make — no-ops: nfc.cpp owns the bus. */
    void begin(int sck = -1, int miso = -1, int mosi = -1, int ss = -1)
    { (void)sck; (void)miso; (void)mosi; (void)ss; }
    void end() {}

    /* NocSif extension: bind the esp-idf device this shim transacts over. */
    void setHandle(spi_device_handle_t dev) { _dev = dev; }
    spi_device_handle_t handle() const { return _dev; }

    /* Hold / release the shared bus around a CS-asserted sequence. */
    void beginTransaction(SPISettings settings);
    void endTransaction();

    /* Full-duplex byte transfer (returns MISO byte). */
    uint8_t transfer(uint8_t data);
    /* Full-duplex in-place block transfer (tx == rx == buf), Arduino semantics. */
    void transfer(void *buf, size_t count);

  private:
    spi_device_handle_t _dev;
};

#endif /* NOCSIF_ARDUINO_COMPAT_SPI_H */
