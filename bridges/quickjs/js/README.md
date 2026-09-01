# embedded-react

**React Native for embedded MCUs** — write JSX, run it on a microcontroller. This npm package is the
**JavaScript layer**: the React-Native-style component API you import, the
[`react-reconciler`](https://www.npmjs.com/package/react-reconciler) host config that drives the C engine
at runtime (**Flow A**), and the JSX→C ahead-of-time compiler (**Flow B**, `aot/`).

```
React  →  react-reconciler  →  host-config.js  →  NativeUI.*  →  er_scene.h (engine)
```

> ### Part of a monorepo
> This package is just the `bridges/quickjs/js` folder of the **embedded-react** project. The C rendering
> engine, the hardware backends, the runnable examples, the demo apps, and the simulator all live in the
> main repo — **https://github.com/TheMasterCoder007/embedded-react**. The engine itself is distributed
> separately as C source (CMake `FetchContent`, the ESP-IDF Component Registry, and PlatformIO) — see the
> repos **Install** section. Everything ships at one lockstep version (this package's version == the engine's).
>
> **`npx embedded-react dev` works in your own project** — it runs the WASM simulator on your app with hot
> reload, no clone, and no native toolchain (the simulator `.wasm` ships prebuilt in this package). The other
> end-to-end CLIs below (`npm run pack`/`build`/AOT, running on a board) still operate on the repo's `demos/`
> and `examples/`. To start a **fresh standalone project** in your own directory, use the scaffolder:
> `npm create embedded-react@latest my-app`.

## What an app imports

The package is the React Native analog — same idiom (hooks from `react`, everything else here):

```jsx
import { useState } from 'react';
import { View, Text, Pressable, StyleSheet, AppRegistry } from 'embedded-react';

function App() { /* ... */ }
AppRegistry.registerComponent('demo', () => App);
```

`embedded-react` resolves as a Node **package self-reference** (`package.json` `name` + `exports`),
so esbuild and Vitest find it with no aliases.

`<FlatList>` renders, but it is a thin `<ScrollView>` alias — no virtualization, every row mounts and
stays mounted, and only `data` / `renderItem` / `keyExtractor` / `style` are honored. See [`FlatList` is
a `ScrollView`
alias](https://github.com/TheMasterCoder007/embedded-react#flatlist-is-a-scrollview-alias) for the node
budget it costs and what to do with a long list.

RN modules that wrap an operating system — `StatusBar`, `SafeAreaView`, `AppState`, `Appearance`,
`Linking`, `AccessibilityInfo` — are **intentionally absent**; a microcontroller has no OS for them to
wrap. See [Intentionally absent React Native
APIs](https://github.com/TheMasterCoder007/embedded-react#intentionally-absent-react-native-apis) for
the full list and what to use instead.

## Simulate — `npx embedded-react dev`

Run your app in a browser with hot reload — no native toolchain, no repo clone. The engine is compiled to
WebAssembly and ships **prebuilt** in this package; the CLI bundles your JSX, serves it, and re-loads on save
(your `useState` survives the reload).

```bash
npx embedded-react dev            # finds ./index.jsx, ./src/index.jsx, or package.json "main"
npx embedded-react dev app.jsx    # or pass the entry explicitly
npx embedded-react dev --port 4000
```

Open the printed URL. The canvas fills the viewport, so the browser's device toolbar drives the board size
(e.g., 240×320) — pixel-accurate to a hardware ARGB panel. Imported images/fonts are baked and hot-reload too.
A floating gear chip locks to a specific panel size and can wrap the screen in a **device frame** (bezel) for a
true-to-hardware preview. This is the same simulator as the repo's `tools/web-sim` (see
[tools/web-sim](https://github.com/TheMasterCoder007/embedded-react/blob/master/tools/web-sim/README.md)).

To **share** a UI, export a self-contained static playground — `index.html` + the prebuilt `.wasm` + your
bundled app — that runs in any browser with no server, ready for GitHub Pages / Netlify / a docs' iframe:

```bash
npx embedded-react export --out sim-export   # then: npx serve sim-export  (or deploy the folder anywhere)
```

## Layout

```
src/
  embedded-react/          the public package surface (what apps import)
    index.js               barrel: components, StyleSheet, Platform, AppRegistry, Animated, PanResponder, Easing
    components.js          host component tags (View, Text, … → ERNodeType)
    StyleSheet.js          create() / flatten()
    Platform.js            { OS: 'embedded', select }
    AppRegistry.js         registerComponent(...) → mounts into a screen-sized root
    Animated.js            Value / timing / spring / decay / View|Text|Image / interpolate
    PanResponder.js        create(config) → panHandlers: drag/swipe/fling over the responder system
    Easing.js              easing tokens (+ bezier) → engine curves
    split-style.js         pure: split style into static props + animated bindings
    __tests__/             co-located UNIT tests for the pure surface
  host-config.js           reconciler host config → NativeUI.* (internal runtime)
  renderer.js              createRoot(props).render(...); LegacyRoot (sync) (internal)
  props.js                 pure prop helpers (flattenStyle / buildProps / isEventProp)
  native-ui.js             re-exports globalThis.NativeUI (installed by the C bridge)
  __tests__/               co-located UNIT tests (Vitest, *.unit.test.js, no engine)
test/runtime/              e2e tests that need the real engine host
  *.runtime.test.jsx       run inside QuickJS + engine via the headless harness
  harness.js               check()/report() — records failures for the C runner
  run.mjs                  bundles each runtime test + runs er-bridge-quickjs-runtest
assets/                    build-time asset bakers (pure JS, no native deps) — see "Assets" below
  rasterize.mjs            glyph path → coverage bitmap (supersampled, nonzero winding)
  bake-font.mjs            TTF/OTF → engine BitmapFont glyph data (opentype.js)
  bake-image.mjs           PNG → premultiplied ARGB8888 (pngjs)
  emit-c.mjs               assemble assets.generated.c + the built-in font_data.c
  build-builtin-font.mjs   regenerate the engine's default Inter font (npm run build:builtin-font)
build.mjs                  esbuild a demo's index.jsx → dist/app.bundle.js + bake its imported assets
pack-container.mjs         bundle + bytecode-compile + bake → dist/app.erpkg config container (npm run pack)
  assets/emit-container.mjs  ERCF container writer (sections + QuickJS version stamp + CRC32)
vitest.config.js           unit test config
```

The demo apps themselves live in the repo's top-level **`demos/`** folder (one folder per demo), *not*
in this package — this package is the library + reconciler + AOT compiler + tests. `build.mjs` bundles a
selected demo and resolves its `'embedded-react'` import to `src/embedded-react/index.js`. See
[demos/ in the repo](https://github.com/TheMasterCoder007/embedded-react/tree/master/demos).

The host config flattens RN `style` (+ nested arrays) into the flat prop bag, routes `on*`
handlers to `setEvent`, and uses `shouldSetTextContent` so a flattenable `<Text>` subtree (a string,
interpolation like `Hi {name}`, or nested `<Text>` runs) becomes the node's `text` + inline spans.
`Animated`, `Easing`, and the web timer globals (`setTimeout` / `setInterval`) are all available;
`useEffect` flushes via the host pump.

### Hiding a subtree (`display: 'none'`) — page caching

```jsx
<View style={{display: page === 'home' ? 'flex' : 'none'}}><Home /></View>
<View style={{display: page === 'settings' ? 'flex' : 'none'}}><Settings /></View>
```

A hidden subtree drops out of layout, rendering, and hit-testing, but stays **mounted** — its React
state and effects survive, and the native nodes behind it keep their props. That is the difference
from the conditional render (`{page === 'home' && <Home/>}`) it replaces: a conditional unmounts the
page and rebuilds every node in interpreted QuickJS on the way back, which is the dominant cost of a
page change. Hiding costs a repaint, and a hidden page costs nothing per frame even while React keeps
rendering into it.

`visible={false}` is the same switch under the other spelling, with an explicit `style.display`
winning over it (`<Modal visible>` is unaffected — that is the Modal's own show/hide prop). Both work
in Flow B too, where `display` can be static or state-driven — `display: page === 'home' ? 'flex' : 'none'`
compiles, but a state-driven one has to be a string literal or a ternary of them, whereas `visible` takes
any boolean expression. Either spelling lowers to a `p.display` write in `app_update`, so hiding costs no
node churn there either. The one gap is `visible` on an `<Svg>`, which the AOT rejects outright: wrap it
(`<View visible={…}><Svg …/></View>`) and the whole subtree goes with the View.
The trade is memory: a cached page holds its nodes for the life of the app, so cache
the pages you flip between, not every page.

### Gestures — `PanResponder`

The raw touch events already carry the travel and the speed (`e.dx`/`e.dy`, `e.vx`/`e.vy` — see below),
which is enough for a one-off flick. A real drag wants more: an anchor that starts at zero, however, the
gesture was won, ownership against a scrolling ancestor, and a teardown hook for when the gesture is
taken away. `PanResponder` is that, in the RN shape (and it compiles in Flow B as well as Flow A):

```jsx
const pan = useRef(
  PanResponder.create({
    onStartShouldSetPanResponder: () => true,
    onPanResponderMove: (e, g) => setX(g.dx),
    onPanResponderRelease: (e, g) => (g.vx > 0.5 ? next() : settle()),
  }),
).current;

<View style={styles.overlay} {...pan.panHandlers} />;
```

Create it **once** per component and keep it in a ref — the handlers close over one gesture state, so
re-creating it per render throws the drag away. `gestureState` carries `x0`/`y0` (the grant point),
`dx`/`dy` (travel since the grant), `vx`/`vy` (px per ms), `moveX`/`moveY`, `numberActiveTouches` and
`stateID`. Returning `true` from `onMoveShouldSetPanResponder` claims the gesture mid-pan and re-anchors
`dx`/`dy` at that point, so a slop threshold never shows up as a jump. With neither should-set callback
supplied nothing is ever granted — same as RN.

`panHandlers` are the RN **responder props** — should-set queries plus `onResponderGrant`/`Move`/
`Release`/`Terminate` — wired straight into the engine's C responder system, so claiming is real
*negotiation*, not just listening:

- **A granted pan OWNS the gesture.** A `ScrollView` ancestor will not auto-scroll under it, and a
  `onMoveShouldSetPanResponder` claim takes the gesture *back* from a scroller that already started
  (which then stops dead — no coasting).
- **Negotiation covers the subtree.** The engine asks every node on the hit chain — capture phase
  root→leaf first (the `*Capture` config keys), then bubble leaf→root — so one responder on a wrapper
  claims its children's touches, and an outer responder can pre-empt an inner one.
- **Losing and defending are callbacks.** A refused claim lands on `onPanResponderReject` (and is
  re-asked on every later move while the predicate keeps returning true); a challenger asking for the
  gesture hits the holder's `onPanResponderTerminationRequest` — return `false` to keep it (absent =
  yield, as in RN).
- **`onPanResponderTerminate` is the undo hook.** It fires when the host cancels the touch sequence,
  when a fresh touch-down arrives on a finger whose previous sequence never ended, and when the
  responder yields to a challenger.
- **Single gesture.** The engine events carry no finger id, so extra fingers fold into ONE gesture:
  `numberActiveTouches` counts them and the last to lift releases — both true in either flow. Flow A
  additionally reports each join and leave through `onPanResponderStart`/`End`; Flow B has no
  equivalent (next bullet).
- **Flow B compiles it too.** The AOT recognises `useRef(PanResponder.create({…})).current` and the
  `{...pan.panHandlers}` spread as a whole and lowers them onto the same C responder system — the
  should-set predicates become `er_responder_query_set` callbacks and the rest become responder event
  handlers, with `g.dx`/`g.vx` reading the engine's own payload. No JS state machine is copied into the
  generated C. Only `onPanResponderStart`/`End` — the two that fold EXTRA fingers into one gesture —
  are Flow A only, and the AOT fails the build by name rather than dropping them. The watch-face demo's
  swipe pager is the worked example: one source, both flows.

As promised above: the raw touch events carry the same fields the responder ones do — `e.dx`/`e.dy` from
touch-down and `e.vx`/`e.vy` in px per ms — so `onTouchEnd={e => e.vx > 0.4 && next()}` needs no
recognizer at all, in either flow.

The low-level responder props (`onStartShouldSetResponder`, `onResponderGrant`, …) are themselves
public on every component for gestures that don't fit the pan shape. Only RN's Android-specific
`onShouldBlockNativeResponder` is unsupported — holding the responder already blocks the native one
(the built-in adjustable-Arc drag deliberately still wins). Passing it, or a typo, warns.

## Build

```
npm install
npm run pack                # default demo → dist/app.erpkg (deployable config: bytecode + assets + CRC)
npm run pack -- marine-dash # pack a specific demo (demos/<name>) instead
npm run build               # lower-level: just bundle → dist/app.bundle.js (+ bake assets.generated.c)
npm run create -- my-app    # scaffold a new app at demos/my-app (App.jsx + scripts); then `cd` + npm run sim
```

`npm run pack` is the deployable artifact the desktop demo and the ESP32 both load — see **Config
container** below. `npm run build` is the lower-level bundle step (used by the bytecode/asset tooling
and by firmware that prefers to compile assets in); both bake the demo's imported images and fonts
(see **Assets**).

## Assets (images and fonts)

Asset handling is **import-driven** and fully build-time — there is no runtime decoder or font
rasterizer on the device, and no Python toolchain. An app just imports a file and uses the name it
returns:

```jsx
import logo from './assets/logo.png';      // → the baked image NAME ("logo")
import Inter from './assets/Inter.ttf';     // → the baked font FAMILY ("Inter")

<Image source={logo} style={{ width: 64, height: 64 }} />
<Text style={{ fontFamily: Inter, fontSize: 18 }}>Hi</Text>
```

The baker produces the assets two ways, from the same bytes: `npm run pack` packs them **into the
config container** (`app.erpkg`), registered at load time — this is what the desktop demo and ESP32
use. `npm run build` also emits `dist/assets.generated.c` exposing **`er_register_assets()`**, for
firmware that prefers to **compile assets into the image** and call it once at boot (`er_image_load` /
`er_font_register`, both flash-resident, zero runtime RAM). Either way the asset name/family is the
file's basename.

- **Images:** PNG → premultiplied ARGB8888 (`bake-image.mjs`, via `pngjs`). Fully opaque art —
  full-screen backgrounds especially — can opt into a 16-bit **RGB565** bake per image (see the
  config below): half the flash and half the source-read bandwidth on every repaint. The baker
  rejects a non-opaque source rather than silently dropping transparency, and the engine renders
  RGB565 images through its opaque copy fast path (no per-pixel blending).
- **Fonts:** TTF/OTF → pre-rasterized `BitmapFont` glyphs (`bake-font.mjs`, via `opentype.js` + a
  pure-JS rasterizer). The engine has no runtime rasterizer, so the baker rasterizes **exactly the
  `fontSize` values the app uses**. Discovery folds constants, so a token-driven type scale bakes
  what it means — `const TYPE = {body: 14, title: 22}` read as `fontSize: TYPE.title`, including a
  scale handed to a component as a prop, and both arms of a `screen.width` ternary. A size that only
  exists at runtime can't be seen; the build **warns** and names the expression, because at runtime
  it silently snaps to the nearest baked size. Sizes the bake missed are warned about the same way,
  against your fonts or against the built-in one — pin them in `assets.config.js` if you want a
  different set. Only the glyphs you ask for are baked: printable ASCII, plus a named symbol set and
  any per-app characters you add.

Optional per-demo overrides live in `demos/<demo>/assets.config.js`:

```js
export default {
  fonts: {
    Inter: {
      sizes: [14, 18, 24], // bpp 1|2|4|8 (4 default)
      bpp: 4,
      glyphs: 'common', // 'ascii'|'minimal'|'common'|'greek'|'common-greek'|[codepoints]
      extraGlyphs: '⌘⏻№', // this app's own characters, on top of `glyphs`
    },
  },
  images: {
    bg: { format: 'rgb565' }, // 16-bit bake for opaque art; default is 'argb8888'
  },
};
```

`extraGlyphs` takes the characters themselves or their codepoints (`[0x2318, '⏻']`) — the named sets
can't anticipate every UI. Every bake path checks the app's text against what it baked and **warns
about characters that have no glyph**, since the engine silently substitutes `?` for them on the
device and nothing else tells you:

```
embedded-react: 1 character(s) in the app's text have no baked glyph and will render as '?':
  U+2318 '⌘'  missing from Inter   in "press ⌘S to save"
  fix: assets.config.js → fonts: {Inter: {extraGlyphs: "⌘"}}
```

The check reads string literals (and, for Flow B's unbundled JSX, element text), so text assembled
at runtime is out of its reach. It also says when the .ttf itself has no such glyph — then no config
change helps and the font is the thing to change.

The engine's **built-in default font** (`engine/font/font_data.c`, the Inter fallback used by any
text without a custom `fontFamily`) is generated by the same baker — regenerate it with
`npm run build:builtin-font` (then re-run the engine text tests, as glyph metrics shift slightly).

## Config container

`npm run pack` wraps the app into one deployable file — **`dist/app.erpkg`** (format `ERCF`):

```
magic "ERCF" | format_version | crc32 | qjs_tag | sections[ bytecode, asset pack ]
```

It bundles the demo, precompiles it to **QuickJS bytecode** (no parser/source shipped), bakes the
imported assets into an ERPK pack, and wraps both with a **QuickJS version stamp** and an **integrity
CRC32**. That one `.erpkg` is "the config": loaded by `er_runtime_load_container()` on the desktop, or
flashed to a device's config partition. The loader verifies CRC + version (a config built for a
different QuickJS is rejected, not run as garbage) and registers the assets before the app mounts. This
is the firmware-vs-config split: the firmware (desktop exe / ESP32 image) ships once; the `.erpkg`
ships and updates independently.

> Two CRCs are different things: the container's internal CRC32 is embedded-react's own integrity
> check (universal). A bootloader's transfer/flash CRC is a separate,
> project-specific step layered on the `.erpkg` by your upload toolchain.

The precompiler tool must be built once (`pack` looks for it in the usual build dirs, or set
`ER_COMPILE_BIN`): `cmake -S bridges/quickjs -B bridges/quickjs/build && cmake --build
bridges/quickjs/build --target er-bridge-quickjs-compile`.

## Run (desktop)

After `npm run pack`, **rebuild the `embedded-react-desktop` target** — its build copies
`dist/app.erpkg` into the "config slot" next to the executable, and the host loads it **by default**
(no argument), exactly as the ESP32 loads its config from flash:

```
examples/linux/build/embedded-react-desktop            # runs the config in the slot
examples/linux/build/embedded-react-desktop  other.erpkg # or an explicit container / .qbc / .js path
```

The firmware ships no app and no baked assets — everything rides in the container. No config / a
corrupt one shows an on-screen panel (no built-in fallback). The C host injects the globals the app
expects: `NativeUI` (the bridge), `screen` (`{ width, height, scale }`), and `console`.

> Iteration loop: edit `src/*` (library) or `demos/<name>/*` (app) → `npm run pack` → rebuild
> `embedded-react-desktop` (re-copies the container into the slot) → run. Or, for an instant
> edit-save-see loop, use the **simulator** (`npm run sim`).

## Tests

Two tiers, by what they need:

```
npm test            # unit: Vitest over src/**/__tests__/*.unit.test.js (pure JS, no engine)
npm run test:runtime  # e2e: bundles test/runtime/*.runtime.test.jsx, runs each in the headless
                      #      QuickJS+engine harness (no window) and checks the result
npm run test:bytecode # same suite, but each bundle is precompiled to a .qbc bytecode blob and run
                      #      via the bytecode path (JS_ReadObject) — proves the MCU load path
```

The runtime tiers need the harness exe built once (no SDL); `test:bytecode` also needs the compiler:

```
cmake --build bridges/quickjs/build --target er-bridge-quickjs-runtest er-bridge-quickjs-compile
```

Pick the tier by what the code touches: pure marshalling/logic → a co-located `*.unit.test.js`;
anything that exercises the reconciler → engine pipeline → a `test/runtime/*.runtime.test.jsx`.

## Status & known gaps

- ✅ **Render, state, keyed-list reorder, and Animated all work end-to-end.** `<App/>` mounts, taps
  re-render via `setState`, keyed reorder moves nodes (`insertBefore`/`appendChild`), and
  `Animated.View` runs **native-driver** animations in the engine (no per-frame JS). Covered by
  `test/runtime/reorder.runtime.test.jsx` and `animated.runtime.test.jsx`.
- ✅ **Timers, Promises, and `useEffect` work.** `setTimeout`/`setInterval`/`clearTimeout`/
  `clearInterval` and the Promise job queue are serviced each frame by the host pump
  (`er_bridge_pump`, off the engine clock). React passive effects (`useEffect`) flush on the pump.
  Covered by `timers.runtime.test.js` and `effects.runtime.test.jsx`.
- ✅ **Animated composition + completion.** `sequence`/`parallel`/`stagger`/`delay`/`loop` and
  `.start(({ finished }) => …)` all work — composition is pure JS over each child's start/stop, with
  completion wired through the engine's `on_complete`. Covered by `anim-compose.runtime.test.js`.
- ✅ **Multi-child `<Text>` + nested spans.** Interpolation (`Hi {name}`) and nested styled
  `<Text>` runs both work — a flattenable `<Text>` owns its subtree and renders as the node's text
  plus, when runs differ in style, an inline span array (`NativeUI.setTextSpans`, max 4). Covered by
  `text-spans` unit + runtime tests.
- ✅ **LayoutAnimation.** `LayoutAnimation.configureNext(...)` before a layout-changing state update
  tweens every node whose computed rect moved on the next commit (in C — no per-frame JS). `Presets`
  / `create` / `Types` / `Properties` and the `easeInEaseOut`/`linear`/`spring` shorthands. Covered
  by `layout-animation` unit + `layout-anim.runtime.test.jsx`.
- ✅ **`PanResponder` + the gesture responder system.** The full RN config minus only the
  Android-specific `onShouldBlockNativeResponder`: should-set predicates (with `*Capture` variants),
  grant / move / release / terminate / reject / termination-request, `onPanResponderStart`/`End` for
  extra fingers, and a `gestureState` (`x0`/`y0`, `dx`/`dy`, `vx`/`vy`, `numberActiveTouches`) — wired
  to the ENGINE's responder negotiation, so a granted pan blocks a ScrollView's auto-scroll and can
  take a started scroll over. The low-level responder props are public on every component, and **Flow B
  compiles the same config** — the AOT lowers it onto that C responder system, minus only
  `onPanResponderStart`/`End`. Covered by `pan-responder` unit + `pan-responder.runtime.test.jsx` + the
  AOT's `compile`/`cc-compile` PanResponder cases + the engine's `test_input` responder cases.
- ✅ **Interpolate `extrapolate`.** `interpolate({ inputRange, outputRange, extrapolate })` supports
  `'extend'` (default) / `'clamp'` / `'identity'`, with per-end `extrapolateLeft`/`extrapolateRight`
  overrides. Math is engine-tested (`test_interpolate`); the bridge path by
  `interpolate-extrapolate.runtime.test.js`.
- ✅ **Bytecode + assets + `useAnimatedValue`.** The build compiles the bundle to a `.qbc` bytecode
  blob (the MCU load path) and bakes imported images/fonts to flash-resident C; `useAnimatedValue` is
  exported. Runs end-to-end on the desktop host and on ESP32-S3 hardware.
- ✅ **State survives hot reload** — in the simulator, plain `useState` transparently persists across
  saves (a sim-only build transform rewrites it to a persisting helper; press R to reset). On a device
  it's just `useState`, so the same app code runs everywhere. `usePersistentState` is the underlying
  helper, also exported for explicit use. See
  [tools/simulator in the repo](https://github.com/TheMasterCoder007/embedded-react/blob/master/tools/simulator/README.md).
- ✅ **`<Dial>` — the native arc widget.** Dials, gauges and progress rings as ONE engine node
  (`ER_NODE_ARC`) instead of a re-tessellated `<Svg>`: track + value indicator, optional backing band,
  segment gaps, round caps, a circle / image / child-anchored knob, solid / conic / radial indicator paint.
  `value` accepts an `Animated.Value` (bound natively — no JS per frame; each tick repaints only the swept
  sliver), and `adjustable` gives built-in drag-to-set with the quantized value delivered to `onChange(v)`.
  `<Switch onValueChange>` now also fires in Flow A (it was AOT-only). Covered by `dial.runtime.test.jsx`
  and the engine's `test_arc`.
- ✅ **`npx embedded-react dev`** — the WASM simulator runs your app in a browser with hot reload, from your
  own project directory, with the engine `.wasm` shipped prebuilt (no Emscripten for consumers). See above.
- ✅ **`npm create embedded-react@latest my-app`** — scaffolds a fresh standalone project (a styled card with a
  pulsing logo + a `count is N` button) wired for `npm run dev` (simulator) and `npm run export` (static
  playground). Published as the `create-embedded-react` package.
