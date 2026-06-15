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
[WASM_SIM.md](https://github.com/TheMasterCoder007/embedded-react/blob/master/WASM_SIM.md)).

To **share** a UI, export a self-contained static playground — `index.html` + the prebuilt `.wasm` + your
bundled app — that runs in any browser with no server, ready for GitHub Pages / Netlify / a docs' iframe:

```bash
npx embedded-react export --out sim-export   # then: npx serve sim-export  (or deploy the folder anywhere)
```

## Layout

```
src/
  embedded-react/          the public package surface (what apps import)
    index.js               barrel: components, StyleSheet, Platform, AppRegistry, Animated, Easing
    components.js          host component tags (View, Text, … → ERNodeType)
    StyleSheet.js          create() / flatten()
    Platform.js            { OS: 'embedded', select }
    AppRegistry.js         registerComponent(...) → mounts into a screen-sized root
    Animated.js            Value / timing / spring / decay / View|Text|Image / interpolate
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
`useEffect` flushes via the host pump (BRIDGE.md §1.2, §1.4, §2).

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

- **Images:** PNG → premultiplied ARGB8888 (`bake-image.mjs`, via `pngjs`).
- **Fonts:** TTF/OTF → pre-rasterized `BitmapFont` glyphs (`bake-font.mjs`, via `opentype.js` + a
  pure-JS rasterizer). The engine has no runtime rasterizer, so the baker rasterizes **exactly the
  literal `fontSize` values the bundle uses**. Computed/dynamic sizes snap to the nearest baked size
  at runtime — pin them in `assets.config.js` if needed.

Optional per-demo overrides live in `demos/<demo>/assets.config.js`:

```js
export default {
  fonts: {
    Inter: { sizes: [14, 18, 24], bpp: 4, glyphs: 'common' }, // bpp 1|2|4|8 (4 default); glyphs: 'ascii'|'common'|'minimal'|'greek'|[codepoints]
  },
};
```

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
  [SIMULATOR.md in the repo](https://github.com/TheMasterCoder007/embedded-react/blob/master/SIMULATOR.md).
- ✅ **`npx embedded-react dev`** — the WASM simulator runs your app in a browser with hot reload, from your
  own project directory, with the engine `.wasm` shipped prebuilt (no Emscripten for consumers). See above.
- ✅ **`npm create embedded-react@latest my-app`** — scaffolds a fresh standalone project (a styled card with a
  pulsing logo + a `count is N` button) wired for `npm run dev` (simulator) and `npm run export` (static
  playground). Published as the `create-embedded-react` package.
