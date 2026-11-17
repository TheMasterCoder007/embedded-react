---
title: 'STM32H7'
description: 'Flow A on an STM32H7 with SDRAM and the Chrom-ART backend. The public example project is in progress.'
---

:::info[Example project in progress]
Embedded React runs Flow A on an STM32H7 with SDRAM today, through the `dma2d` (Chrom-ART) backend,
but the public example project has not landed yet; it will be built on an STM32H7 Discovery board.
Until then this page covers what is known to work and how a host is put together.
:::

## What exists

- **The backend.** `backends/dma2d` drives the STM32's Chrom-ART hardware blitter for the engine's
  fill, copy, and blend operations, SDK-free. It implements the optional format-aware copy, so an
  opaque RGB565 image is one DMA2D transfer.
- **The runtime.** The QuickJS bridge is platform-neutral. A Flow A host on this class of board is
  the same handful of calls as anywhere else: `er_runtime_init`, `er_runtime_load_bytecode` (or
  `er_runtime_load_container`), then `er_runtime_pump` and `er_commit` each frame.
- **The RAM.** An H7 with external SDRAM has what Flow A needs: the JavaScript heap goes in SDRAM,
  the engine's buffers and the interpreter's stack stay in the fast internal RAM.

## Writing a host today

The runtime configuration is where an STM32 host differs from the ESP32 one:

```c
const ErRuntimeConfig cfg = {
    .screen_width  = 800,
    .screen_height = 480,
    .log           = uart_log,
    .memory_limit  = 1024 * 1024, /* a JS out-of-memory error instead of an exhausted system heap */
    /* .malloc_functions = NULL: leave it unless the JS heap needs its own region */
};
```

Three things to get right:

- **Heap accounting.** With `malloc_functions` left `NULL`, the bridge installs an allocator that
  reports real block sizes. QuickJS uses those sizes to decide when to collect garbage and to enforce
  `memory_limit`. If you supply your own allocator to put the heap in SDRAM (a TLSF pool, a FreeRTOS
  heap), its `js_malloc_usable_size` **must** return the block's actual size. A stub returning 0
  silently disables the garbage collector: free memory falls a few KB per re-render and never comes
  back, which looks exactly like a leak in the app. `er_runtime_init` warns through `log` if it
  detects this, and `er_runtime_gc_accounting_ok()` reports it to firmware.
- **Collection frequency in SDRAM.** QuickJS starts its GC trigger at 256 KB and recomputes it to
  1.5× the live set after each collection, so a small app in a multi-megabyte arena mark-sweeps far
  more often than it needs to, walking the object graph over a slow bus each time.
  `ErRuntimeConfig.gc_threshold` sets a floor under that trigger; keep it well under `memory_limit`.
  `er_runtime_run_gc()` collects on demand if you would rather put the pause at a screen change.
- **The stack.** QuickJS has no stack of its own; every JavaScript call frame lives on the C stack
  of the task that calls into it. The default STM32 linker script puts the main stack in DTCM, the
  fastest RAM on the part, which is where you want it. Set `ErRuntimeConfig.max_stack_size` below
  the real stack size so deep recursion raises a JavaScript stack-overflow error rather than running
  off the end.

[Hosts with external RAM](../../api/native-ui-bridge.md#hosts-with-external-ram) has the full
discussion, including why the runtime deliberately has no size-tiered allocator.

## The Chrom-ART backend

`backends/dma2d` wraps the DMA2D accelerator behind the engine's callbacks. It works on any STM32
with DMA2D, carries its own register map, and takes the peripheral base address in its config, so it
builds against bare CMSIS or a HAL project alike.

```c
#include "dma2d_backend.h"

er_set_display_rotation(ER_ROTATION_0, 800, 480);   /* the panel's size, as it scans */

ErDma2dBackendConfig cfg = {
    .dma2d         = (void*)0x52001000,      /* H7 (F4/F7: 0x4002B000) */
    .framebuffer   = fb0,
    .stride_pixels = 800,                    /* LTDC rows often pad to 64 bytes */
    .format        = ER_DMA2D_FRAMEBUFFER_RGB888,     /* or ARGB8888, RGB565 */
    /* optional: interrupt-driven start/wait owned elsewhere (else the backend polls TCIF) */
    .start         = my_dma2d_start,
    .wait_complete = my_dma2d_wait,
    .dead_time     = 100,                    /* AMTCR cycles so blits don't starve the LTDC */
};
er_dma2d_backend_init(&cfg);
```

```cmake
add_subdirectory(path/to/embedded-react/engine          ${CMAKE_BINARY_DIR}/embedded-react)
add_subdirectory(path/to/embedded-react/backends/dma2d  ${CMAKE_BINARY_DIR}/backends/dma2d)
target_link_libraries(my_firmware PRIVATE er-backend-dma2d)
```

| Engine operation                                             | Path                                                          |
| ------------------------------------------------------------ | ------------------------------------------------------------- |
| `fill_rect`, alpha 255                                       | DMA2D register-to-memory fill (R2M)                           |
| `copy_rect`, source scans opaque                             | DMA2D memory-to-memory with pixel-format conversion (M2M/PFC) |
| `copy_rect_format` (the engine guarantees opaque)            | DMA2D M2M/PFC, source in its own format, one transfer         |
| Translucent `fill_rect`, `copy_rect`/`blend_rect` with alpha | CPU source-over                                               |

Translucent work stays on the CPU because DMA2D blends straight alpha and the engine emits
premultiplied pixels. Opaque work, the bulk of UI painting, lands on the peripheral; a fully opaque
image is one transfer, so bake full-screen art with the asset pipeline's `format: 'rgb565'` option
to halve its flash and read bandwidth. With `start`/`wait_complete` left `NULL` the backend polls the
peripheral itself; set them when other firmware owns the DMA2D interrupt.

**Cache coherency on a Cortex-M7.** When the framebuffer is cacheable, provide the `cache_clean` and
`cache_clean_invalidate` hooks (`SCB_CleanDCache_by_Addr` and `SCB_CleanInvalidateDCache_by_Addr` on
CMSIS). Leave both `NULL` for a non-cacheable or write-through region.

### Static full-screen art on a second LTDC layer

The LTDC composites two hardware layers, which takes a static full-screen background out of the
render loop entirely: put the baked art in **layer 0**, pointed at the baked RGB565 array in flash,
and render the UI into **layer 1** with a per-pixel-alpha format (ARGB8888 or ARGB1555), cleared to
transparent once. Do not mount the art as an `<Image>`; the pixels no UI node paints stay transparent
and the background shows through. One caveat: translucent UI composites against the engine's own
framebuffer, not the art below it, so give a half-transparent panel that sits on the art an opaque
backing.

### Page-flipped (double or triple buffered) LTDC panels

Many STM32 boards drive the LTDC from two or three framebuffers in SDRAM and page-flip between them
at vblank. There the buffer the engine renders into was last shown one or two presents ago, so pure
incremental damage would leave the rest of it stale. Tell the engine how many buffers rotate, once at
init, and report flips:

```c
er_set_display_buffer_count(2);              /* 2 = double buffer, 3 = triple */

/* each frame: */
er_commit();                                 /* repaints this buffer's damage debt */
if (er_dma2d_backend_present(&x, &y, &w, &h)) {   /* waits for the blits */
    show_buffer(back);                       /* your flip: LTDC address swap at vblank */
    back ^= 1;
    er_dma2d_backend_set_framebuffer(fb[back]);
    er_display_present();                    /* advance the engine's damage rotation */
}
```

Base the flip decision on `er_dma2d_backend_present()`, which accumulates every paint since the
last flip, not on `er_get_dirty_rect()`, which covers only the most recent commit: in Flow A the
reconciler commits inside the pump, so by the host's own commit that damage is already consumed and
the frame would be stranded in the off-screen buffer. The engine repaints enough each commit that
whichever buffer is the target ends up fully correct.

### A panel mounted against its scan direction

A 480×1280 panel mounted as a 1280×480 bar scans its rows along the bar's short side, so the app
runs at 1280×480 and the picture is turned on the way out. Tell the engine before the backend comes
up, then pick one of two ways to turn it.

**A band buffer** keeps both LTDC framebuffers as scan buffers, so the panel page-flips as an
unrotated one does. The engine draws each damage rect in strips into a small buffer, and the backend
copies each strip into the off-screen framebuffer turned:

```c
er_set_display_rotation(ER_ROTATION_90, 480, 1280);   /* before the backend */
er_set_display_buffer_count(2);

static uint16_t band[1280 * 40];             /* internal RAM; 102 KB at RGB565 */

ErDma2dBackendConfig cfg = {
    .dma2d                  = (void*)0x52001000,
    .framebuffer            = fb[back],      /* 480×1280, the off-screen one */
    .format                 = ER_DMA2D_FRAMEBUFFER_RGB565,
    .band_buffer            = band,
    .band_pixels            = 1280 * 40,
    .cache_clean            = clean_dcache,  /* both, when the buffers are cacheable */
    .cache_clean_invalidate = clean_invalidate_dcache,
};
er_dma2d_backend_init(&cfg);

/* each frame: */
er_commit();                                 /* strips land in fb[back], turned */
if (er_dma2d_backend_present(&x, &y, &w, &h)) {
    show_buffer(back);                       /* your flip, at vblank */
    back ^= 1;
    er_dma2d_backend_set_framebuffer(fb[back]);
    er_display_present();
}
```

The band buffer is a pixel budget, not a shape. A strip is as wide as its damage rect and as tall as
fits the budget, so a 200-wide change renders as 200×256 in one strip and a small change costs its own
area. It must hold at least one logical row: 1280 pixels, 2.5 KB at RGB565. The 1280×40 above is
102 KB (154 KB at RGB888), against 1.2 MB for a third framebuffer. A bigger budget means fewer strips
on a big repaint, and each strip walks the scene once. Put it in internal RAM (AXI SRAM or DTCM), not
SDRAM, and align it to 4 bytes. The D-cache is cleaned over the frame's box in
`er_dma2d_backend_present()`, so call it before every flip.

**A render buffer** is a whole buffer of the app's shape for the engine to draw into, presented each
frame:

```c
er_set_display_rotation(ER_ROTATION_90, 480, 1280);   /* panel size as it scans */

ErDma2dBackendConfig cfg = {
    .dma2d                  = (void*)0x52001000,
    .framebuffer            = fb0,                      /* 480×1280: what the LTDC scans */
    .format                 = ER_DMA2D_FRAMEBUFFER_RGB565,
    .render_buffer          = fb1,                      /* 1280×480: what the engine draws into */
    .render_stride_pixels   = 1280,
    .cache_clean            = clean_dcache,             /* SCB_CleanDCache_by_Addr */
    .cache_clean_invalidate = clean_invalidate_dcache,  /* SCB_CleanInvalidateDCache_by_Addr */
};
er_dma2d_backend_init(&cfg);

/* each frame: */
er_commit();
er_dma2d_backend_present(NULL, NULL, NULL, NULL);   /* copies the damage into fb0, turned */
```

`screen` and `Dimensions` report 1280×480; with `screen_width` and `screen_height` left at 0, the
runtime takes that size from the engine. The touch controller reports points on the 480×1280 glass,
and the driver passes them to `embedded_renderer_touch()` as read: the engine turns them.

The present copies only the frame's damage, in strips 16 pixels wide so the reads and the writes
both stay in cache. On cacheable SDRAM both cache hooks are needed: the DMA2D writes `fb1` before
the CPU reads it, and the CPU writes `fb0` before the LTDC reads it. The second LTDC framebuffer
works as the render buffer when it holds 480 rows of the render stride (a tightly packed one always
does). That leaves the LTDC scanning `fb0` alone, so keep `er_set_display_buffer_count()` at 1; a
copy can land while the panel scans that area. 180 degrees keeps the panel's shape and takes the
same path.

## Which flow

Flow A is the natural fit for an H7 with SDRAM, and it is what runs today. An STM32 without external
RAM (most F4 parts, for example) is a Flow B target: compile the app to a package and run it on the Flow B runtime, as the
[CYD](./esp32-cyd.md) and [RP2040](./rp2040.md) examples do, with the `dma2d` backend underneath.
