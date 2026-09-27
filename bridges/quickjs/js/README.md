<h1 align="center">
  <img src="https://raw.githubusercontent.com/TheMasterCoder007/embedded-react/master/assets/icons/embedded-react-readme-header-v3.png" alt="Embedded React" width="100%">
</h1>

**React Native for embedded MCUs**: write JSX, run it on a microcontroller. This package is the
JavaScript layer: the React Native-style component API you import, the
[`react-reconciler`](https://www.npmjs.com/package/react-reconciler) host config that drives the C
engine at runtime (Flow A), the JSX-to-C ahead-of-time compiler (Flow B), and the `embedded-react`
CLI with the browser simulator.

**Documentation: [embedded-react.dev](https://embedded-react.dev)**

## Start here

```bash
npm create embedded-react@latest my-app          # add -- --ts for TypeScript
cd my-app && npm install && npm run dev          # the simulator, with hot reload, at http://localhost:3333
```

Or add it to an existing project:

```bash
npm install embedded-react react@18.3.1
```

```jsx
import {useState} from 'react';
import {View, Text, Pressable, StyleSheet, AppRegistry} from 'embedded-react';

function App() { /* ... */ }
AppRegistry.registerComponent('demo', () => App);
```

The package is the React Native analog: hooks come from `react`, everything else from here.

| Command | What it does |
|---|---|
| `npx embedded-react dev` | Runs your app in the browser simulator with hot reload; the engine `.wasm` ships prebuilt, so there is no native toolchain |
| `npx embedded-react dev --device` | The same, streamed to a connected board over USB (a firmware built with hot reload) |
| `npx embedded-react build` | The Flow A device artifact, `dist/app.erpkg` (bytecode + baked assets + CRC) |
| `npx embedded-react build --aot --screen WxH` | The Flow B artifact, `app.gen.c` + `assets.generated.c`, to compile into firmware |
| `npx embedded-react export` | A self-contained static copy of the simulator with your app, for any static host |

## Where to read more

- [Getting started](https://embedded-react.dev/getting-started): install, the simulator, your first board
- [Components](https://embedded-react.dev/api/components), [hooks](https://embedded-react.dev/api/hooks), [styles](https://embedded-react.dev/api/styles), [Animated](https://embedded-react.dev/api/animated)
- [Assets](https://embedded-react.dev/concepts/assets): images and fonts are baked at build time from plain imports
- [Hot reload](https://embedded-react.dev/guides/hot-reload): what survives a reload and why
- [The AOT subset](https://embedded-react.dev/guides/aot-subset): what the Flow B compiler accepts
- [Intentionally absent React Native APIs](https://embedded-react.dev/api/components#intentionally-absent-react-native-apis): what a microcontroller has no OS for

## Part of a monorepo

This package is the `bridges/quickjs/js` folder of the
[Embedded React](https://github.com/TheMasterCoder007/embedded-react) repository. The C engine, the
hardware backends, the board examples, the demo apps and the simulators live there, and the engine is
distributed separately as C source (CMake `FetchContent`, the ESP-IDF Component Registry, PlatformIO)
at the same version as this package.

Inside the repository, `npm run pack -- <demo>`, `npm run aot -- <demo>` and `npm run sim -- <demo>`
build one of the apps in `demos/` against the in-repo library, `npm test` runs the unit tests, and
`npm run test:runtime` / `npm run test:bytecode` run the end-to-end tiers against a built bridge.
[Testing](https://embedded-react.dev/internals/testing) and
[Architecture](https://embedded-react.dev/internals/architecture) describe both.

```
src/embedded-react/   the public package surface (components, StyleSheet, Animated, PanResponder, hooks)
src/host-config.js    the react-reconciler host config → NativeUI.* (the C bridge)
aot/                  the Flow B JSX→C compiler
assets/               build-time bakers: images, fonts, SVG, the ERPK container writer
cli.mjs, sim-server.mjs   the `embedded-react` CLI and the shared simulator dev server
sim/                  the prebuilt engine .wasm the simulator runs
test/runtime/         end-to-end tests that run inside QuickJS + the real engine
```
