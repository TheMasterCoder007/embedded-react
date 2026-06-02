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
app/                       the demo app (App.jsx, index.jsx) — bundle entry, not the library
test/runtime/              e2e tests that need the real engine host
  *.runtime.test.jsx       run inside QuickJS + engine via the headless harness
  harness.js               check()/report() — records failures for the C runner
  run.mjs                  bundles each runtime test + runs er-bridge-quickjs-runtest
build.mjs                  esbuild app/index.jsx → dist/app.bundle.js (IIFE, production React)
vitest.config.js           unit test config
```

The host config flattens RN `style` (+ nested arrays) into the flat prop bag, routes `on*`
handlers to `setEvent`, and uses `shouldSetTextContent` so `<Text>string</Text>` becomes the
node's `text`. `Animated` and `Easing` are not exported yet (BRIDGE.md §1.4).

## Build

```
npm install
npm run build      # → dist/app.bundle.js
```

## Run (desktop)

After `npm run build`, **rebuild the `embedded-react-desktop-js` target** — its build copies
`dist/app.bundle.js` next to the executable, and the host loads it **by default** (no argument):

```
examples/linux/build/embedded-react-desktop-js          # runs this React app
examples/linux/build/embedded-react-desktop-js  other.js # or an explicit bundle path
```

App resolution order: explicit CLI path → `app.bundle.js` next to the exe → built-in C demo. The
C host injects the globals the bundle expects: `NativeUI` (the bridge), `screen`
(`{ width, height, scale }`), and `console`.

> Iteration loop: edit `src/*` or `app/*` → `npm run build` → rebuild `embedded-react-desktop-js`
> (re-copies the bundle) → run.

## Tests

Two tiers, by what they need:

```
npm test            # unit: Vitest over src/**/__tests__/*.unit.test.js (pure JS, no engine)
npm run test:runtime  # e2e: bundles test/runtime/*.runtime.test.jsx, runs each in the headless
                      #      QuickJS+engine harness (no window) and checks the result
```

The runtime tier needs the harness exe built once (no SDL):

```
cmake --build bridges/quickjs/build --target er-bridge-quickjs-runtest
```

Pick the tier by what the code touches: pure marshalling/logic → a co-located `*.unit.test.js`;
anything that exercises the reconciler → engine pipeline → a `test/runtime/*.runtime.test.jsx`.

## Status & known gaps

- ✅ **Render, state, keyed-list reorder, and Animated all work end-to-end.** `<App/>` mounts, taps
  re-render via `setState`, keyed reorder moves nodes (`insertBefore`/`appendChild`), and
  `Animated.View` runs **native-driver** animations in the engine (no per-frame JS). Covered by
  `test/runtime/reorder.runtime.test.jsx` and `animated.runtime.test.jsx`.
- ⏳ **Animated composition** — `timing`/`spring`/`decay`/`interpolate` done; `sequence`/`parallel`/
  `loop`/`stagger`/`delay`, `.start(callback)`, and interpolate `extrapolate: 'clamp'` not yet.
- ⏳ **Multi-child `<Text>`** (interpolation like `Hi {name}`, nested `<Text>` spans) — only a
  single string/number child is supported (`shouldSetTextContent`). Spans land with §1.2 span work.
- ⏳ **`qjsc` bytecode** path + `create-embedded-react` scaffold — still to come (§4).
