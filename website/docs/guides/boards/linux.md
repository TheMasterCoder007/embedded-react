---
title: 'Linux and desktop'
sidebar_label: 'Linux'
description: 'The desktop host: the same engine and bridge as the boards, in an SDL2 window, for either flow.'
---

The desktop host is the analogue of a phone emulator, and the on-desktop peer of the boards: the
same QuickJS bridge and C engine, with SDL2 as the backend. It is where the C side of a project can be
learned before hardware arrives, and where the engine's own tests run. It builds on Linux, macOS and
Windows; the folder is called `linux` for its origins.

## Flow A: `examples/linux`

The desktop "firmware" mirrors the ESP32-S3's model exactly. The executable ships no app; at start it
loads a config container, `app.erpkg`, from a slot next to itself, the way the board loads its
config partition. The same `.erpkg` runs here and on the device.

You need CMake 3.16+, a C compiler and SDL2 2.0.6 or newer:

```bash
sudo apt install libsdl2-dev   # Debian and Ubuntu
brew install sdl2              # macOS
```

On Windows, install SDL2 through vcpkg and pass its toolchain file to CMake; the build copies
`SDL2.dll` next to the executable.

Then, from the repository root:

```bash
# 1. Pack a demo into a config container
cd bridges/quickjs/js && npm install && npm run pack     # → dist/app.erpkg
cd ../../..

# 2. Build and run the host
cmake -S examples/linux -B examples/linux/build
cmake --build examples/linux/build --target embedded-react-desktop
examples/linux/build/embedded-react-desktop              # runs the container in the slot
```

The build copies `dist/app.erpkg` into the slot next to the executable. Pass a path to run a
specific `.erpkg`, `.qbc` bytecode blob or `.js` source instead. With no config, the window shows a
"No config loaded" panel and stays up, like firmware would. The mouse is forwarded as touch; ESC
quits.

To try a new UI, drop a fresh `app.erpkg` next to the executable and restart it, which is the desktop
version of writing the config partition. For an edit-and-see loop use a simulator instead; see
[Hot reload](../hot-reload.md).

The host is factored into `host.c`, a thin layer over the portable `er_runtime` core: it creates
QuickJS, installs the `NativeUI` bridge and the host globals (`screen`, `console`, timers), loads the
container and runs the frame loop (pump, commit, present). Read it to see the complete Flow A host
in one file.

## Flow B: `examples/linux-aot`

The same window, with the app compiled ahead of time and no QuickJS in the binary. Generate the C
first, then build:

```bash
cd bridges/quickjs/js && npm run aot -- thermostat     # → dist/app.gen.c + assets.generated.c
cd ../../..
cmake -S examples/linux-aot -B examples/linux-aot/build
cmake --build examples/linux-aot/build --target embedded-react-desktop-aot
examples/linux-aot/build/embedded-react-desktop-aot
```

The build refuses to configure without a generated `app.gen.c` and says how to make one. `main.c` is
the reference Flow B host: register the backend, call `er_app_build()` once, then loop over input,
`er_commit()`, present and tick. The [CYD](./esp32-cyd.md) and [RP2040](./rp2040.md) firmware follow
the same shape.

## Why keep a desktop host

- **It runs under a debugger.** The SDL simulator (`npm run sim`) is the maintainers' engine-debug
  tool because gdb and lldb work on it; the browser simulator, being WebAssembly, is the consumer's.
- **Parity.** The repository's parity harness renders the same app through both flows on the desktop
  and diffs the pixels, which is how "a build flag, not a rewrite" is kept true.
- **Tests.** The engine's CTest suites and the bridge's runtime tests are host-side builds of the
  same code that ships to the boards.
