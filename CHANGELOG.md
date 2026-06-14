# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Versioning is **lockstep** across all distribution channels (npm, GitHub Release,
ESP-IDF Component Registry, PlatformIO) — a single root `VERSION` drives every
artifact. See [README](README.md#releasing) for the release process.

## [Unreleased]

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

[Unreleased]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.1.1...HEAD
[0.1.1]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/TheMasterCoder007/embedded-react/releases/tag/v0.1.0
