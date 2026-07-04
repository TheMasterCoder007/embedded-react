# backends/dma2d

STM32 DMA2D hardware-blitter backend. Wraps the Chrom-ART accelerator's fill / copy /
blend operations behind the five `EmbeddedRenderBackend` callbacks.

Works on any STM32 with DMA2D: F4 / F7 / H7 / U5. The frame tick is typically driven
from the LTDC vsync IRQ.

**Status:** Stub. `renderer_backend.c` is currently a placeholder; real implementation
will land alongside the first STM32H7 example.

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
