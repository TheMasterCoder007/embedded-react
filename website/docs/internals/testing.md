---
title: 'Testing'
description: "The engine's CTest suites, the bridge's unit, runtime and bytecode tiers, the AOT smoke tests, the parity harness, and what CI runs."
---

Tests are tiered by what they need to run: pure C, pure JavaScript, the engine under a headless
JavaScript host, a C compiler, a packed npm package. Pick the tier by what the change touches.

## The engine: CTest

```bash
cmake -S engine -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The suites live in `engine/tests/`, one folder per area: `layout` (the flexbox solver, including a
Yoga-parity suite whose expected rectangles come from the real `yoga-layout` package), `text`,
`rendering` (arc, gradients, images, opacity, occlusion, `display: none`, modal damage, multi-buffer
replay, damage rectangles, content padding, transformed-animation damage), `animation` (curves,
interpolation, layout animation), `input` (hit-testing and the responder system), `scroll`,
`scene` and `resources`. They run against the host `software` path, so they test the engine, not a
board's backend.

Two habits the suites depend on:

- **Assert ink before asserting equivalence.** A stubbed `blend_rect` paints no anti-aliased
  pixels at all, and a "before equals after" assertion passes against two blank screens. Check that
  something was painted first.
- **The first commits after `er_reset` repaint everything.** Settle with two commits, and poison
  the framebuffer before checking that a change repainted only its own rectangle.

CI runs the suite six times with different compile-time flags (gradients on, two render workers,
occlusion culling off, 3D transforms, shadows with a decoupled transform scratch), because a flag
that is off by default is code that is otherwise never compiled. A new `ERUI_*` flag needs a pass.

## The bridge: C tests and smoke

```bash
cmake -S bridges/quickjs -B bridges/quickjs/build -DCMAKE_BUILD_TYPE=Release
cmake --build bridges/quickjs/build
ctest --test-dir bridges/quickjs/build --output-on-failure
./bridges/quickjs/build/er-bridge-quickjs-smoke
```

The bridge's own CTest covers heap accounting: that the allocator reports block sizes and the
collector runs, the bug that looks like a leak on bare metal. The smoke binary is a link check. CI
builds the bridge twice, with the native allocator and with the bare-metal size-prefix one, and
runs the JavaScript runtime tiers against both.

## The JavaScript package: three tiers

From `bridges/quickjs/js`:

| Command                 | What it runs                                                                                                                                                                                                                                           | Needs                                  |
| ----------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | -------------------------------------- |
| `npm test`              | Vitest over `src/**/__tests__/*.unit.test.js`: pure JavaScript, no engine. The style splitter, prop helpers, the AOT compiler's unit and smoke tests, and a parity test that keeps the type declarations, the bridge's prop tables and the AOT in step | Node                                   |
| `npm run test:runtime`  | `test/runtime/*.runtime.test.jsx`, each bundled and run inside QuickJS plus the real engine in a headless harness (no window), with a `check()`/`report()` API the C runner reads                                                                      | The `er-bridge-quickjs-runtest` binary |
| `npm run test:bytecode` | The same suite, each bundle precompiled to `.qbc` bytecode and loaded through `JS_ReadObject`: the path a device takes                                                                                                                                 | Also `er-bridge-quickjs-compile`       |

Build the harness once, without SDL:

```bash
cmake --build bridges/quickjs/build --target er-bridge-quickjs-runtest er-bridge-quickjs-compile
```

Set `ER_BRIDGE_BUILD_DIR` to use a bridge build elsewhere. Choose the tier by what the code touches:
marshalling and pure logic go in a co-located unit test; anything that exercises the reconciler
through to the engine goes in a runtime test.

Runtime tests have their own traps. `NativeUI.tick()` advances time but never paints; use
`NativeUI.commit()` before reading pixels. The root has no background, so put a backdrop under what
you measure. And esbuild drops a call whose result is unused, so assert that something throws
through a call whose value is consumed, or the call vanishes from the bundle. The unit tier must
not call `compileToBytecode`: the simulator wasm is not built in CI's unit job.

## The AOT compiler

`aot/__tests__/` runs under `npm test`: `compile` covers lowering case by case, `date-now` the
64-bit time rules, `text-lowering` the `snprintf` diffing, `typescript` the TS entry path,
`demos.smoke` compiles the thermostat and the watch face at their board sizes and the two starter
templates at 240×320, and `cc-compile.smoke` hands the generated C to the system compiler with
`-fsyntax-only -Wall`, which catches the class of error a C compiler rejects but the generator does
not, such as a pointer-versus-int ternary from a mixed text expression. A separate CI job builds
the watch face and the thermostat's compact layout all the way through `gcc`.

The starters are a guardrail: a newcomer's first build must compile ahead of time and use only baked
font sizes, so the smoke test asserts both. Changing the template means keeping those green.

## Parity

`npm run parity` renders the same demo through both flows and asserts the framebuffers match
pixel for pixel. Flow A packs the demo into a container and renders it headlessly through the
desktop host; Flow B compiles it to C, rebuilds the AOT desktop host and renders that. Optional taps
drive both through the same interaction, so dynamic state is compared too. Because the engine, fonts
and backend are shared, a correct demo renders byte-identically, and any difference is a real Flow A
to Flow B divergence; the harness is how an animated-transform binding bug was once caught. Each
responsive scenario feeds one screen size to both paths.

Known residue: about a 1% sub-pixel divergence in dial and text rendering sits under the harness's
tolerance, and Flow A gets no baked assets unless it loads from a container, which is why the
harness packs one.

## Consumer smoke

`node tools/consumer-smoke.mjs` packs the npm package, installs it into a throwaway project and
runs the consumer commands: the AOT build, the TypeScript template's typecheck, and (when the
prebuilt wasm is present) the Flow A container build. It guards the bugs the repository's own tests
cannot see because they only appear once the package is packed and installed elsewhere: a file
missing from the `files` whitelist, ESM-versus-CJS resolution, the bytecode compile through the
prebuilt wasm. Every consumer regression the project has shipped would have been caught here, which
is why CI runs it.

## On hardware

Nothing above runs on a board. For a change that touches a backend, the frame loop, memory sizing
or anything timing-related, build and flash the relevant example and say so in the pull request:
what board, what you saw. The perf overlay (`-DER_PERF_OVERLAY=1`) gives numbers to quote, and the
ESP32 examples log free RAM at boot and after start-up. Host-side benchmarks have misled this
project more than once (a pixel-move scroll that measured 12 to 28× faster on a laptop gained 15%
on the board), so a claim about speed is a claim about a board.

## What CI runs

Every push and pull request runs five jobs, all required to merge: **JS tests + version drift**
(`npm test` plus `sync-version --check`), **Engine build + ctests** (the six flag passes), **QuickJS
bridge build + heap-accounting test** (two allocator builds, each with the runtime and bytecode
tiers), **AOT compile smoke** (generated C through `gcc`), and **Consumer smoke**. The docs site has
its own workflow that builds on a pull request and deploys on `master`.
