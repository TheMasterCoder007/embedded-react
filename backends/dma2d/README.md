# backends/dma2d

STM32 DMA2D hardware-blitter backend. Wraps the Chrom-ART accelerator's fill / copy /
blend operations behind the five `EmbeddedRenderBackend` callbacks.

Works on any STM32 with DMA2D: F4 / F7 / H7 / U5. The frame tick is typically driven
from the LTDC vsync IRQ.

**Status:** Stub. `renderer_backend.c` is currently a placeholder; real implementation
will land alongside the first STM32H7 example.

## Opaque images: implement `copy_rect_fmt`

A DMA2D implementation should provide the optional `copy_rect_fmt` callback. The engine
routes every fully opaque image blit through it — the whole rect in one call, source
handed over in its registered pixel format, with a hard guarantee that every pixel is
opaque. That maps 1:1 onto a plain M2M(_PFC) transfer:

- `ER_IMG_RGB565` source → `FGPFCCR` color mode `DMA2D_CM_RGB565` (PFC to an ARGB8888
  output, or straight M2M when the framebuffer is RGB565 too).
- `ER_IMG_ARGB8888` source → plain M2M.

No `region_is_opaque` pre-scan and no per-pixel alpha path is needed on this entry point —
the registry already scanned the pixels once at load. Without `copy_rect_fmt` a 16-bit
image is expanded on the CPU and emitted per row, which on a full-screen background is
~1M conversions plus one transfer per scanline: measurably slower than the single-shot
ARGB path it was meant to beat.

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

- The engine's blit callbacks are host-implemented here, and the stock backends keep the
  framebuffer opaque (alpha forced to 0xFF). For the show-through to work your callbacks
  must preserve alpha 0 in never-painted pixels — only pixels a node actually covers get
  written, which the damage-clipped repaint already guarantees.
- Translucent UI composites against the **engine's own framebuffer**, not the art below
  it (the engine can't see layer 0). Fully opaque UI panels over the art are exact;
  a half-transparent panel blends with whatever layer 1 held underneath, so give such
  panels an opaque backing if they sit on the art.

If the art must live in the scene graph instead (it scrolls, fades, or sits between UI
elements), skip the second layer and lean on the engine's opaque-image fast path + RGB565
bake — a fully opaque background already blits with no read-modify-write and half the
source traffic.

## Page-flipped (double / triple buffered) LTDC panels

Many STM32 boards drive the LTDC from two (or three) framebuffers in SDRAM and hardware
page-flip between them at vblank. There the buffer the engine renders into was last shown
one or two presents ago, so pure incremental damage (the default) would leave the rest of
that buffer stale — moved elements ghost, and a change disjoint from other damage lands in
only one buffer. Tell the engine how many buffers rotate, once at init:

```c
er_set_display_buffer_count(2);   // 2 = double buffer, 3 = triple

// each frame:
//   backend get_framebuffer() -> the OFF-screen buffer (ActiveFrameBuffer ^ 1)
er_commit();                      // repaints this buffer's outstanding damage debt into it
Firmware.Data->ActiveFrameBuffer ^= 1;   // firmware page-flips at vblank
er_display_present();             // inform the engine that one flip occurred (advance to next buffer)
```

The engine repaints enough each commit that whichever buffer is the target ends up fully
correct — no third "canonical" framebuffer and no host-side convergence copy. It needs no
knowledge of the framebuffer addresses, the flip mechanism, or vblank; only the buffer count and one er_display_present() call per flip.

> **Cache coherency (host side, independent of the count above):** when SDRAM is mapped
> cacheable and DMA2D writes bypass the D-cache, a CPU read after a DMA2D write can read
> stale data. Clean/invalidate the D-cache around DMA2D transfers (or run CPU-only).
