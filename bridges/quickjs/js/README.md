# embedded-react — QuickJS JS layer (React reconciler)

The JavaScript half of Flow A: a [`react-reconciler`](https://www.npmjs.com/package/react-reconciler)
host config that maps React's host API onto the `NativeUI` bridge, so **JSX components drive the
C engine**. Bundled to a single classic script that QuickJS runs with a plain `JS_Eval`.

```
React  →  react-reconciler  →  host-config.js  →  NativeUI.*  →  er_scene.h (engine)
```

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
build.mjs                  esbuild a demo's index.jsx → dist/app.bundle.js (IIFE, production React)
vitest.config.js           unit test config
```

The demo apps themselves live in the top-level **`demos/`** folder (one folder per demo), *not*
here — this package is the library + reconciler + tests. `build.mjs` bundles a selected demo and
resolves its `'embedded-react'` import to `src/embedded-react/index.js` via an esbuild alias. See
[`demos/README.md`](../../../demos/README.md).

The host config flattens RN `style` (+ nested arrays) into the flat prop bag, routes `on*`
handlers to `setEvent`, and uses `shouldSetTextContent` so a flattenable `<Text>` subtree (a string,
interpolation like `Hi {name}`, or nested `<Text>` runs) becomes the node's `text` + inline spans.
`Animated`, `Easing`, and the web timer globals (`setTimeout` / `setInterval`) are all available;
`useEffect` flushes via the host pump (BRIDGE.md §1.2, §1.4, §2).

## Build

```
npm install
npm run build               # bundles the default demo (demos/thermostat) → dist/app.bundle.js
npm run build -- marine-dash  # bundle a specific demo (demos/<name>) instead
```

## Run (desktop)

After `npm run build`, **rebuild the `embedded-react-desktop-js` target** — its build copies
`dist/app.bundle.js` next to the executable, and the host loads it **by default** (no argument):

```
examples/linux/build/embedded-react-desktop-js          # runs this React app
examples/linux/build/embedded-react-desktop-js  other.js # or an explicit bundle path
```

App resolution order: explicit CLI path (`.qbc` = bytecode, else source) → `app.bundle.qbc`
(compiled bytecode, preferred) → `app.bundle.js` (source) → built-in C demo. The desktop build
compiles the bundle to `app.bundle.qbc` next to the exe, so it boots from **bytecode** by default —
the same `JS_ReadObject` load path the MCU hosts use (no parser, faster boot, source not shipped).
The C host injects the globals the bundle expects: `NativeUI` (the bridge), `screen`
(`{ width, height, scale }`), and `console`.

> Iteration loop: edit `src/*` (library) or `demos/<name>/*` (app) → `npm run build` → rebuild
> `embedded-react-desktop-js` (re-copies the bundle) → run.

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
- ⏳ **`qjsc` bytecode** path + `create-embedded-react` scaffold — still to come (§4).
