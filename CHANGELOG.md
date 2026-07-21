# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Versioning is lockstep across all distribution channels (npm, GitHub Release,
ESP-IDF Component Registry, PlatformIO) — a single version drives every artifact.
See the README for the release process.

## [Unreleased]
### Added

- Lite QuickJS profile for Flow A. The runtime now creates its JS context with only the intrinsics the
  React runtime actually needs (base objects, RegExp, JSON, Map/Set, Promise, plus a `performance.now`
  clock) instead of the full standard library, and the same set runs everywhere — device, desktop,
  simulator, and test harnesses — so an app that works in development works on hardware. Features an app
  legitimately needs beyond that (Date, Proxy, typed arrays, WeakRef, BigInt) can be opted back in per
  host via `ErRuntimeConfig.extra_intrinsics`.
- Parser-less device builds (`-DER_BRIDGE_QUICKJS_LITE=ON`). Firmware that only runs precompiled bytecode
  can drop the JS source parser from QuickJS entirely, cutting roughly 60 KB of flash. The error overlay
  now ships as precompiled bytecode so it renders on such builds too.
- Release bytecode is stripped. `embedded-react build`, the demo packer, and the bytecode precompiler now
  omit the embedded source text and debug tables from `.qbc`/`.erpkg` artifacts — the thermostat demo's
  container shrinks from 1.14 MB to 260 KB (~8x smaller bytecode). The dev loop (hot reload) keeps debug
  info so stack traces retain line numbers; pass `--debug` to `er-bridge-quickjs-compile` to keep it
  elsewhere.
- Optional hard cap on the JS heap (`ErRuntimeConfig.memory_limit`): a runaway app fails with a JS
  out-of-memory error caught by the error overlay instead of exhausting the shared system heap.
- The ESP32-S3 example now builds the lite profile: parser-less QuickJS, the precompiled error
  overlay, and a 4 MB JS heap cap in PSRAM.

### Fixed

- Demo bundles no longer break when a demo folder carries its own `node_modules`. The bundler could
  resolve a second copy of React from the demo's dependencies (installed for device-dev tooling),
  which made every component throw at mount; `react`, `react-reconciler`, and `scheduler` are now
  pinned to the package's own copies.

## [0.7.0] - 2026-07-04
### Added

- Support for page-flipped / multi-buffer displays. Some panels rotate two or more framebuffers and flip
  between them in hardware, so the buffer being drawn into is a frame or two stale — with the standard
  single-buffer assumption, moved elements ghosted and some changes never appeared. Tell the engine how many
  buffers rotate (`er_set_display_buffer_count`) and signal each hardware flip (`er_display_present`), and it
  repaints enough per buffer to keep every one correct, with no extra full-screen buffer or host-side
  copying. The default is unchanged for single-buffer displays.

## [0.6.0] - 2026-07-01
### Added

- On-device hot reload over USB. Save an edit, and it streams to a connected board and swaps in live — no
  reflashing, no rebooting, and component state is kept across reloads. It's a development feature, turned
  off by default; the web simulator stays the standard way to develop, and release builds are unaffected.
- Automatic device detection for on-device hot reload. You can start it without naming a port: ESP32 boards
  are found automatically, and any other board is given an explicit port. New projects created with the
  scaffolder include a script for it, and the tool clearly explains what to do when it can't connect.

## [0.5.3] - 2026-06-29
### Fixed

- SVG gradients render again. Linear, radial, and conic gradients on vector shapes had regressed to drawing
  as transparent in the simulators; they now paint correctly.
- The simulator dev server no longer crashes when a requested file is missing. It returns a normal
  "not found" response and stays running instead of shutting down.

## [0.5.2] - 2026-06-28
### Fixed

- The simulators now enable every engine feature at full capacity. They previously left some limits at the
  lean device defaults, so features could silently disappear (for example, gradients dropping out past a
  certain count). Real device builds still use the lean defaults.

## [0.5.1] - 2026-06-28
### Changed

- The JavaScript and TypeScript starter templates ship an updated app icon.

## [0.5.0] - 2026-06-28
### Added

- TypeScript support, end to end. You can write apps in TypeScript, scaffold a TypeScript starter, and use
  it across every workflow — the simulator (with state kept across reloads), the static export, and both
  device build paths.
- Bundled type declarations. The package ships its own types for the public API, so imports are typed out
  of the box with no extra type packages to install.

## [0.4.1] - 2026-06-27
### Changed

- Animated scaling and rotation do less work per frame, with no change to how they look.

### Fixed

- Animated 2D and 3D transforms no longer repaint the whole screen every frame; only the area that actually
  changed is redrawn.

## [0.4.0] - 2026-06-26
### Added

- Import an SVG file and render it as a live vector graphic. It works on both device paths and supports the
  common SVG shapes and nesting.
- Gradients for vector shapes — linear, radial, and conic — on both fill and stroke.
- Automatic fallback for SVG features the vector engine can't draw directly. Those files are turned into a
  baked image at build time instead of losing content, with a build warning naming what triggered it.
- Configurable memory limits for vectors, with clearer warnings when a graphic exceeds them.

### Changed

- Conic gradients render faster on the device, with no visible change to output.

## [0.3.0] - 2026-06-16
### Added

- A build command that produces the device artifact from your own project — either a bytecode bundle for
  PSRAM-class boards or ahead-of-time C for boards without PSRAM. It needs no native toolchain, and the
  scaffolder template gains a build script.

### Changed

- The build command compiles device bytecode without needing a native compiler.

## [0.2.3] - 2026-06-15
### Fixed

- Fixed a crash ("maximum call stack size exceeded") that hit newly scaffolded apps on the first run of the
  dev simulator. The state-preservation transform was being applied to the installed library as well as
  your app; it's now limited to your own source.

## [0.2.2] - 2026-06-15
### Fixed

- Hardened looping animations so an animation that finishes instantly can't recurse into its next iteration.

## [0.2.1] - 2026-06-15

Maintenance release — dependency, security, and release-pipeline fixes. No changes to the component API,
the engine, or runtime behavior.

### Security

- Updated the build-time bundler dependency to clear two security advisories. embedded-react doesn't expose
  the affected functionality, so real-world impact was low, but the dependency is now patched.

### Changed

- Pinned and adjusted development dependencies, so security audits report clean and installs are reproducible
  across platforms.

### Fixed

- Fixed the release workflow so automated npm publishing completes reliably.

## [0.2.0] - 2026-06-15

First beta.

### Added

- A browser-based simulator that runs your app with hot reload, so you can develop without any hardware.
  The prebuilt simulator ships in the package, so there's nothing extra to install.
- The embedded-react command-line tool: run your project in the simulator with hot reload, or export a
  self-contained static playground to share.
- The create-embedded-react scaffolder for starting a fresh project, published as a companion package.

### Changed

- Documentation restructured for the beta — the project now carries READMEs plus a single roadmap and a
  contributing guide.

### Fixed

- Fixed looping animations built from a sequence (the ping-pong pattern), which could overflow the stack on
  the second pass; sequences now reset correctly each time they start.

## [0.1.1] - 2026-06-14
### Fixed

- Apps that use Math.PI in the ahead-of-time build now compile on all toolchains.

### Added

- Continuous integration and one-step release automation.
- Distribution through PlatformIO and a finalized ESP-IDF component.
- License and notice files are included in every distributed package, and Install and Releasing sections in the
  README.

### Changed

- Rewrote the npm-facing README and fixed links that broke on the package page.

## [0.1.0] - 2026-06-14

Initial public release.

### Added

- The engine: a React Native-style renderer written in C, with layout, text, images, vector graphics, and
  animation.
- Flow A: run React apps directly on-device for instant iteration on PSRAM-class microcontrollers.
- Flow B: compile apps ahead-of-time to C for microcontrollers without PSRAM.
- Backends for desktop development and for ESP32 SPI displays.
- A desktop hot-reload simulator.
- Versioning foundation with a single source of truth propagated to every artifact.
- The first publish to npm as embedded-react.

[Unreleased]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.7.0...HEAD
[0.7.0]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.6.0...v0.7.0
[0.6.0]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.5.3...v0.6.0
[0.5.3]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.5.2...v0.5.3
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
