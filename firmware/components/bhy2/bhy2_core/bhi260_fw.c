/*
 * NocSif — BHI260AP RAM firmware image accessor. See bhi260_fw.h.
 *
 * This is the ONLY translation unit that includes the big image header, so the
 * vendored blob (and its bosch_firmware_* aliases) is defined exactly once.
 */
#include "bhi260_fw.h"

/* Defines: const unsigned char bosch_bhi260_gpio_firmware_image[] = { ... };
 * plus the bosch_firmware_image/_size/_type aliases (unused here — we expose the
 * array directly). Vendored UNMODIFIED from SensorLib v0.3.1. */
#include "bhi260_fw_image.h"

const uint8_t *nocsif_bhi260_fw_image(void)
{
    return (const uint8_t *)bosch_bhi260_gpio_firmware_image;
}

uint32_t nocsif_bhi260_fw_size(void)
{
    return (uint32_t)sizeof(bosch_bhi260_gpio_firmware_image);
}
