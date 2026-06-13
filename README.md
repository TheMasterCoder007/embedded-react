# embedded-react

**React Native for embedded MCUs.**
Write a React app, compile it, flash it onto a microcontroller — the UI runs *on the device*, with no browser, no phone, and no OS required.

```jsx
// src/App.jsx — the same component you'd write for iOS or Android…
import {View, Text, Animated, Pressable, useRef, useEffect} from 'embedded-react';

export default function App() {
    const opacity = useRef(new Animated.Value(0)).current;

    useEffect(() => {
        Animated.timing(opacity, {toValue: 1, duration: 400, useNativeDriver: true}).start();
    }, []);

    return (
        <Animated.View style={{opacity, flex: 1, padding: 20, backgroundColor: '#1a1a2e'}}>
            <Text style={{color: '#fff', fontSize: 24}}>Hello from an ESP32.</Text>
            <Pressable onPress={() => console.log('tapped')}>
                <Text style={{color: '#e94560', marginTop: 12}}>Tap me</Text>
            </Pressable>
        </Animated.View>
    );
}
```

…runs natively on a microcontroller driving a raw SPI or RGB display.

---

## What this is — and what it isn't

Most projects that pair "React" with an "ESP32" run **React in a web browser** on your phone or laptop, talking to the microcontroller over REST or BLE. The MCU is just a backend; the component tree, layout, and rendering all live somewhere else.

**embedded-react is the opposite.** It takes React Native's approach: React is a *component and reconciliation model*, not a DOM thing. React Native swapped the browser's host primitives (`div`, CSS, the browser layout engine) for native ones (`View`, Yoga, native draw calls). embedded-react does that swap again, one level deeper — the host primitives are a **pure C99 engine drawing straight into a framebuffer or SPI display**, with no operating system underneath and no JavaScript engine required.

You write the same JSX components, the same `Animated` API, and the same Yoga-flexbox styles you'd use on iOS or Android. Your app runs on an ESP32, STM32, or RP2040 instead of a phone.

---

## Two ways to ship the same app

The same JSX source can reach the device through one of two flows. Both target the **same C engine** and the same `<View>`/`<Text>`/flexbox model — the only difference is *when* the dynamism is resolved.

### Flow A — C engine + QuickJS  *(runtime)*

The faithful React Native architecture: a real JavaScript runtime ([QuickJS](https://bellard.org/quickjs/)) hosts a React reconciler that drives the native engine at runtime.

```
JSX  →  esbuild bundle  →  QuickJS bytecode  →  flashed to MCU
                                                     ↓
                              React reconciler runs on QuickJS   (bridges/quickjs/js)
                                                     ↓
                              NativeUI bridge → er_scene.h        (bridges/quickjs/*.c)
                                                     ↓
                              Yoga layout + render pass           (engine/)
                                                     ↓
                              backend fill / copy / blend         (backends/<api>/)
                                                     ↓
                                     framebuffer  →  display
```

You keep **full runtime dynamism** — live state, anything JS can express, and hot reload during development. The cost is RAM and per-frame dispatch, so Flow A wants a chip with PSRAM (e.g., ESP32-S3).

### Flow B — C engine + AOT compiler  *(compile-time)*

The thing React Native *can't* do. An ahead-of-time compiler consumes the same JSX and **emits C** targeting the engine directly — no JS engine, no garbage collector, no reconciler on the device. The component tree, `useState` state machine, event handlers, and animations are all baked into C at compile time.

```
JSX  →  AOT compiler (bridges/quickjs/js/aot)  →  app.gen.c / app.gen.h  →  compiled into firmware
                                                                                    ↓
                                                          calls er_scene.h directly  (engine/)
```

Smaller binary, far less RAM, deterministic — at the cost of giving up runtime JS. This is what makes **no-PSRAM microcontrollers** (the class of hardware the browser/RN approaches structurally exclude) viable. The AOT path is built and verified on hardware today.

**One model, one engine, two flows.** A developer writes the same RN-style JSX either way; the choice of runtime-vs-compiled is a build decision, not a rewrite.

---

## Status

In-progress project. Here's what is **verified working** vs. still scaffolded:

| Layer | Status | Notes |
|---|---|---|
| **C engine** (`engine/`) | Working | Scene graph, Yoga flexbox layout, UTF-8 text + font system, anti-aliased rounded rects, borders, shadows, 2D/3D transforms, opacity compositing, image scaling + tint, gradients, the `Animated` value engine, zIndex-aware multitouch hit-testing, ScrollView momentum, and banded RGB565 rendering for low-RAM boards. Host-side CTest suites pass (layout, text, rendering, animation, input, scroll, resources). |
| **Flow A** — React on QuickJS (`bridges/quickjs/`) | Working | End-to-end: `NativeUI` bridge, React reconciler, esbuild bundler, bytecode precompiler, build-time image/font bakers, ERPK asset container. Runs on the desktop host and on ESP32-S3 hardware. |
| **Flow B** — AOT JSX→C (`bridges/quickjs/js/aot/`) | Working | Compiles `useState`, `setState` (incl. updater form), events, conditionals, `.map` lists, child components, refs/`useCallback`/`useMemo`, dynamic styles, the full `Animated` API (timing/spring/decay/sequence/parallel/loop), and static + state-driven `Svg`. Runs on desktop **and on a no-PSRAM ESP32** (Cheap Yellow Display). |
| **Backends** (`backends/`) | Partial | `sdl` (desktop), `esp32-lcd` (RGB parallel), and `esp32-spi-lcd` (banded RGB565) are implemented and run on hardware. `dma2d`, `software`, `opengl`, `framebuffer`, and `web` are stubs. |
| **Simulator** (`tools/simulator/`) | Working | RN-style hot-reload dev loop: file-watch reload, redbox error overlay, asset re-bake, and transparent `useState` preservation across reloads. See [`SIMULATOR.md`](SIMULATOR.md). |
| **Examples** (`examples/`) | Partial | Four run end-to-end (below). `stm32h7`, `raspberry-pi`, and others are READMEs only. |

If you want a finished, drop-in embedded UI framework today, this isn't that yet. If you want to follow along — or contribute to the engine, a backend, or the toolchain — read on.

---

## Working examples

The same demo JSX (`demos/thermostat`, `demos/music-player`) runs across all four:

| Example | Flow | Hardware | Backend |
|---|---|---|---|
| [`examples/linux/`](examples/linux/README.md) | A (QuickJS) | desktop | `sdl` |
| [`examples/esp32/esp32-s3/`](examples/esp32/esp32-s3/README.md) | A (QuickJS) | ESP32-S3 + Waveshare 7" RGB panel | `esp32-lcd` |
| [`examples/linux-aot/`](examples/linux-aot) | B (AOT) | desktop | `sdl` |
| [`examples/esp32/esp32-2432s028r/`](examples/esp32/esp32-2432s028r/README.md) | B (AOT) | ESP32-2432S028R "Cheap Yellow Display", **no PSRAM** | `esp32-spi-lcd` |

---

## Repo layout

A monorepo with one self-contained CMake/npm project per concern:

```
engine/      Pure C99 runtime — scene graph, layout, rendering, text, animation.
             Runtime-agnostic; knows nothing about React. er_scene.h is a plain C ABI.

backends/    Hardware adapters. One folder per rendering API / peripheral. A backend
             implements five function pointers (fill_rect, copy_rect, blend_rect,
             optional wait, optional frame_ready) — the rest of the stack is portable.

bridges/     Frontends that drive the engine.
               quickjs/        Flow A: React reconciler + NativeUI C bridge over QuickJS.
               quickjs/js/aot/ Flow B: the JSX→C ahead-of-time compiler.
               quickjs/js/     The toolchain — bundler, asset bakers, simulator, scaffold.

demos/       JSX demo apps (thermostat, music-player) written against the public
             `embedded-react` API. Each compiles through both Flow A and Flow B.

examples/    End-to-end host integrations — one engine + one backend + one flow,
             packaged for a specific board (linux, linux-aot, esp32-s3, esp32-2432s028r).

tools/       Developer tooling, including the hot-reload simulator.
```

Each top-level folder has its own README. Engine contributors start with
[`engine/README.md`](engine/README.md); backend authors with
[`backends/README.md`](backends/README.md); the long-term architecture lives in
[`PLAN.md`](PLAN.md), the engine internals in [`ENGINE.md`](ENGINE.md), and the
Flow A bridge in [`BRIDGE.md`](BRIDGE.md).

---

## Why a runtime-agnostic engine?

`engine/` deliberately doesn't know about React. `er_scene.h` is a pure C ABI — anything that can call C functions can drive it. That layering is exactly what makes the two-flow design possible: Flow A drives the engine from a JS reconciler, Flow B drives it from generated C, and both share one renderer. It also leaves the door open to other frontends (Lua UI, JSON UI loaders, a visual editor that emits a scene-graph format) without forking the engine.

That doesn't change the project's identity. **embedded-react is React Native for embedded MCUs.** React is the developer-facing model; the engine's neutrality is an implementation choice that keeps the two flows honest.

---

## Toolchain

From `bridges/quickjs/js/` (pick a demo via the build scripts):

```
npm run build      # Flow A: bundle JSX → QuickJS bytecode + baked assets
npm run pack       # Flow A: pack an ERPK app container (bytecode + assets + CRC)
npm run aot        # Flow B: compile JSX → app.gen.c / app.gen.h
npm run sim        # hot-reload simulator (file-watch, redbox, state preservation)
npm run create     # scaffold a new app
npm test           # unit tests (vitest)
npm run parity     # verify Flow A / Flow B render parity
```

The polished `npx create-embedded-react` / `npm run flash` CLI wrapper is still aspirational — today you drive the flows through the scripts above and run an example target.

---

## Building the engine

The root `CMakeLists.txt` builds **nothing** — it only hosts repo-wide `clang-format`
targets. Each component is its own CMake project; configure whichever you're working on:

```
# Engine + host tests
cmake -S engine -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The engine compiles to a single static library, `embedded-react`. The bridge and the
examples pull the engine in themselves via a relative `add_subdirectory`. For a board
bring-up, configure the example directly (e.g. `examples/linux`, or an ESP-IDF build for
the ESP32 targets) — see that example's README.

A new board needs only a C99 compiler, `<math.h>`, and a writable framebuffer (RGB565,
ARGB8888, or anything the backend converts to). No RTOS required — FreeRTOS, Zephyr, and
bare-metal all work.

---

## Architecture and contributing

The engine is intentionally small and self-contained — pure C99, no MCU SDK headers, no
platform `#ifdef`s. For the internals (Yoga implementation, scratch-buffer model,
premultiplied-ARGB pipeline, banded rendering, compile-time feature flags) read
[`ENGINE.md`](ENGINE.md) and [`PLAN.md`](PLAN.md). Code formatting and documentation
rules live in [`RULES.md`](RULES.md).

Contributions are welcome on any layer: the engine, a backend, the Flow A bridge, or the
Flow B AOT compiler.

---

## License

Licensed under the [Apache License 2.0](LICENSE). Created and authored by **Cory Lamming** — see [`NOTICE`](NOTICE).
