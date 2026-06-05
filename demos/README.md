# Demos

Example apps written against the public `embedded-react` API — the same JSX a downstream user
would write. Each subfolder is one self-contained demo; the build picks one and bundles it.

```
demos/
  thermostat/     the default demo (starter features tour, growing into a climate-control UI)
  <your-demo>/    add a sibling folder with its own index.jsx + App.jsx
```

A demo is just a `index.jsx` (the entry — registers the app via `AppRegistry`) plus its components.
It imports from `react` (hooks) and `'embedded-react'` (everything else), exactly like a React Native
screen.

## Building a demo

The bundler lives in the JS package (`bridges/quickjs/js`), where the toolchain + `node_modules`
already are. From there:

```
cd bridges/quickjs/js
npm install                 # once
npm run build               # bundles the default demo (thermostat) -> dist/app.bundle.js
npm run build -- marine-dash  # bundle a specific demo by folder name
```

The output is always `bridges/quickjs/js/dist/app.bundle.js` — the single "active" bundle the
example hosts (`examples/linux`, `examples/esp32`) pick up. Selecting a demo at build time decides
which one gets flashed/run; see each example's README for the run/flash step.

## Adding a demo

1. Create `demos/<name>/index.jsx` and `demos/<name>/App.jsx` (copy `thermostat/` as a starting point).
2. `npm run build -- <name>` from `bridges/quickjs/js`.
3. Run/flash via the relevant `examples/*` host.

> Note: demos resolve `'embedded-react'` through an esbuild alias in `build.mjs` (they live outside
> the package directory). Once `embedded-react` is published / set up as an npm workspace, demos will
> import it as a normal dependency with no alias — see the repo roadmap.
