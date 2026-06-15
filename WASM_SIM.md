# WASM Simulator — design

A browser-based, hot-reloading dev loop for embedded-react front-ends. The engine is compiled to
WebAssembly and renders into a `<canvas>`, so a JSX app authored against `embedded-react` runs
**unmodified in a browser** — no native toolchain, no clone of this repo, pixel-accurate to the device.

It exists to close the gap a published npm consumer hits today: `npm install embedded-react` gives you the
component API, but the only *visual* dev loop (the SDL [simulator](SIMULATOR.md)) needs the compiled C host
and a clone of the monorepo. The WASM simulator is **packaged into the npm package** so consumers get
`npx embedded-react dev` out of the box.

> **Relationship to the SDL simulator.** The desktop [`tools/simulator`](SIMULATOR.md) is **kept**, as a
> maintainer / engine-debug tool — it can run under gdb/lldb to debug the C engine itself, which is painful in
> WASM. The WASM simulator becomes the **primary, shipped, app-developer** loop. Both drive the same
> portable host core (`er_runtime`), so the only thing that differs is the backend and the host shell — the
> reload/state/asset logic is shared, not forked. The SDL backend and the `examples/linux*` hosts also stay
> regardless, because the [parity harness](bridges/quickjs/js/parity.mjs) renders through them.

---

## Why this is mostly reuse

Two pieces already exist that make this small:

- **`backends/software`** — a CPU backend that composites the engine's `fill_rect` / `copy_rect` /
  `blend_rect` ops into a plain ARGB8888 RAM framebuffer. The WASM sim reuses it verbatim; "rendering" is
  already solved. (`backends/web` is the thin present layer on top — see below.)
- **`er_runtime`** (`bridges/quickjs/er_runtime.{c,h}`) — the portable, backend-agnostic Flow A host core
  (init → install globals → load source/bytecode → pump → **reset** → show-error → shutdown). It already
  parameterizes the QuickJS heap (`malloc_functions`) and stack guard (`max_stack_size`), which is exactly
  what a WASM build needs.

Everything else the loop needs is also already built and **shared with the SDL simulator**:

| Need | Reused from |
|---|---|
| Bundle the app + watch for edits | `sim.mjs` (esbuild `--watch`) |
| `useState` survives a reload | the sim's Babel persistence transform (`persist-transform.mjs`) |
| Bake imported images/fonts | the asset bakers (`assets/`) → an ERPK pack (`bakeAssetPack`) |
| Load assets at runtime, from memory | `er_assets_load_pack` (portable, mem-based) |
| Hot reload | `er_runtime_reset` + reload the new bundle/pack |
| Error overlay | `er_runtime_show_message` (the redbox) |

So the **new** code is: a present layer (`backends/web`), an Emscripten build, a browser host page, and a
dev server/CLI. No new rendering, reconciler, asset, or reload logic.

---

## Architecture

```
  esbuild --watch  ─►  app.bundle.js  +  assets.pack
        │                      │
   dev server  ──── SSE reload ──►  browser page
   (Node)                          │  loads
                                   ▼
                          embedded-react.wasm  (built once, app-agnostic)
                          ┌─────────────────────────────────────────────┐
                          │  QuickJS  →  native_ui_bridge  →  engine     │
                          │                          │                   │
                          │                  backends/software           │
                          │                          ▼                   │
                          │                ARGB8888 framebuffer          │
                          └──────────────────────────┬──────────────────┘
                                                      ▼  (ARGB→RGBA)
                                                  <canvas>  ◄── pointer/key events ──► touch
```

It runs **Flow A** (QuickJS *inside* WASM), so a JS edit reloads by re-running a new bundle string with **no
WASM rebuild** — the fast inner loop. (Flow B / AOT is the *device* build, not the dev loop.) Because it's
Flow A, the **same `.wasm` runs any app** → it is built once and shipped prebuilt.

---

## Components

### 1. `backends/web` — present layer
A `<canvas>` present on top of the software framebuffer. It does not rasterize; it converts and shows.

- Owns (or wraps the software backend's) `screen_w × screen_h` ARGB8888 framebuffer in WASM linear memory.
- Maintains a sibling **RGBA** output buffer; on present it converts the engine's ARGB8888 → canvas RGBA
  (a B↔R swizzle), ideally only over the commit's damage rect.
- Exposes the RGBA buffer pointer + dims so JS can wrap it as an `ImageData` and `ctx.putImageData(...)`.

~100 lines of C + a header.

### 2. Emscripten build → `embedded-react.wasm`
`emcc` compiles **engine + `bridges/quickjs` (er_runtime + native_ui_bridge) + `backends/software` +
`backends/web` + QuickJS** into one `.wasm` + JS glue. Exported C ABI (the host page calls these via
`cwrap`):

```c
int   er_web_init(int screen_w, int screen_h);     // runtime + backend + framebuffer
void  er_web_load_source(const char* js, int len); // run the app bundle (Flow A)
int   er_web_load_pack(const uint8_t* buf, int len);// register baked assets (ERPK)
void  er_web_pump(int dt_ms);                       // jobs/timers + commit + present
void  er_web_touch(int phase, int x, int y);        // ER_TOUCH_DOWN/MOVE/UP
void  er_web_reset(void);                           // for hot reload
const uint8_t* er_web_framebuffer(void);            // RGBA, screen_w*screen_h*4
int   er_web_fb_width(void); int er_web_fb_height(void);
```

Flags: `MODULARIZE`, `ALLOW_MEMORY_GROWTH` (QuickJS heap), `EXPORTED_RUNTIME_METHODS=ccall,cwrap,HEAPU8`,
`-O2`. Built via a CMake Emscripten toolchain or a small `emcc` script under `tools/web-sim`.

### 3. Browser host page
Loads the module, sizes a `<canvas>` to the board (e.g. 240×320, CSS-upscaled with `image-rendering:
pixelated` for a device-like preview), and runs a `requestAnimationFrame` loop: `er_web_pump(dt)` → read the
framebuffer → `putImageData`. Wires `pointerdown/move/up` → `er_web_touch`, pipes `console` to devtools, and
shows the redbox on a load error. (HEAPU8 views detach if memory grows — re-create the view each present.)

### 4. Dev server + CLI
The shared core `bridges/quickjs/js/sim-server.mjs`: serve the page / `.wasm` / bundle / pack; on an esbuild
rebuild, bake assets → pack and push a **Server-Sent Events** `"reload"` (SSE — one-way server→client, so no
WebSocket dependency) → the page re-loads the new bundle (+ pack). Both the repo loop (`tools/web-sim/dev.mjs`)
and the shipped CLI (`bridges/quickjs/js/cli.mjs`) wrap it:

```
npx embedded-react dev [entry] [--port 3333]      # watch + hot reload
npx embedded-react export [entry] [--out dir]     # a static, server-free playground
```

This is also the **first consumer-facing CLI** (it operates on the user's cwd, unlike today's repo-bound
`build.mjs`/`compile.mjs`).

---

## Fidelity
The software backend runs the *identical* rrect / text / vector / transform / image code the device runs, so
the preview is pixel-accurate for ARGB targets. Optional later: an **RGB565-quantize** toggle for byte-exact
CYD-style preview, and device-frame chrome. This shared-engine property also enables a **WASM-vs-desktop
parity check** (render the same scene both ways, pixel-diff) to guard the `backends/web` present path —
mirroring the existing Flow A↔B parity harness.

## Hot reload + state
JS save → esbuild rebuild → `reset` + reload bundle. The sim's existing Babel transform rewrites `useState`
to a persisting helper so component state **survives the reload** (press R to reset) — identical UX to the
SDL sim, and the same app code (it's plain `useState` on a real device). Asset edits rebuild the pack →
`er_web_load_pack` + `reset`.

## Build & packaging (the npm story)
The `.wasm` is **app-agnostic** (Flow A runs any bundle), so it is built **once** and shipped prebuilt — npm
consumers need no Emscripten:

- **Locally** (contributors): a `tools/web-sim` build script (needs emsdk) produces `embedded-react.wasm` +
  glue.
- **Released package**: the CI **release workflow** gains an emsdk + `emcc` build step that produces the
  `.wasm`, places it under the npm package, and the `files` whitelist ships it. So `npm install embedded-react`
  carries the prebuilt simulator; `npx embedded-react dev` just works. The artifact is **not committed**
  (built in CI), matching how `dist/` is handled.
- Versioning is lockstep: the shipped `.wasm` is the package's version (the engine + bridge it was built
  from are the same commit).

---

## Phasing — W1–W5 BUILT
- **W1 ✅** — `backends/software` CPU compositor + `backends/web` present layer (ARGB→RGBA swizzle) + the
  Emscripten build; renders a static scene to a canvas, proving the engine→WASM→canvas pipeline.
- **W2 ✅** — `er_runtime` wired (QuickJS-in-WASM); `er_web_load_source` runs a real Flow A bundle;
  pointer→touch → interactive. Build moved to CMake + the Emscripten toolchain (reuses `bridges/quickjs`).
- **W3 ✅** — the dev server (`tools/web-sim/dev.mjs`): esbuild `--watch`, the ERPK asset pack
  (`er_web_load_pack`), and hot reload over **Server-Sent Events** (SSE — a WebSocket would need an extra
  dependency; SSE is one-way server→client, which is all reload needs). `useState` survives via the Babel
  persist transform.
- **W4 ✅** — `npx embedded-react dev` (and `export`), the consumer CLI in the npm package; the dev-server core
  is shared (`bridges/quickjs/js/sim-server.mjs`); CI builds + ships the prebuilt `.wasm`. **Decision:** the
  dev/export CLI lives in the **main `embedded-react` package** (it's a dev/authoring toolchain, not the device
  runtime); the project **scaffolder** is a separate `create-embedded-react` package (`npm create embedded-react`).
- **W5 ✅** (RGB565 preview deferred) — device-frame chrome (a cosmetic bezel toggle) and a static
  `embedded-react export` playground (a self-contained folder, no server).

## Resolved risks / notes
- **QuickJS on WASM** — runs fine; `-sSTACK_SIZE=4MB` gives the parser/eval headroom above QuickJS's own guard;
  the Promise/timer pump is driven per-frame from the rAF loop via `er_web_pump` → `er_runtime_pump`.
- **emsdk in CI** — `mymindstorm/setup-emsdk` + `node tools/web-sim/build.mjs`, gated on `PUBLISH_NPM`, before
  the npm publish step (`release.yml`). `build.mjs` derives the toolchain from `$EMSDK`/`emcc`.
- **Pixel format** — the ARGB→RGBA swizzle is verified (a 0xAARRGGBB background renders as the exact `R,G,B`
  bytes; confirmed against native renders).
- **`.wasm` size** — ~1.2 MB (engine + QuickJS-ng + bridge). Fine for a dev tool.

---

See also: [SIMULATOR.md](SIMULATOR.md) (the SDL simulator), [BRIDGE.md](BRIDGE.md) (the QuickJS bridge),
[backends/web/](backends/web/) (the present layer), and [PLAN.md](PLAN.md).
