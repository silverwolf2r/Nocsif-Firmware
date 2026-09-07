/*
 * NocSif — BHI260AP sensor-hub firmware RAM image accessor.
 *
 * The BHI260AP is a programmable sensor hub: on this board it boots from a host-
 * uploaded RAM image every power-on (LilyGo T-Watch Ultra uses setBootFromFlash(false)),
 * so the ~117 KB Bosch firmware blob is embedded in flash and pushed over I2C at bring-up
 * via bhy2_upload_firmware_to_ram() + bhy2_boot_from_ram(). This wraps the vendored image
 * (Bosch "bhi260_gpio" variant — the one LilyGo's build compiles) behind a clean accessor.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The embedded BHI260AP RAM firmware image and its length in bytes. */
const uint8_t *nocsif_bhi260_fw_image(void);
uint32_t       nocsif_bhi260_fw_size(void);

#ifdef __cplusplus
}
#endif
