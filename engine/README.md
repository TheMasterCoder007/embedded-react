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
loop, so min/max-frozen children redistribute) → compute per-line cross-size → place along
the main axis (`justifyContent`) and cross axis (`alignSelf`/`alignItems`) → write back and
recurse → lay out absolutely positioned children against the parent's padding box. An
absolute axis is pinned by an explicit length, a percentage, or a pair of opposing insets;
`aspectRatio` then derives the other axis from it, but only when exactly one of the two is
pinned (with both auto, or both already pinned, it does nothing — as in Yoga). Any axis still
unresolved sizes to the node's own content, like a flow child. `left`/`top`/`right`/`bottom`
take a percentage as well as a length — of the containing block's width on the horizontal
edges and its height on the vertical ones, or of the parent's content box for a node still in
flow. Scratch arrays are static at module scope, sized to `ERUI_MAX_NODES`.

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
  rotation/scale-up) is unlimited. Damage inside such a subtree is **whole-node**: the
  contents are captured in the node's own untransformed space, and only the blit puts them on
  screen, so a change to one child repaints the node's entire transformed AABB rather than a
  rect around the child. Keep transformed subtrees small if they update often.
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

### Disjoint dirty rects (damage tracking)

Each commit's damage is tracked as up to `ER_DAMAGE_RECTS_MAX` (default 16, an `#ifndef` override
in `er_scene.h` like `ER_DISPLAY_BUFFERS_MAX`) **pairwise-disjoint rects** rather than one
bounding box, so a widget updating top-left and another bottom-right
repaint two small areas — not the span between them. Overlapping or touching damage merges on
insert (disjointness is what makes multiple clipped render passes safe: no pixel composites
twice); when the budget is exceeded, the least-wasteful pair merges, degrading gracefully toward
the old single-box behavior without ever dropping coverage. The multi-buffer page-flip debt
(`er_set_display_buffer_count`) replays disjoint history the same way. Hosts read the rects with
`er_get_dirty_rects()` — one transfer window per region on capable display drivers — while
`er_get_dirty_rect()` still returns the covering box. Both report the last commit that *painted*: a
commit finding nothing dirty leaves the previous answer in place, so a Flow A host — where React
already committed inside `er_runtime_pump()` and the host's own `er_commit()` is the no-op one —
reads the frame's real damage rather than an empty set.

The budget matters most on **screens full of small independent updaters** — a grid of dials, a row
of meters. Saturation there does more than coarsen the reported rect: a vector or arc node
rasterizes against the *active clip*, because the background under it was erased across that whole
clip and must be repainted, so a merged clip makes every dial re-rasterize its full ring.

The trade is memory — `ER_DAMAGE_RECTS_MAX * sizeof(ERRect)` per set, and the engine keeps
`2 + ER_DISPLAY_BUFFERS_MAX` of them, so 4 → 16 costs **1,152 bytes of .bss** — plus one clipped
render pass per rect. Passes are cheap because each prunes to its own rect; the exception is while a
layout animation is running, where the cached subtree bounds are stale and pruning is off, so the
compositor first merges the set back down to a handful of coarser rects for that frame.

Boards that are tight on RAM and have only a few updaters should turn it back down — the CYD and
RP2040 examples set `ER_DAMAGE_RECTS_MAX=4` for exactly that reason.

### Hidden subtrees (`display: none`)

A node with `ERProps.display = ER_DISPLAY_NONE` and everything under it is pruned from the layout
solver, the render walk, and hit-testing, while its nodes stay allocated with their props intact —
so an app can build a page once and flip it on and off instead of destroying and recreating its
nodes. Hiding collapses the node's computed rect to zero, so it takes no space and `onLayout` reports 
an empty box.

The bookkeeping is what makes it cheap and correct. `ERNode::subtree_hidden` — maintained by the
tree and prop mutators, never by a per-frame walk — lets the flat per-commit passes, which have no
top-down parent context, skip a hidden node in O(1). On the transition itself, the engine banks each
node's last painted rect as vacated damage (the same channel node removal uses) and drops the stale
trail, because layout stops maintaining a hidden node's descendants: they would otherwise read as
unchanged-and-in-place, and any pixels they painted outside their parent's box would stay on screen.
Showing marks the subtree dirty so it repaints. Hidden nodes are also swept clear of dirty flags at
the end of each commit — they can never reach the paint that would clear them, and a stuck flag is a
rect re-damaged on every commit forever, which matters because React keeps rendering into a cached
page while it is off screen. The result is that a hidden page costs nothing per frame and reports no
dirty rect.

### Arc widget (`ER_NODE_ARC`)

Dials, gauges, and progress rings used to be `<Svg>` arcs: every value change re-uploaded an op-tape
and re-tessellated a stroked arc (flatten → stroke outline → scanline coverage). `ER_NODE_ARC` draws
the same thing in closed form (`rendering/arc.c` + `scene/arc_widget.c`): one node is an optional
wide **backing band**, the **track** over the sweep, the **value indicator** over `[start, value]`,
optional **segment gaps**, and a **knob** at the value end. Per scanline the ring resolves to four
half-chords (outer and inner radius, each ± half a pixel) — the rounded-rect row-span idea (`rrect.c`,
`er_rrect_fill_ring` being the 360° case) applied to a circle — so only the one-pixel fringe at each
radius costs a distance, the sweep is two half-plane cross-products per pixel (whose magnitudes *are*
the antialiasing distances), and a conic paint is the pixel's angle indexed into a color LUT.

The value is an animatable property (`ER_PROP_ARC_VALUE`) so a ramp runs on the native driver with
zero host involvement, and a value change damages only the **swept sub-arc plus the knob's old and new
footprints** (the node's `vec_dirty` sub-rect, the same channel the vector diff uses) — not the node
box. A knob wider than the ring paints past the box; its overhang is folded into the paint bounds,
the damage, the last-paint trail, and the hit zone. One exception: a **transformed** arc (rotated or
scaled — a plain translate is fine) renders through the transform scratch, which is captured at exactly
the node's `w × h`, so an overhanging knob is clipped there and its transformed damage bounds omit the
overhang. Size the box to include the knob if you need to transform such a dial. Hit-testing is ring-only (plus slop and the knob):
the hole and the unswept gap fall through to whatever is behind. With `arc_adjustable` the node owns
the drag natively — it claims the gesture responder on touch-down ahead of any ScrollView, quantizes
to `arc_step`, pins to the nearer end in the gap without wrapping, and fires `ER_EVENT_VALUE_CHANGE`
(`EREventData::value`) only when the quantized value moves. `ER_ARC_KNOB_CHILD` positions the node's
first child on the value point after each layout pass (multi-knob dials, arbitrary knob content).

**Range mode.** `arc_range` makes the indicator span `[arc_value_start, arc_value]` with a knob at each
end — a dual-setpoint dial (a thermostat's AUTO band). A drag latches whichever end it started nearest and
holds it for the whole gesture, so pushing one setpoint past the other never hands the finger to its
neighbour; `ER_EVENT_VALUE_CHANGE` carries both ends. `arc_min_span` keeps the two a minimum distance
apart: at 0 they may meet and a drag stops at its neighbour, above 0 the far end is carried along and the
pair travels together until it reaches the range bound. A conic indicator ramp follows the BAND on a range arc (so it always
covers exactly what is lit, however wide that is) and the whole SWEEP on a single-ended one. (So a color
belongs to a position on the dial and does not shift as the value grows.) A band-anchored ramp re-anchors
whenever either end moves, so such a band damages its whole span rather than just the swept sliver — the
one case where the tight damage would leave stale pixels. A conic indicator paint is anchored to the
BAND in range mode (so it always ramps across exactly what is lit) and to the full sweep otherwise. (So a
progress ring's colors belong to positions on the dial and don't shift as the value grows.)

**`<Svg>` arcs share this core.** `ER_VOP_ARC` is not tessellated when the shape is *just* an arc: the
rasterizer matches the tape both flows emit for `<Arc>` (a bare `ARC`) and `<Circle>` (`MOVE, ARC 0..2π,
CLOSE`) and routes it to `er_arc_fill_sector`, so an `<Svg>` arc and a native arc node are pixel-identical
(`tests/rendering/test_arc.c` asserts exactly that) at a fraction of the cost. Only shapes that map
EXACTLY are routed — a filled partial arc closes on a chord rather than a sector, a square cap and a
gradient paint have no sector equivalent, and an arc joined to another segment has joins — so everything
else keeps the general path and nothing renders differently than it describes. `er_vector_analytic_arc_count()`
reports which route shapes took; `ERUI_VECTOR_ANALYTIC_ARC=0` restores the all-tessellated behavior.

The half-chords are cached per radius and row phase across nodes and frames (`ERUI_ARC_SPAN_CACHE`
entries of `ERUI_ARC_MAX_RADIUS` rows, ~2 B per px of radius — ~4 KB at the defaults); a radius past
the cap just falls back to a per-row `sqrt`. The shared cache makes arc nodes parallel-unsafe, like
vector nodes.

### Banded rendering (low-RAM panels)

A backend can opt into banded RGB565 (`ER_LCD_BANDED`): it sets a band height and
`band_begin`/`band_flush` callbacks, and the engine renders dirty rows as full-width
strips through a small RGB565 band buffer (~19 KB) while panel GRAM retains the rest —
16-bit color at less RAM than a full framebuffer. Band tiling is applied at backend-emit,
not as a clip, so transform/opacity scratch sources don't truncate at the seam.

### Frame instrumentation

`er_perf.h` splits each frame into the four phases that can independently blow up, and
samples the fixed-size pools alongside them — so an occasional 2-second frame can be
attributed instead of guessed at. An FPS counter can't do this: the average stays fine
and the spike is gone before anyone looks, which is why the **worst frame seen so far is
retained with its whole split** (`er_perf_get_worst`) until you `er_perf_reset()`.

| Phase | Marked by | Covers |
|---|---|---|
| `ER_PERF_PHASE_JS` | host | JS pump + React's commit into the scene graph |
| `ER_PERF_PHASE_LAYOUT` | engine | The flex solve + text measurement inside `er_commit()` |
| `ER_PERF_PHASE_RASTER` | engine | The rest of `er_commit()` — damage pre-pass, composite, blits |
| `ER_PERF_PHASE_PRESENT` | host | Backend flush / panel transfer |

Whatever the four don't cover (input polling, the animation tick, host work) lands in
`other_us`, so the split always reconstructs `frame_us`. Counters sampled per frame:
the repainted region and its area (what raster *and* present both scale with), vector
storage slots in use out of `ERUI_MAX_VECTOR_NODES`, and image registry slots out of
`ERUI_IMAGE_REGISTRY_MAX` — a screen silently missing an asset reads as a full pool here. The vector
field gains a `!FULL` marker once a node has actually been turned away (see the vector section).

RASTER gets one more level of detail (`ERPerfFrame.raster_us`, indexed by `ERPerfRasterSub`),
because its jobs scale with different things and "raster is slow" alone doesn't say which one to fix:

| Sub-step | Scales with | Covers |
|---|---|---|
| `ER_PERF_RASTER_PREPASS` | `ERUI_MAX_NODES` | The damage pre-pass node-pool walk + multi-buffer debt fold/replay — runs even when nothing changed |
| `ER_PERF_RASTER_RENDER` | damage area | The composite passes: tree walk, software raster, scratch composites (net of the blits inside them) |
| `ER_PERF_RASTER_BLIT` | write bandwidth | The backend fill/copy/blend callbacks (the framebuffer writes) + banded `band_begin`/`band_flush` |
| `ER_PERF_RASTER_SWEEP` | `ERUI_MAX_NODES` | The post-paint dirty-flag sweep + per-worker dirty-rect merge |

The buckets are disjoint and sum to at most `phase_us[ER_PERF_PHASE_RASTER]`. Alongside them,
`blit_px` counts the pixels actually handed to the backend (each call's post-clip `w*h`): read it
against `dirty_px` for the frame's write amplification — overlapping layers, erase-then-repaint,
and multi-buffer debt replay all push it above the damage area.

The engine has no clock of its own, so timing is opt-in: hand it one with
`er_perf_set_clock()` (without one the phase times read 0 and the counters still work).
The host owns the frame boundary and its own two phases:

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

`er_perf_overlay_lines()` formats the whole thing into short lines ready to pass to
`er_perf_overlay_draw()`, so a host gets the panel without writing any `snprintf`:

```
FRM 18.4 PK 2013.1     last frame / worst frame, ms
J6.2 L0.3 R9.1 P2.4    last frame: JS, layout, raster, present
PK J1900 L12 R80 P9    the WORST frame's split — what to blame the spike on
PKDRT 800x40 32k       the WORST frame's repainted region (pairs with PK above)
VEC 3/8 IMG 5/32       slots in use, out of the compiled-in pool size
RST P0.4 C7.2 B22.1 S0.9 W96k   last frame's raster split (pre-pass, composite, blit, sweep)
                       + backend pixels — the line to watch during a steady drag, where the
                       PK lines are stuck on the mount frame
PKR P2 C11 B16 S1 W96k the WORST frame's raster split + backend pixels (pairs with PK)
```

The same `ERPerfFrame.dirty_*` data supports three region policies, and the overlay only has room
for one — pick per host:

1. **Peak** — what `PKDRT` shows: the region that accompanied the worst frame, so it pairs with the
   `PK` split above it and answers "did anything ever go full-screen?". Retained until
   `er_perf_reset()`, like the split.
2. **Last frame** — `er_perf_get_last()` directly; nearly always 0 (most frames repaint nothing),
   so it is rarely useful on screen by itself.
3. **Last non-empty frame** — "what did that interaction just cost?", the everyday debugging
   question. Latch it host-side, a few lines per frame:

   ```c
   static ERPerfFrame s_last_paint; /* the most recent frame that actually repainted */
   ERPerfFrame f;
   if (er_perf_get_last(&f) && f.dirty_px > 0U)
       s_last_paint = f; /* format s_last_paint.dirty_* into your own overlay line */
   ```

Gated by `ER_PERF_STATS` (see the flag table below). If `ER_PERF_STATS` is not defined by the build,
it defaults to `ER_PERF_OVERLAY`, so turning the panel on turns the instrumentation on with it. Set it
explicitly to collect the numbers without drawing anything — to log them, or ship them over a debug
link. At 0 every entry point becomes a no-op and the compositor drops even the calls.

## Compile-time feature flags

Set these in CMake before `FetchContent_MakeAvailable` (or at the ESP-IDF component
level). The defaults are desktop-sized — tune them down for a board.

| Flag | Default | Effect |
|---|---|---|
| `ERUI_SHADOWS` | 0 | Box-shadow rasteriser (two-pass box blur) |
| `ERUI_BORDER_AA` | 1 | Anti-aliased border-radius edges |
| `ERUI_OCCLUSION_CULLING` | 1 | Skip layers a fully opaque node covers (off = draw everything) |
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
| `ERUI_IMAGE_REGISTRY_MAX` | 128 | Concurrently registered images. ~80 B/slot (~10 KB at the default), so shrink it on a RAM-tight board — but see below before shrinking it *below* your asset count |
| `ERUI_PERF_STATS` | 1 | Per-frame timing split + resource counters (`ERUI_PERF_STATS=OFF` compiles them out). Defaults to `ER_PERF_OVERLAY` on the ESP-IDF component path, which never sees this CMake option — see [Frame instrumentation](#frame-instrumentation) |
| `ERUI_RENDER_WORKERS` | 1 | Max render workers for multi-core rendering. Above 1, per-worker context/scratch arrays are sized for N workers and a host may install threads via `embedded_renderer_set_workers` (see `native_renderer.h`); the repaint region is then rendered as horizontal slices, one per core. The opacity strip pool is split between workers (`ERUI_MAX_OPACITY_DEPTH / workers` slots each — raise the depth alongside), and each extra worker costs a full transform-source buffer. Scenes with vector or shadow nodes automatically render single-core. 1 (the default) is the plain single-core engine |

### Image registry

`er_image_load()` puts each distinct name in one of `ERUI_IMAGE_REGISTRY_MAX` slots. Past
that, registration is **refused**, and a refused image simply never draws — no crash, no
layout change, just a hole where the art should be, on whichever assets happened to load
last. An icon-heavy app runs well past a hundred images, so the old fixed 32 was well
under a real asset set. The default is now 128; a diagnostics build warns once on the
first refusal (`ERUI_IMAGE_DIAGNOSTICS`, on unless `NDEBUG`), and the perf overlay's
`IMG n/n` counter reads full.

Each slot is ~80 B on a 32-bit target — the 64-byte name field is most of it — so the
default costs ~10 KB of `.bss`. Nothing scales with it per frame (lookups skip free slots
on a bool test), so the only reason to shrink it is RAM: the RP2040 and ESP32-2432S028R
examples pin it to 8 and 16. Set it **at or above the number of images your app bakes**;
the count is whatever `assets.config.js` emits.

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
| `ERUI_VECTOR_MAX_EDGES` | 2048 | edges in one rasterise pass | `EDGES × 34` B (edge + crossing + active + sort lists) |
| `ERUI_VECTOR_MAX_ROW` | 1024 | max vector-node **width** in px | `ROW × 8` B (coverage row + staged pixel row) |
| `ERUI_VECTOR_SORT_BUCKETS` | 256 | buckets in the edge sort | `BUCKETS × 2` B |
| `ERUI_MAX_VECTOR_NODES` | 8 | concurrent `<Svg>` nodes with geometry | `NODES × (TAPE_MAX×4 + PAINTS_MAX×20)` B |
| `ERUI_VECTOR_TAPE_MAX` | 1024 | op-tape floats stored per node | (in the per-node cost) |
| `ERUI_VECTOR_PAINTS_MAX` | 16 | paint entries (shapes) per node | (in the per-node cost) |
| `ERUI_VECTOR_GRAD_LUT` | 256 | gradient colour-LUT entries (`ERUI_GRADIENT` only) | `LUT × 4` B internal |
| `ERUI_VECTOR_EDGE_CACHE` | 1 | edge cache on/off (`vector_cache.c`; 0 compiles it out) | — |
| `ERUI_VECTOR_CACHE_NODES` | 2 | nodes with cached geometry at once (LRU) | `NODES × (CACHE_EDGES×20 + CACHE_PASSES×52)` B |
| `ERUI_VECTOR_CACHE_EDGES` | 4096 | cached edges per node (sum over its passes) | (in the per-cache-node cost) |
| `ERUI_VECTOR_CACHE_PASSES` | 48 | cached rasterise passes per node | (in the per-cache-node cost) |

`ERUI_VECTOR_SORT_BUCKETS` sizes the counting sort that orders edges for the active-edge table. One bucket
per clip row until the clip is taller than the table, then one bucket covers 2/4/… rows — which only means a
few edges activate a row or two early, so it is a memory knob and never a correctness one.

`ERUI_VECTOR_GRAD_LUT` sizes the per-gradient color ramp the rasteriser samples per pixel (built once per
gradient shape) instead of interpolating the stops each pixel — the bulk of an interactive gradient drag's
cost. 256 matches 8-bit color resolution; a RAM-tight board can lower it (e.g., 64–128) for coarser steps,
and there's little benefit above 256.

**The edge cache** (`ERUI_VECTOR_EDGE_CACHE`, pool in `rendering/vector_cache.c`) keeps a static
node's *built* rasterizer geometry — its flattened, stroke-outlined edge lists — so repainting an
unchanged `<Svg>` (a moving sibling's damage rect crossing it every frame, or the same damage
replayed into each buffer of a multi-buffer display) skips the tape parse, bezier/arc flattening, and
stroke outlining and goes straight to the scanline rasterize. It is keyed on the storage slot and the
node's screen origin, invalidated by any `er_vector_store`/`er_vector_free` on the slot, and only
records a tape that survived unchanged from one render to the next (so an animated dial, whose tape
updates every frame, never pays the recording cost). A node whose geometry does not fit the entry
(`ERUI_VECTOR_CACHE_EDGES` is the total across *all* the node's fill+stroke passes — a decorated
gauge face with 60 round-capped ticks measures ~3k) simply keeps rendering uncached; a debug build
prints a one-line warning naming the knobs to raise. Analytic-arc shapes replay through the arc core
(they cache parameters, not edges), and gradients re-resolve from the paint table at replay, so a
cached repaint is pixel-identical to a fresh one — the test suite asserts this
(`tests/rendering/test_vector_cache.c`).

The Arc widget (`rendering/arc.c`) has two more: `ERUI_ARC_SPAN_CACHE` (default 8) row-span cache entries
of `ERUI_ARC_MAX_RADIUS` (default 255) rows — `ENTRIES × (RADIUS + 2) × 2` B ≈ 4 KB, plus a 1 KB
premultiplied row chunk and a 512 B color LUT per render worker.

At the defaults that's ~122 KB, plus ~170 KB of edge cache (2 × ~85 KB; set
`ERUI_VECTOR_EDGE_CACHE=0` to drop it entirely on a RAM-tight board). The fastest-growing terms are
`MAX_EDGES` (~32 B each, across three lists) and the **per-node op-tape**: persistent storage is
`MAX_VECTOR_NODES × VECTOR_TAPE_MAX × 4` bytes, so "many nodes" and "large tape" multiply.

**Placement (PSRAM targets).** The vector code is three objects: `vector.c` (the **hot** per-pixel
rasterize scratch — edge/coverage/crossing lists), `vector_store.c` (the **cold** per-node
op-tape/paint pool) and `vector_cache.c` (the edge cache). The storage pool is read once per node
when it re-rasterizes, not in the scanline inner loop, so a target with far memory can place
`vector_store.o`'s `.bss` there — e.g., ESP32 PSRAM via a linker fragment — while the hot scratch
stays in fast internal RAM. With the storage in PSRAM, **`ERUI_MAX_VECTOR_NODES` (and
`ERUI_VECTOR_TAPE_MAX`) can be raised well past the internal-RAM-bound default**. The edge cache
sits between the two: a replay *does* read cached edges in the scanline crossing loop, but the
per-row active set is small and cache-fronted, and even a PSRAM-resident replay beats rebuilding
the geometry — put it wherever the RAM budget allows. See `examples/esp32/esp32-s3` —
`components/engine/linker_psram.lf` maps `vector_store` and `vector_cache` to `extram_bss` and the
component sets `ERUI_MAX_VECTOR_NODES=32`.

**Overflow is silent truncation, not a crash** — an over-complex shape is clipped or dropped.
A debug build (or `-DERUI_VECTOR_DIAGNOSTICS=1`) prints a one-line `stderr` warning naming the
macro to raise on the first overflow of each pool; it is compiled out under `NDEBUG` so a
release MCU pulls in no `<stdio.h>`.

**`ERUI_MAX_VECTOR_NODES` is the exception, and warns in release builds too.** The other caps
truncate one shape, so the screen shows something recognisably wrong, and the culprit is the shape
you were editing. Running out of *storage slots* instead denies a whole node its geometry — it
draws nothing — and since slots are handed out in mount order, *which* nodes go missing shifts as
screens mount and unmount. On a panel that reads as random glitching with no obvious cause. So the
first refusal prints one `stderr` line even under `NDEBUG`, and raises a sticky flag the perf
overlay shows as `!FULL` on its `VEC` field (`VEC 8/8!FULL`) — the counter alone can't carry this,
since a screen that exactly fills the pool renders perfectly well. Hosts can read the same flag
from `ERPerfFrame::vector_slots_overflow`. The flag clears on `er_reset()`; the warning is
one-shot per process. Set `-DERUI_VECTOR_STORE_WARN=0` on a target that must not link `<stdio.h>`
(the flag and the overlay marker keep working).

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
