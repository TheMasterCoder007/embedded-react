# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Versioning is **lockstep** across all distribution channels (npm, GitHub Release,
ESP-IDF Component Registry, PlatformIO) — a single root `VERSION` drives every
artifact. See [README](README.md#releasing) for the release process.

## [Unreleased]

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

[Unreleased]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.2.3...HEAD
[0.2.3]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.2.2...v0.2.3
[0.2.2]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.2.1...v0.2.2
[0.2.1]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.2.0...v0.2.1
[0.2.0]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.1.1...v0.2.0
[0.1.1]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/TheMasterCoder007/embedded-react/releases/tag/v0.1.0
