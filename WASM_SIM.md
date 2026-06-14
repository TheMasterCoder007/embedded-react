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
   dev server  ◄── websocket ──►  browser page
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
Extend `sim.mjs`: serve the page / `.wasm` / bundle / pack; on an esbuild rebuild, bake assets → pack and
push a websocket **"reload"** → the page calls `er_web_reset()` + reloads the new bundle (+ pack). Wrap as:

```
npx embedded-react dev [--screen 240x320] [--port 5173] [--device cyd]
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

## Phasing
- **W1** — `backends/web` present layer and the Emscripten build; render a *static* scene to a canvas to prove
  the engine→WASM→canvas pipeline (and the ARGB→RGBA swizzle) end to end.
- **W2** — wire `er_runtime`; `er_web_load_source` a real bundle (Flow A); pointer→touch → interactive.
- **W3** — dev server + esbuild watch + websocket hot-reload + the asset pack.
- **W4** — `npx embedded-react dev` CLI; CI builds + ships the prebuilt `.wasm` in the npm package.
- **W5 (optional)** — RGB565 preview, device-frame chrome, a static "share this UI" export (a web
  playground / docs embeds).

## Risks / open questions
- **QuickJS on WASM** — heap/stack tuning (mitigated: `er_runtime` already parameterizes both); validate the
  Promise/timer pump under `requestAnimationFrame`.
- **emsdk in CI** — adds an `mymindstorm/setup-emsdk` (or similar) step to the release workflow; first-time
  toolchain caching.
- **Pixel format** — validate the ARGB→RGBA swizzle against a desktop screenshot (a good first parity test).
- **`.wasm` size** — engine + QuickJS ≈ a few hundred KB to ~1 MB; fine for a dev tool, but worth tracking.
- **Decision deferred:** whether the dev server/CLI lives in the main `embedded-react` package or a separate
  `create-embedded-react` / `@embedded-react/dev` package (ties into the broader consumer-CLI productization).

---

See also: [SIMULATOR.md](SIMULATOR.md) (the SDL simulator), [BRIDGE.md](BRIDGE.md) (the QuickJS bridge),
[backends/web/](backends/web/) (the present layer), and [PLAN.md](PLAN.md).
