# examples/stm32h7

STM32H7 + LTDC panel host: the engine, `backends/dma2d/` (Chrom-ART) and the QuickJS bridge, Flow A
with the JavaScript heap in SDRAM.

**Status:** Planned. The backend runs on hardware; the public board project has not landed yet.

The [STM32H7 guide](https://embedded-react.dev/guides/boards/stm32h7) covers what works today and how
a host is put together: the runtime configuration for an SDRAM heap, the Chrom-ART backend's
configuration and op mapping, a static background on a second LTDC layer, and page-flipped
framebuffers.
