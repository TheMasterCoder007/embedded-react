---
title: 'Hot reload'
description: 'The three edit-and-see loops: the browser simulator, the SDL simulator, and a board over USB. What survives a reload and why.'
---

Save a file, see the change. Embedded React has three Flow A loops that do this, at three distances
from the hardware. All three re-run your app rather than patching it, and all three keep your
component state across the reload, so a counter you had pressed five times still reads five.

| Loop              | Runs on                                      | Command                           | Needs                                    |
| ----------------- | -------------------------------------------- | --------------------------------- | ---------------------------------------- |
| Browser simulator | The engine as WebAssembly, in a tab          | `npx embedded-react dev`          | Node.js                                  |
| SDL simulator     | The engine and QuickJS natively, in a window | `npm run sim` (in the repository) | CMake, SDL2, a C compiler                |
| On the device     | Your board, over its native USB port         | `npx embedded-react dev --device` | A firmware built with `-DER_HOTRELOAD=1` |

Flow B has its own loop on the device, which [compiles a package and sends it](#on-the-device-with-flow-b). It starts
the app over on each save, and the simulator is still where most of the UI work happens.

## In the browser

`npx embedded-react dev` bundles your app with esbuild in watch mode and serves a page that runs the
engine as WebAssembly. On every save it rebundles, re-bakes any changed image or font, and pushes a
reload to the page, which hands the new bundle to the engine in place. No wasm rebuild, no page
reload, and it typically lands well under a second.

This is the loop the [Getting started](../getting-started/simulator.md) pages use, and the one most
development happens in. It renders at any panel size, so a layout can be checked against every
target board without leaving the tab.

## On the desktop

Inside the repository, `npm run sim -- thermostat` from `packages/embedded-react` runs esbuild in watch
mode and launches the SDL simulator, which reloads the window on change. It reads a demo's source
directly against the in-repo library, with no install step. Build the simulator once first, from
the repository root:

```bash
cmake -S tools/simulator -B tools/simulator/build && cmake --build tools/simulator/build
```

The SDL simulator is the maintainers' tool: it runs the real C engine natively, so it works under
gdb and lldb, and it shows JavaScript errors on the error screen. Press **R** for a clean reset that
also forgets persisted state.

## On the device

The ESP32-S3 example accepts a fresh app over the board's native USB port while the old one keeps
running, then swaps to the new one with no reboot and no blank "reloading" moment. It is opt-in:

```bash
idf.py -DER_HOTRELOAD=1 build flash     # the receiver is compiled out of a default build
```

The board must also already hold a container from `embedded-react build`, written to the config
partition as the [ESP32-S3 guide](./boards/esp32-s3.md) describes: the dev loop sends only your app,
and runs it against the vendor section already on the board.

Then, with both of the board's USB ports plugged in (the UART port for flashing, the native USB
port for reload):

```bash
npm i serialport                 # once: the optional native dependency for device upload
npx embedded-react dev --device  # or npm run dev:device in a scaffolded project
```

`--device` with no port finds an ESP32-S3 or C3 by its fixed USB id; other boards take an explicit
port (`--device /dev/cu.usbmodemXXXX`, or `COM5` on Windows). If no board is found, or the firmware
was built without the receiver, the dev loop says which rather than hanging. The device's own logs
come back over the same port, prefixed `▸`.

Only your changed app code crosses the wire. The boot container is split into a **vendor** section
(React, the reconciler and the library, about 140 KB of stripped bytecode, run first) and a small **app** section
(your bytecode and assets, run last). The vendor half never changes between saves, so the dev loop
ships just the app slice, and a reload stays quick however large the app grows.

On the board, a background task reads the frame into a staging buffer while the old app runs. Once
a complete, checksum-valid frame is in, the frame loop applies it: because it carries no vendor
section, the runtime takes the **soft path**, skipping the full context teardown and re-evaluating
only the app into the resident context, where `AppRegistry` re-renders into its existing root and
React reconciles the old tree into the new one in place. A corrupt upload is rejected and never
disturbs the running app.

Release firmware should be built without `ER_HOTRELOAD`; ship by packing once and writing the
config partition, as the [ESP32-S3 guide](./boards/esp32-s3.md) describes.

## On the device with Flow B

The ESP32-S3's Flow B build, the CYD and the RP2040 take a new package over their serial port on every save, with no
reflash and no reboot. It is opt-in in the same way, so build the firmware with `ER_HOTRELOAD`:

| Board  | Build with                               | Port                  |
| ------ | ---------------------------------------- | --------------------- |
| S3     | `idf.py -DER_HOTRELOAD=1` (Flow B build) | Native USB            |
| CYD    | `idf.py -DER_HOTRELOAD=1`                | USB-C, the UART port  |
| RP2040 | `cmake -DER_HOTRELOAD=1`                 | USB-C, the USB serial |

Then, from your project:

```bash
npm i serialport                       # once: the optional native dependency for device upload
npx embedded-react dev --device PORT --aot
```

A board with no package yet is fine: the first save installs one. The CYD's UART moves to 460800 baud once it has
booted, so its logs read at that rate.

Each save compiles a debug package, so a fault names its source line, and sends it to the staging slot a piece at a
time. The running app stays on screen and keeps running while it arrives. Then the board checks the whole package:

- **It passes:** the board tears the old app down, resets the arena, and starts the new one. That slot then becomes the
  active one, so a reset keeps running it, and the old package stays as the fallback.
- **It fails the check:** the app keeps running with its state, under the error screen with the reason. Tap to go back
  to it. The reason also comes back to your terminal. The slots stay as they were.
- **It faults while starting:** the old app is already gone, so the error screen stays up until the next save. A reset
  runs the last package that started.

A build error stays on your computer, and the board isn't touched.

Component state starts over on each reload, since a package's state doesn't carry across yet. Writing flash pauses the
whole chip for a moment, so the app stutters while a package arrives. Release firmware should be built without
`ER_HOTRELOAD`, and ship with [`embedded-react flash`](../internals/flow-b-packages.md).

## What survives

The rest of this page is about Flow A, whose state survives a reload.

State survives because of a build-time transform, not runtime magic. In every Flow A hot-reload loop, the
bundler rewrites each `useState(init)` in _your_ files (never the library's) to
`usePersistentState("file::Component#n", init)`, keyed by the component's name and the hook's
order within it. The store behind that hook lives on the C side of the runtime, outside the
JavaScript context, so it outlives the reload. The device loop applies the transform too, and an
`ER_HOTRELOAD` firmware installs the store. A release build has neither, and `useState` is exactly
`useState`.

That keying has consequences worth knowing:

- Editing JSX, styles or handler logic keeps state. That is the common case.
- Adding, removing, or reordering the `useState` calls in a component shifts its keys and resets that
  component's state. Renaming the component does the same.
- Values must be JSON-serializable. A value that is not (a function, say) is kept in memory for
  the session but not persisted across the next reload.
- `usePersistentState` is exported, so you can use it directly with your own key when you want
  state that is explicitly meant to survive.

The [playground](/playground) on this site runs the same transform in the browser, which is why
its counter survives an edit there too.
