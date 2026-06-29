# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Versioning is **lockstep** across all distribution channels (npm, GitHub Release,
ESP-IDF Component Registry, PlatformIO) — a single root `VERSION` drives every
artifact. See [README](README.md#releasing) for the release process.

## [Unreleased]
### Fixed

- **Vector gradients render again** — every `<Svg>` gradient paint (`fill`/`stroke="url(#id)"` with a linear,
  radial, or conic gradient) was drawing as transparent. A stray `return NULL;` in `resolve_grad`
  (`engine/rendering/vector.c`) bailed out of every gradient lookup, so the renderer fell back to the shape's
  solid color — which is transparent for `url()` paints. The bug only surfaced in the simulators (gradients
  enabled); the engine's gradient tests are compiled out of the default build, where the feature is off. The
  rendering ctests now also run in a gradients-enabled CI pass so this path is covered going forward.
- **Dev server no longer crashes on a missing file** — the WASM simulator dev server (`npx embedded-react
  dev`, `sim-server.mjs`) wrote the `200` response headers before reading the requested file, so any miss
  (a stray `/favicon.ico`, a not-yet-built `/public/` asset) threw `ERR_HTTP_HEADERS_SENT` from the 404
  fallback and took the whole server down. It now reads the file first and only then writes headers, so a
  miss returns a clean 404 and the server stays up.

## [0.5.2] - 2026-06-28
### Fixed

- **Simulators now enable every engine feature at full capacity** — the web (`tools/web-sim`) and desktop
  (`tools/simulator`) simulators left several caps at the lean MCU defaults, so features could silently
  disappear (e.g., conic gradients vanished once a node had more than eight gradients). Both now turn on every
  optional feature and raise every static cap — engine pools and the QuickJS bridge's own pools alike. 
  Per-board/device builds keep the lean engine defaults.

## [0.5.1] - 2026-06-28
### Changed

- **Updated template icon** — both the JavaScript and TypeScript starter templates now ship the new v2
  embedded-react icon (`assets/icons/embedded-react.png`).

## [0.5.0] - 2026-06-28
### Added

- **TypeScript apps, end to end** — author embedded-react apps in TypeScript. `npm create
  embedded-react@latest my-app -- --ts` (or `npx create-embedded-react my-app --ts`) scaffolds a TS starter
  with a `tsconfig.json`, ambient declarations for asset imports, and a `npm run typecheck` script. Every
  flow accepts `.ts`/`.tsx`: the dev simulator (with hot-reload state preservation), the static export, the
  Flow A device build, and the Flow B AOT compiler. Types are erased by esbuild on the dev/export/Flow A
  paths and by an in-place AST scrub on the AOT path that preserves source locations — so compiler
  code-frames still point at your real `.tsx`, and the generated C for a `.tsx` app is identical to its
  untyped `.jsx` twin.
- **Bundled type declarations** — the `embedded-react` npm package now ships `.d.ts` types for its public
  API (the components, `StyleSheet`, `Animated`, `useAnimatedValue`/`usePersistentState`, `AppRegistry`, …),
  wired through `package.json` `exports`/`types`. `import { View } from 'embedded-react'` is typed out of the
  box — no separate `@types/embedded-react` needed.

## [0.4.1] - 2026-06-27
### Changed

- Animated 2D transforms (scale/rotate) now clear only the touched region of the offscreen scratch buffer
  each frame instead of the whole `ERUI_SCRATCH_W × ERUI_SCRATCH_H` buffer.
- Animated 2D scale transforms sample through a separable fast path that hoists the per-pixel inverse-map and `floorf` out of the inner loop (constant per row/column under a pure axis-aligned scale), at full bilinear quality (output unchanged). Rotation/shear keep the general affine path.

### Fixed

- Animated 2D transforms (scale/rotate) no longer force a full-screen repaint every frame — the damage
  pre-pass bounds the repaint to the transformed node's region (and its previous footprint, so a node moved
  by reflow or scroll leaves no trail).
- Animated 3D/perspective transforms (rotateX/rotateY/perspective) no longer force a full-screen repaint
  every frame — the damage pre-pass projects the node's 3D AABB (the same homography render_tree paints
  with) and bounds the repaint to it, mirroring the 2D fix. Only affects `ERUI_3D_TRANSFORMS` builds.

## [0.4.0] - 2026-06-26
### Added

- **Import an `.svg` file and render it as a live vector** — `import logo from './logo.svg'` then
  `<Svg source={logo} />`. The SVG is baked to the engine's op-tape at build time, with every transform
  (matrix/translate/rotate/scale/skew, nested) folded into absolute coordinates so the device needs no
  transform support, and inlined into the bundle as compact numeric data. Works in both Flow A (QuickJS) and
  Flow B (AOT). Supports `<path>`, `<rect>`, `<circle>`, `<ellipse>`, `<line>`, `<polygon>`, `<polyline>`,
  `<g>` nesting, `viewBox`, and presentation-attribute / inline-style / inheritance paint resolution.
- **Vector gradients** — linear, radial, and conic gradients on both the fill and the stroke of vector
  shapes (inline `<Svg>` children and imported `.svg` files), up to eight color stops, in both flows. Gradients
  share a per-node table, so many shapes can reference few gradients. An elliptical radial (a non-square
  `objectBoundingBox`, or a non-uniform / sheared `gradientTransform`) is approximated by its best-fit
  circle. Gated by `ERUI_GRADIENT` / `ERUI_GRADIENT_RADIAL` plus the new `ERUI_GRADIENT_CONIC`.
- **Automatic raster fallback for unsupported SVG features** — when an imported `.svg` uses features the
  vector engine can't represent (`<text>`, `<mask>`, `<filter>`, `<use>`, `<pattern>`, a `filter`/`mask`/
  `clip-path` on a shape, or a **dashed stroke**), the whole SVG is rasterized at build time (via resvg) and
  rendered as a baked image instead of silently dropping the content. It is transparent to the app — the same
  `<Svg source>` renders a live vector or a raster image — and works in Flow A (asset pack) and Flow B
  (compiled into `assets.generated.c`). A build-time warning names the feature that triggered the fallback.
  Adds the build-time dependency `@resvg/resvg-js` (it never reaches the device).
- **Tunable vector pools + overflow diagnostics** — the `ERUI_VECTOR_*` pool sizes are exposed as CMake
  cache variables (and IDF compile definitions), with debug-build diagnostics that warn — naming the macro to
  raise — when an SVG overflows a pool instead of silently truncating. The per-node op-tape store lives in its
  own translation unit so it can be mapped to PSRAM on boards that have it.

### Changed

- Vector conic-gradient rendering now builds a precomputed color LUT per shape and uses a polynomial `atan2`
  approximation, replacing the per-pixel stop interpolation and the libm `atan2f` call — substantially faster
  on the soft-float MCU path, with no visible change in output.

## [0.3.0] - 2026-06-16
### Added

- **`embedded-react build`** — produce the device artifact from your own project, the deploy step
  alongside `dev`/`export`:
  - default → **`dist/app.erpkg`** (Flow A): the app bundle compiled to **QuickJS bytecode** + baked
    assets + CRC, ready to upload to a device's config region (`er_runtime_load_container`). The bytecode
    is compiled through the prebuilt simulator `.wasm`, so this needs **no native toolchain**.
  - `--aot` → **`dist/app.gen.c` / `.h` + `assets.generated.c`** (Flow B): the app compiled
    ahead-of-time to C for no-PSRAM boards (compiled into firmware; supports the AOT subset — animation
    composition such as `Animated.loop`/`sequence` isn't supported yet and reports a located error).
  The scaffolder template gains a `build` script.

### Changed

- The WASM simulator module now also targets Node (`-sENVIRONMENT=web,node`) and exports
  `er_web_compile_bytecode` (a `qjsc`-style entry, backed by the new `er_runtime_compile_bytecode`), so
  `embedded-react build` compiles bytecode with no native compiler. A `.cjs` copy of the module ships
  for Node `require` (the package is ESM).

## [0.2.3] - 2026-06-15

### Fixed

- **The scaffolded starter (and any consumer app) crashed on `npx embedded-react dev`** with
  "Maximum call stack size exceeded". The simulator's `useState`→persist transform was applied to every
  file under the project root — which, in a published install, includes
  `node_modules/embedded-react`. It therefore rewrote the library's own `usePersistentState` into a
  call to itself, recursing forever. The transform now excludes `node_modules`, so only the app's own
  source is rewritten. This only reproduced in a real install (in the monorepo the library sits outside
  the project root, which is why it was missed). Covered by a regression test on the file-selection rule.

## [0.2.2] - 2026-06-15

### Fixed

- `Animated.loop` now defers each iteration (instead of restarting it inline from the completion
  callback), so a child animation that completes synchronously can't recurse into the next iteration.
  Defensive hardening of the loop/sequence composition; complements the 0.2.0 sequence-restart fix.
  (Note: this did **not** fix the scaffolded-starter crash — that was the persist transform, fixed in
  0.2.3.)

## [0.2.1] - 2026-06-15

Maintenance release — dependency, security, and release-pipeline fixes. No changes to the component
API, the engine, or runtime behavior.

### Security

- Updated `esbuild` (the build-time bundler dependency) from 0.24 to **0.28.1**, clearing two
  high-severity esbuild advisories (dev-server arbitrary file read; Deno binary integrity).
  embedded-react does not expose esbuild's dev server, so real-world impact was low, but the dependency
  is now patched.

### Changed

- Dev toolchain: pinned `vitest` to **3.x** and added an `esbuild` override so `npm audit` reports zero
  vulnerabilities and `npm ci` installs reproducibly across platforms. (vitest 4's oxc transformer pulled
  nested, platform-specific optional deps that npm could not capture cross-platform, breaking `npm ci`.)

### Fixed

- Release workflow: restored `registry-url` so npm OIDC **trusted publishing** completes its token
  exchange (tokenless publish). Documented the gotcha that the trusted-publisher owner/repo must match
  the GitHub OIDC claim's exact case (`TheMasterCoder007`).

## [0.2.0] - 2026-06-15

First beta.

### Added

- **WASM simulator** — the engine compiled to WebAssembly, running React (Flow A on
  QuickJS) in the browser. A CPU compositor (`backends/software`) renders into an
  ARGB framebuffer that a canvas present layer (`backends/web`) shows; pointer events
  drive it, imported images/fonts are baked into an asset pack, the canvas is 1:1 and
  responsive (with optional device-frame chrome), and edits hot-reload over
  Server-Sent Events. The prebuilt `.wasm` ships in the npm package, so consumers need
  no Emscripten. See [tools/web-sim](tools/web-sim/README.md).
- **`embedded-react` CLI** — `npx embedded-react dev [entry]` runs the simulator on your
  own project with hot reload (`useState` preserved); `npx embedded-react export` builds
  a self-contained static playground (no server) for sharing or docs embeds.
- **`create-embedded-react`** — `npm create embedded-react@latest my-app` scaffolds a
  fresh standalone project (a styled starter: a pulsing logo + a `count is N` button),
  wired for `npm run dev` and `npm run export`. Published as a second lockstep package.

### Changed

- **Documentation restructured for the beta.** The standalone design docs (`ENGINE.md`,
  `BRIDGE.md`, `PLAN.md`, `SIMULATOR.md`, `WASM_SIM.md`) are removed; the project now
  carries READMEs only, plus a single root [`ROADMAP.md`](ROADMAP.md) (known issues,
  planned work, performance backlog, vision) and [`CONTRIBUTING.md`](CONTRIBUTING.md)
  (engine invariants + code/doc rules, formerly `RULES.md`). Engine internals (Yoga
  passes, pixel format, scratch model, the `ERUI_*` feature-flag table) moved into
  [`engine/README.md`](engine/README.md).

### Fixed

- `Animated.sequence` was not restartable: its internal step index was never reset, so
  `Animated.loop(Animated.sequence([…]))` (the ping-pong pattern) re-entered synchronously
  on the second iteration and overflowed the stack, freezing the animation. Sequences now
  reset their state on each `start()`.

## [0.1.1] - 2026-06-14

### Fixed

- AOT-generated C now emits a fallback `M_PI` definition when the toolchain's
  `<math.h>` does not expose it (strict ISO C99 / MSVC). Apps that use `Math.PI`
  now compile across all targets instead of failing with `M_PI undeclared`.

### Added

- Continuous integration (GitHub Actions): JS unit tests (vitest), engine build
  + `ctest`, and AOT compile with a `-std=c99` syntax check of the generated C.
- Release automation: `tools/release.mjs` performs a one-step lockstep release
  (bump `VERSION` → sync every artifact → commit → annotated tag), and a
  tag-driven release workflow gates `tag == VERSION` and publishes all channels.
- PlatformIO distribution via root `library.json`.
- ESP-IDF component finalized: `idf_component.yml` relocated to `engine/` with a
  dual-mode `CMakeLists.txt` (`idf_component_register` under ESP-IDF, standalone
  `project()` otherwise).
- `LICENSE` and `NOTICE` propagated into each distributed package (Apache-2.0
  §4(d)) by `tools/sync-version.mjs`.
- `Install` and `Releasing` sections added to the root README.

### Changed

- npm-facing README rewritten: clarifies the monorepo relationship, documents
  Flow B (AOT), and fixes relative links that 404'd on the npm package page
  (now absolute GitHub URLs).

## [0.1.0] - 2026-06-14

Initial public release.

### Added

- **Engine** — pure C99 React Native-style renderer (`engine/`, public API
  `er_scene.h`): flexbox-style layout, text, images, vector/SVG primitives, and
  animation, with a backend-agnostic render interface (the integrator supplies
  the framebuffer flush).
- **Flow A** — JSX/React interpreted on-device through the QuickJS NativeUI
  bridge for instant iteration on PSRAM-class MCUs.
- **Flow B (AOT)** — `bridges/quickjs/js/aot` compiles JSX ahead-of-time to C
  (no QuickJS) for no-PSRAM MCUs: `useState`/events, control flow, dynamic
  lists, components, images, and native-driver animations.
- **Backends** — desktop SDL (development) and ESP32 SPI-LCD, including a banded
  RGB565 mode for 16-bit color on RAM-constrained panels.
- **Simulator** — RN-style hot-reload desktop simulator (`tools/simulator`,
  `npm run sim`).
- **Versioning foundation** — root `VERSION` as the single source of truth,
  `engine/include/er_version.h` macros, `tools/sync-version.mjs` to propagate the
  version to every artifact, and an AOT compile-time version-pin
  (`_Static_assert`) that fails a build against a mismatched engine.
- First publish to npm as `embedded-react`.

[Unreleased]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.5.2...HEAD
[0.5.2]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.5.1...v0.5.2
[0.5.1]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.5.0...v0.5.1
[0.5.0]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.4.1...v0.5.0
[0.4.1]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.4.0...v0.4.1
[0.4.0]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.2.3...v0.3.0
[0.2.3]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.2.2...v0.2.3
[0.2.2]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.2.1...v0.2.2
[0.2.1]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.2.0...v0.2.1
[0.2.0]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.1.1...v0.2.0
[0.1.1]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/TheMasterCoder007/embedded-react/releases/tag/v0.1.0
