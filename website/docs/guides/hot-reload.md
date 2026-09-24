---
title: 'Hot reload'
description: 'The three edit-and-see loops: the browser simulator, the SDL simulator, and a board over USB. What survives a reload and why.'
---

Save a file, see the change. Embedded React has three loops that do this, at three distances from
the hardware. All three re-run your app rather than patching it, and all three keep your component
state across the reload, so a counter you had pressed five times still reads five.

| Loop              | Runs on                                      | Command                           | Needs                                    |
| ----------------- | -------------------------------------------- | --------------------------------- | ---------------------------------------- |
| Browser simulator | The engine as WebAssembly, in a tab          | `npx embedded-react dev`          | Node.js                                  |
| SDL simulator     | The engine and QuickJS natively, in a window | `npm run sim` (in the repository) | CMake, SDL2, a C compiler                |
| On the device     | Your board, over its native USB port         | `npx embedded-react dev --device` | A firmware built with `-DER_HOTRELOAD=1` |

All three are Flow A. Flow B has no hot reload, because the app is part of the firmware image; the
loop there is rebuild and reflash, and the simulator is still where the UI work happens.

## In the browser

`npx embedded-react dev` bundles your app with esbuild in watch mode and serves a page that runs the
engine as WebAssembly. On every save it rebundles, re-bakes any changed image or font, and pushes a
reload to the page, which hands the new bundle to the engine in place. No wasm rebuild, no page
reload, and it typically lands well under a second.

This is the loop the [Getting started](../getting-started/simulator.md) pages use, and the one most
development happens in. It renders at any panel size, so a layout can be checked against every
target board without leaving the tab.

## On the desktop

Inside the repository, `npm run sim -- thermostat` from `bridges/quickjs/js` runs esbuild in watch
mode and launches the SDL simulator, which reloads the window on change. It reads a demo's source
directly against the in-repo library, with no install step.

The SDL simulator is the maintainers' tool: it runs the real C engine natively, so it works under
gdb and lldb, and it has a redbox overlay for JavaScript errors. Press **R** for a clean reset that
also forgets persisted state.

## On the device

The ESP32-S3 example accepts a fresh app over the board's native USB port while the old one keeps
running, then swaps to the new one with no reboot and no blank "reloading" moment. It is opt-in:

```bash
idf.py -DER_HOTRELOAD=1 build flash     # the receiver is compiled out of a default build
```

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
(React, the reconciler and the library, about 1 MB of bytecode, run first) and a small **app** section
(your bytecode and assets, run last). The vendor half never changes between saves, so the dev loop
ships just the app slice, and a reload stays quick however large the app grows.

On the board, a background task reads the frame into a staging buffer while the old app runs. Once
a complete, checksum-valid frame is in, the frame loop applies it: because it carries no vendor
section, the runtime takes the **soft path**, skipping the full context teardown and re-evaluating
only the app into the resident context, where `AppRegistry` re-renders into its existing root and
React reconciles the old tree into the new one in place. A corrupt upload is rejected and never
disturbs the running app.

A release firmware should be built without `ER_HOTRELOAD`; ship by packing once and writing the
config partition, as the [ESP32-S3 guide](./boards/esp32-s3.md) describes.

## What survives

State survives because of a build-time transform, not runtime magic. In every hot-reload loop, the
bundler rewrites each `useState(init)` in _your_ files (never the library's) to
`usePersistentState("file::Component#n", init)`, keyed by the component's name and the hook's
order within it. The store behind that hook lives on the C side of the runtime, outside the
JavaScript context, so it outlives the reload. On a device, or in a release build, there is no
transform and no store, and `useState` is exactly `useState`.

That keying has consequences worth knowing:

- Editing JSX, styles or handler logic keeps state. That is the common case.
- Adding, removing or reordering the `useState` calls in a component shifts its keys and resets that
  component's state. Renaming the component does the same.
- Values must be JSON-serialisable. A value that is not (a function, say) is kept in memory for
  the session but not persisted across the next reload.
- `usePersistentState` is exported, so you can use it directly with your own key when you want
  state that is explicitly meant to survive.

The [playground](/playground) on this site runs the same transform in the browser, which is why
its counter survives an edit there too.
