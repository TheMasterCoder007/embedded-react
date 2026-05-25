# backends/software

Pure CPU-blit backend — works on any board with a writable linear framebuffer and no
hardware accelerator worth using. Used as the fallback for RP2040, low-end Cortex-M
boards, and anything else without DMA2D / PXP / GL.

`fill` / `copy` / `blend` are straightforward scanline loops with ARGB8888 → display
format conversion inline.

**Status:** Stub.
