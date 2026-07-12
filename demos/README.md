# Demos

Full example apps written against the public `embedded-react` API — the same JSX a downstream user
would write. Each subfolder is a **self-contained consumer project**: its own `package.json` depending
on `embedded-react`, wired to the `embedded-react` CLI exactly like a scaffolded app.

```
demos/
  thermostat/     thermostat with weather widget
  music-player/   music player app
  <your-demo>/    add a sibling folder with its own index.jsx + App.jsx
```

A demo is an `index.jsx` (the entry — registers the app via `AppRegistry`) plus its components. It
imports from `react` (hooks) and `'embedded-react'` (everything else), like a React Native screen.

These folders are also the **source for the `create-embedded-react` starter templates**: `npm run
sync-templates` in `create-embedded-react/` stages each one into that package (it also runs automatically
on `prepack`), so a consumer can start from any demo with one command.

## Start from a demo (consumers)

No repo checkout required — scaffold your own copy with the toolchain:

```bash
npm create embedded-react@latest my-app -- --template thermostat   # or --template music-player
cd my-app
npm install
npm run dev          # WASM simulator with hot reload → http://localhost:3333
```

`npm create embedded-react@latest -- --list` shows every available template.

## Run a demo from a repo checkout

Two ways, depending on which simulator you want:

```bash
# Browser (WASM) simulator, from the demo folder — the consumer path:
cd demos/thermostat && npm install && npm run dev

# SDL (native) simulator + the lower-level build tools, driven by demo name from the JS package:
cd bridges/quickjs/js
npm run sim   -- thermostat     # native hot-reload simulator (see tools/simulator/README.md for setup)
npm run build -- thermostat     # bundle + bake assets → dist/app.bundle.js
npm run pack  -- thermostat     # deployable config container → dist/app.erpkg
```

The name-based scripts in `bridges/quickjs/js` read `demos/<name>/index.jsx` directly (via an esbuild
alias to the in-repo library), so they build a demo against the local source without installing anything.

## Build a demo for a device

From a demo folder (or a scaffolded copy):

```bash
npm run build        # Flow A → dist/app.erpkg  (QuickJS bytecode + baked assets; PSRAM-class chips)
npm run build:aot    # Flow B → app.gen.c       (compiled to C; no-PSRAM boards)
```

Flow B bakes a target panel size (`embedded-react build --aot --screen <WxH>`) so a responsive app folds
to the layout that board renders — the thermostat's `build:aot` targets `240x320`; change it for your
panel. Run/flash via the relevant `examples/*` host.

## Add a demo

Scaffold one in-repo with the generator (creates `demos/<name>/` wired to the in-repo build tools):

```bash
cd bridges/quickjs/js
npm run create -- my-app
cd ../../demos/my-app
npm run sim
```

Or by hand: create `demos/<name>/index.jsx` + `App.jsx` (copy `thermostat/`), then build it by name with
the `bridges/quickjs/js` scripts. To ship it as a `create-embedded-react` template too, give it the
consumer-form `package.json` the other demos have and re-run `npm run sync-templates`.
