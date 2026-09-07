# M3 — LVGL v9 on the CO5300 esp_lcd panel + CST9217 touch (implementation plan)

**Status:** researched & planned, **not yet implemented**. As of 2026-08-06.
Derived from a 4-agent research workflow (LVGL 9 docs, Espressif `esp_lvgl_port`/esp-bsp,
`esp_lcd_co5300` source, LVGL indev docs) — **overall confidence: high**. A fresh session
should implement M3 directly from this doc.

## Decisions (the load-bearing ones)

1. **Use `espressif/esp_lvgl_port` (`"^2"`, resolves to 2.8.x, defaults to LVGL 9.3).**
   Add it to `firmware/src/idf_component.yml`. It transitively pulls `lvgl/lvgl` v9 — do NOT
   list `lvgl` separately unless you want to pin (`lvgl/lvgl: "~9.3.0"`). The port is
   Espressif's canonical LVGL9↔esp_lcd glue and owns the **tick, LVGL task, mutex, flush_cb,
   and the `on_color_trans_done → lv_display_flush_ready` wiring** — i.e. all the fiddly parts.
   Hand-rolling is possible but not worth it.

2. **Color depth = 24 (RGB888).** This branch exists to keep the smooth, banding-free 24-bit
   look, so RGB565 (16) is rejected (switching COLMOD back to 0x55 reintroduces the banding we
   killed). 32-bit XRGB8888 is rejected (4→3 repack + 33% more buffer RAM for identical output
   on a GPU-less S3). Set `CONFIG_LV_COLOR_DEPTH=24` and the display `color_format =
   LV_COLOR_FORMAT_RGB888`.
   - **Byte-order gotcha (critical):** LVGL v9 `lv_color_t` is `{blue, green, red}` in memory,
     so its RGB888 buffers are **B,G,R**. Our panel is currently `rgb_ele_order = RGB` (we
     verified R,G,B). Fix with **one panel line**: set `rgb_ele_order =
     LCD_RGB_ELEMENT_ORDER_BGR` (toggles the MADCTL BGR bit in `esp_lcd_co5300_spi.c`), then
     hand LVGL's `px_map` straight to `draw_bitmap` — zero CPU conversion. Keep
     `flags.swap_bytes = false` (it only swaps 16-bit pairs; no-op for 24-bit).
   - **Consequence for the existing test patterns:** after flipping to BGR, `draw_full()` in
     `display.c` must write **B,G,R** (`px[0]=b; px[1]=g; px[2]=r`) instead of R,G,B, or the
     bands/gradient render R↔B swapped. Two-token change. **Sanity-check on-device: a red fill
     must still read red before building UI.**

3. **Draw buffers = partial mode, internal DMA RAM.** A full RGB888 frame is 410·502·3 ≈
   **603 KB** — far over the ~320 KB internal RAM, so **never allocate a full framebuffer**.
   Use `LV_DISPLAY_RENDER_MODE_PARTIAL` with two 410×40-line buffers (~49 KB each, ~98 KB
   total) or a single 410×80-line buffer (~96 KB) if internal RAM is tight after SPI DMA +
   task stacks. `flags.buff_dma = true`; do **not** use `flags.buff_spiram` (PSRAM buffers need
   a bounce/`trans_size` and are slower). Retire the old `draw_full()`/PSRAM full-frame path
   once LVGL owns the panel (keep the test-pattern functions for the red-fill sanity check).

4. **Config via Kconfig (`sdkconfig.defaults`), NOT an `lv_conf.h`.** Under PlatformIO+ESP-IDF
   the managed `lvgl` component ships its own Kconfig; the `LV_CONF_PATH`/build-flag route is
   fragile. Add to `firmware/sdkconfig.defaults`:
   - `CONFIG_LV_COLOR_DEPTH=24`  ← must match the RGB888 display format or colors garble
   - `CONFIG_LV_FONT_MONTSERRAT_14=y`  ← a default font
   - `CONFIG_LV_MEM_SIZE_KILOBYTES=48`  ← LVGL object heap (raise / route to PSRAM if UI grows)
   - Do **NOT** enable `LV_TICK_CUSTOM` — esp_lvgl_port drives the tick.
   - **After editing, delete the generated `firmware/.pio/build/*/sdkconfig` (or `pio run -t
     fullclean`)** so the new options actually apply. Verify the exact symbol name against the
     pulled lvgl version (the color-depth `choice` yields `CONFIG_LV_COLOR_DEPTH=24`).

## Files to add / change

- **`firmware/src/idf_component.yml`** — add `espressif/esp_lvgl_port: "^2"`.
- **`firmware/src/display.{c,h}`** — (a) hoist the panel-IO handle to `static
  esp_lcd_panel_io_handle_t s_io;` (currently a local in `nocsif_display_init`); (b) add
  accessors `esp_lcd_panel_handle_t nocsif_display_panel(void)` (returns `s_panel`) and
  `esp_lcd_panel_io_handle_t nocsif_display_io(void)` (returns `s_io`), declared in
  `display.h`; (c) flip `rgb_ele_order` RGB→**BGR** (line ~92) and swap `draw_full`'s inner
  writes to B,G,R; (d) fix the stale line-~82 comment (built-in init sends COLMOD **0x77**, not
  0x55, since bits_per_pixel=24). **Keep `CO5300_PANEL_IO_QSPI_CONFIG(CS, NULL, NULL)` NULL
  callbacks** — esp_lvgl_port registers the trans-done callback itself from the exposed IO
  handle; registering our own would conflict.
- **`firmware/src/ui.{c,h}`** (NEW) — `nocsif_ui_init()`:
  1. `lvgl_port_init(&ESP_LVGL_PORT_INIT_CONFIG())`
  2. `lvgl_port_add_disp(&cfg)` with `.io_handle = nocsif_display_io()`, `.panel_handle =
     nocsif_display_panel()`, `hres=410`, `vres=502`, `buffer_size` (e.g. 410*40),
     `color_format = LV_COLOR_FORMAT_RGB888`, `flags.buff_dma = true`. The port sets flush_cb =
     `esp_lcd_panel_draw_bitmap(panel, a->x1, a->y1, a->x2+1, a->y2+1, px_map)` internally.
     **The 22 px x-gap is already added inside the co5300 driver's draw_bitmap — do not add it.**
  3. Custom **POINTER indev** reusing `nocsif_touch_read` (see sketch below) — NOT
     `lvgl_port_add_touch` (that needs an `esp_lcd_touch_handle_t`, which our hand-rolled driver
     is not). Coords are already native (X 0..410, Y 0..502) → pass through, no transform.
  4. Minimal demo screen (below).
  - **All `lv_*` calls from app_main/other tasks must be wrapped in `lvgl_port_lock(0)` /
    `lvgl_port_unlock()`** (LVGL is not thread-safe). The indev read_cb runs inside the port's
    LVGL task with the lock already held, so blocking I2C touch reads there are safe.
- **`firmware/src/CMakeLists.txt`** — add `"ui.c"` to SRCS (managed deps auto-wire into
  REQUIRES, like `esp_lcd_co5300` already does).
- **`firmware/sdkconfig.defaults`** — the three `CONFIG_LV_*` above.
- **`firmware/src/main.c`** — call `nocsif_ui_init()` after display + touch init, and
  **DELETE the `touch_task`** (and its function). ⚠ **CRITICAL:** the LVGL indev now owns the
  CST9217 — two independent readers of `nocsif_touch_read` race to consume reports and drop
  touches.

## Indev read_cb sketch

```c
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    static int32_t lx = 0, ly = 0;
    nocsif_touch_point_t p; int n = 0;
    if (nocsif_touch_read(&p, 1, &n) == ESP_OK && n > 0) {
        lx = p.x; ly = p.y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;   /* report last point on release */
    }
    data->point.x = lx; data->point.y = ly;
}
/* after the display exists, under lvgl_port_lock(0):
   lv_indev_t *in = lv_indev_create();
   lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
   lv_indev_set_read_cb(in, touch_read_cb);
   lv_indev_set_display(in, disp);                */
```

## Minimal demo (proves display + input in one glance)

```c
lvgl_port_lock(0);
lv_obj_t *scr = lv_screen_active();
lv_obj_t *btn = lv_button_create(scr);
lv_obj_center(btn);
lv_label_set_text(lv_label_create(btn), "Hello NocSif");
lv_obj_t *co = lv_label_create(scr);
lv_obj_align(co, LV_ALIGN_BOTTOM_MID, 0, -20);
lv_timer_create(coord_timer_cb, 200, co);  /* reads last touch -> lv_label_set_text_fmt(co, "%d,%d", x, y) */
lvgl_port_unlock();
```
Tapping the button fires `LV_EVENT_CLICKED` (proves hit-testing / indev routing); the bottom
label tracks live touch coords (proves the read_cb feeds real points). If the button reacts and
the coords move under your finger, the display+touch stack is fully wired.

## Risks / gotchas

1. **Double touch reader** — must delete `main.c`'s `touch_task` when the indev goes in (else
   dropped touches). *(highest-impact)*
2. **BGR flip breaks the bare test patterns** unless `draw_full` also swaps to B,G,R. Verify a
   red fill still reads red on-device before building UI.
3. **`CONFIG_LV_COLOR_DEPTH` must == 24 AND match `color_format = RGB888`** — mismatch garbles
   color. Confirm the Kconfig symbol name against the pulled lvgl version.
4. **RGB888 partial-buffer RAM** — two 40-line buffers (~98 KB) must fit in ~320 KB internal
   alongside SPI DMA + stacks. If alloc fails: single 80-line buffer, or fewer lines.
5. **PlatformIO sdkconfig staleness** — LVGL options won't take until the generated `.pio`
   `sdkconfig` is regenerated (delete it / `fullclean`) after editing `sdkconfig.defaults`.
6. **No TE sync** — TE (GPIO6) is unused, so fast full-screen redraws can tear. Acceptable for a
   watch UI; wire TE later if visible tearing matters.
7. **QSPI chunking** — bus `max_transfer_sz` ~13 KB, so a partial flush splits into multiple SPI
   transactions; `on_color_trans_done` fires only after the last (which `lv_display_flush_ready`
   relies on). `trans_queue_depth=10` in the IO macro is adequate. Confirm no early flush_ready.
8. **Verify `esp_lvgl_port` 2.8.x accepts IDF 5.5.4** and defaults to LVGL 9 (it does at v2)
   before trusting the transitive lvgl pin.
9. **Minor perf** — 24-bit pixels aren't 4-byte aligned, so LVGL software blend is marginally
   slower than 16/32-bit. A nuance, not correctness; fine for a watch UI.

## Build/flash/verify (unchanged methodology)

Build from **PowerShell** (`python -m platformio run -d …\firmware`), flash with esptool
`--no-stub` to COM7 (watch in download mode: BOOT→RST→BOOT), read serial via
`scratchpad read_serial.py`/`reset_read.py`. Success gate: red-fill still red, then the demo
button reacts to taps and the coord label tracks a finger.
