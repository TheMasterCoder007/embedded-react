# tools/web-sim — the WebAssembly simulator

The engine, QuickJS-ng, the bridge and the `software` + `web` backends compiled to one WebAssembly
module, rendering into a `<canvas>`. This is the simulator that ships in the npm package as
`npx embedded-react dev`; the `.wasm` is prebuilt by CI on release, so consumers never need
Emscripten. Inside the repository this folder builds the module and runs the same dev loop over
`demos/`.

**Docs:** [Run it in the simulator](https://embedded-react.dev/getting-started/simulator),
[Hot reload](https://embedded-react.dev/guides/hot-reload), and the build table in
[Architecture](https://embedded-react.dev/internals/architecture#building-the-pieces).

## Develop

Requires the [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html) once to
build the module (`emcc` on PATH, `$EMSDK`, or `em-config`; a Homebrew `emscripten` works as-is, and
`EMSCRIPTEN_ROOT="$(em-config EMSCRIPTEN_ROOT)"` overrides the detection).

```bash
node tools/web-sim/build.mjs          # once → public/embedded-react.{js,wasm}; also stages the package's sim/
node tools/web-sim/dev.mjs [demo]     # watch + bake assets + hot reload → http://localhost:3333/  (default: watch-face)
```

`build.mjs --debug` builds `-O0 -g` with assertions. For a static preview of an already-built bundle:
`node tools/web-sim/bundle-app.mjs [demo]` then `node tools/web-sim/serve.mjs`. The prebuilt wasm goes
stale after engine changes; rebuild it.

## Layout

| File | Role |
|---|---|
| `CMakeLists.txt` | Emscripten build: engine + QuickJS bridge + `backends/software` + `backends/web` → the wasm module |
| `build.mjs` | drives `cmake` with the Emscripten toolchain → `public/`, and stages the package's `sim/` |
| `dev.mjs` | the repo dev loop over `demos/`: a thin wrapper over the shared `bridges/quickjs/js/sim-server.mjs` |
| `bundle-app.mjs`, `serve.mjs` | one-shot bundle and a static server, for previewing a prebuilt bundle |
| `index.html` | the host page: loads the module, fetches `app.js`/`assets.pack`, rAF pump → `putImageData`, pointer → touch, SSE reload |
| `public/` | build output (gitignored) |

The exported C ABI is `backends/web/web_backend.h`. The desktop SDL simulator in `../simulator` is
the maintainers' engine-debug tool (it runs under gdb/lldb); this one is the shipped app-developer
loop.
