# tools/web-sim — WASM simulator

The browser dev loop for embedded-react front-ends: the engine compiled to WebAssembly, rendering through
the [software backend](../../backends/software/) into a `<canvas>`. A React app authored against
`embedded-react` runs **unmodified in a browser**, pixel-accurate to an ARGB device. This is the simulator
that ships in the npm package (`npx embedded-react dev`, eventually) — no native toolchain for consumers.

See the full design — architecture, exported C ABI, packaging, phasing — in [**WASM_SIM.md**](../../WASM_SIM.md).

## Status — phase W3 (hot-reload dev loop)

W3 adds the **dev server** (`dev.mjs`): esbuild `--watch` rebundles on every save, bakes imported images/fonts
into an ERPK pack (loaded via `er_web_load_pack`), and pushes a Server-Sent reload event so the open page
re-loads the new bundle/pack with **no wasm rebuild** — the React Native inner loop in a browser. `useState`
survives the reload via the same Babel persist transform the SDL simulator uses. (W2 brought interactive Flow A
on `er_runtime`; the `.wasm` bundles engine + QuickJS-ng + bridge, ~1.2 MB, and is app-agnostic.)

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
| `build.mjs` | drives `cmake` with the Emscripten toolchain → `public/embedded-react.{js,wasm}` |
| `dev.mjs` | dev server: esbuild `--watch` + asset baking + SSE hot reload (the main dev loop) |
| `bundle-app.mjs` | one-shot esbuild a demo's JSX → `public/app.js` (for a static preview) |
| `serve.mjs` | minimal static server (correct MIME); for previewing a prebuilt bundle |
| `index.html` | host page: loads the module, fetches `app.js`/`assets.pack`, rAF pump → `putImageData`, pointer → touch, SSE reload |
| `public/` | build + bundle output (git-ignored; the wasm is shipped prebuilt by CI) |

The exported C ABI lives in [`backends/web/web_backend.h`](../../backends/web/web_backend.h); the present
layer (ARGB→RGBA) is [`backends/web/renderer_backend.c`](../../backends/web/renderer_backend.c).

## Relationship to the SDL simulator

The desktop [`tools/simulator`](../../SIMULATOR.md) (SDL) is **kept** as the maintainer / engine-debug tool
(it runs under gdb/lldb against the C engine). The WASM simulator is the **shipped, app-developer** loop.
Both drive the same engine and reload/state/asset logic — only the backend and host shell differ.
