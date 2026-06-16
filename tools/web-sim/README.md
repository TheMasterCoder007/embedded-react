# tools/web-sim — WASM simulator

The browser dev loop for embedded-react front-ends: the engine compiled to WebAssembly, rendering through
the [software backend](../../backends/software/) into a `<canvas>`. A React app authored against
`embedded-react` runs **unmodified in a browser**, pixel-accurate to an ARGB device. This is the simulator
that ships in the npm package as `npx embedded-react dev` — no native toolchain for consumers.

## Status — shipped (device frame and static export)

The simulator ships in the npm package as **`npx embedded-react dev`**: it runs on a consumer's own project
(cwd), with the engine `.wasm` shipped prebuilt so consumers need no Emscripten. The bundler + dev-server core
is shared — [`bridges/quickjs/js/sim-server.mjs`](../../bridges/quickjs/js/sim-server.mjs) drives both the
shipped CLI ([`cli.mjs`](../../bridges/quickjs/js/cli.mjs)) and this repo's `dev.mjs`; only the paths differ.
CI builds the `.wasm` (emsdk) and stages it into the package on release.

The dev loop: esbuild `--watch` rebundles on save, bakes imported images/fonts into an ERPK pack
(`er_web_load_pack`), and pushes a Server-Sent reload so the page hot-reloads with **no wasm rebuild**;
`useState` survives via the Babel persist transform. It runs interactive Flow A on `er_runtime` — the `.wasm`
bundles engine + QuickJS-ng + bridge (~1.2 MB, app-agnostic).

Two finishing touches (both in the shipped CLI): **device-frame chrome** (a cosmetic bezel + speaker slit +
home indicator around the canvas, toggled from the gear chip at a locked size — purely visual) and a **static
export** — `embedded-react export` produces a self-contained folder (`index.html` + the prebuilt `.wasm` + your
bundled app) that runs in any browser with no server, for GitHub Pages / docs iframes / sharing.

## Develop (hot reload)

Requires the [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html) (`emcc` on PATH) once
to build the module; after that, day-to-day dev is just `dev.mjs`.

```bash
node tools/web-sim/build.mjs          # once → public/embedded-react.{js,wasm} (also fetches+builds QuickJS-ng)
node tools/web-sim/dev.mjs [demo]     # watch + bake assets + hot reload → http://localhost:3333/  (default: music-player)
```

Open the URL and edit the demo's JSX (or an image/font) — the page hot-reloads on save, preserving component
state. The canvas renders **1:1** (crisp, no upscale) and fills the viewport, so the browser's device toolbar
drives the board size (set it to 240×320 and the app renders at exactly 240×320, like a responsive web
project). The floating gear chip locks to a specific panel size or a custom W×H. `?screen=WxH` sets the
initial size. Port defaults to **3333** (override with `--port`).

For a **static** preview of an already-built bundle (no watch), use `bundle-app.mjs` + `serve.mjs`:

```bash
node tools/web-sim/bundle-app.mjs [demo]   # one-shot → public/app.js
node tools/web-sim/serve.mjs               # static server → http://localhost:3333/
```

`build.mjs --debug` builds `-O0 -g` with assertions for troubleshooting.

## Layout

| File | Role |
|---|---|
| `CMakeLists.txt` | Emscripten build: engine + QuickJS bridge + `backends/software` + `backends/web` → the wasm module |
| `build.mjs` | drives `cmake` with the Emscripten toolchain → `public/` + stages the package's `sim/` |
| `dev.mjs` | repo dev loop over `demos/` — a thin wrapper over the shared `sim-server.mjs` |
| `bundle-app.mjs` | one-shot esbuild a demo's JSX → `public/app.js` (for a static preview) |
| `serve.mjs` | minimal static server (correct MIME); for previewing a prebuilt bundle |
| `index.html` | host page: loads the module, fetches `app.js`/`assets.pack`, rAF pump → `putImageData`, pointer → touch, SSE reload |
| `public/` | build + bundle output (git-ignored; the wasm is shipped prebuilt by CI) |

The shared dev-server core and the consumer CLI live in the npm package:
[`sim-server.mjs`](../../bridges/quickjs/js/sim-server.mjs) (watch + bake + serve + SSE) and
[`cli.mjs`](../../bridges/quickjs/js/cli.mjs) (`npx embedded-react dev`).

The exported C ABI lives in [`backends/web/web_backend.h`](../../backends/web/web_backend.h); the present
layer (ARGB→RGBA) is [`backends/web/renderer_backend.c`](../../backends/web/renderer_backend.c).

## Relationship to the SDL simulator

The desktop [`tools/simulator`](../simulator/README.md) (SDL) is **kept** as the maintainer / engine-debug tool
(it runs under gdb/lldb against the C engine). The WASM simulator is the **shipped, app-developer** loop.
Both drive the same engine and reload/state/asset logic — only the backend and host shell differ.
