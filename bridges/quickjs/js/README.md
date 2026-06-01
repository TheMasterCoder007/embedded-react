# embedded-react — QuickJS JS layer (React reconciler)

The JavaScript half of Flow A: a [`react-reconciler`](https://www.npmjs.com/package/react-reconciler)
host config that maps React's host API onto the `NativeUI` bridge, so **JSX components drive the
C engine**. Bundled to a single classic script that QuickJS runs with a plain `JS_Eval`.

```
React  →  react-reconciler  →  host-config.js  →  NativeUI.*  →  er_scene.h (engine)
```

## Layout

| File | Role |
|---|---|
| `src/host-config.js` | The reconciler host config — `createInstance`/`appendChild`/`commitUpdate`/… → `NativeUI.*`. Flattens RN `style` (+ nested arrays) into the flat prop bag, routes `on*` handlers to `setEvent`, and uses `shouldSetTextContent` so `<Text>string</Text>` becomes the node's `text`. |
| `src/renderer.js` | `createRoot(props).render(<App/>)`. Creates a screen-sized container View, sets it as the scene root, and drives a **LegacyRoot (synchronous)** reconciler so the first render flushes without the async scheduler. |
| `src/components.js` | Host components — string tags (`View`, `Text`, `Pressable`, …) that map to `ERNodeType`. |
| `src/native-ui.js` | Re-exports the `globalThis.NativeUI` the C bridge installs. |
| `src/App.jsx`, `src/index.jsx` | Sample app + bundle entry. |
| `build.mjs` | esbuild → `dist/app.bundle.js` (IIFE, production React, JSX automatic). |

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

> Iteration loop: edit `src/*` → `npm run build` → rebuild `embedded-react-desktop-js` (re-copies
> the bundle) → run.

## Status & known gaps

- ✅ **Initial render works end-to-end** — `<App/>` mounts through the reconciler and the engine
  lays it out (verified: a `flex: 1` root fills the 480×320 screen).
- ⏳ **`NativeUI.insertBefore` is not implemented in the bridge yet.** Initial mount only uses
  `appendChild`, so it isn't hit today — but reordering / mid-list insertion will call it. Needs an
  engine `er_tree_insert_before` primitive + a bridge method. Until then, dynamic list reordering
  is unsupported.
- ⏳ **Multi-child `<Text>`** (interpolation like `Hi {name}`, nested `<Text>` spans) — only a
  single string/number child is supported (`shouldSetTextContent`). Spans land with §1.2 span work.
- ⏳ **Bundler/toolchain (§4)** — this is a hand-run esbuild step; the Metro-compatible
  `'embedded-react'` package + `qjsc` bytecode path are still to come.
