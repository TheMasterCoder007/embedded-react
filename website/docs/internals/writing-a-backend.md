---
title: 'Writing a backend'
description: 'The five callbacks, the optional extensions, and the conventions learned from the backends that run on hardware.'
---

A backend is the only part of the stack that knows about hardware. It is one folder per rendering
API or peripheral, not per chip: any STM32 with a Chrom-ART blitter uses `dma2d`, any RP2040 with an
SPI panel uses `pico-spi-lcd`. Before writing one, check whether an existing backend already covers
your display; a new board is usually a new `board.c` in an example, not a new backend.

## The contract

```c
#include "native_renderer.h"

static void fill (uint32_t argb, int x, int y, int w, int h, void *ctx);
static void copy (const void *src, int stride, int x, int y, int w, int h, void *ctx);
static void blend(const void *src, int stride, uint8_t alpha, int x, int y, int w, int h, void *ctx);
static void wait_fn(void *ctx);        /* may be NULL */
static void on_frame(void *ctx);       /* may be NULL */

static const EmbeddedRenderBackend backend = {fill, copy, blend, wait_fn, on_frame, &my_state};
embedded_renderer_set_backend(&backend);
```

- **`fill_rect`** paints a solid rectangle. The colour is straight-alpha `0xAARRGGBB`; the engine
  premultiplies internally, and an alpha below 255 means blend, not replace.
- **`copy_rect`** writes a premultiplied ARGB8888 buffer into the framebuffer. `stride` is the
  source row stride in bytes. The engine only calls it for content it treats as opaque, but the
  buffer's own alpha may still be below 255 in anti-aliased edges; most backends blend it.
- **`blend_rect`** composites a premultiplied ARGB8888 buffer at a global `alpha`. Per channel:
  `out = src + dst * (1 - srcA * alpha / 255)`, with `src` already premultiplied. Every
  anti-aliased edge, every glyph, every shadow and every translucent group arrives here. A backend
  that stubs it paints hard edges and no text, and the engine's own tests guard against exactly that
  (a stubbed blend once let a pixel-equivalence test pass against a blank screen).
- **`wait`** blocks until the hardware has finished consuming the previous frame's pixels, for
  DMA-driven panels. NULL for a synchronous backend.
- **`frame_ready`** says a frame is complete. NULL if you present from your own loop.

Coordinates are in framebuffer pixels, already clipped to the screen. The backend converts to the
panel's format (RGB565, RGB888, BGR) inside the callbacks; the engine never knows.

## Presenting

The engine paints; it never presents. After `er_commit()`, the host asks the engine what changed and
pushes that to the panel:

```c
er_commit();
ERRect rects[16];
int n = er_get_dirty_rects(rects, 16);     /* the disjoint rectangles this commit repainted */
for (int i = 0; i < n; i++) panel_flush(rects[i]);   /* one transfer window per region */
```

`er_get_dirty_rect()` returns the covering box if the driver wants one transfer. Both report the
last commit that actually painted, so a Flow A host, where the reconciler already committed inside
the pump, still reads the frame's real damage.

**The dirty-rectangle convention.** Track an accumulated box as **inclusive minimum corner,
exclusive maximum corner**, the same as `ERRect`, and name the fields `dx0/dy0` and `dx_end/dy_end`
so the convention is visible at every use; empty is `dx_end <= dx0`. Backends are exactly where code
gets copied from one board to the next, and an inclusive box lifted into an exclusive flush loop (or
the reverse) leaves a one-pixel column of stale panel that survives every review, because both
versions look right in isolation. Convert at the panel call instead: `esp_lcd_panel_draw_bitmap`
takes exclusive bounds, an ST7789-style `set_window` takes inclusive ones.

## Optional extensions

**Banded rendering**, for a board with no RAM for a framebuffer. Set `band_height` and provide
`band_begin(x, y, w, h)` and `band_flush()`. The engine then renders each commit's damage as
full-width horizontal strips: for each strip it calls `band_begin`, emits fill/copy/blend calls with
**band-local** Y (already offset by the strip's top) into a `screen_w × band_height` buffer, then
calls `band_flush` to push it to the panel, whose own memory retains the rest of the picture. Band
tiling is applied when ops are emitted, not as a clip, so transform and opacity scratch sources do
not truncate at a seam. `esp32-spi-lcd` keeps two band buffers and ping-pongs them, compositing the
next strip while the panel DMAs the previous one; without that overlap a large repaint shows as a
stepped top-to-bottom wave.

**Format-aware copy.** Provide `copy_rect_fmt(src, stride, fmt, x, y, w, h)` and the engine hands
you images it has proved fully opaque, in their baked format (`ER_IMG_ARGB8888` or
`ER_IMG_RGB565`), as one call for the whole rectangle: replace destination pixels outright, no
per-pixel alpha, no read-modify-write. On DMA2D this is a single M2M/PFC transfer; on an RGB565
framebuffer a 565 source is a row `memcpy`. Leave it NULL and the engine expands non-ARGB sources on
the CPU and goes through `copy_rect`.

**Multiple display buffers.** A page-flipping panel renders into a buffer that was last shown one
or two presents ago, so plain incremental damage would leave the rest of it stale. Call
`er_set_display_buffer_count(2)` (or 3) once at init and `er_display_present()` after each flip; the
engine keeps a damage debt per buffer and repaints enough each commit that whichever buffer is the
target ends up fully correct, with no canonical third buffer and no host-side convergence copy. Base
the flip decision on a box the backend accumulates across every paint since the last flip (the
`dma2d` backend's `take_dirty`), not on `er_get_dirty_rect()`, which covers only the most recent
commit: in Flow A the reconciler commits inside the pump, so by the host's own commit that damage
is already consumed.

## Lessons from the four that run on hardware

- **Premultiplied sources and hardware blenders.** DMA2D's blender takes straight-alpha foregrounds;
  the engine emits premultiplied ones. Blending in hardware would multiply the colour channels by
  alpha a second time and darken every anti-aliased edge. The `dma2d` backend therefore sends opaque
  work to the peripheral and does translucent work on the CPU, deciding with one read-only alpha
  scan of the source. Ops below a size threshold skip the peripheral entirely; register setup costs
  more than a tiny blit.
- **Cache coherency on a Cortex-M7.** When the framebuffer is cacheable, DMA bypasses the D-cache.
  Clean before the peripheral reads CPU-written pixels, clean-and-invalidate before it overwrites
  rows (`SCB_CleanDCache_by_Addr` and friends). Leave the hooks NULL for a non-cacheable or
  write-through region.
- **Panel pixel order.** Some SPI panels want the two colour bytes swapped and red/blue in BGR
  order; the Cheap Yellow Display's ST7789 is one. The telltale is grey rendering pastel green and
  blue rendering pink while red and white look fine. `esp32-spi-lcd` pre-compensates behind a flag.
- **Only one DMA outstanding.** With ping-pong band buffers, wait on a bank before reusing it and a
  plain binary done-semaphore stays correct.
- **The caller owns the panel.** The backends take an initialised panel handle and register their
  transfer-done callback; bring-up (SPI bus, reset, inversion, MADCTL orientation, backlight) is the
  board's job, in `board.c`.
- **Log what you can see.** Both ESP32 examples log free RAM at boot and after start-up, and the
  RP2040 example has a colour-bar test pattern behind a build flag: clean bands prove the driver end
  to end, so anything wrong after that is upstream.

## Build integration

A backend is its own small CMake library that links the engine:

```cmake
add_subdirectory(path/to/embedded-react/engine          ${CMAKE_BINARY_DIR}/embedded-react)
add_subdirectory(path/to/embedded-react/backends/dma2d  ${CMAKE_BINARY_DIR}/backends/dma2d)
target_link_libraries(my_firmware PRIVATE er-backend-dma2d)
```

On ESP-IDF the examples wrap each backend as a component whose `CMakeLists.txt` compiles the
fetched sources. Keep the backend free of the engine's internals: `native_renderer.h` and
`er_scene.h` are the only headers it should need.

## Testing a backend

The engine's CTest suites run against the host `software` path, so they verify the engine, not your
callbacks. For a backend, the useful checks are: a solid `fill_rect` of the whole screen in each
primary (byte order and inversion), a `copy_rect` of a baked image (stride and format), a `blend_rect`
of text over a colour (premultiplied maths), and a dirty-rect flush of a one-pixel-wide change at
each screen edge (the inclusive/exclusive convention). `-DER_PERF_OVERLAY=1` then shows what present
costs per frame.
