---
title: 'Engine internals'
description: 'How the engine is built inside: layout, scratch buffers, damage tracking, hidden subtrees, the arc widget, frame instrumentation, and every compile-time flag.'
---

This page is for people changing `engine/`, or sizing it for a board. It assumes the outside view
from [Engine and backends](../concepts/engine-and-backends.md) and the
[Rendering pipeline](../concepts/rendering-pipeline.md).

## Folders

| Folder       | What lives there                                                                                                                                                                   |
| ------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `include/`   | The public headers: `er_scene.h` (the scene API), `native_renderer.h` (the backend interface), `er_perf.h` and `perf_overlay.h` (instrumentation), `font_bitmap.h`, `er_version.h` |
| `core/`      | Backend glue, the frame tick, time advance, the multi-core fork/join                                                                                                               |
| `scene/`     | Node pool, the tree, props, dirty tracking, the damage set, render-pass orchestration, hit-testing                                                                                 |
| `layout/`    | The Yoga-compatible flexbox solver                                                                                                                                                 |
| `rendering/` | Rounded rectangles, shadows, transforms, gradients, image scaling, vectors, the arc rasteriser, the perf overlay                                                                   |
| `text/`      | UTF-8 decoding, glyph rasterisation, multi-line layout                                                                                                                             |
| `animation/` | `Animated.Value`, timing/spring/decay curves, the native driver                                                                                                                    |
| `font/`      | The font registry, the runtime font-blob loader and the built-in font data                                                                                                         |
| `tests/`     | The CTest suites. See [Testing](./testing.md)                                                                                                                                      |

```bash
cmake -S engine -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## Layout

`layout/layout_engine.c` is a Yoga-compatible solve per container: collect the in-flow children,
wrap them into lines, resolve `flexGrow`/`flexShrink` against the free space (iteratively, so
min/max-frozen children redistribute), place along both axes, recurse, then lay out absolutely
positioned children against the parent's padding box. Its scratch arrays are static, sized to
`ERUI_MAX_NODES`. [Layout](../concepts/layout.md) lists what it supports from the app's side.

## Scratch buffers

A subtree with `opacity < 1`, a transform, or a shadow is composited into a static offscreen
buffer first; a render pass never allocates.

- **Opacity strips**: `ERUI_MAX_OPACITY_DEPTH` strips of `ERUI_SCRATCH_W × ERUI_SCRATCH_BAND_H`
  pixels. A group taller than a strip is composited in band passes, so any node up to
  `ERUI_SCRATCH_W` wide fades correctly whatever its height. A smaller band height saves RAM and
  costs passes. Past the depth, a group's opacity is multiplied into each draw instead of dropped.
- **Transform source**: one `ERUI_XFORM_W × ERUI_XFORM_H` buffer holding the untransformed subtree.
  It cannot be banded (a rotation reads across the whole source), so it caps the largest node you
  can rotate or scale. Damage inside a transformed subtree is whole-node: a change to one child
  repaints the node's entire transformed box.
- **Shadow plane**: `ERUI_SCRATCH_W × ERUI_SCRATCH_H` bytes of coverage (`ERUI_SHADOWS` only).
- **Fade cache** (optional, `ERUI_FADE_CACHE_W/H`): keeps the last translucent group's composite so
  a pure opacity animation is one blend per frame instead of a re-render, roughly double the frame
  rate on fades of static content. Any content change invalidates it.

## Damage

Each commit tracks up to `ER_DAMAGE_RECTS_MAX` (default 16) disjoint dirty rectangles rather than
one box. Touching rectangles merge on insert; past the budget the least wasteful pair merges, so
coverage is never dropped, only coarsened. Hosts read them with `er_get_dirty_rects()` (one transfer
per region) or `er_get_dirty_rect()` (the covering box); both report the last commit that painted,
so a Flow A host, whose reconciler already committed inside the pump, still sees the frame's damage.

The budget matters on screens full of small independent updaters (a grid of dials): a vector or
arc node rasterises against the whole active clip, so a merged clip makes every dial redraw its full
ring. The cost is `ER_DAMAGE_RECTS_MAX × sizeof(ERRect)` per set, and the engine keeps
`2 + ER_DISPLAY_BUFFERS_MAX` sets (4 → 16 costs about 1.1 KB), plus one clipped render pass per
rectangle. Boards with few updaters set it to 4.

## Hidden subtrees

`display: 'none'` prunes a node and everything under it from layout, rendering and hit-testing while
the nodes stay allocated with their props, so a page can be built once and shown or hidden instead
of rebuilt. A `subtree_hidden` flag maintained by the tree and prop mutators lets the flat
per-commit passes skip hidden nodes in O(1); on hiding, each node's last painted rectangle is banked
as vacated damage and hidden nodes are swept clear of dirty flags at the end of every commit. A
hidden page costs nothing per frame and reports no damage, even while React keeps rendering into it.

## The arc widget

`ER_NODE_ARC` (`rendering/arc.c`, `scene/arc_widget.c`) draws a dial in closed form: a backing band,
the track, the value indicator, optional segment gaps and a knob, resolved per scanline to four
half-chords so only the one-pixel fringe at each radius costs a distance. The value is an animatable
property, so a ramp runs on the native driver, and a change damages only the swept sub-arc plus the
knob's old and new footprints. Hit-testing is ring-only: the hole and the unswept gap fall through.
With `arc_adjustable` the node owns the drag, claiming the gesture ahead of any ScrollView and
quantising to `arc_step`. In range mode (`arc_range`) the indicator spans two values with a knob at
each end; a drag latches the nearer end, and `arc_min_span` keeps the two apart, carrying the far
end along when it is above 0.

A knob wider than the ring paints past the box and is folded into damage and hit-testing, except on
a rotated or scaled arc, which renders through the transform scratch at exactly `w × h`; size the box
to include the knob if you need to transform such a dial.

`<Svg>` arcs share this core: a shape that is exactly an arc or a circle (the tape both flows emit for
`<Arc>` and `<Circle>`) is routed to the same rasterizer rather than tessellated, so it is
pixel-identical to a native arc at a fraction of the cost. Half-chords are cached per radius
(`ERUI_ARC_SPAN_CACHE` entries of `ERUI_ARC_MAX_RADIUS` rows, about 4 KB; a larger radius computes
its chords directly); the shared cache makes arc nodes single-core, like vector nodes.

## Frame instrumentation

`er_perf.h` splits each frame into four phases and keeps the **worst frame seen with its whole
split** until `er_perf_reset()`, so a one-off spike can be attributed after the fact.

| Phase                   | Marked by | Covers                                                            |
| ----------------------- | --------- | ----------------------------------------------------------------- |
| `ER_PERF_PHASE_JS`      | host      | The JS pump and React's commit, net of any `er_commit()` it drove |
| `ER_PERF_PHASE_LAYOUT`  | engine    | The flex solve and text measurement inside `er_commit()`          |
| `ER_PERF_PHASE_RASTER`  | engine    | The rest of `er_commit()`: damage pre-pass, composite, blits      |
| `ER_PERF_PHASE_PRESENT` | host      | Backend flush and panel transfer                                  |

Anything else lands in `other_us`, so the split always reconstructs `frame_us`. Per frame the engine
also samples the repainted area, `blit_px` (pixels handed to the backend; read against `dirty_px`
for write amplification), and the vector and image pool usage.

RASTER splits further (`ERPerfFrame.raster_us`): `PREPASS` (the node-pool walk, scales with
`ERUI_MAX_NODES`, runs even when nothing changed), `RENDER` (`C` on the overlay: compositing, scales
with damage area),
`BLIT` (the backend callbacks, write bandwidth) and `SWEEP` (the post-paint flag sweep). JS splits
too (`ERPerfFrame.js_us`, marked by the QuickJS bridge): `DISPATCH` (touches, timers, microtasks and
the handlers they run), `RECONCILE` (React's render), `MARSHAL` (the `NativeUI` calls per changed
node) and `COMMIT` (the `er_commit()` the batch closes with, which `frame_end` subtracts from the JS
phase since LAYOUT and RASTER already report it). Flow B marks none of these and they read 0.

The engine has no clock; hand it one, and mark the host's two phases yourself:

```c
er_perf_set_clock(now_us);                 /* once, at startup */

er_perf_frame_begin();
er_perf_phase_begin(ER_PERF_PHASE_JS);
er_runtime_pump();
er_perf_phase_end(ER_PERF_PHASE_JS);
er_commit();                               /* times LAYOUT + RASTER itself */
er_perf_phase_begin(ER_PERF_PHASE_PRESENT);
er_display_present();
er_perf_phase_end(ER_PERF_PHASE_PRESENT);
er_perf_frame_end();
```

`er_perf_overlay_lines()` formats it for `er_perf_overlay_draw()`:

```text
FRM 18.4 PK 2013.1               last frame / worst frame, ms
J6.2 L0.3 R9.1 P2.4              last frame: JS, layout, raster, present
PK J1900 L12 R80 P9              the WORST frame's split
PKDRT 800x40 32k                 the WORST frame's repainted region
VEC 3/8 IMG 5/32                 slots in use, out of the pool size
RST P0.4 C7.2 B22.1 S0.9 W96k    last frame's raster split + backend pixels
PKR P2 C11 B16 S1 W96k           the WORST frame's raster split
JSS D2.1 R7.4 M3.8 C9.0          last frame's JS split
PKJ D3 R40 M12 C31               the WORST frame's JS split
```

For "what did that interaction just cost?", latch the last frame with `dirty_px > 0` from
`er_perf_get_last()` into your own overlay line; most frames repaint nothing. Instrumentation is
gated by the C define `ER_PERF_STATS`, which defaults to `ER_PERF_OVERLAY` (itself 0 unless defined);
set it explicitly, or configure the engine with `-DERUI_PERF_STATS=ON`, to collect without drawing.
Set these on the engine target, not only on the host: `perf_stats.c` and `perf_overlay.c` otherwise
compile to stubs and the overlay stays blank.
[Performance](../guides/performance.md#measuring-on-the-device) shows how to read the overlay.

## Compile-time flags

Set these in CMake before `FetchContent_MakeAvailable`, or with
`idf_build_set_property(COMPILE_DEFINITIONS "ERUI_MAX_NODES=256" APPEND)` in an ESP-IDF project.
The pool and buffer sizes default desktop-sized; the optional features default off.
[Memory](../guides/memory.md) walks through sizing them for a board.

| Flag                                 | Default          | Effect                                                                                                                                                                                                                                         |
| ------------------------------------ | ---------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `ERUI_MAX_NODES`                     | 512              | Scene-graph node pool                                                                                                                                                                                                                          |
| `ERUI_SCRATCH_W`, `_H`               | 240, 240         | Strip width and transform-source height: the largest node that can fade, rotate or scale                                                                                                                                                       |
| `ERUI_SCRATCH_BAND_H`                | `ERUI_SCRATCH_H` | Opacity strip height; shrink to trade band passes for RAM                                                                                                                                                                                      |
| `ERUI_XFORM_W`, `_H`                 | scratch size     | Transform-source size, when it should differ from the strips                                                                                                                                                                                   |
| `ERUI_MAX_OPACITY_DEPTH`             | 4                | Nested opacity strips                                                                                                                                                                                                                          |
| `ERUI_FADE_CACHE_W`, `_H`            | 0                | The fade cache; 0 disables                                                                                                                                                                                                                     |
| `ER_DAMAGE_RECTS_MAX`                | 16               | Disjoint dirty rectangles per commit                                                                                                                                                                                                           |
| `ERUI_IMAGE_REGISTRY_MAX`            | 128              | Registered images, about 80 bytes each. Past it an image is refused and does not draw, so keep it at or above the asset count                                                                                                                  |
| `ERUI_FONT_POOL_BYTES`               | 0                | Pool for fonts loaded at runtime; 0 disables `er_font_load`                                                                                                                                                                                    |
| `ERUI_SHADOWS`                       | 0                | Box shadows                                                                                                                                                                                                                                    |
| `ERUI_3D_TRANSFORMS`                 | 0                | `rotateX`/`rotateY`/`perspective`; needs `ERUI_TRANSFORMS=FULL`                                                                                                                                                                                |
| `ERUI_TRANSFORMS`                    | `TRANSLATE_ONLY` | `FULL` adds scale and rotate (the resampling paths); translate-only builds leave them out                                                                                                                                                      |
| `ERUI_GRADIENT`, `_RADIAL`, `_CONIC` | 0, 0, 0          | Linear gradients; radial and conic (vector) gradients on top of `ERUI_GRADIENT`                                                                                                                                                                |
| `ERUI_BILINEAR_SCALE`                | 0                | Bilinear image scaling (versus nearest-neighbour)                                                                                                                                                                                              |
| `ERUI_BORDER_AA`                     | 1                | Anti-aliased border-radius edges                                                                                                                                                                                                               |
| `ERUI_OCCLUSION_CULLING`             | 1                | Skip layers a fully opaque node covers                                                                                                                                                                                                         |
| `ERUI_PERF_STATS`                    | `OFF`            | CMake option: `ON` compiles in the frame instrumentation (`ER_PERF_STATS=1`) without the overlay. Otherwise `ER_PERF_STATS` follows `ER_PERF_OVERLAY`                                                                                          |
| `ERUI_ONSCREEN_KEYBOARD`             | 0                | The built-in on-screen keyboard for `TextInput`, for touch-only devices                                                                                                                                                                        |
| `ERUI_MAX_ANIM_VALUES`               | 16               | Standalone `Animated.Value`s on the native driver                                                                                                                                                                                              |
| `ERUI_RENDER_WORKERS`                | 1                | Render workers for multi-core rendering: the repaint region is sliced per core. The opacity strips are split between workers and each extra worker costs a transform-source buffer; scenes with vector, arc or shadow nodes render single-core |
| `ERUI_DIAGNOSTICS`                   | 1 or 2           | One-shot developer warnings: `0` none (no `<stdio.h>`), `1` only failures that leave no trace on the panel, `2` everything. Defaults to 1 under `NDEBUG`                                                                                       |

### Vector pools

The vector rasterizer's buffers stay in **internal RAM** on a PSRAM board (the scanline loops touch
them per pixel), so they are sized to fit there: about 100 KB of scanline scratch at the defaults,
plus about 36 KB of per-node storage and 170 KB of edge cache.

| Flag                       | Default | Bounds                                                           |
| -------------------------- | ------- | ---------------------------------------------------------------- |
| `ERUI_MAX_VECTOR_NODES`    | 8       | `<Svg>` nodes with geometry at once                              |
| `ERUI_VECTOR_PAINTS_MAX`   | 16      | Shapes per `<Svg>`                                               |
| `ERUI_VECTOR_TAPE_MAX`     | 1024    | Op-tape floats stored per node                                   |
| `ERUI_VECTOR_MAX_PTS`      | 2048    | Flattened vertices in one shape                                  |
| `ERUI_VECTOR_MAX_EDGES`    | 2048    | Edges in one rasterise pass (about 32 bytes each)                |
| `ERUI_VECTOR_MAX_SUBPATHS` | 256     | Contours or holes in one shape                                   |
| `ERUI_VECTOR_MAX_ROW`      | 1024    | The widest vector node, in pixels                                |
| `ERUI_VECTOR_GRAD_LUT`     | 256     | Gradient colour-ramp entries; 64 to 128 is fine on a tight board |
| `ERUI_VECTOR_EDGE_CACHE`   | 1       | The edge cache; 0 compiles it out                                |
| `ERUI_VECTOR_CACHE_NODES`  | 2       | Nodes with cached geometry at once                               |
| `ERUI_VECTOR_CACHE_EDGES`  | 4096    | Cached edges per node, summed over its passes                    |

The **edge cache** keeps a static `<Svg>`'s built geometry so repainting it unchanged (a moving
sibling's damage crossing it, or a multi-buffer replay) skips the tape parse, flattening and stroke
outlining. It only records a tape that survived unchanged between renders, so an animated dial never
pays for it; a node whose geometry does not fit (a gauge face with 60 round-capped ticks is about
3,000 edges) keeps rendering uncached, and a debug build says which knob to raise.

On a PSRAM target, the per-node storage (`vector_store.c`) and the edge cache (`vector_cache.c`) can
live in external RAM, since only the hot scratch in `vector.c` is touched per pixel; the ESP32-S3
example's linker fragment does this and raises `ERUI_MAX_VECTOR_NODES` to 32.

**Overflow is silent truncation**: an over-complex shape is clipped or dropped, and a debug build
warns once naming the macro. Running out of `ERUI_MAX_VECTOR_NODES` is worse, since a whole node
draws nothing and which one depends on mount order, so that refusal warns in release builds too and
the perf overlay shows `!FULL` on its `VEC` field.

## Rules

- **No platform headers.** Pure C99. Hardware specifics live in `backends/`.
- **No React assumptions.** Bindings to React, or anything else, live in `bridges/`.
- **No heap during rendering.** Every scratch buffer is static, sized at compile time.
- **Section banners and documented functions**, per [Contributing](./contributing.md).
