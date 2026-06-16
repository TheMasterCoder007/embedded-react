# Roadmap

The bulk of embedded-react is built and verified — see the **Status** and **Working
examples** tables in the [README](README.md). This file is the single home for what's
left: known issues, planned work toward 1.0, the performance backlog, and the
longer-term vision.

It is intentionally honest. If something here looks unfinished, it is — the engine,
both flows, the backends that run on hardware, and the simulators all work today, but
this is a beta. From here on the work is fixes and feature additions, not
re-architecture.

---

## Known issues & caveats

- **AOT animation `toValue` uses a snapshot, not live state.** A Flow B (`Animated`)
  animation whose `toValue` depends on current state may animate toward the value
  captured at compile/setup time rather than the live one. Tracked for the imperative /
  animation hardening pass.
- **Simulator asset-pack reload leaks buffers.** The engine has no asset *unregister*,
  so each hot-reload of an image/font keeps the previous pack's buffers alive (a few KB
  per changed asset). Harmless for a dev session; never happens on-device.
- **Browser host re-creates its `HEAPU8` view every frame.** WebAssembly memory growth
  detaches typed-array views, so the WASM simulator's present layer re-derives the view
  on each present. This is a workaround, in place and working — noted so it isn't
  "simplified" away.
- **Text is re-measured whenever a commit runs layout.** The layout-dirty flag gates the
  layout pass, but any commit that *does* run layout re-measures every `<Text>` node,
  even if the change was unrelated. This is the top remaining per-frame layout cost (see
  the performance backlog).
- **Static RAM footprint is desktop-sized by default.** The opacity/transform/shadow
  scratch pools are sized to the framebuffer; at desktop defaults they total well over a
  megabyte. Tune the `ERUI_*` flags (scratch dimensions, `ERUI_MAX_OPACITY_DEPTH`,
  shadows/transforms) down for a board — the defaults are not board defaults.
- **Performance ceiling on JS-driven drag (Flow A).** Continuous drag (e.g., the
  thermostat dial) tops out around ~24 fps on PSRAM-QuickJS hardware; the bottleneck is
  per-event JS dispatch, not rendering. 30 fps needs a native-side drag path or the AOT
  flow. Native-driver animations are unaffected (they never enter JS per frame).

---

## Near-term (beta → 1.0)

### Backends

The backends that run on hardware (`sdl`, `esp32-lcd`, `esp32-spi-lcd`) and power the
simulators (`software`, `web`) are done. The remaining backends are stubs / README-only:

- **`backends/dma2d/`** — STM32 DMA2D hardware blitter. Pairs with the STM32H7 bring-up.
- **`backends/framebuffer/`** — Linux `/dev/fb0` direct framebuffer (embedded Linux SBCs).
- **`backends/opengl/`** — OpenGL ES 2.0 (Raspberry Pi, Android).

### Examples / board bring-up

Four examples run end-to-end (see README). These are README-only and need wiring:

- **`examples/stm32h7/`** — first STM32 bring-up (with `backends/dma2d/`).
- **`examples/raspberry-pi/`** — RPi reference app (with `backends/framebuffer/` or `opengl`).
- **`examples/dashboard-demo/`**, **`examples/marine-display/`** — larger reference apps.

### Flow A — React on QuickJS (runtime)

- **On-device hot reload.** A dev-server / socket transport that pushes fresh bytecode to
  the board over serial / Wi-Fi (OTA), so the device gets the same edit-save-see loop the
  simulator has. The "later luxury", deferred deliberately.
- **Runtime asset registration.** A `NativeUI.loadImage` / `loadFont` path for
  dynamically generated or OTA-delivered assets. `er_font_load` (blob → pool) already
  exists for this future case; the JS-facing API does not.
- **SDL keyboard → input.** Mouse is forwarded as touch; hardware keyboard feed into the
  engine is still TODO (needed for `TextInput` on the desktop host).

### Flow B — AOT JSX→C (compile-time)

- Resolve the live-vs-snapshot `toValue` gap (see Known issues).
- Imperative refs / `Animated` sequence / parallel / loop / interpolate hardening on the
  no-PSRAM hardware path.
- Thermostat capstone under a frame-time gate; ping-pong band buffers for banded RGB565.

### Engine features

- **`FlatList` virtualization.** Currently scrolls like a `ScrollView` and renders all
  children; windowing (mount only the visible range + overscan) is the next step.
- **Canvas API** (`canvas_bindings.c`) — deliberately a stub; land only if the bundled
  React surface needs it.
- **Ellipsize modes** — `tail` and `clip` ship; `head` and `middle` are deferred.
- **`textAlign: justify`** — left/center/right ship; justified text deferred.
- **Per-container layout-dirty.** The dirty flag is whole-tree and conservative
  (`er_node_set_props` raises it even for a pure visual change). Next: compare
  layout-relevant fields and re-solve only the changed subtrees.

### Open API decisions

- **Gradient style key.** Gradients are an engine primitive with no React Native prop;
  pick a custom style key (`experimental_gradient`?) and shape for app authors.
- **Non-native-driver animation.** Decide whether to support `Animated` without
  `useNativeDriver` (needs a JS-side rAF loop calling `setValue` each frame) or require
  `useNativeDriver: true`.
- **Controlled `TextInput` value.** `placeholder` / `placeholderTextColor` / `editable` /
  `cursorColor` are done; a controlled `value` still routes through
  `er_text_input_set_text`, not `ERProps`.
- **`Modal` `transparent` prop.** No `ERProps` field yet — handled via backdrop alpha at
  the JS layer; formalize or record as a gap.

### Tooling / simulators

- **Prebuilt / auto-built SDL simulator binary.** Today `npm run sim` needs a one-time
  CMake build of the sim binary. A scaffold + prebuilt/auto-built binary would remove the
  step. (The WASM simulator already ships prebuilt.)
- **Fast Refresh.** The SDL/WASM simulators preserve `useState` across reloads via a
  Babel transform; full per-component Fast Refresh (no keying fragility) is a later
  upgrade.
- **Workspace packages.** The JS layer incubates in-repo at `bridges/quickjs/js`. Once
  the public surface stabilizes, extract it into published workspace packages.

> **Done:** `embedded-react build` now produces the device artifact from a consumer project —
> `app.erpkg` (Flow A, bytecode compiled through the prebuilt `.wasm`, no native toolchain) and
> `--aot` → `app.gen.c` (Flow B). The AOT subset still excludes animation composition (see Flow B above).

---

## Performance backlog (engine)

Catalogued optimizations — correctness is fine, these are speed/RAM wins. Roughly
highest-impact first.

- **Cache text measurement** on the node; invalidate only when text/font/size/spacing
  props change (top remaining layout cost).
- **Skip clean subtrees in the render walk.** Track a `subtree_dirty` bit and cache
  sorted child order instead of descending the whole tree every frame.
- **Cache blurred shadow buffers** keyed on geometry (shadows are fully re-blurred every
  repaint).
- **Gradient color LUT.** Precompute a 256-entry premultiplied LUT from the stops instead
  of evaluating per pixel.
- **Cache / DDA-step scaled images** instead of re-sampling (per-pixel division) every
  repaint.
- **Resolve `BitmapFont*` once** when text props are set and cache on the node (currently
  a `strncmp` registry scan per text op).
- **Batch anti-aliased corner and 1-bit glyph blits** into a per-row buffer, one
  `copy`/`blend` per row, instead of per-pixel / per-span.
- **Draw only the border ring** of bordered rounded rects; fill the interior once
  (currently overdrawn).
- **Clamp software-composite loops once per row** instead of a per-pixel bounds check.
- **Step transform source coordinates incrementally** (and consider fixed-point) instead
  of a float matrix-multiply + bilinear sample per output pixel.
- **`memset` only the live `w×h` scratch region**, not the whole slot.
- **Right-size recursive walker stack arrays** (today each recursion level allocates
  `uint16_t[ERUI_MAX_NODES]`); walk the sibling list in place or share scratch by depth.
- **`er_tree_append_child` is O(n) per append → O(n²)** to build a sibling list; keep a
  `last_child` pointer and `prev_sibling`/`parent` back-pointers.
- **Skip the zIndex insertion-sort** when all children have `zIndex == 0`, or cache the
  sorted order.
- **Bound the input-tick scan** (currently scans all `ERUI_MAX_NODES` slots twice for
  momentum) with a small active-scroller list.
- **Reduce static scratch RAM:** size slots to the largest composited node, tile large
  transformed/opacity subtrees, or share one arena across transform/opacity/shadow.

---

## Vision / longer-term

The engine is **runtime-agnostic by construction** — `er_scene.h` is a pure C ABI, and
nothing in `engine/` knows about React. That layering keeps Flow A and Flow B honest
against one C surface and leaves the door open for other frontends without forking the
engine. None of these are committed work; they're why the engine stays neutral:

- **Lua UI** — drive the scene graph from Lua tables / a Lua DSL.
- **JSON UI** — a serialized React-tree format loaded at runtime.
- **Visual editor runtime** — a desktop GUI designer that emits that JSON format.
- **Other scripting APIs** — language-agnostic C bindings other runtimes can wrap.

This does not change the project's identity: **embedded-react is React Native for
embedded MCUs.** React is the supported developer path; the engine's neutrality is an
implementation choice.

---

## Non-goals

Deliberately out of scope:

- **MCU SDK headers** (`stm32h7xx_hal.h`, `esp_lcd.h`, …) anywhere in `engine/`. Hardware
  specifics live in `backends/`.
- **Flash tooling, OpenOCD scripts, deploy automation** — firmware tooling, not engine
  concerns.
- **RTOS setup / task creation** — the caller's responsibility (bare-metal, FreeRTOS,
  Zephyr all work).
- **Display initialization** (SPI, MIPI DSI, LTDC) — the backend's responsibility.
- **Network / fetch** — provided by the firmware if needed.
- **Web-only React APIs** — DOM refs, portals, Suspense, Server Components.
- **Pitching as a general-purpose multi-runtime UI engine.** Non-React frontends *could*
  live in `bridges/` later, but React is the supported path.
