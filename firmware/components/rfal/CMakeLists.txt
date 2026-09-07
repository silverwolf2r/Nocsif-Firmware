# NocSif — RFAL / ST25R3916 driver as an ESP-IDF (C++) component.
#
# Sources are the vendored ST "RF Abstraction Layer" + ST25R3916 driver (elechouse
# fork of wilson-elechouse/ST25R3916), copied UNMODIFIED into rfal_core/ (ST RFAL core,
# BSD-3 — see LICENSE-RFAL.txt) and st25r3916/ (chip driver + SPI/timer/interrupt glue,
# ST MyLiberty — see LICENSE-ST25R3916.txt).  The Arduino API the fork was written
# against is supplied by arduino_compat/ (NocSif, over ESP-IDF) — that shim is the whole
# platform port, so the library files themselves need no edits.
#
# Provenance: git clone --depth 1 https://github.com/wilson-elechouse/ST25R3916.git
#   NFC-RFAL/src/*            -> rfal_core/
#   ST25R3916_ELECHOUSE/src/* -> st25r3916/

file(GLOB RFAL_CORE_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/rfal_core/*.cpp)
file(GLOB RFAL_CHIP_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/st25r3916/*.cpp)

idf_component_register(
    SRCS ${RFAL_CORE_SRCS} ${RFAL_CHIP_SRCS}
         "arduino_compat/arduino_compat.cpp"
    INCLUDE_DIRS "rfal_core" "st25r3916" "arduino_compat"
    REQUIRES      esp_driver_spi          # SPI.h exposes spi_device_handle_t to nfc.cpp
    PRIV_REQUIRES esp_driver_gpio esp_timer
)

# Force the fork's HARDWARE-SPI path (its ESP32-S3 default is a bit-banged soft-SPI on
# fixed pins 11/12/13 — that would bypass esp-idf and the shared SPI3 bus entirely).
# NocSif routes every transfer through the SPIClass shim -> esp-idf spi_device instead.
target_compile_definitions(${COMPONENT_LIB} PUBLIC ST25R3916_FORCE_SOFT_SPI=0)

# Vendored third-party sources: silence their warnings so they don't drown NocSif's.
target_compile_options(${COMPONENT_LIB} PRIVATE -w)
