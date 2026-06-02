# Flow A Bridge — Build Tracker

The working checklist for wiring **React → QuickJS → `er_scene.h`** (Flow A in
[PLAN.md](PLAN.md)). PLAN.md owns the architecture and the *why*; this doc owns the
*what's-done* and the order we build it in. When every box here is checked, `App.jsx`
runs on the SDL desktop target and — with only a backend swap — on an MCU.

The engine is frozen as a dependency: nothing below changes anything under `engine/`.
Every React-facing concept must reduce to a call already declared in
[er_scene.h](engine/include/er_scene.h). If something can't, that's a gap to record in
[Open Decisions](#open-decisions), not a reason to edit the engine.

Checkbox convention: `[x]` = shipped and exercised by a running JS program; `[~]` = partially
landed (the parenthetical says what remains); `[ ]` = outstanding. Deferred-on-purpose items are
described in prose under [Out of Scope (for now)](#out-of-scope-for-now), never as unchecked boxes.

---

## 0. Vertical Slice — the first thing to finish

Before widening any checklist below, get the **thinnest possible JS→engine path** running
end-to-end on `backends/sdl/`. This de-risks the marshalling layer before we invest in the
reconciler and build tooling. No React, no JSX, no bundler yet — just a hardcoded JS string
calling the bridge directly.

- [x] QuickJS compiles and links against the engine — `er-bridge-quickjs-smoke` (headless) and
  now the `embedded-react-desktop-js` SDL host (QuickJS-ng v0.15.0 via FetchContent)
- [x] A `NativeUI` global is reachable from JS (`typeof NativeUI === 'object'`)
- [x] A hardcoded JS string builds a tree: one `View` root with a child `View` and a `Text`
  (smoke test builds + commits it; layout produces the expected 200×120 root rect)
- [x] `NativeUI.commit()` drives `er_commit()` and the **SDL window shows the result** — the
  `embedded-react-desktop-js` target boots QuickJS, runs a JS app, and paints it via the SDL
  backend (verified: app evaluates, builds the tree, frame loop presents)
- [x] `console.log` from JS reaches stdout (smoke-test shim; host shim proper in §2)
- [x] One `onPress` round-trips: touch → engine hit-test → JS callback fires → JS mutates a
  prop → next commit repaints (smoke verifies it headlessly: tap → `onPress` → JS counter
  increments; desktop demo cards are `Pressable`s that update the subtitle on tap)

Hitting this milestone proves the five hard problems are solved in miniature: QuickJS
embedding, the `NativeUI` surface, `ERProps` marshalling, the frame loop, and the
JS↔C event trampoline. Everything after is breadth, not risk.

---

## 1. Native Bridge — `bridges/quickjs/native_ui_bridge.c`

The keystone. A thin (~300-line target) C adapter that publishes a `NativeUI` object into
the QuickJS global scope and forwards each method to `er_scene.h`. The only genuinely new
C in the whole bridge. Keep the engine ABI as the hard boundary — this file may include
`er_scene.h` and `quickjs.h` and nothing else from either side's internals.

### 1.1 Lifecycle & node handle model

- [x] Decide the JS→`ERNode*` handle representation — **integer handle table** (Open Decisions #2)
- [x] `NativeUI.createNode(typeString)` → `er_node_create`; all 10 `ERNodeType`s mapped
- [x] `NativeUI.destroyNode(node)` → `er_node_destroy` (frees the handle slot)
- [x] `NativeUI.appendChild(parent, child)` → `er_tree_append_child`
- [ ] `NativeUI.insertBefore(parent, child, beforeChild)` — engine has append+remove only.
  Plan: JS reconciler re-appends the tail (it owns child order); revisit if an engine
  `er_tree_insert_before` primitive proves cheaper. Not needed for static trees.
- [x] `NativeUI.removeChild(parent, child)` → `er_tree_remove_child`
- [x] `NativeUI.setRoot(node)` → `er_tree_set_root`
- [x] `NativeUI.commit()` → `er_commit`
- [x] `NativeUI.now()` → `er_now_ms`

### 1.2 Props marshaling — `propsObject → ERProps`

The bulk of the work. Translate a JS style/props object into the flat `ERProps` struct
(~120 fields). Start from a zero-init `ERProps` with all `int16_t` layout fields set to
`ER_LAYOUT_AUTO`, then overwrite only the keys present. Group-by-group below; every box is
one prop family verified against a JS value.

**Shared value coercers (build these first — everything else depends on them):**

- [x] Color parser: `'#rgb'` / `'#rrggbb'` / `'#rrggbbaa'` / `'rgba()'` / `'rgb()'` / 12 named
  colors + raw numbers → straight-alpha `uint32_t` ARGB8888
- [x] Dimension coercer: JS number → `int16_t` px; percent strings → the matching `_pct` field
  (`flexBasis: '50%'`, `width: '50%'`, `height: '100%'`). Padding/margin/min/max % still pixels-only
- [x] Angle coercer: `'45deg'` / `'Nrad'` / number → degrees float
- [x] Enum string maps: all 16 done — `flexDirection`, `flexWrap`, `justifyContent`,
  `alignItems`, `alignSelf`, `alignContent`, `position`, `display`, `overflow`, `resizeMode`,
  `textAlign`, `fontStyle`, `borderStyle`, `ellipsizeMode`, `textDecorationLine`, `pointerEvents`

**Layout (Yoga) — maps onto the layout block of `ERProps`:**

- [x] `flex` shorthand → expand to `flex_grow`/`flex_shrink`/`flex_basis` with RN semantics
  (positive flex = grow:flex, shrink:1, basis:0)
- [x] `flexGrow` / `flexShrink` / `flexBasis` (+ `flexBasis: '50%'` → `flex_basis_pct`).
  ⚠️ Fixed a marshaling bug here: `props_init_defaults` was seeding `flex_grow`/`flex_shrink` to
  `ER_LAYOUT_AUTO` (`INT16_MIN`) instead of **0**. The engine reads any non-zero `flex_grow` as a
  real factor, so *every* node without explicit flex silently grew to fill its parent (a fixed-size
  child filled the root; un-flexed siblings each grabbed 1/N). `flex_grow`/`flex_shrink` now default 0
- [x] `flexDirection` / `flexWrap`
- [x] `justifyContent` / `alignItems` / `alignSelf` / `alignContent`
- [x] `position` + `top` / `right` / `bottom` / `left`
- [x] `width` / `height` / `minWidth` / `maxWidth` / `minHeight` / `maxHeight`
- [x] `aspectRatio` → `float aspect_ratio`
- [x] `margin` (+ per-edge, `marginHorizontal`, `marginVertical`)
- [x] `padding` (+ per-edge, `paddingHorizontal`, `paddingVertical`)
- [x] `gap` / `rowGap` / `columnGap`
- [x] `display` / `overflow`

**View visual:**

- [x] `backgroundColor` / `opacity` (RN `0.0–1.0` float → `ERProps.opacity` `uint8 0–255`)
- [x] `borderRadius` (+ four per-corner radii)
- [x] `borderWidth` (+ four per-edge widths) / `borderColor` (+ four per-edge colors)
- [x] `borderStyle`
- [x] `zIndex`
- [x] `shadowColor` / `shadowOffset` (`{width,height}` object → `shadow_offset_x/y`) /
  `shadowOpacity` / `shadowRadius` / `elevation` (marshalled; rendered only when `ERUI_SHADOWS`)

**Text (some are props, not style, in RN):**

- [x] Text content: passed via a flat `text` key → `ERProps.text` (reconciler will route
  `<Text>` string children to it)
- [x] `color` / `fontSize` / `fontFamily` / `fontWeight` / `fontStyle`
- [x] `lineHeight` / `letterSpacing` / `textAlign` / `textDecorationLine`
- [x] `numberOfLines` / `ellipsizeMode`
- [x] Multi-child `<Text>` + nested spans → `er_node_set_text_spans` (`ERTextSpan[]`, max 4).
  `shouldSetTextContent` now owns any flattenable `<Text>` subtree (strings, interpolation,
  nested `<Text>`); the host config flattens it into the node's `text` (full string) plus, when the
  runs differ in style, a span array marshalled by `NativeUI.setTextSpans` (per-span color / fontSize
  / fontWeight / fontStyle / textDecorationLine / letterSpacing, sentinels inherit the base). Uniform
  text takes the plain-text path (no spans). A non-`Text` element child falls back to mounted
  instances. Verified by `text-spans` unit (14 cases) + runtime tests

**Image:**

- [~] `imageName` key → `ERProps.image_name` done; `source`-object resolution + `er_image_load`
  registration pending (§1.5)
- [x] `resizeMode` / `tintColor`

**Transforms — RN `transform` is an array of single-key objects:**

- [x] Flatten `[{translateX:5},{rotate:'45deg'},{scale:2}]` into `transform_*` fields
- [x] `translateX` / `translateY` / `scaleX` / `scaleY` / `scale` (→ both scale fields)
- [x] `rotate` / `rotateZ` (angle); `rotateX` / `rotateY` / `perspective` — marshaled into the
  fields regardless; engine honors scale/rotate only with `ERUI_TRANSFORMS=FULL`, and
  `rotateX/Y`/`perspective` only with `ERUI_3D_TRANSFORMS`
- [x] `transformOrigin` → `transform_origin_x/y` (two-element `[x, y]` fractional array; default
  pivot seeded to 0.5/0.5 to match RN's centre origin)

**Component-specific props:**

- [x] `ActivityIndicator`: `color` → `indicator_color`, `animating` (defaults to 1)
- [x] `Switch`: `value` → `switch_value`, `trackColor` (`{false,true}`) → `track_color_*`,
  `thumbColor`
- [~] `TextInput`: `placeholder` / `placeholderTextColor` / `editable` / `cursorColor` done;
  controlled `value` deferred — it routes through `er_text_input_set_text`, not `ERProps` (lands
  with events/controlled input)
- [~] `Modal`: `visible` → `modal_visible`, `backdropColor` done; `transparent` has no `ERProps`
  field (handle via backdrop alpha at the JS layer, or record as a gap)
- [ ] Gradient (engine extra, no RN equivalent): `gradient_type` / `gradient_angle` /
  `gradient_stops` — needs a custom style-key decision (Open Decisions #6)

### 1.3 Events — JS callbacks over `er_event_set`

- [x] C trampoline: a single `EREventFn` looks up the JS callback for `(node, eventType)`
  (the pair is encoded in `er_event_set`'s `user_data`) and calls it with a marshalled event
  object; handler exceptions are reported and swallowed, not unwound through the engine
- [x] GC rooting: callbacks stored in a `NativeUI.__er_event_handlers` registry object owned by
  the global graph; `JS_DupValue` on register, replaced/cleared entries release their ref;
  `destroyNode` clears a node's handlers
- [x] `NativeUI.setEvent(node, eventName, fn)` → `er_event_set`; a non-function fn clears it
- [x] Map RN handler names → `EREventType`: all of `onPress`/`onLongPress`/`onPressIn`/`onPressOut`,
  `onTouchStart`/`onTouchMove`/`onTouchEnd`/`onTouchCancel`, `onScroll`, `onChangeText`,
  `onSubmitEditing`, `onFocus`/`onBlur`, `onLayout`
- [x] Marshal `EREventData` → JS object (`type`, `x`, `y`, `dx`, `dy`; `scrollX/Y` for scroll,
  `layout` rect for layout, `text` for changeText)
- [ ] Gesture responder queries (`er_responder_query_set`) — deferred until PanResponder lands

### 1.4 Animated (useNativeDriver path) over `ERAnimValueHandle`

Native driver: the JS `Animated.Value` is a handle to an engine-side float; binding it to a node
prop lets the engine advance the animation each frame with **no per-frame JS**. JS module:
`src/embedded-react/Animated.js` + `Easing.js`. Verified by `animated.runtime.test.jsx` (linear
0→100 advances to mid=50, end=100 via `NativeUI.tick`).

- [x] `NativeUI.animValueCreate(initial)` → `er_anim_value_create`
- [x] `NativeUI.animValueBind(handle, node, propName)` → `er_anim_value_bind`;
  `animValueBindInterpolated(...)` → `er_anim_value_bind_interpolated`
- [x] `NativeUI.animValueAnimate(handle, toValue, configObj)` → `er_anim_value_animate`
  (marshals `ERAnimConfig`: type timing/spring/decay, easing token or bezier, duration, spring
  stiffness/damping/mass/velocity, decay deceleration, delay, loop)
- [x] `NativeUI.animValueSet` / `animValueGet` → `er_anim_value_set` / `_get`; `animStop`; `tick`
- [x] Interpolation: `value.interpolate({inputRange, outputRange})` →
  `er_anim_value_bind_interpolated`. `extrapolate` clamp/identity not wired yet (defaults extend)
- [x] Map animatable prop names → `ERAnimProp` (opacity, translateX/Y, scaleX/Y, scale, rotate/Z/X/Y,
  backgroundColor, color)
- [x] JS `Animated.View` / `.Text` / `.Image` — `createAnimatedComponent` splits animated style
  props (incl. `transform` array) and binds them via a host-instance ref on mount
- [x] `Animated.timing` / `spring` / `decay`, plus composition: `sequence` / `parallel` (with
  `stopTogether`) / `stagger` / `delay` / `loop` (with `iterations`, resets the value per
  iteration). `.start(callback)` completion is wired through the engine's `on_complete` via a
  bridge trampoline (4th arg to `animValueAnimate`): `{ finished }` is `true` on natural end, `false`
  when superseded by a new animation or `stop()`. Composition is pure JS over each child's
  start/stop + the §2 timer globals; every leg still runs in C (native driver). Verified by
  `anim-compose.runtime.test.js`
- [ ] LayoutAnimation: `NativeUI.configureNextLayoutAnimation(cfg)` →
  `er_layout_anim_configure_next`

Engine touch (no-regression): `er_anim_value_bind` now applies the value's current value on bind
(matching the interpolated path); the interpolated bind gained the same duplicate-(node,prop) guard
the plain bind had — so the reconciler can re-bind on every render without flicker or pool
exhaustion.

### 1.5 Asset & font registration

- [ ] `NativeUI.loadImage(name, bytes, w, h)` → `er_image_load` (premultiplied ARGB8888;
  buffer lifetime is caller-owned — keep it rooted)
- [ ] `NativeUI.loadFont(name, bytes)` → `er_font_load` (requires `ERUI_FONT_POOL_BYTES > 0`)

---

## 2. QuickJS Integration — `examples/linux` host

Embed the interpreter and drive it from the existing SDL frame loop. The SDL backend and
`embedded_renderer_tick()` loop you already have stay unchanged; we replace the hand-built
C scene with "run the JS bundle, then tick."

- [x] Pull in QuickJS — FetchContent, pinned v0.15.0 (the bridge's CMakeLists; the example
  `add_subdirectory`s it)
- [x] `JSRuntime` / `JSContext` init + teardown on exit (`main_js.c`)
- [x] Register the `NativeUI` bridge into the global object at startup
- [x] `console.log` / `console.warn` / `console.error` shims → stdout
- [x] Pump the QuickJS job queue (Promises/microtasks) once per frame — `er_bridge_pump()` drains
  `JS_ExecutePendingJob` before and after firing timers; called by the desktop loop and by
  `NativeUI.tick()` (so JS-driven loops / tests pump too)
- [x] `setTimeout` / `setInterval` shim driven off `er_now_ms` — fixed timer pool in the bridge,
  callbacks GC-rooted in a `__er_timer_handlers` registry (auto-freed at teardown). `setTimeout` /
  `setInterval` / `clearTimeout` / `clearInterval` installed as globals; extra args forwarded.
  Verified by `timers.runtime.test.js` and `effects.runtime.test.jsx` (**`useEffect` now flushes** —
  mount, deps, no-spurious-rerun, cleanup-on-unmount)
- [x] Load + evaluate a JS app from a file path (`argv[1]`) with a built-in default app;
  embedded-byte-array / bytecode parity still to come
- [ ] Bytecode path: precompile bundle → `qjsc` bytecode, embed as a C array, load on boot
- [x] Frame loop: `er_bridge_pump()` → `er_commit()` → `er_sdl_present()` → `embedded_renderer_tick(dt)`
  each frame; the pump services Promises + timers before painting so async/timer state lands in the
  same frame's commit
- [~] Feed SDL touch into `embedded_renderer_touch` (mouse→touch wired); keyboard feed still TODO
- [x] Error reporting: uncaught JS exceptions printed to stderr with `.stack`

---

## 3. React Reconciler — JS host config

Pure JS in `bridges/quickjs/js/` (react 18.3 + react-reconciler 0.29, bundled with esbuild to a
single IIFE the QuickJS host runs). **Proven end-to-end** — `<App/>` mounts through the reconciler,
state updates re-render (counter), and keyed-list **reordering** moves nodes correctly. The desktop
demo (`embedded-react-desktop-js`) runs the bundle by default and is interactive.

- [x] Vendor `react` + `react-reconciler` (npm; `bridges/quickjs/js/package.json`)
- [x] `createInstance(type, props)` → `NativeUI.createNode` + `setProps` (style flattened, on* routed)
- [x] `createTextInstance` — fallback Text node; `<Text>string</Text>` handled via `shouldSetTextContent`
- [x] `appendChild` / `appendInitialChild` / `appendChildToContainer`
- [x] `insertBefore` / `removeChild` / `removeChildFromContainer` — `NativeUI.insertBefore` added,
  backed by engine `er_tree_insert_before`; append/insert/remove are move-safe (detach-then-splice).
  Verified by a keyed-list reverse-driving correct node moves (`src/reorder-test.jsx`)
- [x] `prepareUpdate` / `commitUpdate` → `NativeUI.setProps`
- [x] `commitTextUpdate` for `<Text>` content changes
- [x] `finalizeInitialChildren`, `shouldSetTextContent`, host-context stubs
- [x] `prepareForCommit` / `resetAfterCommit` → `NativeUI.commit()` once per React commit
- [x] Event prop wiring: `on*` props → `NativeUI.setEvent` (set on create/update, cleared when removed)
- [x] `createRoot(props).render(<App/>)` entry — screen-sized container set as scene root, LegacyRoot (sync)

---

## 4. `'embedded-react'` Module + Bundler

The developer-facing JS package and the build step that turns `App.jsx` into something
QuickJS runs. `import { View } from 'embedded-react'` resolves via a Node **package
self-reference** (`package.json` `name: "embedded-react"` + `exports`) — works in esbuild and
Vitest with no aliases. Public surface lives in `src/embedded-react/`.

- [x] `'embedded-react'` module: re-export `View`, `Text`, `Image`, `ScrollView`, `FlatList`,
  `Pressable`, `TouchableOpacity`, `TextInput`, `ActivityIndicator`, `Switch`, `Modal`
- [x] `Animated.View` / `.Text` / `.Image` + `Animated.Value` / `timing` / `spring` / `decay` +
  composition (`sequence` / `parallel` / `stagger` / `delay` / `loop`) and `.start(callback)` (§1.4)
- [x] `Easing` module — tokens mapping to `ERAnimEasing` (+ `Easing.bezier`)
- [ ] `useAnimatedValue` hook over the native value handle
- [x] `StyleSheet.create` (identity pass-through) + `flatten`; also `Platform.OS`/`select`
- [x] Hooks come from React core (`useState` verified running under QuickJS); apps import hooks
  from `react`, components/APIs from `embedded-react` (RN convention)
- [x] JSX transform — esbuild `jsx: 'automatic'` (no Babel needed)
- [x] Bundler resolving `'embedded-react'` → single IIFE (esbuild self-reference; the
  Metro-compatible story can layer on later, but esbuild covers the bundle role now)
- [x] `AppRegistry.registerComponent(name, () => App)` entry (RN idiom; mounts into a
  screen-sized root immediately, since running the bundle = starting the app)
- [ ] `qjsc` bytecode compile step wired into the example build
- [ ] `npx create-embedded-react` scaffold (last — nice-to-have, not slice-critical)

---

## Open Decisions

Resolve these as we reach them; each blocks a checklist item above.

1. **QuickJS sourcing** — ✅ **RESOLVED: CMake FetchContent with a pinned tag.** Matches the
   engine's distribution story; pinned for reproducible builds.
2. **JS→`ERNode*` handle model** — ✅ **RESOLVED: integer handle table.** JS holds an integer
   index into a C-side table; explicit `destroyNode` frees the slot for reuse. Simpler than a
   GC finalizer and avoids finalizer-ordering traps.
3. **Color parsing scope** — full CSS color set vs. the subset RN actually documents
   (`#rgb`, `#rrggbb`, `#rrggbbaa`, `rgb()`, `rgba()`, named). *Leaning: RN subset.*
4. **Percentage dimensions** — ✅ **RESOLVED: `width`/`height` percentages implemented.** Added
   `width_pct`/`height_pct` to `ERProps`/`ERLayoutSpec`, resolved against the parent's content box
   in layout Pass 1 (and respected by Pass 5 stretch), plus bridge marshalling and parity
   fixtures. Percentage padding/margin/min/max/position are still pixels-only (lower frequency).
5. **`alignContent`** — ✅ **RESOLVED: implemented.** Added the `ERAlignContent` enum + `ERProps`/
   `ERLayoutSpec` field + Pass 4/5 cross-line distribution in the engine, plus `alignContent`
   marshalling and parity fixtures (center / space-between / stretch).
6. **Gradient style key** — gradients are an engine feature with no RN prop. Pick a custom
   style key (`experimental_gradient`?) and shape so app authors can reach it.
7. **Non-native-driver animation** — `Animated` without `useNativeDriver` needs a JS-side rAF
   loop calling `setValue` each frame. Confirm we support it at all, or require
   `useNativeDriver: true` (which RN-on-MCU wants regardless).

---

## Engine Gaps Found via Flow A

Behaviors in the engine that diverged from React Native parity, surfaced while building the
bridge. Policy (per project owner): **fix them in the engine as we hit them** — nobody is on the
old behavior yet. **None were bridge bugs.**

**Parity harness:** `engine/tests/layout/test_yoga_parity.c` (CTest `yoga_parity`) compares the
engine's computed rects against Yoga/Chrome-correct values. Each assertion is tagged `EXPECT`
(must match — catches regressions) or `XFAIL` (known divergence — stays green while broken, but
turns the suite **red when the engine starts matching**, as a reminder to promote it). Add a
`fixture_*()` per case. This is how we make divergences deterministic instead of eyeballing the
demo. Current: 31 EXPECT pass, 0 XFAIL.

- ✅ **FIXED — container auto cross/main-size.** A `flexDirection: 'row'` View with no explicit
  `height`, whose children set their own height, collapsed to `height: 0` instead of sizing to
  the tallest child (and the column analogue didn't sum children's heights). Root cause: the
  layout engine only computed intrinsic size for `ER_NODE_TEXT` leaves; every other node defaulted
  to intrinsic 0, so an auto-sized container with no `flex_basis` got `hypo_main = 0`. Children
  still rendered (overflowing the 0-height parent) but were **not hit-testable** (hit-testing
  descends through the parent's empty bounds first) — why the desktop demo's card row wasn't
  clickable. **Fix:** added `measure_content()` in `engine/layout/layout_engine.c` — a
  self-contained (no `s_scratch`) recursive content-size measure used by Pass 1 for all node
  types; a container now sums children along its main axis and takes the max along its cross axis
  (+ padding). Covered by `tests/layout` (`auto cross-size`) and parity fixtures
  `auto-cross-row` / `auto-main-col`. All engine CTest suites pass; the demo needs no workaround.

- ✅ **FIXED — iterative flex resolution (parity `flex-max-redist`, now `EXPECT`).** When a flex
  child hits its `min`/`max`, Yoga freezes it and **redistributes** the freed space to other
  growable siblings; the engine previously did a single grow/shrink pass and left the space unused.
  **Fix:** rewrote Pass 3 in `layout_engine.c` as Yoga's resolve-flexible-lengths loop — distribute
  free space over unfrozen items, freeze any that hit a bound, redistribute the remainder, repeat
  (bounded by child count). Found and promoted via the parity harness (XFAIL → PROMOTE → EXPECT).

- ✅ **FIXED — `alignContent`** (parity `aligncontent-center` / `-between` / `-stretch`). Added
  the `ERAlignContent` enum, the `ERProps`/`ERLayoutSpec` field (+ compositor copy/default), and
  cross-line distribution between Pass 4 and Pass 5 (offset / between-line spacing / per-line
  stretch). Bridge marshals `alignContent`. All suites pass.

- ✅ **FIXED — percentage `width`/`height`** (parity `pct-width-main` / `pct-height-main` /
  `pct-width-cross` / `pct-content-box`). Added `width_pct`/`height_pct` (`ERProps`/`ERLayoutSpec`
  + compositor), resolved against the parent content box in Pass 1, respected by Pass 5 stretch;
  bridge marshals `'N%'` strings. Percentage padding/margin/min/max/position still pixels-only.

- ✅ **FIXED — bold / span text measured narrower than rendered (trailing-glyph clipping).**
  `er_text_measure` summed plain glyph advances, but the renderer synthesises **bold** by drawing
  each glyph twice 1px apart and advances the cursor `+1px/glyph` to match — so a bold (or bold-span)
  run rendered wider than its measured box and the last glyph(s) clipped. Surfaced by the demo's
  `uptime <Text bold>{n}s</Text>`: the trailing "s" dropped as the digit count grew. **Fix
  (engine/text):** `er_text_measure` gained a `font_weight` arg and adds the same `+1px/glyph` for
  bold; new `er_text_measure_spans()` measures a span run exactly as the renderer draws it (base
  font, per-span weight/letter-spacing from sentinels). `measure_content` (layout) now calls the
  span-aware path when `span_count > 0`, else the weight-aware plain path; the TextInput cursor
  measure passes its weight too. Covered by `test_text` (bold = normal + glyph-count; span == plain;
  bold-span == bold; two-span == joined). No `ERProps`/API surface change beyond the measure args.

---

## Out of Scope (for now)

Deliberately deferred so the slice stays small; revisit after the vertical slice is green.

- **Flow B AOT compiler** — separate effort; same `er_scene.h` target (PLAN.md).
- **PanResponder / gesture responder JS API** — engine support exists
  (`er_responder_query_set`); JS surface deferred until after basic events work.
- **`NavigationContainer` / `Stack.Navigator`** — pure-JS libraries on top of the component
  set; land after the component set is proven.
- **Hot reload / Fast Refresh** — dev-experience polish, post-slice.
- **MCU bring-up** (`examples/esp32`, `examples/stm32h7`) — only a backend swap once the SDL
  slice runs, but not part of bridge bring-up.

---

## Build Order Summary

1. **Vertical slice (§0)** — prove the path with hardcoded JS.
2. **Bridge breadth (§1)** — fill out marshalling, events, Animated, assets.
3. **QuickJS host polish (§2)** — job queue, timers, bytecode, error reporting.
4. **Reconciler (§3)** — React drives the bridge instead of hand-written JS.
5. **Module + bundler (§4)** — real `App.jsx` with `import 'embedded-react'`.
6. Widen to MCU backends (out of scope here; backend swap only).
   </content>
   </invoke>
