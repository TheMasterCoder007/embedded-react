# Engine Completion Plan

Checklist of everything the C engine still needs before a React app can run end-to-end
on top of it. Items are grouped by subsystem and ordered roughly by dependency. Items
already implemented are checked. Each unchecked box is deliverable; cross them off as
they land.

The engine surface that React (via `bridges/quickjs/`) targets is `engine/include/er_scene.h`
and `engine/include/native_renderer.h`. Anything React-facing must reduce to those calls.

---

## 1. Input & Touch (resume point)

Touch dispatch is partially wired in [hit_test.c](engine/scene/hit_test.c). Single-finger
press, long-press, press-in/out, touch start/move/end, cancel, bubbling, and zIndex-aware
hit-testing are working. The remaining work is everything above raw events.

- [x] Press / press-in / press-out / long-press
- [x] Raw touch start / move / end / cancel + bubbling through ancestors
- [x] Multitouch (up to 5 fingers)
- [x] zIndex-aware hit-testing
- [x] **Hit-test rejection for `display: none` and `opacity == 0` nodes** — currently any
  sized node is hittable regardless of visibility.
- [x] **Hit-test clipping to `overflow: hidden` ancestors** — point must lie inside every
  clipping ancestor, not just the leaf rect.
- [ ] **Hit-test through 2D transforms** — once `transform.c` lands, hit-testing must
  apply the inverse transform to the query point at each transformed ancestor.
- [x] **Gesture/PanResponder support** — slop threshold, capture phase, `onMoveShouldSetResponder`,
  `onStartShouldSetResponder` equivalents, gesture cancel when scroll grabs the
  responder.
- [ ] **ScrollView gesture handler** — translate vertical/horizontal pan into scroll
  offset; emit `ER_EVENT_SCROLL` with `scroll_x`/`scroll_y` already in `EREventData`.
- [ ] **Momentum scrolling** — exponential decay on touch-up; honors `decelerationRate`;
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
- [ ] **Free-slot reuse in `er_node_destroy`** — `s_next_tag` only grows; destroyed nodes
  leak their slots. Need a free-list or bitmap so long-lived apps don't exhaust the
  pool.
- [ ] **Dirty-rectangle tracking** — record the union of dirty rects per frame so the
  backend can blit only changed regions. Today every dirty subtree forces a full
  ancestor repaint.
- [ ] **`display: none` short-circuit in layout + render** — skip layout pass, skip paint,
  skip hit-testing.
- [ ] **Clip-rect stack during render** — push/pop on `overflow: hidden` and on scroll
  viewports; intersect with backend draw rect before each blit.

## 3. Layout

The Yoga 7-pass engine in [layout_engine.c](engine/layout/layout_engine.c) is working for
the props currently in `ERLayoutSpec`. Additions:

- [x] flex direction / wrap / grow / shrink / basis
- [x] justify / align-items / align-self
- [x] margin, padding (shorthand + per-edge), gap
- [x] absolute positioning
- [x] min/max width/height
- [ ] **`aspectRatio`** prop — declared in PLAN.md, not in `ERLayoutSpec` yet.
- [ ] **`marginHorizontal` / `marginVertical` / `paddingHorizontal` / `paddingVertical`** —
  either expand in the prop-copy step or add fields.
- [ ] **`flexBasis: '50%'` percent support** — currently int16_t pixels only. Decide on a
  sentinel encoding or a separate percent field, document in `er_scene.h`.
- [ ] **`display: none`** node skip (see Scene Graph item).
- [ ] **`overflow: scroll`** path that produces a virtual content size larger than the
  computed rect, consumed by ScrollView.

## 4. Rendering

[rrect.c](engine/rendering/rrect.c) handles rounded rectangles with anti-aliased corners.
Everything else under `engine/rendering/` is a stub.

### 4.1 Borders & background

- [x] Rounded-rect fill (scanline)
- [x] Anti-aliased corner edges (`ERUI_BORDER_AA`)
- [x] Solid border ring
- [ ] **Per-corner `borderRadius`** (`borderTopLeftRadius`, etc.) — currently single
  `border_radius`.
- [ ] **Per-edge `borderWidth`** (`borderLeftWidth`, etc.) — currently single
  `border_width`.
- [ ] **`borderStyle`** — `solid` / `dashed` / `dotted`.
- [ ] **Per-edge `borderColor`** (`borderLeftColor`, etc.).

### 4.2 Shadows ([shadow.c](engine/rendering/shadow.c) is a stub)

- [ ] **Two-pass box blur** rasterizer (horizontal + vertical separable kernel).
- [ ] Honor `shadowColor`, `shadowOffset`, `shadowOpacity`, `shadowRadius`, `elevation`.
- [ ] Gated behind `ERUI_SHADOWS`.
- [ ] Uses scratch buffer pool (see 4.7).
- [ ] Add the shadow fields to `ERProps` and the View prop union.

### 4.3 Transforms ([transform.c](engine/rendering/transform.c) is a stub)

- [ ] **Translate-only fast path** — pure integer offset, no rasterization (`ERUI_TRANSFORMS=TRANSLATE_ONLY`).
- [ ] **2D affine pass** — `scaleX/Y`, `rotateZ`, `translateX/Y`, `transformOrigin`. Rasterize
  transformed subtree into a scratch slot, blend to parent.
- [ ] **3D pass** — `rotateX`, `rotateY`, `perspective` — gated behind `ERUI_3D_TRANSFORMS`.
- [ ] **Transform matrix propagation** during render and during hit-test (inverse mapping).
- [ ] Add transform fields to `ERProps` (currently animatable via `ERAnimProp` enum but
  no static prop fields exist).

### 4.4 Opacity compositing

- [x] Per-node opacity prop stored in `ERViewProps`
- [ ] **Offscreen composite for `opacity < 255` on a non-leaf subtree** — rasterize into
  scratch slot, then blend at the node's alpha. Currently `opacity` is stored but the
  compositor doesn't use it.
- [ ] **Opacity nesting depth** capped at `ERUI_MAX_OPACITY_DEPTH`.

### 4.5 Image rendering ([image_scaler.c](engine/rendering/image_scaler.c) is a stub)

- [x] `ERImageProps` + `image_name` registered on the node
- [ ] **`er_image_load` actual implementation** — current body is empty.
- [ ] **Image registry** — name → (buf, w, h) lookup keyed like font registry.
- [ ] **Nearest-neighbor scaling** (default).
- [ ] **Bilinear scaling** behind `ERUI_BILINEAR_SCALE`.
- [ ] **`resizeMode`** — cover / contain / stretch / repeat / center.
- [ ] **`tintColor`** — modulate sampled RGB by tint color before blend.
- [ ] **Render path in compositor** — `ER_NODE_IMAGE` currently falls through the switch.

### 4.6 Gradients

- [ ] **Linear gradient** rasterizer (gated behind `ERUI_GRADIENT`).
- [ ] **Radial gradient** rasterizer (gated behind `ERUI_GRADIENT_RADIAL`).
- [ ] Gradient prop encoding in `ERProps` (stop list).

### 4.7 Scratch buffer pool

- [ ] **Static pool declaration** — `ERUI_SCRATCH_W × ERUI_SCRATCH_H × ERUI_SCRATCH_POOL_DEPTH × 4`
  bytes at module scope.
- [ ] **Slot alloc / release API** for shadow / opacity / transform consumers.
- [ ] **Premultiplied ARGB8888 blend helper** that walks scratch rows into the backend
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
- [ ] **Multi-line wrapping** — wrap on word boundaries; honor parent width.
- [ ] **`numberOfLines`** truncation.
- [ ] **`ellipsizeMode`** (`head` / `middle` / `tail`).
- [ ] **`textAlign`** (`left` / `center` / `right` / `justify`).
- [ ] **`lineHeight`** override (default is font ascent + descent).
- [ ] **`letterSpacing`**.
- [ ] **`fontWeight: bold`** rendering path (already in `ERTextProps`, no glyph switch yet).
- [ ] **`fontStyle: italic`** — needs italic font variants in the baked font blob or a
  synthetic skew.
- [ ] **`textDecorationLine`** (`underline` / `line-through`).
- [ ] **Nested `<Text>` spans** — currently `ERTextProps.text` is one string. Either flatten
  at bridge layer or introduce text-run children.
- [ ] **Text measurement API** — needed by layout for intrinsic-size pass (already
  invoked implicitly; expose so layout can request it).

## 6. Animation

[animation.c](engine/animation/animation.c) currently handles timing animations on
opacity and the two color props.

- [x] Timing curve (linear interpolation)
- [x] Color interpolation (per-channel lerp)
- [x] Opacity property
- [x] Loop flag (ping-pong via swap)
- [ ] **Easing functions** — `Easing.ease`, `linear`, `quad`, `cubic`, `bezier(...)`,
  `bounce`, `elastic`, `in`/`out`/`inOut` variants.
- [ ] **Spring animations** (`ER_ANIM_SPRING`) — damped harmonic oscillator using
  `stiffness` / `damping` / `mass` / initial velocity. The config fields exist but
  `er_anim_start` rejects non-timing.
- [ ] **Decay animations** (`ER_ANIM_DECAY`) — exponential friction; honor `velocity` and
  `deceleration`.
- [ ] **Translate/scale/rotate property support** — `apply_numeric_value` only handles
  `ER_PROP_OPACITY`. Add translateX/Y, scaleX/Y, rotateZ once transform props exist
  in `ERViewProps` (or its successor).
- [ ] **`Animated.delay`** — start time offset before the first tick contributes.
- [ ] **`Animated.sequence`** — chained group; needs animation group/handle IDs.
- [ ] **`Animated.parallel`** — group started together, joint completion callback.
- [ ] **`Animated.stagger`** — sequence with constant delay between starts.
- [ ] **Completion callback** — bridge needs to know when a `.start(callback)` resolves.
  Requires animation handles returned from `er_anim_start`.
- [ ] **`useNativeDriver: true` binding model** — bind a JS-owned `Animated.Value` ID to
  a C-side float without re-entering JS each frame. Design lives in the bridge, but
  the engine must support naming animatable values independently of nodes (or accept
  that all native-driver animations go through `er_anim_start` directly).
- [ ] **Interpolation ranges** — `value.interpolate({inputRange, outputRange})`. Engine
  already lerps internally; the bridge may translate ranges into a sequence of
  `er_anim_start` calls, or this becomes an engine concept.
- [ ] **`LayoutAnimation`** — observe layout deltas and animate computed rects with
  spring/timing (separate pass after layout, before render).

## 7. Components

The switch in `render_tree` in [compositor.c](engine/scene/compositor.c) falls through
for several node types declared in `ERNodeType`.

| Node                         | Status          | What's needed                                                          |
|------------------------------|-----------------|------------------------------------------------------------------------|
| `ER_NODE_VIEW`               | working         | -                                                                      |
| `ER_NODE_PRESSABLE`          | working         | -                                                                      |
| `ER_NODE_MODAL`              | renders as View | Z-order to top of root, backdrop, animation entry/exit                 |
| `ER_NODE_TEXT`               | working         | (see Text section)                                                     |
| `ER_NODE_IMAGE`              | not rendered    | Image registry + scaler (see 4.5)                                      |
| `ER_NODE_SCROLL_VIEW`        | renders as View | Content offset, clip to viewport, scroll gesture, momentum, `onScroll` |
| `ER_NODE_FLAT_LIST`          | not rendered    | Virtualization: render visible rows + overscan only; row recycling     |
| `ER_NODE_TEXT_INPUT`         | not rendered    | Cursor, IME callback hook, hardware keyboard input path, selection     |
| `ER_NODE_ACTIVITY_INDICATOR` | not rendered    | Animated spinner driven by built-in `Animated.loop` on rotate          |
| `ER_NODE_SWITCH`             | not rendered    | Thumb track + animated slide; bool state prop                          |

Each entry corresponds to an item to land: render case, prop fields in `ERProps`, type
in the props union, any per-node state.

## 8. Resources & Registries

- [x] Font registry + built-in Inter blob
- [x] Runtime `er_font_load` (`ERUI_FONT_POOL_BYTES`)
- [ ] **Image registry** — analogous to font registry; `er_image_load` populates, the
  Image node looks up by name.
- [ ] **Resource teardown / replace** — `er_font_load`/`er_image_load` re-registering the
  same name should release the prior entry's pool bytes (or document that it leaks).

## 9. Compile-Time Feature Flags

PLAN.md lists these. None are wired through actual `#if` guards in the source yet.

- [ ] `ERUI_SHADOWS` — gate shadow code paths.
- [ ] `ERUI_BORDER_AA` — already wired in rrect.
- [ ] `ERUI_3D_TRANSFORMS` — gate rotateX/Y/perspective.
- [ ] `ERUI_BILINEAR_SCALE` — switch image scaler kernel.
- [ ] `ERUI_GRADIENT` / `ERUI_GRADIENT_RADIAL` — gate gradient rasterizer.
- [ ] `ERUI_TRANSFORMS` — `FULL` vs `TRANSLATE_ONLY` paths.
- [ ] `ERUI_FONT_SIZES` — already drives baked font selection.
- [ ] `ERUI_MAX_NODES` — already honored.
- [ ] `ERUI_MAX_OPACITY_DEPTH`, `ERUI_SCRATCH_W/H`, `ERUI_SCRATCH_POOL_DEPTH` — for the
  scratch pool work (4.7).

## 10. Tests

Host CTest suites in [engine/tests/](engine/tests/) are green for what exists.

- [x] animation, input, layout, text, rrect
- [x] **Hit-test under opacity / display:none** (covered by existing tests)
- [ ] **Hit-test under transforms**
- [ ] **Scroll gesture + momentum**
- [x] **Layout event dispatch**
- [ ] **Spring / decay animations**
- [ ] **Sequence / parallel / stagger**
- [ ] **Image scaling + tint + resizeMode**
- [ ] **Shadow rasterizer (visual baseline comparison)**
- [ ] **Transform render + hit-test**
- [ ] **Opacity offscreen compositing**
- [ ] **Multi-line / ellipsize text**
- [ ] **Node pool free-slot reuse**

---

## Suggested Order

A path that keeps the engine demoable at each step:

1. **Finish touch** — `display:none` / `opacity:0` hit-rejection, `pointerEvents`, layout
   event dispatch. Closes the input chapter.
2. **Image rendering** — registry + nearest scaler + resizeMode. Unlocks half the React
   demo apps.
3. **ScrollView** — clip + gesture + momentum. Most apps need this before they look real.
4. **Opacity compositing + scratch pool** — required by shadows and transforms; small
   self-contained landing.
5. **Transforms (2D)** — translate fast path + affine pass + hit-test inverse.
6. **Shadows** — straightforward once the scratch pool exists.
7. **Animation engine completion** — spring, decay, easing, transform properties,
   sequence/parallel, completion callbacks.
8. **Text upgrades** — wrapping, `numberOfLines`, alignment, weight.
9. **Remaining components** — TextInput, Switch, ActivityIndicator, Modal, FlatList.
10. **Dirty-rect tracking + node pool reuse** — perf / longevity polish before MCU bring-up.
11. **Feature flag plumbing** — wrap the optional code paths in `#if` so the smallest
    target build can drop them.
12. **Test coverage** — fill the matrix in §10 as features land.

After step 9 the engine surface in `er_scene.h` should be capable of hosting the
`bridges/quickjs/` reconciler against any sample React app written for the documented
component + style subset.
