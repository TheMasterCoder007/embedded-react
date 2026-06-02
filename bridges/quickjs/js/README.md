# embedded-react — QuickJS JS layer (React reconciler)

The JavaScript half of Flow A: a [`react-reconciler`](https://www.npmjs.com/package/react-reconciler)
host config that maps React's host API onto the `NativeUI` bridge, so **JSX components drive the
C engine**. Bundled to a single classic script that QuickJS runs with a plain `JS_Eval`.

```
React  →  react-reconciler  →  host-config.js  →  NativeUI.*  →  er_scene.h (engine)
```

## Layout

```
src/                       the library (the future 'embedded-react' runtime)
  host-config.js           reconciler host config → NativeUI.*
  renderer.js              createRoot(props).render(<App/>); LegacyRoot (sync)
  components.js            host component tags (View, Text, … → ERNodeType)
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
node's `text`.

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

- ✅ **Render, state updates, and keyed-list reordering all work end-to-end.** `<App/>` mounts,
  taps re-render via `setState`, and reversing a keyed list moves the nodes (`insertBefore` /
  `appendChild`, backed by engine `er_tree_insert_before`). Covered by
  `test/runtime/reorder.runtime.test.jsx`.
- ⏳ **Multi-child `<Text>`** (interpolation like `Hi {name}`, nested `<Text>` spans) — only a
  single string/number child is supported (`shouldSetTextContent`). Spans land with §1.2 span work.
- ⏳ **Bundler/toolchain (§4)** — this is a hand-run esbuild step; the Metro-compatible
  `'embedded-react'` package + `qjsc` bytecode path are still to come.
