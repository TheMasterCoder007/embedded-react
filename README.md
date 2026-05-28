# embedded-react

**React Native for embedded MCUs.**
Write a React app, compile it, flash it onto a microcontroller.

`embedded-react` brings the React Native developer experience to bare-metal hardware. You
write the same JSX components, the same `Animated` API, and the same Yoga-flexbox styles
you would on iOS or Android — and your app runs on an STM32, ESP32, or RP2040 instead.
The runtime is pure C99, so it ports cleanly to any board with a writable framebuffer; the
toolchain hides the firmware build behind a `react-native`-style developer flow.

---

## Repo layout

This is a monorepo with four top-level concerns:

```
engine/      Pure C99 runtime — scene graph, layout, rendering, text, animation.
             Runtime-agnostic; doesn't know about React.

backends/    Hardware adapters. One folder per rendering API or peripheral
             (dma2d, sdl, opengl, esp32-lcd, software, framebuffer, web).

bridges/     Frontends that drive the engine. quickjs/ hosts a React reconciler
             — the supported developer path. Others (Lua, JSON UI, visual
             editor, AOT compiler) can be added later without touching the engine.

examples/    End-to-end sample apps — one engine + one backend + one bridge,
             packaged for a specific board (stm32h7, esp32, raspberry-pi,
             linux, dashboard-demo, marine-display).
```

Each top-level folder has its own README with more detail. Engine contributors should
start with [`engine/README.md`](engine/README.md). Backend authors:
[`backends/README.md`](backends/README.md). Anyone curious about the long-term
architecture: [`PLAN.md`](PLAN.md).

---

## Status

This is an in-progress project. Here's what is and isn't real today:

| Layer                                   | Status      | Notes                                                                                                                                                                                                                                                            |
|-----------------------------------------|-------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **C engine** (`engine/`)                | In progress | Scene graph, Yoga flexbox layout, text rendering, font system, rounded rectangle rasterizer (with AA), Pressable/touch interactions with zIndex-aware stacking, timing animations for existing color/opacity props, and host-side CTest (animation, input, layout, text, rendering/rrect) work today. Shadows, transforms, ScrollView gestures, TextInput focus/input, and image scaling remain scaffolded or incomplete. |
| **Backends** (`backends/`)              | In progress | `backends/sdl/` fully implemented (fill, copy, blend with premultiplied blend mode). Other six backends remain stubs.                                                                                                                                            |
| **QuickJS bridge** (`bridges/quickjs/`) | Stub        | Metro-compatible bundler + React reconciler integration is the next major milestone.                                                                                                                                                                             |
| **Examples** (`examples/`)              | Partial     | `examples/linux/` is implemented as a pure-C SDL demo that drives `er_scene.h` directly. Bridge-backed examples are not built yet; the remaining examples are READMEs only.                                                                                      |

If you're looking for a finished embedded UI framework today, this isn't it yet. If you
want to follow along — or contribute to the engine, the toolchain, or a backend — read
on.

---

## What it will look like

The eventual developer flow:

```
$ npx create-embedded-react my-app
$ cd my-app
$ vim src/App.jsx
$ npm run build -- --target stm32h7
$ npm run flash
```

```jsx
// src/App.jsx
import {View, Text, Animated, Pressable, useRef, useEffect} from 'embedded-react';

export default function App() {
    const opacity = useRef(new Animated.Value(0)).current;

    useEffect(() => {
        Animated.timing(opacity, {toValue: 1, duration: 400, useNativeDriver: true}).start();
    }, []);

    return (
        <Animated.View style={{opacity, flex: 1, padding: 20, backgroundColor: '#1a1a2e'}}>
            <Text style={{color: '#fff', fontSize: 24}}>Hello from an STM32.</Text>
            <Pressable onPress={() => console.log('tapped')}>
                <Text style={{color: '#e94560', marginTop: 12}}>Tap me</Text>
            </Pressable>
        </Animated.View>
    );
}
```

This is the **target experience**, not the current one. Today you would have to drive
the C engine directly via `er_scene.h` — see [`engine/include/er_scene.h`](engine/include/er_scene.h).

---

## How it works

The supported path is Flow A: React on QuickJS. Once shipped, it looks like this:

```
JSX source  →  bundler  →  QuickJS bytecode + JS  →  flashed to MCU
                                                          ↓
                                       React reconciler runs on QuickJS  (bridges/quickjs/)
                                                          ↓
                                       calls into er_scene.h             (engine/)
                                                          ↓
                                       Yoga layout + render pass         (engine/layout, engine/scene)
                                                          ↓
                                       backend fill / copy / blend       (backends/<api>/)
                                                          ↓
                                              framebuffer  →  display
```

The reconciler is a small piece of JS (ported from React Native's host config) that turns
React's diff output into `er_*` C calls. The engine owns the scene tree, runs the layout
pass, and dispatches paint calls through five backend function pointers (`fill_rect`,
`copy_rect`, `blend_rect`, optional `wait`, optional `frame_ready`). Your display driver
implements those five callbacks once; the rest of the stack is portable.

A later Flow B shortcuts the JS engine entirely: the same JSX source is consumed by an
AOT compiler that emits C code targeting `er_scene.h` directly. Smaller binary, smaller
RAM, no garbage collector — at the cost of giving up runtime JS features. The runtime ABI
is the same, so Flow A apps and Flow B apps can share components. See `PLAN.md` for the
full picture.

---

## Why a runtime-agnostic engine?

The engine in `engine/` deliberately doesn't know about React. `er_scene.h` is a pure C
ABI: anything that can call C functions can drive it. React-on-QuickJS is the supported
frontend today (Flow A), and React-AOT-to-C is the planned second frontend (Flow B). But
the layering leaves the door open for other frontends without forking the engine — Lua
UI, JSON UI loaders, a desktop visual editor that emits a scene-graph format, scripting
APIs.

That doesn't change the project's identity. **embedded-react is React Native for embedded
MCUs.** React is the supported developer path; other frontends are architectural
possibilities the layering enables, not roadmap items.

---

## Roadmap

**Engine (in progress)**

- Finish runtime: shadows, transforms, full animation engine, image scaling

**Flow A — React on QuickJS** (next)

- Thin `NativeUI` bridge surface around `er_scene.h`
- Metro-compatible bundler that emits a QuickJS bytecode payload + JS bundle
- React reconciler hosted in QuickJS, calling `er_scene.h`
- `examples/linux/` end-to-end — write JSX, run on desktop
- `examples/stm32h7/` — first MCU bring-up (LTDC + DMA2D)
- `examples/esp32/` — ESP32-S3 bring-up

**Flow B — React as a compile target** (later)

- JSX → C source AOT compiler
- Static layout / static animation passes
- Benchmarks: Flow A vs. Flow B on the same app (binary size, RAM, frame time)

---

## Hardware support

| Platform               | Backend                 | Status                                 |
|------------------------|-------------------------|----------------------------------------|
| Linux + SDL2           | `backends/sdl/`         | **Implemented** — dev / test target    |
| STM32H7 + DMA2D        | `backends/dma2d/`       | Reference (planned first MCU bring-up) |
| ESP32-S3 + LCD         | `backends/esp32-lcd/`   | Planned                                |
| RP2040 / bare-metal    | `backends/software/`    | Planned                                |
| Raspberry Pi / Android | `backends/opengl/`      | Planned                                |
| Embedded Linux SBC     | `backends/framebuffer/` | Planned                                |
| WebAssembly            | `backends/web/`         | Planned                                |

A new board needs a C99 compiler, `<math.h>`, and a writable framebuffer — RGB565,
ARGB8888, or anything the backend can convert to. No RTOS required; FreeRTOS, Zephyr,
and bare-metal all work.

---

## Building

```
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The engine builds a single static library `embedded-react`. Backends, bridges, and most
examples are not built by the top-level CMakeLists yet. `examples/linux/` is a standalone
CMake project that pulls in the engine and SDL backend directly.

---

## Architecture and contributing

The engine is intentionally small and self-contained — pure C99, no MCU SDK headers, no
platform `#ifdef`s. If you want to dig into the architecture (Yoga implementation,
scratch buffer model, premultiplied ARGB pipeline, compile-time feature flags) read
[`PLAN.md`](PLAN.md), especially its appendices.

Code formatting and documentation rules live in [`RULES.md`](RULES.md).

Contributions are welcome on any of the three layers: engine, backends, or bridges
(including the React/QuickJS frontend).
