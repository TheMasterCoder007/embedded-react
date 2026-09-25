---
title: 'Architecture'
description: "The monorepo's layers, and the C ABI boundary that lets one engine serve two flows."
---

The repository is a monorepo with one self-contained CMake or npm project per concern. The root
`CMakeLists.txt` builds nothing; it only hosts the repo-wide formatting targets.

```text
engine/                 Pure C99 runtime: scene graph, layout, rendering, text, animation.
                        Knows nothing about React. er_scene.h is a plain C ABI.

backends/               Hardware adapters, one folder per rendering API or peripheral. A backend
                        implements five function pointers; the rest of the stack is portable.

bridges/                Frontends that drive the engine.
  quickjs/              Flow A: the NativeUI C bridge and er_runtime host core over QuickJS.
  quickjs/js/           The npm package `embedded-react`: components, reconciler, bundler,
                        asset bakers, and the dev/export/build CLI.
  quickjs/js/aot/       Flow B: the JSX-to-C ahead-of-time compiler.

create-embedded-react/  The `npm create embedded-react` scaffolder and its starter templates.

demos/                  JSX demo apps (thermostat, watch-face) written against the public API.
                        Each compiles through both flows.

examples/               End-to-end host integrations: one engine + one backend + one flow,
                        packaged for a board (linux, linux-aot, esp32-s3, esp32-2432s028r,
                        rp2040-touch-lcd-1.69).

tools/                  Developer tooling: the SDL simulator, the WebAssembly simulator build and
                        dev server, the release and version-sync scripts, the consumer smoke test.

website/                This site.
```

## The boundary

One line runs through the whole design: **the engine does not know who is calling it.**
`er_scene.h` is a C API for creating nodes, setting props, arranging a tree and committing a frame.
The engine never imports React, never sees JSX, never includes a platform header. That neutrality
is not a stylistic preference; it is what makes the two flows possible.

```text
        React on QuickJS                    JSX compiled to C
        (bridges/quickjs)                   (bridges/quickjs/js/aot)
               │                                    │
               └──────────────┬─────────────────────┘
                              ▼
                         er_scene.h
                              │
                           engine/
                              │
                     EmbeddedRenderBackend
                              │
                    backends/<display>/
```

Flow A drives the engine from a JavaScript reconciler through the bridge; Flow B drives it from
generated C; both share one renderer, so a layout or rendering fix lands once. The same boundary
leaves room for frontends that do not exist yet (a Lua UI, a JSON tree loader, a visual editor's
output) without forking the engine. None of those is on the roadmap; the door is simply kept open.

That does not change what the project is. Embedded React is React Native for embedded MCUs; the
engine's neutrality is an implementation choice that keeps the two flows honest with each other.

## The layers, top down

**The npm package** (`bridges/quickjs/js/src/`) is what an app imports. `embedded-react/` holds the
public surface: the component tags, `StyleSheet`, `Animated`, `PanResponder`, `AppRegistry`, the
hooks. Beside it, `host-config.js` is the `react-reconciler` host config that turns React's
`createInstance`/`appendChild`/`commitUpdate` into `NativeUI.*` calls, `renderer.js` creates the
root, and `props.js` flattens styles into the prop bag the bridge reads. The `assets/` folder holds
the build-time bakers (images to premultiplied ARGB, fonts to bitmap glyphs, SVG to an op-tape) and
the container writer. `cli.mjs` and `sim-server.mjs` are the `dev`/`export`/`build` commands.

**The QuickJS bridge** (`bridges/quickjs/`) is C. `native_ui_bridge.c` publishes the `NativeUI`
object into a QuickJS context and forwards each method to the engine, interning prop names and
hashing prop bags so an unchanged `setProps` is cheap. `er_runtime.c` is the portable host core:
runtime and context lifecycle, the lite JavaScript profile, the host globals (`console`, `screen`,
timers, the persist store), container loading with CRC and version checks, the frame pump, the error
overlay. `er_js_alloc.c` is the allocator that reports real block sizes so QuickJS's collector
works on bare metal. `er_hotreload.c` parses the USB reload frames. `er_assets.c` registers an ERPK
pack. [NativeUI bridge](../api/native-ui-bridge.md) documents the surface.

**The AOT compiler** (`bridges/quickjs/js/aot/compile.mjs`, with `style-map.mjs`) parses the
app's JSX with Babel, folds module-level constants (including the `screen` size), and emits
`app.gen.c`: node construction, a `useState` state machine, handlers as C functions, animations as
engine calls, `PanResponder` configs as responder registrations. It rejects anything it cannot lower,
by message, at build time. `screenshot-smoke.mjs` renders the result for the parity harness.

**The engine** (`engine/`) is organised by what it does: `scene/` (node pool, tree, props, dirty
tracking, render orchestration, hit-testing), `layout/` (the Yoga-compatible solver), `rendering/`
(rounded rectangles, shadows, transforms, images, vectors, the arc widget), `text/` (UTF-8, glyphs,
line layout), `animation/` (values, curves, the native driver), `resources/` (fonts, images), and
`core/` (backend glue, the clock, the frame tick). `include/er_scene.h` and `native_renderer.h` are
the only headers downstream code includes. [Engine and backends](../concepts/engine-and-backends.md)
and the [C engine](../api/c-engine.md) reference cover it from the outside; `engine/README.md`
covers the internals.

**The backends** (`backends/`) each implement `EmbeddedRenderBackend` for one API: `esp32-lcd`
(RGB parallel, PSRAM framebuffer), `esp32-spi-lcd` (banded RGB565), `pico-spi-lcd` (RP2040, one
RGB565 framebuffer), `dma2d` (STM32 Chrom-ART), `sdl` (desktop), `software` and `web` (the browser
simulator). [Writing a backend](./writing-a-backend.md) is the guide.

**The examples** (`examples/`) are the reference hosts, one per board and flow, and the place a
new board starts: copy the closest one and change `board.c`.

## Where a frame's work happens

For a Flow A frame: the host polls touch and calls `er_runtime_pump()`. The pump delivers touches
and due timers to JavaScript inside one batch, React renders and diffs, the host config marshals
changes through `NativeUI.setProps` and friends, and the batch closes with one `er_commit()`, which
lays out what moved, computes damage, and paints each damaged rectangle through the backend. The
host presents the dirty region and advances the clock with `embedded_renderer_tick()`, which also
steps every native animation. [Rendering pipeline](../concepts/rendering-pipeline.md) follows the
commit in detail; the [Performance](../guides/performance.md) guide shows how to measure each phase.

For a Flow B frame, the JavaScript half does not exist: an event handler is a C function that writes
state and props, and the host's `er_commit()` does the rest.
