# tools/web-sim — WASM simulator

The browser dev loop for embedded-react front-ends: the engine compiled to WebAssembly, rendering through
the [software backend](../../backends/software/) into a `<canvas>`. A React app authored against
`embedded-react` runs **unmodified in a browser**, pixel-accurate to an ARGB device. This is the simulator
that ships in the npm package (`npx embedded-react dev`, eventually) — no native toolchain for consumers.

See the full design — architecture, exported C ABI, packaging, phasing — in [**WASM_SIM.md**](../../WASM_SIM.md).

## Status — phase W2 (interactive Flow A)

W2 runs a **real Flow A app** (QuickJS-in-WASM) on the portable [`er_runtime`](../../bridges/quickjs/er_runtime.h)
host core: an esbuild bundle is handed to `er_web_load_source`, pointer events drive it, and `er_web_pump`
services Promises/timers each frame. The `.wasm` now bundles the engine + QuickJS-ng + the bridge (~1 MB), and
is **app-agnostic** — the same module runs any bundle. Asset packs (`<Image>`/custom fonts) and esbuild-watch
hot reload arrive in W3.

## Build & run

Requires the [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html) (`emcc` on PATH).

```bash
node tools/web-sim/build.mjs               # → public/embedded-react.{js,wasm}  (first run also fetches+builds QuickJS-ng)
node tools/web-sim/bundle-app.mjs [demo]   # → public/app.js   (default: music-player; an asset-free demo)
node tools/web-sim/serve.mjs               # → http://localhost:3333/
```

Open the URL — the bundled app runs interactively. The canvas renders **1:1** (native resolution, no upscale,
so it stays crisp) and fills the viewport, so the browser's device toolbar drives the board size (set it to
240×320 and the app renders at exactly 240×320, like a responsive web project). The floating gear chip locks
to a specific panel size or a custom W×H. `?screen=WxH` sets the initial size.

The dev-server port defaults to **3333** (off the usual front-end ports so it won't collide with a Vite/CRA
server); override with `--port`. `build.mjs --debug` builds `-O0 -g` with assertions for troubleshooting.

## Layout

| File | Role |
|---|---|
| `CMakeLists.txt` | Emscripten build: engine + QuickJS bridge + `backends/software` + `backends/web` → the wasm module |
| `build.mjs` | drives `cmake` with the Emscripten toolchain → `public/embedded-react.{js,wasm}` |
| `bundle-app.mjs` | esbuild a demo's JSX → `public/app.js` (the Flow A bundle the page loads) |
| `serve.mjs` | minimal static server (correct `application/wasm` MIME); grows into the W3 dev server |
| `index.html` | host page: loads the module, fetches `app.js` → `er_web_load_source`, rAF pump → `putImageData`, pointer → touch |
| `public/` | build output (git-ignored; built locally or shipped prebuilt by CI) |

The exported C ABI lives in [`backends/web/web_backend.h`](../../backends/web/web_backend.h); the present
layer (ARGB→RGBA) is [`backends/web/renderer_backend.c`](../../backends/web/renderer_backend.c).

## Relationship to the SDL simulator

The desktop [`tools/simulator`](../../SIMULATOR.md) (SDL) is **kept** as the maintainer / engine-debug tool
(it runs under gdb/lldb against the C engine). The WASM simulator is the **shipped, app-developer** loop.
Both drive the same engine and reload/state/asset logic — only the backend and host shell differ.
