# examples/linux

The desktop host for Flow A: the same QuickJS bridge and C engine the boards run, with SDL2 as the
backend. It mirrors the ESP32-S3's model exactly: the executable ships no app and at start loads a
config container (`app.erpkg`) from a slot next to itself, the way the board loads its config
partition. It builds on Linux, macOS and Windows.

**Guide:** [embedded-react.dev/guides/boards/linux](https://embedded-react.dev/guides/boards/linux)
(this host, the Flow B twin in `../linux-aot`, and why a desktop host is kept).

## Build and run

CMake 3.16+, a C compiler, and SDL2 2.0.6+ (`apt install libsdl2-dev`, `brew install sdl2`, or vcpkg
on Windows with its toolchain file passed to CMake).

```sh
# 1. pack a demo into a config container (from the repo root)
cd bridges/quickjs/js && npm install && npm run pack       # → dist/app.erpkg
cd ../../..

# 2. build and run the host
cmake -S examples/linux -B examples/linux/build
cmake --build examples/linux/build --target embedded-react-desktop
examples/linux/build/embedded-react-desktop                # runs the container in the slot
```

The build copies `dist/app.erpkg` into the slot next to the executable; pass a path to run a specific
`.erpkg`, `.qbc` or `.js` instead. The mouse is forwarded as touch; ESC quits. `host.c` is the complete
Flow A host in one file, a thin layer over `er_runtime`, and is shared with the SDL simulator in
`tools/simulator`.
