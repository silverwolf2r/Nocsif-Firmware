# NocSif — Bosch BHY2 Sensor API for the BHI260AP smart sensor, as an ESP-IDF component.
#
# Vendored UNMODIFIED from lewisxhe/SensorLib v0.3.1 (commit ac6d58c, MIT — see
# LICENSE-BHY2.txt) under bhy2_core/: the base BHY2 host-interface C driver (bhy2 + hif +
# parse + defs) plus the BHI260AP RAM firmware image. The optional Bosch feature modules
# (klio / bsec / swim / head-tracker / bhi3) and SensorLib's C++ wrappers are intentionally
# NOT vendored — the base C API is all the M11 IMU bring-up needs; bhi3 (wrist-wear-wakeup /
# multi-tap) can be added later when those virtual sensors are wired. bhi260_fw.{c,h} is the
# NocSif accessor over the image; imu.c (main component) supplies the I2C read/write/delay glue.
#
# Firmware image = the Bosch "bhi260_gpio" RAM variant, which is the one LilyGo's T-Watch Ultra
# build actually compiles (its BoschFirmware.h #elif chain selects GPIO ahead of KLIO when both
# are defined). Boot path: bhy2_upload_firmware_to_ram() + bhy2_boot_from_ram() (not from flash).
#
# Provenance: git clone --depth 1 --branch v0.3.1 https://github.com/lewisxhe/SensorLib.git
#   src/bosch/{bhy2,bhy2_hif,bhy2_parse}.{c,h} + bhy2_defs.h  -> bhy2_core/
#   src/bosch/firmware/bosch_bhi260_gpio.h                    -> bhy2_core/bhi260_fw_image.h

idf_component_register(
    SRCS "bhy2_core/bhy2.c" "bhy2_core/bhy2_hif.c" "bhy2_core/bhy2_parse.c"
         "bhy2_core/bhi260_fw.c"
    INCLUDE_DIRS "bhy2_core"
)

# Vendored third-party sources: silence their warnings so they don't drown NocSif's own.
target_compile_options(${COMPONENT_LIB} PRIVATE -w)
