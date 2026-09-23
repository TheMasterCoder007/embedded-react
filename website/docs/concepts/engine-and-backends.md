---
title: 'Engine and backends'
description: 'The C99 engine owns nodes, layout and rendering. A backend owns fill, copy, blend and present.'
---

Everything you see on a panel is drawn by one C99 library, `engine/`, and everything that touches
the hardware lives in a backend. The line between them is a struct of five function pointers.

## The engine

The engine is the runtime for everything visible: the scene graph, flexbox layout, rendering,
text, animation and fonts. It is built as a CMake static library named `embedded-react`, and it
has no dependencies beyond a C99 compiler and `<math.h>`. It never includes a platform header, an
RTOS header or an MCU SDK. That is what lets the same code run on an ESP32, an RP2040, an STM32,
a Linux desktop and in a browser through WebAssembly.

Its public API is `er_scene.h`: create nodes, set their props, arrange them in a tree, commit a
frame. Both [flows](./two-flows.md) end up calling it, and so could a future frontend in another
language; the engine has no opinion about who calls it.

What it owns:

| Area        | What lives there                                                                                                             |
| ----------- | ---------------------------------------------------------------------------------------------------------------------------- |
| Scene graph | A fixed pool of nodes (`ERUI_MAX_NODES`, 512 by default), the parent/child tree, props, dirty tracking, hit-testing          |
| Layout      | A Yoga-compatible flexbox solver. See [Layout](./layout.md)                                                                  |
| Rendering   | Rounded rectangles, borders, shadows, gradients, images, 2D and 3D transforms, opacity groups, vector shapes, the arc widget |
| Text        | A UTF-8 decoder, glyph rendering from pre-rasterised bitmap fonts, multi-line layout, inline spans                           |
| Animation   | The `Animated.Value` engine: timing, spring and decay curves, driven natively without per-frame JavaScript                   |
| Input       | Multitouch hit-testing that respects `zIndex`, the gesture responder system, scroll momentum                                 |
| Damage      | Which pixels changed this frame, so a backend repaints only those. See [Rendering pipeline](./rendering-pipeline.md)         |

Two design rules run through all of it:

- **No allocation while rendering.** Scratch buffers for opacity groups, transforms and shadows
  are statically allocated and sized by compile-time flags. A render pass never calls `malloc`, so
  it can never fail for lack of heap, and its memory cost is known before the firmware boots.
- **Everything is a compile-time flag.** Shadows, 3D transforms, bilinear scaling, gradients, the
  node pool, the scratch buffer sizes, the number of damage rectangles: each is an `ERUI_*` CMake
  option, with desktop-sized defaults that a board turns down. The RP2040 example runs the same
  engine as the 800×480 ESP32-S3, with smaller numbers. [Memory](../guides/memory.md) walks
  through sizing them.

## Pixels

The engine composes in **premultiplied ARGB8888**: four bytes per pixel, with the colour channels
already multiplied by alpha. Every buffer it hands a backend is in that format, and so are baked
images. The one exception is `fill_rect`'s colour, which is straight-alpha `0xAARRGGBB` because
that is what a style sheet writes; the engine premultiplies it once per call.

A backend converts to its panel's native format, usually RGB565, inside its callbacks. The engine
does not know or care what the panel wants.

## Backends

A backend is one rendering API or peripheral, not one board. Any STM32 with a Chrom-ART blitter
uses `dma2d`; any board driving an SPI panel from an RP2040 uses `pico-spi-lcd`. Where the engine
is portable, backends are deliberately not: they are where the SDK calls and the DMA descriptors
live.

| Backend                 | Hardware                                                                         | Status           |
| ----------------------- | -------------------------------------------------------------------------------- | ---------------- |
| `esp32-lcd`             | ESP32-S3 RGB-parallel panels, framebuffer in PSRAM                               | Runs on hardware |
| `esp32-spi-lcd`         | SPI panels on an ESP32 with no PSRAM: one internal-RAM framebuffer, banded flush | Runs on hardware |
| `pico-spi-lcd`          | SPI panels on the RP2040: one RGB565 framebuffer, dirty-rect flush               | Runs on hardware |
| `dma2d`                 | STM32 Chrom-ART hardware blitter                                                 | Runs on hardware |
| `sdl`                   | SDL2 window: the desktop host and the test target                                | Working          |
| `software` + `web`      | A CPU compositor and the WebAssembly present layer behind the browser simulator  | Working          |
| `framebuffer`, `opengl` | Linux `/dev/fb0`, OpenGL ES                                                      | Planned          |

### The interface

A backend fills in `EmbeddedRenderBackend` from `native_renderer.h`: five callbacks plus an
opaque context pointer.

```c
#include "native_renderer.h"

static void fill (uint32_t argb, int x, int y, int w, int h, void *ctx) { /* solid rectangle */ }
static void copy (const void *src, int stride, int x, int y, int w, int h, void *ctx) { /* opaque pixels */ }
static void blend(const void *src, int stride, uint8_t alpha, int x, int y, int w, int h, void *ctx) { /* translucent pixels */ }
static void wait_fn(void *ctx) { /* block until the panel has taken the last frame; may be NULL */ }
static void on_frame(void *ctx) { /* a frame is ready; may be NULL */ }

void my_backend_init(void) {
    static const EmbeddedRenderBackend b = {fill, copy, blend, wait_fn, on_frame, NULL};
    embedded_renderer_set_backend(&b);
}
```

- `fill_rect` paints a solid rectangle.
- `copy_rect` writes opaque premultiplied pixels into the framebuffer.
- `blend_rect` composites translucent pixels at a global alpha. Anti-aliased edges, text and
  shadows all arrive through this call, so a backend that stubs it out paints hard edges and no
  text.
- `wait` blocks until the hardware has finished with the previous frame, for DMA-driven panels.
- `frame_ready` says a frame is complete and can be presented.

Three optional extensions cover hardware that the basic contract would waste:

- **Banded rendering.** A backend with no RAM for a full framebuffer sets `band_height` and
  provides `band_begin`/`band_flush`. The engine then renders each frame's damage as horizontal
  strips through a band buffer of that many rows (about 19 KB for a 240-wide RGB565 panel), and
  the panel's own memory retains the rest. This is how the no-PSRAM ESP32 drives a 240×320 panel
  in 16-bit colour.
- **Format-aware copy.** `copy_rect_fmt` receives an image that the engine has already proved
  fully opaque, in its baked format (ARGB8888 or RGB565), as one call for the whole rectangle. A
  DMA2D blitter turns that into a single transfer; an RGB565 framebuffer turns a 565 source into a
  row copy.
- **Multiple display buffers.** A page-flipping panel tells the engine how many buffers it
  rotates through, and the engine replays each frame's damage into every buffer so none is left
  stale.

[Writing a backend](../internals/writing-a-backend.md) covers the conventions in detail, including
the one that bites most often: dirty rectangles use an inclusive minimum corner and an exclusive
maximum corner, and a backend that mixes the two leaves a one-pixel column of stale panel.

## Who calls the engine

The engine does not run a loop of its own. The **host**, which is your firmware or one of the
example projects, owns the frame loop: it polls touch, advances the engine's clock, lets the
frontend commit, and presents. In Flow A that is a few lines around `er_runtime`, the portable
QuickJS host core; in Flow B it is a call to the generated `er_app_build()` at boot and
`er_commit()` each frame. The board examples are the reference for each.
