/*
 * jpeg_mem.c — keep the companion mirror's JPEG encoder OUT of internal RAM (NocSif).
 *
 * THE STARVATION (COM7, 2026-09-27): with a phone on the live mirror the heartbeat's internal-DMA pool
 * fell from ~51 KB free / 30 KB largest to ~9 KB free / 3,456 B largest, and stayed there. Below that the
 * WiFi driver cannot get TX/RX buffers, so every WebSocket frame send timed out (`httpd_sock_err: error in
 * send : 11` → `Failed to send WS header`, once per 5 s send-wait), the httpd task sat in those blocked
 * sends instead of reading the touch uplink, and the phone froze until a page refresh — the same 3.4 KB
 * also sits under the 4,096 B floor ui.c's LoRa Signal-Alerts watch needs to arm.
 *
 * WHERE IT WENT — read from the prebuilt library (esp32s3/libesp_new_jpeg.a, esp_jpeg_memory.c.obj):
 *   jpeg_calloc(n, size)               = heap_caps_calloc_prefer(n, size, 2, INTERNAL|8BIT, SPIRAM|8BIT)
 *   jpeg_calloc_inner(size)            = heap_caps_calloc_prefer(1, size, 2, INTERNAL|8BIT, SPIRAM|8BIT)
 *   jpeg_calloc_align_inner(size, al)  = heap_caps_aligned_calloc(al, 1, size, INTERNAL|8BIT), else SPIRAM
 *   jpeg_calloc_align(size, al)        = heap_caps_aligned_calloc(al, 1, size, SPIRAM|8BIT),   else INTERNAL
 * i.e. three of the four helpers take internal DRAM FIRST and only spill to PSRAM when it is gone — fine
 * on a devkit, fatal on a watch whose internal pool is tuned to the byte for BLE + WiFi coexistence
 * (docs/RAM-BUDGET.md). The encoder (jpeg_enc_process / jpeg_enc_huff / esp_jpeg_enc) uses all three
 * for its block, line and Huffman work buffers, every one of which is plain CPU-accessed memory — the
 * S3 has no JPEG hardware, so nothing here needs DMA-capable or internal placement.
 *
 * THE FIX: GNU ld `--wrap` (platformio.ini) routes the three internal-first helpers here and they
 * allocate PSRAM first, internal only as a last resort. jpeg_calloc_align is already PSRAM-first and is
 * left alone; jpeg_free / jpeg_free_align are heap_caps_free, which takes either heap. Cost: the encode
 * reads/writes its scratch through the PSRAM cache instead of DRAM (a somewhat slower encode on the
 * core-0 httpd task — the frame rate self-throttles on s_thumb_busy, the watch's own render on core 1
 * is untouched). The encoder's open/encode cost is logged by wifi.c comp_encode_jpeg so the trade stays
 * visible on the console.
 */
#include <stddef.h>
#include "esp_heap_caps.h"
#include "esp_log.h"

#define JPEG_MEM_PSRAM    (MALLOC_CAP_SPIRAM   | MALLOC_CAP_8BIT)
#define JPEG_MEM_INTERNAL (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

/* One-time map of what the encoder asks for (the first calls of a session): which buffers are the big,
 * hot ones decides how much of the encode can go back to internal RAM for speed. */
static void jpeg_mem_trace(const char *who, size_t bytes, int aligned)
{
    static int n;
    if (n < 24) { n++; ESP_LOGI("jpeg_mem", "%s %u B align %d", who, (unsigned)bytes, aligned); }
}

/* The library's own definitions survive as __real_*; unused, declared so the link stays honest. */
void *__real_jpeg_calloc(size_t n, size_t size);
void *__real_jpeg_calloc_inner(size_t size);
void *__real_jpeg_calloc_align_inner(size_t size, int aligned);

void *__wrap_jpeg_calloc(size_t n, size_t size)
{
    jpeg_mem_trace("calloc", n * size, 0);
    return heap_caps_calloc_prefer(n, size, 2, JPEG_MEM_PSRAM, JPEG_MEM_INTERNAL);
}

void *__wrap_jpeg_calloc_inner(size_t size)
{
    jpeg_mem_trace("calloc_inner", size, 0);
    return heap_caps_calloc_prefer(1, size, 2, JPEG_MEM_PSRAM, JPEG_MEM_INTERNAL);
}

void *__wrap_jpeg_calloc_align_inner(size_t size, int aligned)
{
    jpeg_mem_trace("calloc_align_inner", size, aligned);
    void *p = heap_caps_aligned_calloc((size_t)aligned, 1, size, JPEG_MEM_PSRAM);
    if (p == NULL) {
        p = heap_caps_aligned_calloc((size_t)aligned, 1, size, JPEG_MEM_INTERNAL);
    }
    return p;
}
