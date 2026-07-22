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

### Scratch buffers (no heap during rendering)

A subtree with `opacity < 1`, a transform, or a shadow blur is composited into an
offscreen premultiplied-ARGB8888 buffer first. Everything is statically allocated — no
allocation happens in a render pass. Three pools exist, each sized by its own constraint:

- **Opacity strips** — `ERUI_MAX_OPACITY_DEPTH` strips of
  `ERUI_SCRATCH_W × ERUI_SCRATCH_BAND_H × 4` bytes. A translucent group is composited
  through one strip; a group **larger than one strip is composited in multiple band
  passes** (the subtree is re-walked once per strip-sized tile, with off-tile subtrees
  pruned), so any node up to `ERUI_SCRATCH_W` wide fades correctly regardless of height.
  Small `ERUI_SCRATCH_BAND_H` = big RAM saving, more passes for tall fades.
- **Transform source** — one `ERUI_XFORM_W × ERUI_XFORM_H × 4` buffer (defaults to the
  `ERUI_SCRATCH_W/H` dims) holding the untransformed subtree while it is resampled. This
  is the one buffer that cannot be banded (rotation reads across the whole source), so
  `ERUI_XFORM_W/H` cap the largest rotatable/scalable node — decouple them when strips
  are screen-wide but transforms only ever hit small widgets. The transformed **output**
  is streamed out per row segment, so the destination AABB (which grows under
  rotation/scale-up) is unlimited.
- **Shadow plane** — `ERUI_SCRATCH_W × ERUI_SCRATCH_H` bytes of A8 coverage
  (`ERUI_SHADOWS` only).

When no opacity strip is available (nesting deeper than `ERUI_MAX_OPACITY_DEPTH`), the
group's opacity is multiplied into each primitive draw instead of being dropped — exact
wherever siblings don't overlap.

- **Fade cache** (optional) — one `ERUI_FADE_CACHE_W × ERUI_FADE_CACHE_H × 4` buffer
  holding the composited subtree of the most recent translucent group. During a pure
  opacity animation the subtree's content is identical every frame, so after one capture
  each frame is a single blend at the new alpha instead of a full re-render — roughly
  double the frame rate on fades of static content. Any content mutation anywhere in the
  scene invalidates it (coarse but O(1) and always safe). Off by default (`0`); size it to
  the largest node you animate opacity on (device boards typically place it in external
  RAM).

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
| `ERUI_MAX_OPACITY_DEPTH` | 4 | Max nested offscreen-composite layers (opacity strips) |
| `ERUI_SCRATCH_W` | 240 | Strip width / max transformable node width |
| `ERUI_SCRATCH_H` | 240 | Transform-source height (max transformable node height) |
| `ERUI_SCRATCH_BAND_H` | `ERUI_SCRATCH_H` | Opacity strip height; shrink to trade band passes for RAM |
| `ERUI_XFORM_W` | `ERUI_SCRATCH_W` | Transform-source width (max rotatable/scalable node width) |
| `ERUI_XFORM_H` | `ERUI_SCRATCH_H` | Transform-source height (max rotatable/scalable node height) |
| `ERUI_FADE_CACHE_W` | 0 | Fade-cache width (composited-subtree reuse across fade frames); 0 disables |
| `ERUI_FADE_CACHE_H` | 0 | Fade-cache height; 0 disables |
| `ERUI_FONT_POOL_BYTES` | 0 | Static pool for runtime-loaded fonts; 0 disables `er_font_load` |

### Vector pools (SVG / `<Svg>` rasteriser)

The vector rasteriser (`rendering/vector.c`) pre-allocates static buffers sized by the
macros below. Unlike the pixel scratch buffers above, these stay in **internal RAM** on
a PSRAM board (the scanline loops touch them per pixel), so they're sized to fit there —
raise them for bigger / more complex SVGs and watch the internal-RAM budget. They split
into transient rasterize scratch (reused per shape) and persistent per-node storage.

| Flag | Default | Bounds | Static cost |
|---|---|---|---|
| `ERUI_VECTOR_MAX_PTS` | 2048 | flattened vertices in one shape | `2 × PTS × 4` B |
| `ERUI_VECTOR_MAX_SUBPATHS` | 256 | contours / holes in one shape | `SUBPATHS × 12` B |
| `ERUI_VECTOR_MAX_EDGES` | 2048 | edges in one rasterise pass | `EDGES × 32` B (edge + crossing + active lists) |
| `ERUI_VECTOR_MAX_ROW` | 1024 | max vector-node **width** in px | `ROW × 4` B |
| `ERUI_MAX_VECTOR_NODES` | 8 | concurrent `<Svg>` nodes with geometry | `NODES × (TAPE_MAX×4 + PAINTS_MAX×20)` B |
| `ERUI_VECTOR_TAPE_MAX` | 1024 | op-tape floats stored per node | (in the per-node cost) |
| `ERUI_VECTOR_PAINTS_MAX` | 16 | paint entries (shapes) per node | (in the per-node cost) |
| `ERUI_VECTOR_GRAD_LUT` | 256 | gradient colour-LUT entries (`ERUI_GRADIENT` only) | `LUT × 4` B internal |

`ERUI_VECTOR_GRAD_LUT` sizes the per-gradient color ramp the rasteriser samples per pixel (built once per
gradient shape) instead of interpolating the stops each pixel — the bulk of an interactive gradient drag's
cost. 256 matches 8-bit color resolution; a RAM-tight board can lower it (e.g., 64–128) for coarser steps,
and there's little benefit above 256.

At the defaults that's ~122 KB. The fastest-growing terms are `MAX_EDGES` (~32 B each, across
three lists) and the **per-node op-tape**: persistent storage is `MAX_VECTOR_NODES ×
VECTOR_TAPE_MAX × 4` bytes, so "many nodes" and "large tape" multiply.

**Placement (PSRAM targets).** The vector code is two objects: `vector.c` (the **hot** per-pixel
rasterize scratch — edge/coverage/crossing lists) and `vector_store.c` (the **cold** per-node
op-tape/paint pool). The storage pool is read once per node when it re-rasterizes, not in the
scanline inner loop, so a target with far memory can place `vector_store.o`'s `.bss` there — e.g.,
ESP32 PSRAM via a linker fragment — while the hot scratch stays in fast internal RAM. With the
storage in PSRAM, **`ERUI_MAX_VECTOR_NODES` (and `ERUI_VECTOR_TAPE_MAX`) can be raised well past
the internal-RAM-bound default**. See `examples/esp32/esp32-s3` —
`components/engine/linker_psram.lf` maps `vector_store` to `extram_bss` and the component sets
`ERUI_MAX_VECTOR_NODES=32`.

**Overflow is silent truncation, not a crash** — an over-complex shape is clipped or dropped.
A debug build (or `-DERUI_VECTOR_DIAGNOSTICS=1`) prints a one-line `stderr` warning naming the
macro to raise on the first overflow of each pool; it is compiled out under `NDEBUG` so a
release MCU pulls in no `<stdio.h>`.

Override from CMake (`-DERUI_VECTOR_MAX_PTS=4096`), or in an ESP-IDF build from your project's
`CMakeLists.txt`:

```
idf_build_set_property(COMPILE_DEFINITIONS "ERUI_VECTOR_MAX_PTS=4096" APPEND)
```

## Rules

- **No platform headers.** Pure C99. No `stm32h7xx_hal.h`, no `esp_lcd.h`, no
  `<windows.h>`. Hardware specifics live in `backends/`.
- **No React assumptions.** The engine does not import React. Bindings to React (or
  Lua, JSON, visual editors, anything else) live in `bridges/`.
- **No heap during rendering.** All scratch buffers are static, sized at compile time.
- **Section banners + JSDoc-style function docs** per [`CONTRIBUTING.md`](../CONTRIBUTING.md).
