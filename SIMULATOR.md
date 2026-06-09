# Simulator & Hot Reload — Design

Design doc for the embedded-react **simulator**: a dedicated dev tool, launched from a JSX app the
way you'd run a React Native app, that **hot-reloads your code as you edit it**. This is the
developer-experience counterpart to Flow A (see [PLAN.md](PLAN.md)) — the runtime exists; this is
about the edit→see loop.

> Status: **Phases 0, 1, 2, 3 shipped.** `npm run sim` runs the simulator with file-watch +
> full-remount hot reload (JS **and** images/fonts), an RN-redbox error overlay, an R reload key, and
> **transparent state preservation** — plain `useState` survives a reload (a sim-only build
> transform). Still ahead: on-device hot reload (4). See **Running it** and the phase table.

## Running it

```
# one-time: build the simulator binary
cmake -S tools/simulator -B tools/simulator/build [-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake]
cmake --build tools/simulator/build

# then, from your app folder (React Native style):
cd demos/thermostat && npm run sim
```

Each demo has a thin `package.json` whose `sim` script delegates to the package's `sim.mjs`, so you
run it from the app you're editing. (Equivalent: `cd bridges/quickjs/js && npm run sim <demo>`.)

`npm run sim` bundles the demo (+ bakes its assets once), starts `esbuild --watch`, and launches the
simulator window. Edit a file under `demos/<demo>/` (or the library `src/`), save, and the window
live-reloads. A JS error doesn't close the window — fix the source and save to reload. The component state 
is reset on each reload (Fast Refresh is Phase 3). Asset *changes* need a sim rebuild (Phase 2).

> Output (`app.bundle.js`, baked assets) always lands in `bridges/quickjs/js/dist/` regardless of the
> folder you run from — that's the single "active app" the simulator and example hosts read.

---

## Goal

Match the React Native inner loop on embedded:

```
npm run sim         # (eventually: npx embedded-react sim)
# → a window opens running your app; edit src/App.jsx, save, the window updates instantly
```

Two principles:

1. **The simulator is a dev tool, not a demo.** It watches your source, rebundles on save, and
   live-updates a running engine window.
2. **The desktop "demo" is just a demo.** `examples/linux` stays a *board-like example* — peer to
   `examples/esp32/esp32-s3`: one engine + one backend + one bridge running a baked bundle, with no
   dev affordances. The dev-iteration role moves entirely to the simulator.

This separation is the point: today `examples/linux` quietly doubles as both, and that conflates "show
the thing" with "build the thing."

---

## Architecture

The simulator reuses the same C host core as the desktop example (QuickJS + engine + SDL backend) and
adds a watch + reload layer:

```
your JSX project          dev machine                               window
─────────────────         ──────────────────────────────────        ──────────
src/App.jsx  ──edit──▶  esbuild --watch ──rebundle──▶ dist/app.bundle.js
                                                                  │ (change detected)
                                                                  ▼
                                              simulator host (QuickJS + engine + SDL)
                                              reset scene → re-eval bundle → repaint
```

Three pieces:

1. **Watcher / bundler** — `esbuild` in watch/incremental mode (already the bundler; see
   `bridges/quickjs/js/build.mjs`). Rewrites `dist/app.bundle.js` on save.
2. **Simulator host** — the SDL host (`examples/linux/main.c` today), but instead of loading a baked
   bundle once, it reloads when the bundle changes. Shares a factored host core with the demo.
3. **Reload channel** — how the host learns the bundle changed (see Decision 1).

---

## Key decisions

Each lists the options and the leaning; resolve when the work starts.

### 1. Reload transport — file-watch vs dev-server

- **File-watch** *(leaning, for the MVP)* — the host polls `dist/app.bundle.js` mtime (or a small
  trigger file) and reloads on change. No protocol; works because bundler and host are on the same
  machine. Minimal code.
- **Dev-server (Metro-style)** — a Node WS/TCP server pushes the new bundle to a host WS client; on
  rebuild it sends the bytes + a "reload" command, and streams logs back. More infra, but it's the
  path that extends to **on-device hot reload** (push bytecode to the ESP32 over serial / Wi-Fi).

**Plan:** ship file-watch first, but design the host's reload entry point as `reload(bundleBytes)` so
a socket transport can be layered on later (Phase 4) without reworking the host.

### 2. Reload level — live reload vs Fast Refresh

- **Live reload (full remount)** *(leaning, for the MVP)* — on change: reset the engine scene,
  dispose/recreate the QuickJS context, re-eval the new bundle. **Loses component state.** Robust.
- **Fast Refresh (state-preserving HMR)** — needs the `react-refresh` runtime + Metro-style
  module-boundary HMR. Much larger; matches BRIDGE.md's "Hot reload / Fast Refresh — post-slice."

**Resolved:** live reload (full remount) is the model. Rather than full Fast Refresh (module HMR +
react-refresh — large, with real embedded-QuickJS risk), Phase 3 shipped **transparent state
preservation**: a sim-only Babel transform rewrites the app's `useState` into a persisting helper
(`usePersistentState`, backed by a host-side store), so plain `useState` survives the remount with no
opt-in. On a device there's no transform, so it's plain `useState`. Full Fast Refresh (per-component,
no keying fragility) remains a possible future upgrade.

### 3. Asset hot reload — baked vs runtime-loaded

Today assets are baked into the binary (`dist/assets.generated.c` → `er_register_assets()`), which
defeats hot-reloading an image or font. The simulator is the use case for the **runtime**
`loadImage`/`loadFont` path deferred in BRIDGE.md §1.5:

- **Shipped (Phase 2b)** — the JS bakers emit a binary **ERPK pack** (`assets/emit-pack.mjs` →
  `dist/assets.pack`); the sim host loads it at runtime (`tools/simulator/asset_pack.c` →
  `er_image_load` / `er_font_register`) and re-registers when it changes. Reusing the same bakers
  means sim fonts/images are pixel-identical to the device's baked assets, with no C rasterizer.
  **Simulator only** — on a device, assets stay baked into the firmware.

### 4. Where it lives

A new **`tools/simulator/`** (the C host variant + the JS watcher), sharing the host core with
`examples/linux`. Alternative: a top-level `simulator/`. Minor call; resolve at start.

---

## The main engineering unknown: a clean engine/bridge reset

Live reload requires tearing the runtime back down to an empty scene and bringing up a fresh QuickJS
context. The engine uses **static pools** (node pool, Animated value/binding pools), and the bridge
holds JS-rooted registries (handle table, `__er_event_handlers`, `__er_timer_handlers`, anim-complete
slots). A reload must clear all of it. Likely needs:

- `er_scene_reset()` (or equivalent) — free/clear all nodes, reset the root, reset the static pools to
  their init state.
- `er_bridge` teardown that releases the rooted registries, then a fresh `er_bridge_install()` on a
  new `JSContext` (or a documented full-runtime recreate).

This is the piece to **scope against the actual code before estimating** — the rest (esbuild watch,
mtime poll, re-eval) is straightforward. Note: the desktop host already has a clean teardown path on
exit (`JS_FreeContext`/`JS_FreeRuntime`); the question is doing it mid-process and re-initializing the
engine's static state safely.

---

## Phased plan

| Phase | Scope | Status |
|-------|-------|--------|
| **0** | Factor the SDL host core into a shared unit (`examples/linux/host.{c,h}`); frame `examples/linux` as a pure demo (peer to esp32). | ✅ done |
| **1 — MVP** | `tools/simulator/` target + `npm run sim` (esbuild `--watch` + launch). File-watch → **live reload** (full remount) on save. JS-only. Backed by `er_reset()` (engine) + handle-table reset (bridge) + `er_host_reload()` (host). | ✅ done |
| **2a** | In-window **error overlay** (RN redbox) for uncaught JS exceptions; **manual reload key** (R). | ✅ done |
| **2b** | Runtime asset loading (image/font hot reload) via a binary **ERPK pack**: the JS bakers emit `dist/assets.pack`, the sim host loads it (`asset_pack.c`) and re-registers on change. Fonts identical to device. | ✅ done |
| **3** | **State preservation across reload** — plain `useState` transparently persists across saves via a **sim-only Babel transform** (`persist-transform.mjs` rewrites app `useState` → a persisting helper keyed by `module::component#index`, backed by the host-side `__erPersist` store). Nothing to opt into; on a device it's plain `useState`. Press R to reset. Chosen over full Fast Refresh (module HMR + react-refresh) — same transparency for a fraction of the effort/risk. | ✅ done |
| **4** | **On-device hot reload** — dev-server/socket transport pushing bytecode to the ESP32 over serial / Wi-Fi (the "later luxury"). | ☐ |

The reload primitive is **`er_reset()`** (public, `er_scene.h`): it empties the node pool, root, and
the animation / layout-animation / vector / input subsystems, while keeping the backend and registered
images/fonts. The bridge resets its handle table on `er_bridge_install`, and `er_host_reload()` ties it
together (free context → `er_reset()` → fresh context + bridge + globals → re-eval).

---

## Relationship to `create-embedded-react`

The simulator is the natural partner of the remaining Flow A item, the `create-embedded-react`
scaffold (PLAN.md). The scaffold would generate a `package.json` with `"sim": "embedded-react sim"`,
so `npm run sim` works from a fresh app with zero setup — the React Native feel. Build them together,
or the scaffold immediately after the simulator MVP.

---

## Open questions

1. ✅ **Reload transport** — file-watch (the sim polls the bundle's mtime+size every 200 ms). A
   dev-server/socket transport is deferred to Phase 4 (on-device).
2. ✅ **Simulator location** — `tools/simulator/` (reuses the shared `examples/linux/host.c`).
3. ✅ **Desktop demo** — kept symmetric with esp32: it runs whatever `npm run build` last produced; no
   pinning.
4. ✅ **Asset hot-reload decoder** — reuse the JS bakers (fonts identical to device) via a binary
   **ERPK pack** (`assets/emit-pack.mjs`) the sim host loads (`tools/simulator/asset_pack.c`). No C
   rasterizer; the sim loads assets at runtime instead of compiling them in.

## Known limitations / rough edges (for later phases)

- **Live reload resets component state** (by design until Fast Refresh, Phase 3).
- **One-time CMake build** of the sim binary is required before `npm run sim` (the script prints how if
  it's missing). A future `create-embedded-react` scaffold + prebuilt/auto-built binary would remove
  this step.
- **Asset-pack reload leaks** the previous pack's buffers (a few KB per asset change) — the engine has
  no "unregister", so the sim keeps old buffers alive rather than risk a dangling reference. Fine for a
  short-lived dev session.
- **Mid-write races** are handled by self-correction: a partial read fails (eval throws / pack parse
  returns false, window stays up) and the next poll reloads the finished file.
