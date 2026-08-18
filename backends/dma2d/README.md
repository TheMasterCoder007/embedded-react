# backends/dma2d

STM32 DMA2D (Chrom-ART) hardware-blitter backend. Wraps the accelerator behind the
`EmbeddedRenderBackend` callbacks, with a CPU compositor for the ops the peripheral
cannot express.

Works on any STM32 with DMA2D: F4 / F7 / H7 / U5. The backend is SDK-free — it carries
its own register map (identical across all four families) and takes the peripheral base
address in its config, so it builds against bare CMSIS, HAL projects, or firmware that
owns the peripheral behind a service interface.

**Status:** Implemented. Proven on STM32F746 and STM32H743 boards driving LTDC panels
from double-buffered SDRAM framebuffers, in both interrupt-driven (host-owned DMA2D
IRQ via the start/wait hooks) and polled configurations.

## Op mapping

| Engine op | Path |
|---|---|
| `fill_rect`, alpha = 255 | DMA2D register-to-memory fill (R2M) |
| `copy_rect`, source scans opaque | DMA2D memory-to-memory + pixel-format conversion (M2M/PFC) |
| `copy_rect_fmt` (engine guarantees opaque) | DMA2D M2M/PFC, source in its own format, one transfer |
| `fill_rect` translucent, `copy_rect`/`blend_rect` with alpha | CPU source-over |

The CPU paths exist because DMA2D's blender takes **straight-alpha** foregrounds while the
engine emits **premultiplied** sources — blending in hardware would multiply the color
channels by alpha a second time and darken every anti-aliased edge. Opaque work (the bulk
of UI painting) still lands on the peripheral; `copy_rect` decides with a single read-only
alpha scan of the source region. Ops smaller than `min_dma_pixels` skip the peripheral
entirely (register setup costs more than a tiny blit).

Framebuffer formats: ARGB8888, RGB888, RGB565 (output PFC does the conversion; the CPU
paths use exact-inverse load/store helpers).

## Usage

```c
#include "dma2d_backend.h"

ErDma2dBackendConfig cfg = {
    .dma2d         = (void*)0x52001000,      /* H7 (F4/F7: 0x4002B000) */
    .framebuffer   = fb0,
    .width         = 800,
    .height        = 480,
    .stride_pixels = 800,                    /* LTDC rows often pad to 64 bytes */
    .format        = ER_DMA2D_FB_RGB888,
    /* optional: interrupt-driven start/wait owned elsewhere (else the backend polls TCIF) */
    .start         = my_dma2d_start,
    .wait_complete = my_dma2d_wait,
    .dead_time     = 100,                    /* AMTCR cycles so blits don't starve the LTDC */
};
er_dma2d_backend_init(&cfg);
```

When `start`/`wait_complete` are NULL the backend sets `CR.START` itself and polls `TCIF` —
correct anywhere, no interrupt required. When the DMA2D IRQ is owned by other firmware,
`start` is called after every register except START is programmed; `wait_complete` must
block until that transfer finishes.

## Opaque images: `copy_rect_fmt`

The backend implements the engine's optional `copy_rect_fmt` callback. The engine routes
every fully opaque image blit through it — the whole rect in one call, source handed over
in its registered pixel format, with a hard guarantee that every pixel is opaque. That maps
1:1 onto a plain M2M(_PFC) transfer:

- `ER_IMG_RGB565` source → `FGPFCCR` color mode `DMA2D_CM_RGB565` (PFC to an ARGB8888 or
  RGB888 output, or straight M2M when the framebuffer is RGB565 too).
- `ER_IMG_ARGB8888` source → plain M2M.

No opacity pre-scan and no per-pixel alpha path is needed on this entry point — the registry
already scanned the pixels once at load. Bake full-screen art with the asset pipeline's
`format: 'rgb565'` option to halve both flash footprint and source-read bandwidth.

One constraint the code enforces: DMA2D requires `FGMAR` aligned to the source color mode's
pixel size. A word-misaligned ARGB8888 source raises a configuration error — the transfer
never starts and the image is silently absent — so misaligned sources fall back to the CPU
replace path.

## Static full-screen art on a second LTDC layer

The LTDC composites two hardware layers per scanout, which is a zero-cost way to take a
static full-screen background out of the render loop entirely: the engine never re-blits
art the panel controller composites for free.

- Put the baked art in **layer 0** (bottom), pointed at a flash- or SDRAM-resident bitmap
  the engine never touches. RGB565 halves the LTDC's own fetch bandwidth vs ARGB8888 —
  bake it with the asset pipeline's `format: 'rgb565'` option and point the layer at the
  baked array directly.
- Render the UI into **layer 1** (top) with a per-pixel-alpha pixel format (ARGB8888 /
  ARGB1555) and an initial clear to fully transparent. Don't mount the art as an
  `<Image>` at all — regions no UI node ever paints stay transparent and the background
  shows through.

Two caveats to design around:

- This backend keeps the framebuffer opaque (alpha forced to `0xFF` on every write), like
  the other stock backends. For the show-through to work the never-painted pixels must keep
  alpha 0, which the damage-clipped repaint already guarantees — only pixels a node actually
  covers get written.
- Translucent UI composites against the **engine's own framebuffer**, not the art below
  it (the engine can't see layer 0). Fully opaque UI panels over the art are exact;
  a half-transparent panel blends with whatever layer 1 held underneath, so give such
  panels an opaque backing if they sit on the art.

If the art must live in the scene graph instead (it scrolls, fades, or sits between UI
elements), skip the second layer and lean on the opaque-image fast path above — a fully
opaque background already blits with no read-modify-write and half the source traffic.

## Page-flipped (double / triple buffered) LTDC panels

Many STM32 boards drive the LTDC from two (or three) framebuffers in SDRAM and hardware
page-flip between them at vblank. There the buffer the engine renders into was last shown
one or two presents ago, so pure incremental damage (the default) would leave the rest of
that buffer stale — moved elements ghost, and a change disjoint from other damage lands in
only one buffer. Tell the engine how many buffers rotate, once at init, and report flips:

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

Base the flip decision on `er_dma2d_backend_take_dirty()` — the box accumulated across
**every** paint into the buffer since the last flip — not on `er_get_dirty_rect()`, which
reports only the most recent `er_commit()`. The QuickJS bridge's React reconciler calls
`er_commit()` itself when a render lands (`NativeUI.commit`, e.g. synchronously inside a
Pressable's touch dispatch), so by the time the host's own commit runs the damage is
already consumed: gating the flip on `er_get_dirty_rect` leaves those frames stranded in
the off-screen buffer.

The engine repaints enough each commit that whichever buffer is the target ends up fully
correct — no third "canonical" framebuffer and no host-side convergence copy. It needs no
knowledge of the framebuffer addresses, the flip mechanism, or vblank; only the buffer
count and one `er_display_present()` call per flip.

## Cache coherency (Cortex-M7)

When the framebuffer/scratch memory is mapped cacheable, DMA2D transfers bypass the
D-cache. Provide the two cache hooks (`cache_clean` before the peripheral reads
CPU-written pixels, `cache_clean_invalidate` before it overwrites framebuffer rows) —
on CMSIS these map to `SCB_CleanDCache_by_Addr` / `SCB_CleanInvalidateDCache_by_Addr`.
Leave both NULL when the region is non-cacheable or write-through (common MPU setups).

## Build

```cmake
add_subdirectory(path/to/embedded-react/engine   ${CMAKE_BINARY_DIR}/embedded-react)
add_subdirectory(path/to/embedded-react/backends/dma2d ${CMAKE_BINARY_DIR}/backends/dma2d)
target_link_libraries(my_firmware PRIVATE er-backend-dma2d)
```

`er-backend-dma2d` links the engine and exports this directory's include path, so
`#include "dma2d_backend.h"` works from firmware. The backend compiles as plain C99 with
no vendor headers and no libc beyond `<string.h>`.
