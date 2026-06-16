# engine

The pure C99 runtime that does everything visible on screen: scene graph, layout,
rendering, text, animation, fonts. Runtime-agnostic by design — `er_scene.h` is the
public ABI any frontend (React-on-QuickJS, AOT-compiled React, future Lua / JSON / visual
editor) calls into.

Contributors working on the engine itself: this is your README. End users writing React
apps don't need to know about the layout here.

## Layout

| Folder | What lives here |
|---|---|
| `include/` | Public headers — `er_scene.h` (scene API) and `native_renderer.h` (backend interface). The only headers downstream code is allowed to include directly. |
| `core/` | Backend glue, frame tick, time advance. Everything that connects the engine to the hardware-blitting backend. |
| `scene/` | Node pool, parent/child/sibling tree, props, dirty tracking, render pass orchestration, hit-testing. |
| `layout/` | Yoga-compatible 7-pass flexbox. |
| `rendering/` | Painters for the renderable primitives — rounded rectangles, shadows, transforms, image scaling, canvas. |
| `text/` | UTF-8 decoder, glyph rasterizer, multi-line layout. |
| `animation/` | `Animated.Value` engine, timing/spring/decay curves, native driver. |
| `resources/` | Font registry, font blob loader, font bitmaps, built-in font data. Future home for image assets. |
| `platform/` | Platform-abstraction hooks the engine needs (time source, optional memory abstractions). Empty today. |
| `tests/` | Host-side CTest suites — layout, text, rendering, animation, input, scroll, resources. |

## Building

The engine is a CMake STATIC library named `embedded-react`. Configure it from this
folder (it pulls in nothing else):

```
cmake -S engine -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

A new board needs only a C99 compiler, `<math.h>`, and a writable framebuffer — no RTOS,
no MCU SDK. The engine never includes a platform header; it paints through one backend
struct of function pointers (see [`backends/README.md`](../backends/README.md)).

## Internals

### Layout — Yoga 7-pass flexbox

`layout/layout_engine.c` implements a Yoga-compatible flexbox solve per container:
collect in-flow children with hypothetical sizes → wrap into lines → resolve
`flexGrow`/`flexShrink` against free space (iterative, like Yoga's resolve-flexible-lengths
loop, so min/max-frozen children redistribute) → compute per-line cross size → place along
main axis (`justifyContent`) and cross axis (`alignSelf`/`alignItems`) → write back and
recurse → lay out absolutely-positioned children against the padding box. Scratch arrays
are static at module scope, sized to `ERUI_MAX_NODES`.

### Pixel format — premultiplied ARGB8888

All bitmap data — buffers passed to `copy_rect` / `blend_rect`, internal offscreen
buffers, and images from `er_image_load` — is **premultiplied ARGB8888** (memory order
A, R, G, B; word `0xAARRGGBB` with R, G, B already multiplied by A/255). The one
exception is `fill_rect`'s `argb`, which is **straight-alpha** `0xAARRGGBB` (CSS-friendly)
— the engine premultiplies it once at call time. Backends convert to their display's
native format (RGB565, BGR888, …) inside the callback. Blend, per channel:

```
out.C = src.C * a + dst.C * (1 - sA * a)      // src.C already premultiplied
```

### Scratch buffer pool (no heap during rendering)

A subtree with `opacity < 1`, a transform, or a shadow blur is composited into an
offscreen premultiplied-ARGB8888 buffer first. The pool is a single static array sliced
into equal slots at init — `ERUI_SCRATCH_W × ERUI_SCRATCH_H × ERUI_SCRATCH_POOL_DEPTH × 4`
bytes — so no allocation happens in a render pass. At desktop defaults these slots are
framebuffer-sized and dominate static RAM; size them down per board.

### Banded rendering (low-RAM panels)

A backend can opt into banded RGB565 (`ER_LCD_BANDED`): it sets a band height and
`band_begin`/`band_flush` callbacks, and the engine renders dirty rows as full-width
strips through a small RGB565 band buffer (~19 KB) while panel GRAM retains the rest —
16-bit color at less RAM than a full framebuffer. Band tiling is applied at backend-emit,
not as a clip, so transform/opacity scratch sources don't truncate at the seam.

## Compile-time feature flags

Set these in CMake before `FetchContent_MakeAvailable` (or at the ESP-IDF component
level). The defaults are desktop-sized — tune them down for a board.

| Flag | Default | Effect |
|---|---|---|
| `ERUI_SHADOWS` | 0 | Box-shadow rasteriser (two-pass box blur) |
| `ERUI_BORDER_AA` | 1 | Anti-aliased border-radius edges |
| `ERUI_3D_TRANSFORMS` | 0 | `rotateX` / `rotateY` / `perspective` |
| `ERUI_BILINEAR_SCALE` | 0 | Bilinear image scaling (vs. nearest-neighbour) |
| `ERUI_GRADIENT` | 1 | Linear gradient rasteriser |
| `ERUI_GRADIENT_RADIAL` | 1 | Radial gradients (requires `ERUI_GRADIENT`) |
| `ERUI_TRANSFORMS` | FULL | `TRANSLATE_ONLY` strips rasterisation paths |
| `ERUI_FONT_SIZES` | 7 | Number of pre-rasterised font sizes |
| `ERUI_MAX_NODES` | 512 | Scene-graph node pool size |
| `ERUI_MAX_OPACITY_DEPTH` | 4 | Max nested offscreen-composite layers |
| `ERUI_SCRATCH_W` | *(fb width)* | Width of one scratch slot |
| `ERUI_SCRATCH_H` | *(fb height)* | Height of one scratch slot |
| `ERUI_SCRATCH_POOL_DEPTH` | `ERUI_MAX_OPACITY_DEPTH + 2` | Number of live scratch slots |
| `ERUI_FONT_POOL_BYTES` | 0 | Static pool for runtime-loaded fonts; 0 disables `er_font_load` |

## Rules

- **No platform headers.** Pure C99. No `stm32h7xx_hal.h`, no `esp_lcd.h`, no
  `<windows.h>`. Hardware specifics live in `backends/`.
- **No React assumptions.** The engine does not import React. Bindings to React (or
  Lua, JSON, visual editors, anything else) live in `bridges/`.
- **No heap during rendering.** All scratch buffers are static, sized at compile time.
- **Section banners + JSDoc-style function docs** per [`CONTRIBUTING.md`](../CONTRIBUTING.md).
