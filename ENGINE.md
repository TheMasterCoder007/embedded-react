# Engine Feature Reference & Roadmap

A subsystem-by-subsystem map of what the C engine implements, with notes on how each
feature is wired. This began as a build checklist; every box is now checked, so it reads
as a feature inventory rather than a to-do list. Implementation notes on each item double
as a quick orientation guide to the renderer internals.

The forward-looking work that remains is collected in **[Future Work](#future-work)** at
the bottom — nothing in the checklists below is outstanding.

The engine surface that React (via `bridges/quickjs/`) targets is `engine/include/er_scene.h`
and `engine/include/native_renderer.h`. Anything React-facing must reduce to those calls.

Checkbox convention: `[x]` marks a shipped, tested feature. The few intentionally deferred
items are described in prose (not as unchecked boxes) and summarized under Future Work.

---

## 1. Input & Touch

Touch dispatch is handled in [hit_test.c](engine/scene/hit_test.c). Single-finger press,
long-press, press-in/out, touch start/move/end, cancel, bubbling, and zIndex-aware
hit-testing all work, along with the higher-level gesture/responder layer below.

- [x] Press / press-in / press-out / long-press
- [x] Raw touch start / move / end / cancel + bubbling through ancestors
- [x] Multitouch (up to five fingers)
- [x] zIndex-aware hit-testing
- [x] **Hit-test rejection for `display: none` and `opacity == 0` nodes** — currently any
  sized node is hittable regardless of visibility.
- [x] **Hit-test clipping to `overflow: hidden` ancestors** — point must lie inside every
  clipping ancestor, not just the leaf rect.
- [x] **Hit-test through 2D transforms** — once `transform.c` lands, hit-testing must
  apply the inverse transform to the query point at each transformed ancestor.
- [x] **Gesture/PanResponder support** — slop threshold, capture phase, `onMoveShouldSetResponder`,
  `onStartShouldSetResponder` equivalents, gesture cancel when scroll grabs the
  responder.
- [x] **ScrollView gesture handler** — translate vertical/horizontal pan into scroll
  offset; emit `ER_EVENT_SCROLL` with `scroll_x`/`scroll_y` already in `EREventData`.
- [x] **Momentum scrolling** — exponential decay on touch-up; honors `decelerationRate`;
  snap-to-offset when configured.
- [x] **Layout event dispatch** — `ER_EVENT_LAYOUT` is declared in `er_scene.h` but never
  fired. Compare previous vs. computed rect after layout and dispatch when changed.
- [x] **`pointerEvents` prop** — `auto` / `none` / `box-only` / `box-none`. Add to ERProps
  and respect in hit-testing.
- [x] **`hitSlop`** — extend node rect by configurable per-edge slop in hit-testing.

## 2. Scene Graph & Commit

- [x] Static node pool with tag/index addressing
- [x] Parent / first-child / next-sibling tree
- [x] Dirty propagation up the ancestor chain
- [x] zIndex-aware paint order
- [x] **Free-slot reuse in `er_node_destroy`** — LIFO free-list; destroyed nodes return
  their tag to the stack so the next `er_node_create` reuses it. Double-free guard
  (checks `in_use`) prevents free-list corruption.
- [x] **Dirty-rectangle tracking (v1 — source-dirty only)** — `er_get_dirty_rect()` returns
  the union of source-dirty leaf rects after each `er_commit()`. Shadow bleed is included
  conservatively. Host MCU drivers can use this for partial DMA transfers.
- [x] **`display: none` short-circuit in layout + render** — skip layout pass, skip paint,
  skip hit-testing.
- [x] **Clip-rect stack during render** — push/pop on `overflow: hidden` and on scroll
  viewports; intersect with backend draw rect before each blit.

## 3. Layout

The Yoga 7-pass engine in [layout_engine.c](engine/layout/layout_engine.c) is working for
the props currently in `ERLayoutSpec`. Additions:

- [x] flex direction / wrap / grow / shrink / basis
- [x] justify / align-items / align-self
- [x] margin, padding (shorthand and per-edge), gap
- [x] absolute positioning
- [x] min/max width/height
- [x] **`aspectRatio`** prop — `float aspect_ratio` in `ERLayoutSpec`; layout engine derives
  the auto dimension using `width/height` semantics in Pass 1.
- [x] **`marginHorizontal` / `marginVertical` / `paddingHorizontal` / `paddingVertical`** —
  added to `ERProps`; expanded to per-edge fields in `er_node_set_props()`.
- [x] **`flexBasis: '50%'` percent support** — `float flex_basis_pct` in `ERLayoutSpec`;
  resolved against parent main-axis size in Pass 1 (takes precedence over `flex_basis`).
- [x] **`display: none`** node skip (see Scene Graph item).
- [x] **`overflow: scroll`** path that produces a virtual content size larger than the
  computed rect, consumed by ScrollView.

## 4. Rendering

[rrect.c](engine/rendering/rrect.c) handles rounded rectangles with anti-aliased corners.
Everything else under `engine/rendering/` is a stub.

### 4.1 Borders & background

- [x] Rounded-rect fill (scanline)
- [x] Anti-aliased corner edges (`ERUI_BORDER_AA`)
- [x] Solid border ring
- [x] **Per-corner `borderRadius`** (`borderTopLeftRadius`, etc.) — four fields in `ERProps`
  and `ERViewProps`; `er_rrect_fill_corners()` renders with per-corner AA scanlines.
- [x] **Per-edge `borderWidth`** (`borderLeftWidth`, etc.) — four fields in `ERProps`
  and `ERViewProps`; `render_view_bg()` dispatches to per-edge path when non-uniform.
- [x] **`borderStyle`** — `ER_BORDER_SOLID` / `ER_BORDER_DASHED` / `ER_BORDER_DOTTED`;
  `er_rrect_border_edge()` renders dashed/dotted patterns (solid path unchanged).
- [x] **Per-edge `borderColor`** (`borderLeftColor`, etc.) — four fields in `ERProps`
  and `ERViewProps`; resolved with fallback to `border_color` in `render_view_bg()`.

### 4.2 Shadows ([shadow.c](engine/rendering/shadow.c))

- [x] **Two-pass box blur** rasterizer (horizontal + vertical separable kernel).
- [x] Honor `shadowColor`, `shadowOffset`, `shadowOpacity`, `shadowRadius`, `elevation`.
- [x] Gated behind `ERUI_SHADOWS`.
- [x] Uses static scratch buffers inside shadow.c (independent of the scratch pool).
- [x] Shadow fields added to `ERProps` and `ERViewProps`.

### 4.3 Transforms ([transform.c](engine/rendering/transform.c))

- [x] **Translate-only fast path** — pure integer offset, no rasterization (`ERUI_TRANSFORMS=TRANSLATE_ONLY`).
- [x] **2D affine pass** — `scaleX/Y`, `rotateZ`, `translateX/Y`, `transformOrigin`. Rasterize
  transformed subtree into a scratch slot, blend to parent.
- [x] **3D pass** — `rotateX`, `rotateY`, `perspective` — gated behind `ERUI_3D_TRANSFORMS`.
- [x] **Transform matrix propagation** during render and during hit-test (inverse mapping).
- [x] Add transform fields to `ERProps` (currently animatable via `ERAnimProp` enum but
  no static prop fields exist).

### 4.4 Opacity compositing

- [x] Per-node opacity prop stored in `ERViewProps`
- [x] **Offscreen composite for `opacity < 255` on a non-leaf subtree** — rasterize into
  scratch slot, then blend at the node's alpha. Currently `opacity` is stored but the
  compositor doesn't use it.
- [x] **Opacity nesting depth** capped at `ERUI_MAX_OPACITY_DEPTH`.

### 4.5 Image rendering

- [x] `ERImageProps` + `image_name` registered on the node
- [x] **`er_image_load` actual implementation** — delegates to image registry.
- [x] **Image registry** — name → (buf, w, h) lookup keyed like font registry.
- [x] **Nearest-neighbor scaling** (default).
- [x] **Bilinear scaling** behind `ERUI_BILINEAR_SCALE`.
- [x] **`resizeMode`** — cover / contain / stretch / repeat / center.
- [x] **`tintColor`** — modulate sampled RGB by tint color before blend.
- [x] **Render path in compositor** — `ER_NODE_IMAGE` case added to `render_tree`.

### 4.6 Gradients

- [x] **Linear gradient** rasterizer (gated behind `ERUI_GRADIENT`).
- [x] **Radial gradient** rasterizer (gated behind `ERUI_GRADIENT_RADIAL`).
- [x] Gradient prop encoding in `ERProps` (stop list).

### 4.7 Scratch buffer pool

- [x] **Static pool declaration** — `ERUI_SCRATCH_W × ERUI_SCRATCH_H × ERUI_SCRATCH_POOL_DEPTH × 4`
  bytes at module scope.
- [x] **Slot alloc / release API** for shadow / opacity / transform consumers.
- [x] **Premultiplied ARGB8888 blend helper** that walks scratch rows into the backend
  via `blend_rect`.

### 4.8 Canvas API ([canvas_bindings.c](engine/rendering/canvas_bindings.c) is a stub)

Out of scope for v1 unless the bundled React surface needs it. Leave the file as a stub
placeholder.

## 5. Text

[text_renderer.c](engine/text/text_renderer.c) handles single-run rendering of the
built-in Inter font at fixed sizes.

- [x] UTF-8 decode
- [x] Glyph blit at all baked sizes
- [x] Solid color
- [x] **Multi-line wrapping** — word-boundary wrap; character-boundary fallback for overlong words.
- [x] **`numberOfLines`** truncation.
- [x] **`ellipsizeMode`** — `tail` (appends U+2026) and `clip`; head/middle deferred.
- [x] **`textAlign`** — `left` / `center` / `right`; justify deferred.
- [x] **`lineHeight`** override (default is font line_height).
- [x] **`letterSpacing`**.
- [x] **`fontWeight: bold`** rendering path — synthetic double-pass at cursor_x+1; advance
  widens by 1 px per glyph.
- [x] **`fontStyle: italic`** — synthetic horizontal shear (ITALIC_SLOPE = 0.2 ≈ 11°);
  each row shifted right by (height − 1 − row) × slope, no extra font data needed.
- [x] **`textDecorationLine`** — `underline` (below baseline) and `line-through` (mid-cap).
- [x] **Nested `<Text>` spans** — `ERTextSpan` struct with per-span color, weight, style,
  decoration, letter_spacing overrides (sentinels inherit from parent). Up to
  `ER_TEXT_MAX_SPANS` (4) spans per node. `er_node_set_text_spans()` sets spans directly;
  spans can also be passed via `ERProps.spans[]`. Renderer merges spans into one buffer for
  line-breaking, then renders per-span runs with resolved styles.
- [x] **Text measurement API** — `er_text_measure()` exposed; accepts `letter_spacing`.

## 6. Animation

[animation.c](engine/animation/animation.c) currently handles timing animations on
opacity and the two color props.

- [x] Timing curve (linear interpolation)
- [x] Color interpolation (per-channel lerp)
- [x] Opacity property
- [x] Loop flag (ping-pong via swap)
- [x] **Easing functions** — `ER_EASE_LINEAR`, `EASE`, `EASE_IN`, `EASE_OUT`, `EASE_IN_OUT`,
  `QUAD_IN/OUT/IN_OUT`, `CUBIC_IN/OUT/IN_OUT`, `BOUNCE_OUT`, `ELASTIC_OUT`, `BEZIER`
  (cubic-bezier with four control points). Newton's-method solver used for bezier X→t.
- [x] **Spring animations** (`ER_ANIM_SPRING`) — Euler-integrated damped harmonic oscillator
  using `stiffness` / `damping` / `mass` / initial velocity; settles with dual threshold.
- [x] **Decay animations** (`ER_ANIM_DECAY`) — per ms exponential friction; `velocity` /
  `deceleration`; stops when |velocity| < DECAY_VEL_STOP.
- [x] **Translate/scale/rotate property support** — `apply_numeric_value` handles translateX/Y,
  scaleX/Y, rotateZ; `update_has_transform` keeps the dirty flag correct.
- [x] **`Animated.delay`** — `delay_ms` field in ERAnimConfig; delay counted before the
  animation clock starts; leftover time after expiry applied correctly on the same tick.
- [x] **`Animated.sequence`** — `er_anim_sequence()` starts each entry only after the
  previous finishes; fires group on_complete when the last entry is done.
- [x] **`Animated.parallel`** — `er_anim_parallel()` starts all entries simultaneously;
  fires group on_complete once all entries have completed.
- [x] **`Animated.stagger`** — `er_anim_stagger()` adds i×stagger_ms delay to each entry.
- [x] **Completion callback** — `ERAnimCompleteFn on_complete` in ERAnimConfig; fired with
  `finished=true` on natural end, `finished=false` via `er_anim_stop()`.
- [x] **`useNativeDriver: true` binding model** — `ERAnimValue` is a standalone animatable
  float (pool size `ERUI_MAX_ANIM_VALUES`, default 16) with up to `ERUI_MAX_VALUE_BINDINGS`
  (default 4) node-property subscriptions.  `er_anim_value_create/destroy/bind/unbind_all/
  animate/set/get` form the public API. When the value ticks, every bound node+prop pair
  is updated in C without re-entering any higher-level layer — the engine-side foundation
  for `useNativeDriver: true`. Demo: Panel 6 "ANIMATED VALUE" section.
- [x] **Interpolation ranges** — `value.interpolate({inputRange, outputRange})`. Engine concept:
  `er_interpolate()` is a pure piecewise-linear mapper (up to `ER_INTERPOLATE_MAX_POINTS` breakpoints,
  per-side `ERExtrapolate` of EXTEND/CLAMP/IDENTITY). `er_anim_value_bind_interpolated()` attaches a
  shared value to a node property through an `ERInterpolation`; the mapping is applied on every value
  change (set and runtime tick) inside `push_to_value_bindings`. Demo: Panel 6 "ANIMATED VALUE" uses
  three interpolated bindings (linear tx, triangle-wave tx, opacity) from one 0→1 driver value.
- [x] **`LayoutAnimation`** — observe layout deltas and animate computed rects with
  spring/timing (separate pass after layout, before render). Engine concept:
  `er_layout_anim_configure_next()` sets a one-shot config (ERLayoutAnimConfig: type, duration,
  easing, spring k/c/m); on the next `er_commit()` every node whose computed rect changed from
  its current display rect starts a layout animation (`ERLayoutAnim` pool, up to
  `ERUI_MAX_LAYOUT_ANIMS` = 16 simultaneous). `er_layout_anim_tick()` (called from
  `embedded_renderer_tick()`) advances all active slots and writes `node->animated`; the
  compositor reads `animated` instead of `computed` for pixel positions. Retarget-in-flight
  (layout changes again mid-animation) restarts from the current display position.
  Preset constants: `ER_LAYOUT_ANIM_EASE_IN_EASE_OUT`, `ER_LAYOUT_ANIM_LINEAR`,
  `ER_LAYOUT_ANIM_SPRING`. Demo: Panel 11 "LAYOUT ANIMATION" shows three boxes (two
  explicit-width, one flex_grow=1) resizing simultaneously with TIMING/SPRING toggle.

## 7. Components

The switch in `render_tree` in [compositor.c](engine/scene/compositor.c) falls through
for several node types declared in `ERNodeType`.

| Node                         | Status  | What's needed                                                            |
|------------------------------|---------|--------------------------------------------------------------------------|
| `ER_NODE_VIEW`               | working | -                                                                        |
| `ER_NODE_PRESSABLE`          | working | -                                                                        |
| `ER_NODE_MODAL`              | working | Visible/hidden via `modal_visible`; backdrop fill; z-index 1000 layer    |
| `ER_NODE_TEXT`               | working | (see Text section)                                                       |
| `ER_NODE_IMAGE`              | working | -                                                                        |
| `ER_NODE_SCROLL_VIEW`        | working | -                                                                        |
| `ER_NODE_FLAT_LIST`          | working | Scrolls like ScrollView; virtualisation still TODO                       |
| `ER_NODE_TEXT_INPUT`         | working | Auto-focus on press; key input via `embedded_renderer_key`; cursor blink |
| `ER_NODE_ACTIVITY_INDICATOR` | working | 8-dot fading ring driven by built-in looping `rotate_z` animation        |
| `ER_NODE_SWITCH`             | working | Pill track + animated thumb; value prop drives 200 ms ease-in-out        |

Each entry corresponds to an item to land: render case, prop fields in `ERProps`, type
in the props union, any per-node state.

## 8. Resources & Registries

- [x] Font registry and built-in Inter blob
- [x] Runtime `er_font_load` (`ERUI_FONT_POOL_BYTES`)
- [x] **Image registry** — analogous to font registry; `er_image_load` populates, the
  Image node looks up by name.
- [x] **Resource teardown / replace** — `er_image_load` replaces an existing name in place
  (buffers are caller-owned, so nothing leaks). `er_font_load`/`font_blob_register` replace a
  same-`pixel_size` entry in place via `font_registry_add` (new `font_registry_get_exact`
  helper); the blob loader repacks into the prior pool footprint when the new blob fits
  (no bytes consumed) and only falls back to a fresh bump allocation when it does not — that
  fallback case is documented as leaking until `font_blob_init()`/`font_registry_init()`
  reclaim the whole pool. Covered by the `resources` CTest suite (image replace, registry
  replace, blob reuse vs. fresh-alloc). The pool footprint tracking compiles out entirely
  when `ERUI_FONT_POOL_BYTES == 0`.

## 9. Compile-Time Feature Flags

PLAN.md lists these. Several are now wired through `#if` guards in the source; the
remainder still lack guards or the underlying feature is not yet implemented.

- [x] `ERUI_SHADOWS` — gate shadow code paths.
- [x] `ERUI_BORDER_AA` — already wired in rrect.
- [x] `ERUI_3D_TRANSFORMS` — gate rotateX/Y/perspective.
- [x] `ERUI_BILINEAR_SCALE` — switch image scaler kernel.
- [x] `ERUI_GRADIENT` / `ERUI_GRADIENT_RADIAL` — gate gradient rasterizer.
- [x] `ERUI_TRANSFORMS` — `FULL` vs `TRANSLATE_ONLY` paths (wired as `ERUI_TRANSFORMS_FULL`).
- [x] `ERUI_FONT_SIZES` — already drives baked font selection.
- [x] `ERUI_MAX_NODES` — already honored.
- [x] `ERUI_MAX_OPACITY_DEPTH`, `ERUI_SCRATCH_W/H`, `ERUI_SCRATCH_POOL_DEPTH` — for the
  scratch pool work (4.7).

## 10. Tests

Host CTest suites in [engine/tests/](engine/tests/) are green for what exists.

- [x] animation, input, layout, text, rrect
- [x] **aspectRatio**, **flex_basis_pct**, **marginHorizontal/Vertical**, **paddingHorizontal/Vertical** layout tests
- [x] **Hit-test under opacity / display:none** (covered by existing tests)
- [x] **Hit-test under transforms**
- [x] **Scroll gesture + momentum**
- [x] **Layout event dispatch**
- [x] **Spring / decay animations**
- [x] **Sequence / parallel / stagger**
- [x] **Image scaling + tint + resizeMode**
- [x] **Shadow rasterizer (visual baseline comparison)**
- [x] **Transform render + hit-test**
- [x] **Opacity offscreen compositing**
- [x] **Multi-line / ellipsize text**
- [x] **Node pool free-slot reuse**
- [x] **Gradient rasterizer** — vertical, horizontal, diagonal, 3-stop, radial, degenerate (stop_count < 2)
- [x] **Interpolation ranges** — two/three-point linear mapping, EXTEND/CLAMP/IDENTITY per-side extrapolation,
  degenerate guards (point_count < 2, NULL arrays, zero-width segment)
- [x] **Layout-dirty fast path** (§11.1) — `er_layout_pass_count()` confirms: first commit runs a pass,
  idle commits and an opacity animation skip it, prop changes / `LayoutAnimation.configureNext` force a
  pass, and ER_EVENT_LAYOUT does not fire on skipped commits (in the `layout` suite)

---

## 11. Performance & Efficiency Backlog

Everything in §1–§10 is shipped, tested, and **correct**. This section is the opposite of a
to-do list of missing features: it is a catalogue of places where the current implementation
is correct but does **more work than it needs to**. None of these are bugs. They are the items
to attack to make the engine run smoothly on a constrained MCU: think a few hundred KB of RAM,
no FPU or a slow one, an LCD pushed over SPI/DMA. (Hot reload is not the engine's concern — that
lives in the React/Metro tooling and the browser preview during development. The engine's job is
to render the committed tree fast and small on-device.)

The guiding principle for every item below is the same: **the engine recomputes per-frame
things that only change when a prop changes.** A static screen — the common case on an
embedded panel — should cost almost nothing to keep on screen. Today several subsystems pay
full freight every commit regardless of what actually changed.

Severity legend: 🔴 high (affects every frame / every node, or dominates RAM), 🟡 medium
(affects a feature when it is used), 🟢 low (polish, measurable only under stress).

### 11.1 Per-frame redundant work (the dirty model stops at render, not layout)

- [x] **Layout-dirty flag — DONE.** `er_commit()` now gates the entire layout block (flex solve +
  text measure + the post-layout passes below) behind a module-level `s_layout_dirty`
  ([compositor.c](engine/scene/compositor.c)). The flag is raised by `mark_layout_dirty()` from
  every mutation that can move a computed rect — `er_node_set_props`, `er_node_set_text_spans`,
  `er_tree_append_child`/`remove_child`/`set_root`, and `er_node_destroy` — and lowered after a
  pass run. Animations mutate render-only props (opacity, color, transform) directly via
  `apply_numeric_value` and never raise it, so a static or animation-only frame skips the whole
  pass and just repaints dirty nodes. A pending `LayoutAnimation` (`er_layout_anim_has_pending()`)
  also forces a pass so `configureNext` keeps its "evaluate on the next commit" contract. The new
  public `er_layout_pass_count()` exposes how often layout actually ran, for profiling and tests;
  the `layout` CTest suite asserts idle/animation commits skip and prop/tree/LayoutAnimation
  changes trigger a pass. *Remaining refinement (deferred):* the flag is whole-tree and
  conservative — `er_node_set_props` raises it even when only a visual prop changed. The next
  step is per-container layout-dirty (re-solve only changed subtrees) and comparing the
  layout-relevant fields in `set_props` so a pure color change does not relayout.

- 🔴 **Text is re-measured on every layout pass.** Pass 1 calls `er_text_measure()` for every
  `ER_NODE_TEXT` child ([layout_engine.c:191](engine/layout/layout_engine.c#L191)), which
  walks the whole UTF-8 string doing a glyph lookup per codepoint
  ([text_renderer.c:763](engine/text/text_renderer.c#L763)). The layout-dirty flag above means
  this no longer runs on idle/animation frames, but it still re-measures every Text node on any
  commit that runs layout (e.g., a single unrelated prop change). *Fix:* cache the measured
  `(w, h)` on the node and invalidate only when the text/font/size/spacing props change.

- [x] **`dispatch_layout_events()` and `refresh_scroll_content_sizes()` walk the whole tree —
  now gated.** Both are inside the `s_layout_dirty` block in `er_commit()`, so they only run on
  commits that actually recompute layout. (Safe because they read `computed`, which is unchanged
  when layout is skipped — same results, simply not recomputed.)

- 🟡 **The render traversal descends the entire tree every frame even when nothing is dirty.**
  `render_tree()` must visit every node to discover whether any descendant is dirty, and at
  *every* node it unconditionally runs `collect_children()` + `sort_children_by_z_index()`
  ([compositor.c:655-657](engine/scene/compositor.c#L655)). For a clean subtree this is pure
  waste. *Fix:* track a `subtree_dirty` bit so a clean subtree can be skipped without descending,
  and cache the sorted child order (see 11.6).

### 11.2 Missing result caches (recompute-from-scratch on every repaint)

These subsystems produce an output that is a pure function of a node's props and size, yet
recompute it from zero every time the node is repainted (which happens whenever the node *or any
ancestor* is dirty).

- 🔴 **Shadows are fully re-blurred every repaint.** `er_shadow_render()` rasterises the
  silhouette (`O(w·h)`), runs two separable box-blur passes (`O(sw·sh)`), and tints the result
  every call ([shadow.c:152](engine/rendering/shadow.c#L152)). The blurred alpha depends only on
  `(w, h, radius, corner)`. A static shadowed card that repaints because a child animated pays
  the entire blur cost each frame. *Fix:* cache the blurred alpha buffer keyed on its geometry;
  only re-blur when those change.

- 🟡 **Gradients evaluate stops per pixel with no LUT.** `render_linear`/`render_radial` call
  `eval_stops()` (a linear scan over stops) plus `premul()` for **every pixel**, every frame
  ([gradient.c:159](engine/rendering/gradient.c#L159),
  [gradient.c:202](engine/rendering/gradient.c#L202)); the radial path also does a `sqrtf` per
  pixel ([gradient.c:199](engine/rendering/gradient.c#L199)). *Fix:* precompute a 256-entry
  premultiplied color LUT from the stops once, then index it by quantised `t`. For axis-aligned
  linear gradients the per-row value is constant, so a single row can be computed and repeated.

- 🟡 **Scaled images are re-sampled every repaint.** `er_image_render()` rescales (and re-tints)
  the source into the destination on every paint ([image_scaler.c:199](engine/rendering/image_scaler.c#L199));
  nothing caches the scaled result. The nearest path also does an integer divide per pixel
  (`(dx * src_w) / dst_w`, [image_scaler.c:179](engine/rendering/image_scaler.c#L179)) and the
  bilinear path a float divide per pixel. *Fix:* replace per-pixel division with a fixed-point
  DDA step (`src_step = (src_w << 16) / dst_w`, accumulate), and optionally cache the scaled
  bitmap when the node size is stable.

- 🟢 **Font lookup is a `strncmp` registry scan per text op.** `font_registry_get()` linearly
  scans the registry comparing family names on every `er_text_render` and every
  `er_text_measure` ([font_registry.c:148](engine/font/font_registry.c#L148)). *Fix:* resolve
  the `BitmapFont*` once when text props are set and cache it on the node.

### 11.3 Per-pixel hot loops that emit one backend call per pixel/run

- 🔴 **Anti-aliased rounded-corner fringes blit one pixel at a time.** Both `er_rrect_fill` and
  `er_rrect_fill_corners` emit individual `1×1 er_blit_fill()` calls for each AA edge pixel
  ([rrect.c:134](engine/rendering/rrect.c#L134),
  [rrect.c:269](engine/rendering/rrect.c#L269)). Everyone pays the full `apply_clip` + backend
  dispatch cost for a single pixel. Across four corners of every rounded view there are dozens of
  one-pixel calls per node. *Fix:* accumulate each scanline's coverage into a small row buffer and
  flush it with one `er_blit_copy`/`blend` per row, the way the AA glyph path already does
  ([text_renderer.c:330](engine/text/text_renderer.c#L330)).

- 🔴 **1-bit glyphs blit one run per set-bit span.** `draw_glyph()` emits an `er_blit_fill()` per
  horizontal run of lit pixels, per row, per glyph ([text_renderer.c:182](engine/text/text_renderer.c#L182)).
  A paragraph of text becomes thousands of tiny fill calls. *Fix:* assemble a premultiplied row
  buffer and blit once per row (the AA path already does this); or have the backend expose a
  1-bit mask blit.

- 🟡 **Bordered rounded rects overdraw their entire interior.** `er_rrect_fill_bordered()` fills
  the whole rect in the border color, then fills the inset interior again in the background color
  ([rrect.c:328-337](engine/rendering/rrect.c#L328)). Every bordered view writes most of its
  pixels twice. For a full-screen background with a border that is a screen-sized overdraw. *Fix:*
  draw only the border ring (four edge spans / arc fringes) and fill the interior once.

- 🟡 **Software compositing does a bound check per pixel instead of clamping the loop.** The
  scratch blend/fill/copy inner loops test `col < 0 || col >= s_scratch_w` for every pixel
  ([native_renderer.c:116](engine/core/native_renderer.c#L116),
  [native_renderer.c:161](engine/core/native_renderer.c#L161)). *Fix:* clamp the loop start/end
  once per row and drop the per-pixel branch. These loops also use a `/255U` per channel per
  pixel; the standard `(x*257+257)>>16` (or `x + (x>>8)` rounding) approximation removes the
  divide.

- 🟡 **Affine/3D transforms do float matrix-multiplies + a bilinear sample per output pixel.**
  `er_transform_source_end_blit()` inverse-maps every destination pixel with two float muls and
  a 4-tap bilerp ([transform.c:460-478](engine/rendering/transform.c#L460)), recomputed every
  frame for an animating transform. *Fix:* step the source coordinate incrementally per row/col
  (add the matrix column once per pixel instead of a full mul), and consider fixed-point sampling
  on FPU-less targets.

### 11.4 Static RAM footprint (the headline embedded constraint)

The compositing scratch buffers are sized for a 240×240 panel and allocated at module scope.
They dominate the engine's RAM budget and, at the defaults, exceed the total SRAM of many
mid-range MCUs:

- 🔴 **Opacity scratch pool:** `ERUI_MAX_OPACITY_DEPTH (4) × ERUI_SCRATCH_W·H (240×240) × 4 B
  ≈ 900 KB** ([scratch_pool.c:33](engine/rendering/scratch_pool.c#L33)).
- 🔴 **Transform buffers:** `s_xform_src + s_xform_dst = 2 × 240×240 × 4 B ≈ 450 KB`
  (transform.c module scope).
- 🟡 **Shadow buffer:** `s_alpha = 240×240 ≈ 57 KB` ([shadow.c:19](engine/rendering/shadow.c#L19)).

Together that is **~1.4 MB of static scratch RAM** before the node pool, font pool, or backend
framebuffer. *Directions to explore:* (a) document the real cost of `ERUI_SCRATCH_W/H` and
`ERUI_MAX_OPACITY_DEPTH` so integrators can shrink them — the effective transformed/opacity-composited
node size is capped by these dimensions; (b) allow the scratch slots to be sized to
the largest composited node rather than the full screen; (c) tile large transformed/opacity
subtrees through a smaller scratch instead of requiring one that holds the whole node; (d) share
one arena between the transform, opacity, and shadow buffers since they are not all live
simultaneously.

Also, worth a pass: every scratch acquisition `memset`s the **entire** slot, not just the active
region — `er_scratch_push` clears all 240×240 px even for a 20×20 node
([scratch_pool.c:78](engine/rendering/scratch_pool.c#L78)), and the transform source clears the
full buffer too ([transform.c:418](engine/rendering/transform.c#L418)). Clearing only `w×h` (or
the AABB) turns a fixed ~230 KB clear into a few hundred bytes for small nodes.

### 11.5 Per-call stack usage in recursion

Several recursive walkers allocate a `uint16_t[ERUI_MAX_NODES]` (1 KB at the default 512) on the
stack at **every level of recursion**:

- `render_tree` → `child_tags[ERUI_MAX_NODES]` ([compositor.c:655](engine/scene/compositor.c#L655))
- `hit_test_node` → `child_tags[ERUI_MAX_NODES]` ([hit_test.c:389](engine/scene/hit_test.c#L389))
- `er_dispatch_touch` → `chain[ERUI_MAX_NODES]` ([hit_test.c:803](engine/scene/hit_test.c#L803),
  [hit_test.c:857](engine/scene/hit_test.c#L857))

🟡 A deep tree multiplies this by depth (e.g., 12 levels deep ≈ 12 KB of transient stack just for
child arrays), which is significant on an MCU with an 8–16 KB stack. *Fix:* size these to the
actual child count, walk the sibling list in place, or use a single shared scratch indexed by
recursion depth.

### 11.6 Algorithmic / data-structure costs

- 🟡 **`er_tree_append_child` is O(n) per append → O(n²) to build a sibling list.** It walks the
  entire sibling chain to find the tail on every append
  ([compositor.c:1296](engine/scene/compositor.c#L1296)). When the React reconciler mounts or
  re-renders, large lists are built child-by-child, making initial mount and big diffs quadratic.
  *Fix:* keep a
  `last_child_tag` on the parent (and a `prev_sibling`/`parent` back-pointer would also speed
  `er_tree_remove_child`, currently also an O(n) scan at
  [compositor.c:1328](engine/scene/compositor.c#L1328)).

- 🟡 **zIndex insertion-sort runs every frame for every node with children**, in both render
  ([compositor.c:657](engine/scene/compositor.c#L657)) and hit-test
  ([hit_test.c:391](engine/scene/hit_test.c#L391)) — even when no child sets a non-zero `zIndex`
  (the common case). *Fix:* skip the sort when all children share `zIndex == 0`, or cache the
  sorted order and only re-sort when children/zIndex change.

- 🟡 **Input tick scans all `ERUI_MAX_NODES` slots twice for momentum.** `er_input_tick` loops over
  the full pool (512 by default) every tick to find ScrollViews with non-zero velocity
  ([hit_test.c:728](engine/scene/hit_test.c#L728)); `er_input_reset` does the same
  ([hit_test.c:691](engine/scene/hit_test.c#L691)). This cost is paid every tick even when nothing
  is scrolling and most slots are empty. *Fix:* keep a small active-scroller list, or track
  `s_next_tag` as the scan bound instead of the full capacity.

- 🟢 **`accumulate_scroll_offsets` re-walks ancestors on every touch move** and is called multiple
  times per `ER_TOUCH_MOVE` ([hit_test.c:277](engine/scene/hit_test.c#L277)). Minor (touch is not
  per-frame) but easy to hoist to one call per event.

### 11.7 Reconciliation bursts (on-device tree updates)

This is **not** about hot reload — hot reload happens in the React/Metro tooling and the browser
preview during development, never on the device. But the same code path that a reload would
exercise runs on-device every time app state changes: React re-renders, and the
`bridges/quickjs/` reconciler pushes a burst of `er_node_set_props` / append / remove calls
through the engine before the next commit. A list that grows, a screen that swaps, a modal that
opens — each is a reconciliation burst, and they stress exactly the paths above.

- 🟡 Each `er_node_set_props` copies **every** layout field and the full type-specific prop block
  and then calls `er_mark_dirty_upward` ([compositor.c:866](engine/scene/compositor.c#L866)). With
  no layout-dirty separation (11.1), a state update that re-props N nodes forces a full-tree
  layout resolve on the next commit no matter how localized the change was. The 11.1/11.6 fixes
  (layout-dirty flag, O(1) append) are what keep a large screen's updates smooth on the MCU.
- 🟢 Consider a batched "begin/end commit" boundary so the bridge can apply a whole tree diff and
  trigger exactly one layout and one paint, instead of relying on per-mutation dirty marking to
  coalesce. (`er_commit()` is already the natural barrier — the point is to make the per-mutation
  work between barriers cheap.)

### 11.8 Suggested order of attack

1. ~~**Layout-dirty flag (11.1)**~~ — **done.** The single biggest win; static and animation-only
   frames now skip the flex + text-measure pass entirely. Added `er_layout_pass_count()` and
   `layout` test coverage. (Whole-tree, conservative; per-container refinement still open — see 11.1.)
2. **Result caches for shadow / gradient / image / text-measure (11.1, 11.2)** — removes the
   largest per-frame recompute spikes. (Text-measure caching is now the top remaining layout cost,
   since it still re-measures every Text node on any commit that runs layout.)
3. **Scratch RAM sizing + partial clears (11.4)** — what actually decides whether the engine fits
   on a given MCU; pairs well with documenting the `ERUI_SCRATCH_*` trade-offs.
4. **Batch the per-pixel blit loops (11.3)** — AA fringes, 1-bit glyphs, bordered overdraw.
5. **O(1) child append + skip-sort + bounded input scan (11.5, 11.6)** — inexpensive structural wins.

One small exception to "no public-surface change": the layout-dirty work added a read-only
diagnostic, `er_layout_pass_count()` — it does not alter behavior or the React-facing API. The
remaining items are internal optimizations that preserve current behavior and visual output. Each
should land with a before/after measurement (frame time on the SDL/software backend, and static RAM
from the map file) and, where behavior-preserving, a test asserting identical pixels/metrics to today.

---

## Future Work

Everything in the checklists above is shipped and tested. The items below are intentionally
deferred — they are not regressions or gaps in the documented v1 surface, just the natural
next steps if the engine grows.

- **FlatList virtualisation** — `ER_NODE_FLAT_LIST` currently scrolls like a ScrollView and
  renders all children. Windowing (mounting only the visible range + overscan) is the next
  step for large lists on constrained RAM. See the component table in §7.
- **Canvas API** ([canvas_bindings.c](engine/rendering/canvas_bindings.c)) — deliberately a
  stub (§4.8). Land an immediate-mode drawing surface only if a bundled React surface needs
  it; it is out of scope for the documented component and style subset.
- **Additional ellipsize modes** — text truncation supports `tail` and `clip`; `head` and
  `middle` are deferred (§5).
- **`textAlign: justify`** — left/center/right ship; justified text is deferred (§5).

When picking up any of these, add the feature to the relevant subsystem section above (with
the same implementation-note style) and extend the §10 test matrix.

---

## Build History

The order the engine was built in, kept for context. Each step kept the engine demoable:

1. ~~**Finish touch**~~ — `display:none` / `opacity:0` hit-rejection, `pointerEvents`, layout
   event dispatch — **done**.
2. ~~**Image rendering**~~ — registry + nearest scaler + resizeMode. Unlocks half the React
   demo apps — **done**.
3. ~~**ScrollView**~~ — clip + gesture + momentum. Most apps need this before they look real — **done**.
4. ~~**Opacity compositing + scratch pool**~~ — required by shadows and transforms; small
   self-contained landing — **done**.
5. ~~**Transforms (2D)**~~ — translate the fast path + affine pass + hit-test inverse — **done**.
6. ~~**Shadows**~~ — straightforward once the scratch pool exists — **done**.
7. ~~**Animation engine completion**~~ — spring, decay, easing, transform properties,
   sequence/parallel, completion callbacks — **done**.
8. ~~**Text upgrades**~~ — word-wrap, `numberOfLines`, `textAlign`, `ellipsizeMode`, `letterSpacing`,
   `textDecorationLine` — **done**.
9. ~~**Remaining components**~~ — TextInput, Switch, ActivityIndicator, Modal, FlatList — **done**.
10. ~~**Dirty-rect tracking + node pool reuse**~~ — perf / longevity polish before MCU bring-up — **done**.
11. ~~**Layout additions**~~ — `aspectRatio`, `marginHorizontal/Vertical`, `paddingHorizontal/Vertical`,
    `flexBasis %`, per-corner border radius, per-edge border width/color, `borderStyle` — **done**.
12. ~~**Feature flag plumbing**~~ — gradient rasterizer (linear and radial, `ERUI_GRADIENT` /
    `ERUI_GRADIENT_RADIAL`), bilinear image scaler (`ERUI_BILINEAR_SCALE`), all optional paths
    wrapped in `#if` guards — **done**.
13. ~~**Test coverage**~~ — fill the matrix in §10 as features land — **done** (16 CTest suites green, including
    `animation_layout_anim` and `resources`).

With all steps landed, the engine surface in `er_scene.h` is capable of hosting the
`bridges/quickjs/` reconciler against any sample React app written for the documented
component + style subset.
