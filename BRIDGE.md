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

- [x] QuickJS compiles and links against the engine — proven by the `er-bridge-quickjs-smoke`
  target (QuickJS-ng v0.15.0 via FetchContent). Still to be folded into the `examples/linux`
  SDL host once the `NativeUI` surface exists.
- [x] A `NativeUI` global is reachable from JS (`typeof NativeUI === 'object'`)
- [x] A hardcoded JS string builds a tree: one `View` root with a child `View` and a `Text`
  (smoke test builds + commits it; layout produces the expected 200×120 root rect)
- [~] `NativeUI.commit()` drives `er_commit()` — proven headlessly (1 layout pass, dirty rect
  200×120 via a no-op backend). The **SDL window** half is pending the examples/linux host.
- [x] `console.log` from JS reaches stdout (smoke-test shim; host shim proper in §2)
- [ ] One `onPress` round-trips: SDL touch → engine hit-test → JS callback fires → JS mutates
  a prop → next commit repaints

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
- [x] Dimension coercer: JS number → `int16_t` px; `flexBasis: '50%'` → `flex_basis_pct`.
  `width/height: '%'` still has no field (Open Decisions #4)
- [x] Angle coercer: `'45deg'` / `'Nrad'` / number → degrees float
- [x] Enum string maps: all 15 done — `flexDirection`, `flexWrap`, `justifyContent`,
  `alignItems`, `alignSelf`, `position`, `display`, `overflow`, `resizeMode`, `textAlign`,
  `fontStyle`, `borderStyle`, `ellipsizeMode`, `textDecorationLine`, `pointerEvents`

**Layout (Yoga) — maps onto the layout block of `ERProps`:**

- [x] `flex` shorthand → expand to `flex_grow`/`flex_shrink`/`flex_basis` with RN semantics
  (positive flex = grow:flex, shrink:1, basis:0)
- [x] `flexGrow` / `flexShrink` / `flexBasis` (+ `flexBasis: '50%'` → `flex_basis_pct`)
- [x] `flexDirection` / `flexWrap`
- [x] `justifyContent` / `alignItems` / `alignSelf` (note: `alignContent` has no `ERProps`
  field — Open Decisions #5)
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
- [ ] Nested `<Text>` spans → `er_node_set_text_spans` (`ERTextSpan[]`, max 4)

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

- [ ] C trampoline: a single `EREventFn` that looks up the registered `JSValue` callback for
  `(node, eventType)` and calls it with a marshalled `EREventData` JS object
- [ ] GC rooting: `JS_DupValue` callbacks on register, `JS_FreeValue` on replace/node-destroy —
  prevents the collector from reclaiming a live handler
- [ ] `NativeUI.setEvent(node, eventName, fn)` → `er_event_set`; `null` fn removes
- [ ] Map RN handler names → `EREventType`: `onPress`/`onLongPress`/`onPressIn`/`onPressOut`,
  `onTouchStart`/`onTouchMove`/`onTouchEnd`/`onTouchCancel`, `onScroll`, `onChangeText`,
  `onSubmitEditing`, `onFocus`/`onBlur`, `onLayout`
- [ ] Marshal `EREventData` → JS object (`x`, `y`, `dx`, `dy`, `scroll_x/y`, `layout_rect`,
  `changed_text`)
- [ ] Gesture responder queries (`er_responder_query_set`) — defer until PanResponder lands;
  note here so it isn't forgotten

### 1.4 Animated (useNativeDriver path) over `ERAnimValueHandle`

- [ ] `NativeUI.createAnimatedValue(initial)` → `er_anim_value_create`
- [ ] `NativeUI.bindAnimatedValue(handle, node, propName)` → `er_anim_value_bind`
- [ ] `NativeUI.animateValue(handle, toValue, configObj)` → `er_anim_value_animate`
  (marshal `ERAnimConfig`: type, easing, duration, spring stiffness/damping/mass, delay, loop)
- [ ] `NativeUI.setAnimatedValue` / `getAnimatedValue` → `er_anim_value_set` / `_get`
- [ ] Interpolation: `value.interpolate({inputRange, outputRange, extrapolate})` →
  `er_anim_value_bind_interpolated` (marshal `ERInterpolation`)
- [ ] Map animatable prop names → `ERAnimProp` (opacity, translateX/Y, scaleX/Y, rotateZ,
  backgroundColor, color, …)
- [ ] LayoutAnimation: `NativeUI.configureNextLayoutAnimation(cfg)` →
  `er_layout_anim_configure_next`

### 1.5 Asset & font registration

- [ ] `NativeUI.loadImage(name, bytes, w, h)` → `er_image_load` (premultiplied ARGB8888;
  buffer lifetime is caller-owned — keep it rooted)
- [ ] `NativeUI.loadFont(name, bytes)` → `er_font_load` (requires `ERUI_FONT_POOL_BYTES > 0`)

---

## 2. QuickJS Integration — `examples/linux` host

Embed the interpreter and drive it from the existing SDL frame loop. The SDL backend and
`embedded_renderer_tick()` loop you already have stay unchanged; we replace the hand-built
C scene with "run the JS bundle, then tick."

- [ ] Pull in QuickJS — vendored vs. FetchContent ([Open Decisions](#open-decisions))
- [ ] `JSRuntime` / `JSContext` init with a configured heap size; teardown on exit
- [ ] Register the `NativeUI` bridge into the global object at startup
- [ ] `console.log` / `console.warn` / `console.error` shims → stdout/stderr
- [ ] Pump the QuickJS job queue (Promises/microtasks) once per frame
- [ ] `setTimeout` / `setInterval` shim driven off `er_now_ms` (needed for non-native-driver
  `Animated` and React scheduling)
- [ ] Load + evaluate a JS bundle from a file path (dev), then from an embedded byte array
  (firmware parity)
- [ ] Bytecode path: precompile bundle → `qjsc` bytecode, embed as a C array, load on boot
- [ ] Frame loop order per tick: drain JS jobs → `embedded_renderer_tick(dt)` → SDL present
- [ ] Feed SDL touch/keyboard into `embedded_renderer_touch` (already wired in the C demo)
- [ ] Error reporting: surface uncaught JS exceptions with stack traces to stderr

---

## 3. React Reconciler — JS host config

Pure JS. A `react-reconciler` host config mapping React's mutation API onto `NativeUI.*`.
Mostly ported from React Native's renderer; lives entirely on the JS side, no C.

- [ ] Vendor `react` + `react-reconciler` into the JS toolchain
- [ ] `createInstance(type, props)` → `NativeUI.createNode` + initial `setProps`
- [ ] `createTextInstance` — raw text only legal inside `<Text>`; handle string children
- [ ] `appendChild` / `appendInitialChild` / `appendChildToContainer`
- [ ] `insertBefore` / `removeChild` / `removeChildFromContainer`
- [ ] `prepareUpdate` / `commitUpdate` — diff prop bag → `NativeUI.setProps`
- [ ] `commitTextUpdate` for `<Text>` content changes
- [ ] `finalizeInitialChildren`, `shouldSetTextContent`, host-context stubs
- [ ] `prepareForCommit` / `resetAfterCommit` → call `NativeUI.commit()` once per React commit
- [ ] Event prop wiring: reconciler routes `onPress`-style props to `NativeUI.setEvent`
- [ ] `render(<App/>, container)` entry point creates the root container node

---

## 4. `'embedded-react'` Module + Bundler

The developer-facing JS package and the build step that turns `App.jsx` into something
QuickJS runs. This is what makes `import { View } from 'embedded-react'` resolve.

- [ ] `'embedded-react'` module: re-export `View`, `Text`, `Image`, `ScrollView`, `FlatList`,
  `Pressable`, `TouchableOpacity`, `TextInput`, `ActivityIndicator`, `Switch`, `Modal`
- [ ] `Animated.View` / `.Text` / `.Image` + `Animated.Value` / `timing` / `spring` / `decay` /
  `sequence` / `parallel` / `loop` / `stagger` / `delay`
- [ ] `Easing` module mapping to `ERAnimEasing`
- [ ] `useAnimatedValue` hook over the native value handle
- [ ] `StyleSheet.create` (identity/validate pass-through)
- [ ] Hooks come from React core (`useState`/`useEffect`/`useRef`/`useMemo`/`useCallback`/
  `useContext`) — verify they run under QuickJS unmodified
- [ ] Babel config: JSX transform + RN preset subset
- [ ] Metro-compatible bundler resolving `'embedded-react'` to the module shim, emitting one
  bundle
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
4. **Percentage dimensions** — only `flexBasis: '%'` has an `ERProps` field today
   (`flex_basis_pct`). `width: '50%'` / `height: '50%'` have no field. Decide: document as
   unsupported, or revisit with the engine owner. (Engine change — out of bounds for this doc
   without sign-off.)
5. **`alignContent`** — listed in PLAN.md's style table but has no `ERProps` field. Same
   choice as #4.
6. **Gradient style key** — gradients are an engine feature with no RN prop. Pick a custom
   style key (`experimental_gradient`?) and shape so app authors can reach it.
7. **Non-native-driver animation** — `Animated` without `useNativeDriver` needs a JS-side rAF
   loop calling `setValue` each frame. Confirm we support it at all, or require
   `useNativeDriver: true` (which RN-on-MCU wants regardless).

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
