# backends/dma2d

STM32 DMA2D hardware-blitter backend. Wraps the Chrom-ART accelerator's fill / copy /
blend operations behind the five `EmbeddedRenderBackend` callbacks.

Works on any STM32 with DMA2D: F4 / F7 / H7 / U5. The frame tick is typically driven
from the LTDC vsync IRQ.

**Status:** Stub. `renderer_backend.c` is currently a placeholder; real implementation
will land alongside the first STM32H7 example.
