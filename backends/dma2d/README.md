# backends/dma2d

STM32 DMA2D (Chrom-ART) hardware-blitter backend: the accelerator behind the `EmbeddedRenderBackend`
callbacks, with a CPU compositor for the operations the peripheral cannot express (translucent
sources, because DMA2D blends straight alpha and the engine emits premultiplied). Works on any STM32
with DMA2D (F4 / F7 / H7 / U5), SDK-free: it carries its own register map and takes the peripheral
base address in its config. Proven on STM32F746 and STM32H743 boards driving LTDC panels from
double-buffered SDRAM framebuffers.

**Docs:** the [STM32H7 guide](https://embedded-react.dev/guides/boards/stm32h7#the-chrom-art-backend)
has the op mapping, the opaque-image path, cache coherency, a static background on a second LTDC
layer, and page-flipped framebuffers; [Writing a backend](https://embedded-react.dev/internals/writing-a-backend)
has the contract.

## Usage

```c
#include "dma2d_backend.h"

er_set_display_rotation(ER_ROTATION_0, 800, 480);   /* the panel's size, as it scans */

ErDma2dBackendConfig cfg = {
    .dma2d         = (void*)0x52001000,      /* H7 (F4/F7: 0x4002B000) */
    .framebuffer   = fb0,
    .stride_pixels = 800,                    /* LTDC rows often pad to 64 bytes */
    .format        = ER_DMA2D_FRAMEBUFFER_RGB888,     /* ARGB8888, RGB888 or RGB565 */
    .start         = my_dma2d_start,         /* optional pair, for a host-owned DMA2D IRQ; */
    .wait_complete = my_dma2d_wait,          /* NULL = the backend starts and polls TCIF */
    .cache_clean   = NULL,                   /* SCB_CleanDCache_by_Addr etc. when the fb is cacheable */
    .dead_time     = 100,                    /* AMTCR cycles so blits don't starve the LTDC */
};
er_dma2d_backend_init(&cfg);
```

Per frame: `er_commit()`, then `er_dma2d_backend_present(&x, &y, &w, &h)`, which waits for the blitter
and reports whether anything was painted. On a page-flipped panel, flip when it returns true, point at
the next buffer with `er_dma2d_backend_set_framebuffer()`, and call `er_display_present()`.

A page-flipped panel renders each change once. At its next commit the backend copies what the
off-screen buffer missed from the buffer set before it (one M2M transfer per rect, at any rotation
with a band buffer), and the engine renders only the new damage. The copy is not reported by
`er_dma2d_backend_present()`, so a commit that changes nothing causes no flip. Wait for a flip to
take effect before the next commit. With `cache_clean` and
`cache_clean_invalidate` set, the copy cleans its source and invalidates its destination. A render
buffer is single-buffered and does not copy.

The copy reads the buffer the LTDC is scanning and shares the SDRAM bus with it. The backend waits
for each transfer, so the engine does not render while it runs. If the picture still shifts sideways
for a frame when a large area changes, set `.no_buffer_sync = true`: the engine then renders what
each buffer missed again, once per buffer.

## Rotated panels

A 480×1280 panel mounted as a 1280×480 bar runs a 1280×480 app, and the picture is turned on its
way into the framebuffer. There are two ways to do it.

### Band buffer: page-flipped

The engine draws each damage rect in strips into a small band buffer, and the backend copies each
strip into the off-screen framebuffer turned. Both LTDC framebuffers stay scan buffers, so the panel
flips as an unrotated one does:

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

The band buffer is a pixel budget, not a shape: a strip is as wide as its damage rect and as tall as
fits, so a 200-wide change renders as 200×256 in one strip, and a small change costs its own area. It
must hold at least one logical row (1280 pixels here). More pixels means fewer strips for a big
repaint; each strip walks the scene once. Keep it in internal RAM and aligned to 4 bytes.

### Render buffer

The engine draws into a whole buffer of the logical shape, and `er_dma2d_backend_present()` copies
each frame's damage into the scanned framebuffer turned, then cleans the D-cache over what it wrote:

```c
er_set_display_rotation(ER_ROTATION_90, 480, 1280);   /* before the backend */

ErDma2dBackendConfig cfg = {
    .dma2d                  = (void*)0x52001000,
    .framebuffer            = fb0,           /* 480×1280, what the LTDC scans */
    .format                 = ER_DMA2D_FRAMEBUFFER_RGB565,
    .render_buffer          = fb1,           /* 1280×480, what the engine draws into */
    .render_stride_pixels   = 1280,
    .cache_clean            = clean_dcache,  /* both, when the buffers are cacheable */
    .cache_clean_invalidate = clean_invalidate_dcache,
};
er_dma2d_backend_init(&cfg);

/* each frame: */
er_commit();
er_dma2d_backend_present(NULL, NULL, NULL, NULL);
```

The second LTDC framebuffer can be the render buffer when it holds 480 rows of the render stride,
which a tightly packed one always does. The LTDC then scans `fb0` alone, so the copy lands in the
buffer on screen and can tear; leave the buffer count at 1.

Either way, touch drivers pass panel points to `embedded_renderer_touch()` as they read them.

## Build

```cmake
add_subdirectory(path/to/embedded-react/engine   ${CMAKE_BINARY_DIR}/embedded-react)
add_subdirectory(path/to/embedded-react/backends/dma2d ${CMAKE_BINARY_DIR}/backends/dma2d)
target_link_libraries(my_firmware PRIVATE er-backend-dma2d)
```

`er-backend-dma2d` links the engine and exports this directory's include path. Plain C99, no vendor
headers, no libc beyond `<string.h>`.
