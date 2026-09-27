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

ErDma2dBackendConfig cfg = {
    .dma2d         = (void*)0x52001000,      /* H7 (F4/F7: 0x4002B000) */
    .framebuffer   = fb0,
    .width         = 800,
    .height        = 480,
    .stride_pixels = 800,                    /* LTDC rows often pad to 64 bytes */
    .format        = ER_DMA2D_FB_RGB888,     /* or ARGB8888, RGB565 */
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
| `copy_rect_fmt` (the engine guarantees opaque)               | DMA2D M2M/PFC, source in its own format, one transfer         |
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
if (er_dma2d_backend_take_dirty(&x, &y, &w, &h)) {
    er_dma2d_backend_wait();                 /* fence: blits done before the flip */
    show_buffer(back);                       /* your flip: LTDC address swap at vblank */
    back ^= 1;
    er_dma2d_backend_set_framebuffer(fb[back]);
    er_display_present();                    /* advance the engine's damage rotation */
}
```

Base the flip decision on `er_dma2d_backend_take_dirty()`, which accumulates every paint since the
last flip, not on `er_get_dirty_rect()`, which covers only the most recent commit: in Flow A the
reconciler commits inside the pump, so by the host's own commit that damage is already consumed and
the frame would be stranded in the off-screen buffer. The engine repaints enough each commit that
whichever buffer is the target ends up fully correct.

## Which flow

Flow A is the natural fit for an H7 with SDRAM, and it is what runs today. An STM32 without external
RAM (most F4 parts, for example) is a Flow B target: compile the app to C and link it in, as the
[CYD](./esp32-cyd.md) and [RP2040](./rp2040.md) examples do, with the `dma2d` backend underneath.
