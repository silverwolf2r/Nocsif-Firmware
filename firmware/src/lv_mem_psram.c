/*
 * NocSif — route LVGL's heap to external PSRAM (M11 internal-RAM relief).
 *
 * LVGL's built-in allocator keeps a fixed ~96 KB object pool in INTERNAL RAM (.bss) — on the
 * T-Watch Ultra that was the single biggest consumer of the scarce internal, DMA-capable RAM
 * that the BLE controller and WiFi driver compete for (they are already mutually exclusive
 * because of it). Adding the IMU pushed even the single-radio case over the edge
 * (esp_bt_controller_init -> "Malloc failed", ESP_ERR_NO_MEM).
 *
 * Switching LVGL to a CUSTOM allocator (CONFIG_LV_USE_CUSTOM_MALLOC) that draws from the 8 MB
 * external PSRAM frees ~96 KB of internal RAM — an order of magnitude more than the IMU costs —
 * so the IMU + a radio can coexist. This is capability-safe: LVGL widget/style/anim structs are
 * NOT DMA targets (the RGB888 draw buffers are allocated separately from PSRAM in display.c), and
 * the reserved internal DMA pool is untouched. The trade-off is slower random access to the object
 * tree in PSRAM, which is validated on-device (screen-build times / scroll smoothness).
 *
 * These *_core hooks are the externally-implemented set LVGL links against when
 * LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM. They run only on the LVGL task (single-threaded behind
 * esp_lvgl_port), so the usage counters need no locking.
 */
#include <string.h>

#include "lvgl.h"
#include "esp_heap_caps.h"

/* LVGL's own live / peak byte footprint, for the pool log in ui.c (LVGL task only). */
static size_t s_lv_used;
static size_t s_lv_peak;

void lv_mem_init(void) { }
void lv_mem_deinit(void) { }

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;   /* pools not used — every alloc goes straight to the PSRAM heap */
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    LV_UNUSED(pool);
}

void *lv_malloc_core(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (p) {
        s_lv_used += heap_caps_get_allocated_size(p);
        if (s_lv_used > s_lv_peak) s_lv_peak = s_lv_used;
    }
    return p;
}

void *lv_realloc_core(void *p, size_t new_size)
{
    size_t old = p ? heap_caps_get_allocated_size(p) : 0;
    void *np = heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM);
    if (np) {
        s_lv_used += heap_caps_get_allocated_size(np);
        s_lv_used -= old;
        if (s_lv_used > s_lv_peak) s_lv_peak = s_lv_used;
    } else if (new_size == 0) {
        s_lv_used -= old;   /* realloc(p, 0) frees p and returns NULL */
    }
    return np;
}

void lv_free_core(void *p)
{
    if (p) s_lv_used -= heap_caps_get_allocated_size(p);
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
    memset(mon_p, 0, sizeof(*mon_p));
    /* LVGL now lives in the shared PSRAM heap; report that heap's state plus LVGL's own peak so
     * the ui.c "LVGL pool" log stays meaningful (max_used = LVGL peak; total/free = PSRAM heap). */
    size_t total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t freeb = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    mon_p->total_size        = total;
    mon_p->free_size         = freeb;
    mon_p->free_biggest_size = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    mon_p->max_used          = s_lv_peak;
    mon_p->used_pct          = total ? (uint8_t)(((total - freeb) * 100) / total) : 0;
    mon_p->frag_pct          = freeb ? (uint8_t)(100 - (mon_p->free_biggest_size * 100) / freeb) : 0;
}

lv_result_t lv_mem_test_core(void)
{
    return LV_RESULT_OK;   /* the PSRAM heap integrity is owned by the ESP-IDF allocator */
}
