# backends/software

Pure CPU-blit backend — works on any board with a writable linear framebuffer and no
hardware accelerator worth using. Also the reference compositor: it runs the identical
primitive code the device runs, so its output is pixel-accurate to a hardware ARGB
target. It is the foundation the [WASM simulator](../web/) presents to a canvas, and a
fallback for boards without DMA2D / PXP / GL.

It owns an ARGB8888 framebuffer in RAM (0xAARRGGBB, persists between commits like panel
GRAM). `fill_rect` (straight-alpha source-over), `copy_rect` (premultiplied source-over),
and `blend_rect` (premultiplied, scaled by a global alpha) are clipped scanline loops.

## Files

- [`software_backend.h`](software_backend.h) — `er_software_backend_init` / `_destroy` /
  `_clear` and the framebuffer accessors.
- [`renderer_backend.c`](renderer_backend.c) — the compositor.

**Status:** Implemented (full-framebuffer path). Banded mode and display-format
conversion (e.g., RGB565) can layer on later as needed.
