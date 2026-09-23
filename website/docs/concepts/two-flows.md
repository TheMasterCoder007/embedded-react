---
title: 'Two flows: runtime and ahead of time'
sidebar_label: 'Two flows'
description: 'Flow A runs React on QuickJS on the chip. Flow B compiles the same JSX to C ahead of time. Same engine, same app; the difference is when the dynamism is resolved.'
---

The same JSX reaches the device through one of two flows. Both drive the **same C engine** and
the same `<View>`/`<Text>`/flexbox model. The only difference is _when_ the dynamism in your app
is resolved: on the device as it runs, or on your computer before it is flashed.

```jsx title="App.jsx"
export default function App() {
  const [count, setCount] = useState(0);
  return (
    <Pressable onPress={() => setCount(c => c + 1)}>
      <Text>count is {count}</Text>
    </Pressable>
  );
}
```

In Flow A, that `useState` is React's, running on a JavaScript engine on the chip. In Flow B, the
compiler turns it into a C variable, the handler into a C function, and the JSX into direct calls
that build the engine's node tree. Either way the button counts.

## Flow A: React at runtime

The React Native architecture, faithfully: a real JavaScript runtime ([QuickJS](https://bellard.org/quickjs/))
hosts a React reconciler that drives the engine as the app runs.

```text
JSX  →  esbuild bundle  →  QuickJS bytecode  →  app.erpkg  →  flashed to the device
                                                                   ↓
                                    React reconciler, on QuickJS   (bridges/quickjs/js)
                                                                   ↓
                                    NativeUI bridge → er_scene.h   (bridges/quickjs/*.c)
                                                                   ↓
                                    layout + render                (engine/)
                                                                   ↓
                                    backend fill / copy / blend    (backends/<display>/)
                                                                   ↓
                                    framebuffer  →  display
```

`npx embedded-react build` bundles your app, compiles it to QuickJS bytecode, and packs it with the
baked images and fonts into one container, `dist/app.erpkg`. The firmware brings up the display and
the JavaScript runtime, then loads the container: from a flash partition, an SD card, over the
air, or however you choose to get it there. The container carries a checksum and the QuickJS
version it was built for, so a mismatched build shows an error panel rather than running garbage.

**What you get**

- Everything JavaScript can express. State, effects, timers, promises, dynamic component trees.
- **UI updates without reflashing.** The firmware and the app are separate artifacts. To ship a new
  UI, write a new `app.erpkg`; the firmware stays as it is.
- **Hot reload on the device.** A firmware built with `-DER_HOTRELOAD=1` accepts a fresh container
  over USB on every save, with state preserved. See [Hot reload](../guides/hot-reload.md).
- The browser simulator _is_ Flow A, so what you see there is what runs on the chip.

**What it costs**

- **RAM for the JavaScript heap.** React, the reconciler and your app live in QuickJS's heap, which
  wants external RAM: PSRAM on an ESP32-S3, SDRAM on an STM32H7. The heap can go there while
  the engine's own buffers stay in internal RAM.
- **Per-frame dispatch.** A state change runs React's render and diff in JavaScript, then crosses the
  bridge into C. The bridge batches a whole frame into one engine commit, and animations run
  natively once started, but the JavaScript half is still the slowest part of a Flow A frame.
- **Flash.** The bytecode for React and the library is about 140 KB; a small app adds a few KB on
  top, plus its assets.

The runtime is deliberately small. It creates its JavaScript context with only what React needs:
base objects, `RegExp`, `JSON`, `Map`/`Set`, `Promise`, and `Date.now()`/`performance.now()` on
the engine's clock. There are no `Date` objects, no `Proxy`, no typed arrays unless the firmware
opts in. The same profile runs on the device, the desktop and the simulator, so nothing works in
one place and not another.

## Flow B: JSX compiled to C

The thing React Native cannot do. An ahead-of-time compiler consumes the same JSX and **emits C**
that calls the engine directly. There is no JavaScript engine on the device, no garbage collector,
no reconciler. The component tree, the `useState` state machine, event handlers and animations are
all resolved at compile time.

```text
JSX  →  AOT compiler (bridges/quickjs/js/aot)  →  app.gen.c + app.gen.h + assets.generated.c
                                                                   ↓
                                                    compiled into the firmware
                                                                   ↓
                                                    calls er_scene.h directly   (engine/)
```

`npx embedded-react build --aot --screen 240x320` writes the generated C. Your firmware compiles it
in and calls `er_app_build()` at boot. Everything the engine does at runtime in Flow A it still does
here: layout, rendering, touch, scrolling and animation are engine features, not JavaScript ones.

**What you get**

- **Fits chips with only internal RAM.** The watch face demo compiles to about 25 KB of C and runs
  on an RP2040 with 264 KB of SRAM; the thermostat runs on a classic ESP32 with no PSRAM.
- **Deterministic.** No garbage collector pauses, no interpreter. Integer arithmetic is defined at
  the edges (overflow saturates, division by zero has an answer), because C would otherwise leave
  those undefined.
- A smaller, simpler firmware: the engine and your app, nothing else.

**What it costs**

- **A subset of the API.** The compiler handles what it can resolve statically: components, props,
  `useState` and `setState` (including the updater form), `useEffect`, refs, `useCallback`/`useMemo`,
  conditionals, `.map` lists, dynamic styles, the `Animated` API, `PanResponder`, `<Svg>` and
  `<Dial>`. It rejects what it cannot lower to C, and says so at build time with the file and line.
  [The AOT subset](../guides/aot-subset.md) lists the rules.
- **A fixed panel size.** The size you pass to `--screen` is baked in, so a responsive app resolves
  to exactly one layout. Build once per board.
- **Every UI change is a rebuild and a reflash**, because the app is part of the firmware image.
- No hot reload and no on-device state preservation.

## Choosing

Your board usually decides.

|                   | Flow A                                          | Flow B                                    |
| ----------------- | ----------------------------------------------- | ----------------------------------------- |
| RAM               | External RAM for the JS heap (PSRAM or SDRAM)   | Internal RAM is enough                    |
| Flash for the app | ~140 KB of runtime bytecode + your app + assets | Your app as compiled C                    |
| Update the UI by  | Replacing `app.erpkg`; no firmware rebuild      | Rebuilding and reflashing the firmware    |
| API               | Everything                                      | [The AOT subset](../guides/aot-subset.md) |
| Development loop  | Simulator, then hot reload on the device        | Simulator, then rebuild and flash         |
| Runtime errors    | An on-screen redbox                             | Most are compile errors instead           |
| Runs on           | ESP32-S3 (PSRAM), STM32H7 (SDRAM), Linux        | ESP32 "CYD" (no PSRAM), RP2040, Linux     |

Two things make the choice cheaper than it looks:

- **It is a build flag, not a rewrite.** The demos in the repository build both ways from one
  source. If you keep to the subset, an app written for Flow A compiles for Flow B, and a Flow B
  app runs unchanged under Flow A.
- **The simulator runs Flow A**, and accepts everything. If you are heading for a board without
  external RAM, run `npx embedded-react build --aot` early and often so the compiler tells you
  about anything outside the subset while it is still easy to change.

## Why one engine

Both flows end at the same place: `er_scene.h`, the engine's C API for creating nodes, setting
props and committing a frame. The engine has no opinion about who calls it. That neutrality is what
makes Flow B possible at all, and it keeps the two flows honest with each other: a layout or
rendering fix lands once and both flows get it. [Engine and backends](./engine-and-backends.md)
explains the layers underneath.
