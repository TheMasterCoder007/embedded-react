<h1 align="center">
  <img src="assets/icons/embedded-react-readme-header-v3.png" alt="Embedded React" width="100%">
</h1>

**React Native for embedded MCUs.**
Write a React app, compile it, flash it onto a microcontroller. The UI runs *on the device*, with no browser, no phone, and no OS required.

```jsx
// App.jsx — the same component you'd write for iOS or Android…
import {useState, useEffect} from 'react';
import {Text, Animated, Pressable, useAnimatedValue} from 'embedded-react';

export function App() {
    const opacity = useAnimatedValue(0);
    const [taps, setTaps] = useState(0);

    useEffect(() => {
        Animated.timing(opacity, {toValue: 1, duration: 400, useNativeDriver: true}).start();
    }, []);

    return (
        <Animated.View style={{opacity, flex: 1, padding: 20, backgroundColor: '#1a1a2e'}}>
            <Text style={{color: '#fff', fontSize: 24}}>Hello from an ESP32.</Text>
            <Pressable onPress={() => setTaps(taps + 1)}>
                <Text style={{color: '#e94560', marginTop: 12}}>Tapped {taps} times</Text>
            </Pressable>
        </Animated.View>
    );
}
```

…runs natively on a microcontroller driving a raw SPI or RGB display.

**Documentation: [embedded-react.dev](https://embedded-react.dev)** — this README is the short version.

---

## What this is

Most projects that pair "React" with an "ESP32" run React in a web browser on your phone, talking to
the microcontroller over REST or BLE. Embedded React is the opposite. It takes React Native's
approach one level deeper: the host primitives are a **pure C99 engine drawing straight into a
framebuffer or SPI display**, with no operating system underneath. You write the same JSX, the same
`Animated` API, and the same flexbox styles you would use on iOS or Android, and the app runs on an
ESP32, STM32, or RP2040 instead of a phone.

The same app reaches the device through one of two flows, both driving the same engine:

| | Flow A — runtime | Flow B — ahead of time |
|---|---|---|
| How it runs | A real React reconciler on [QuickJS](https://bellard.org/quickjs/), on the chip | JSX compiled to C and linked into the firmware |
| Needs | External RAM for the JS heap: PSRAM on an ESP32-S3, SDRAM on an STM32H7 | Internal RAM only; no JS engine on the device |
| Update the UI by | Replacing `app.erpkg`, no firmware rebuild | Rebuilding and reflashing |
| Trade-off | RAM and per-frame dispatch | A [subset of the API](https://embedded-react.dev/guides/aot-subset) |

Choosing is a build flag, not a rewrite. [The two flows](https://embedded-react.dev/concepts/two-flows) goes deeper.

## Quick start

```
npm create embedded-react@latest my-app          # add -- --ts for TypeScript
cd my-app && npm install && npm run dev          # the browser simulator, with hot reload
```

Or try it with nothing installed in the [playground](https://embedded-react.dev/playground).
[Getting started](https://embedded-react.dev/getting-started) takes it from there to a board.

## Status

Beta. The engine, both flows, the hardware backends and the simulators are built and verified; from
here the work is fixes and features, tracked in [`ROADMAP.md`](ROADMAP.md).

| Board | Flow | Status | Guide |
|---|---|---|---|
| Waveshare ESP32-S3-Touch-LCD-7 (800×480 RGB) | A | Verified on hardware | [ESP32-S3](https://embedded-react.dev/guides/boards/esp32-s3) |
| ESP32-2432S028R "Cheap Yellow Display" (no PSRAM, SPI) | B | Verified on hardware | [ESP32 CYD](https://embedded-react.dev/guides/boards/esp32-cyd) |
| Waveshare RP2040-Touch-LCD-1.69 (240×280 SPI) | B | Verified on hardware | [RP2040](https://embedded-react.dev/guides/boards/rp2040) |
| Linux / macOS / Windows desktop (SDL) | A and B | Working | [Linux](https://embedded-react.dev/guides/boards/linux) |
| Browser (WebAssembly simulator) | A | Working | [Simulator](https://embedded-react.dev/getting-started/simulator) |
| STM32H7 with SDRAM (Chrom-ART backend) | A | Running on hardware; public example planned | [STM32H7](https://embedded-react.dev/guides/boards/stm32h7) |

## Documentation

- [Introduction](https://embedded-react.dev/intro) — what it is and isn't
- [Getting started](https://embedded-react.dev/getting-started) — install, the simulator, your first board
- [Concepts](https://embedded-react.dev/concepts) — the two flows, the engine and backends, the rendering pipeline, layout, assets
- [Guides](https://embedded-react.dev/guides) — per-board setup, hot reload, the AOT subset, performance, memory, the demo apps
- [API reference](https://embedded-react.dev/api) — components, hooks, styles, `Animated`, the NativeUI bridge, the C engine
- [Internals](https://embedded-react.dev/internals) — architecture, engine internals, writing a backend, testing, releasing, contributing

## Install

Everything ships at one lockstep version (the same `vX.Y.Z` on every channel).

**npm** — the component API and reconciler (Flow A), the Flow B compiler, and the simulator CLI:

```
npm install embedded-react react@18.3.1     # embedded-react pins React to 18.3.1
```

**CMake / FetchContent** — the C engine as a source (you add a backend and your app):

```cmake
include(FetchContent)
FetchContent_Declare(embedded-react
  GIT_REPOSITORY https://github.com/TheMasterCoder007/embedded-react.git
  GIT_TAG        v0.15.0
  SOURCE_SUBDIR  engine)
FetchContent_MakeAvailable(embedded-react)
target_link_libraries(my_firmware PRIVATE embedded-react)
```

**ESP-IDF** — Flow B needs only the engine, which is on the component registry; Flow A uses
`FetchContent` (it also needs QuickJS, which is not an IDF component):

```
idf.py add-dependency "TheMasterCoder007/embedded-react^0.15.0"
```

**PlatformIO** — the engine (Flow B) as a library:

```ini
lib_deps = https://github.com/TheMasterCoder007/embedded-react.git#v0.15.0
```

[Installation](https://embedded-react.dev/getting-started/installation) has the details for each.

## Repository layout

```
engine/                 Pure C99 runtime: scene graph, layout, rendering, text, animation.
backends/               Hardware adapters, one folder per rendering API or peripheral.
bridges/quickjs/        Flow A: the NativeUI C bridge and er_runtime host core over QuickJS.
bridges/quickjs/js/     The npm package `embedded-react`: components, reconciler, bundler,
                        asset bakers, the dev/export/build CLI, and the Flow B compiler (aot/).
create-embedded-react/  The `npm create embedded-react` scaffolder and its templates.
demos/                  The thermostat and watch-face demo apps; each builds through both flows.
examples/               Board firmware projects: one engine + one backend + one flow per board.
tools/                  The SDL simulator, the WebAssembly simulator build, the release scripts.
website/                The documentation site.
```

Each folder's README says how to build what is in it and points at the relevant docs page.
[Architecture](https://embedded-react.dev/internals/architecture) explains how the layers fit.

## Contributing and releasing

Contributions are welcome on any layer. The rules are in
[Contributing](https://embedded-react.dev/internals/contributing) (summarised in
[`CONTRIBUTING.md`](CONTRIBUTING.md)), the release process in
[Releasing](https://embedded-react.dev/internals/releasing), and what is planned or known-broken in
[`ROADMAP.md`](ROADMAP.md).

## License

Licensed under the [Apache License 2.0](LICENSE). Created and authored by **Cory Lamming** — see [`NOTICE`](NOTICE).
