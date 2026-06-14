# tools/web-sim — WASM simulator

The browser dev loop for embedded-react front-ends: the engine compiled to WebAssembly, rendering through
the [software backend](../../backends/software/) into a `<canvas>`. A React app authored against
`embedded-react` runs **unmodified in a browser**, pixel-accurate to an ARGB device. This is the simulator
that ships in the npm package (`npx embedded-react dev`, eventually) — no native toolchain for consumers.

See the full design — architecture, exported C ABI, packaging, phasing — in [**WASM_SIM.md**](../../WASM_SIM.md).

## Status — phase W1 (pipeline proven)

W1 renders a **static C scene** (`er_web_demo_scene`) to prove the `engine → WASM → canvas` path and the
ARGB→RGBA swizzle end to end. There is **no QuickJS yet** — the `.wasm` is engine-only (~215 KB). Driving a
real Flow A bundle (`er_web_load_source`), pointer-driven interactivity, asset packs, and hot reload arrive
in W2–W4.

## Build & run

Requires the [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html) (`emcc` on PATH).

```bash
node tools/web-sim/build.mjs          # → tools/web-sim/public/embedded-react.{js,wasm}
node tools/web-sim/serve.mjs          # → http://localhost:3333/
```

Open the URL. The canvas renders **1:1** (native resolution, no upscale, so it stays crisp). The board size
defaults to **800×480** and can be changed at runtime from the size selector (presets + a custom W×H) — like
resizing a responsive web preview; the scene rebuilds to fit. `?screen=WxH` sets the initial size.

The dev-server port defaults to **3333** (off the usual front-end ports so it won't collide with a Vite/CRA
server); override with `--port`. `build.mjs --debug` builds `-O0 -g` with assertions for troubleshooting.

## Layout

| File | Role |
|---|---|
| `build.mjs` | emcc build: engine + `backends/software` + `backends/web` → `public/embedded-react.{js,wasm}` |
| `serve.mjs` | minimal static server (correct `application/wasm` MIME); grows into the W3 dev server |
| `index.html` | host page: loads the module, sizes the canvas, rAF pump → `putImageData`, pointer → touch |
| `public/` | build output (git-ignored; built locally or shipped prebuilt by CI) |

The exported C ABI lives in [`backends/web/web_backend.h`](../../backends/web/web_backend.h); the present
layer (ARGB→RGBA) is [`backends/web/renderer_backend.c`](../../backends/web/renderer_backend.c).

## Relationship to the SDL simulator

The desktop [`tools/simulator`](../../SIMULATOR.md) (SDL) is **kept** as the maintainer / engine-debug tool
(it runs under gdb/lldb against the C engine). The WASM simulator is the **shipped, app-developer** loop.
Both drive the same engine and reload/state/asset logic — only the backend and host shell differ.
