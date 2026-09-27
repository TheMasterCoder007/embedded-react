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

ErDma2dBackendConfig cfg = {
    .dma2d         = (void*)0x52001000,      /* H7 (F4/F7: 0x4002B000) */
    .framebuffer   = fb0,
    .width         = 800,
    .height        = 480,
    .stride_pixels = 800,                    /* LTDC rows often pad to 64 bytes */
    .format        = ER_DMA2D_FB_RGB888,     /* ARGB8888, RGB888 or RGB565 */
    .start         = my_dma2d_start,         /* optional: host-owned DMA2D IRQ; NULL = poll TCIF */
    .wait_complete = my_dma2d_wait,
    .cache_clean   = NULL,                   /* SCB_CleanDCache_by_Addr etc. when the fb is cacheable */
    .dead_time     = 100,                    /* AMTCR cycles so blits don't starve the LTDC */
};
er_dma2d_backend_init(&cfg);
```

Per frame: `er_commit()`, then `er_dma2d_backend_take_dirty(&x, &y, &w, &h)` to know whether anything
was painted, `er_dma2d_backend_wait()` before a flip, `er_dma2d_backend_set_framebuffer()` to point at
the next buffer, and `er_display_present()` after each flip on a page-flipped panel.

## Build

```cmake
add_subdirectory(path/to/embedded-react/engine   ${CMAKE_BINARY_DIR}/embedded-react)
add_subdirectory(path/to/embedded-react/backends/dma2d ${CMAKE_BINARY_DIR}/backends/dma2d)
target_link_libraries(my_firmware PRIVATE er-backend-dma2d)
```

`er-backend-dma2d` links the engine and exports this directory's include path. Plain C99, no vendor
headers, no libc beyond `<string.h>`.
