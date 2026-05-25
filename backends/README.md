# backends

Reference implementations of the `EmbeddedRenderBackend` struct (five function pointers:
`fill_rect`, `copy_rect`, `blend_rect`, `wait`, `frame_ready`). The engine is portable;
backends are not. Each folder is one rendering API or peripheral, not one chip — multiple
chips can share a backend (any STM32 with DMA2D uses `dma2d/`; any board with no GPU
falls back to `software/`).

| Backend | Description | Status |
|---|---|---|
| `dma2d/` | STM32 DMA2D hardware blitter | Stub (planned reference backend) |
| `esp32-lcd/` | ESP32-S3 LCD peripheral + PSRAM framebuffer, CPU blit | Stub |
| `framebuffer/` | Generic Linux `/dev/fb0` blitter | Planned (README only) |
| `opengl/` | OpenGL ES 2.0 — RPi, Android, anything with a GL context | Planned (README only) |
| `sdl/` | SDL2 — desktop dev and host-side test target | Stub (first end-to-end target) |
| `software/` | Pure CPU blit — works on any board with a writable framebuffer | Stub |
| `web/` | WebGL/Canvas via WASM | Planned (README only) |

## Writing a backend

```c
#include "native_renderer.h"

static void fill (uint32_t argb, int x, int y, int w, int h, void *ctx) { /* ... */ }
static void copy (const void *src, int stride, int x, int y, int w, int h, void *ctx) { /* ... */ }
static void blend(const void *src, int stride, uint8_t alpha, int x, int y, int w, int h, void *ctx) { /* ... */ }
static void wait_fn(void *ctx) { /* may be NULL for synchronous backends */ }
static void on_frame(void *ctx) { embedded_renderer_tick(16); }

void my_backend_init(void) {
    static const EmbeddedRenderBackend b = { fill, copy, blend, wait_fn, on_frame, NULL };
    embedded_renderer_set_backend(&b);
}
```

That's it. The engine never includes any platform header — it only calls through the
struct.
