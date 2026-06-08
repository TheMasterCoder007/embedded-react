# examples/linux

The **desktop host** for Flow A — the analog of the iOS Simulator / Android emulator. It boots the
same QuickJS bridge + C engine the MCU runs and paints a JSX app into an SDL2 window, with only the
backend swapped to SDL. Develop here (instant), then flash the *same* bundle to a device.

```
JSX bundle (+ baked assets)  →  QuickJS + er_scene.h engine  →  backends/sdl  →  SDL2 window
```

**Status:** Implemented. One target, `embedded-react-desktop` (`main.c`): it inits QuickJS, installs
the `NativeUI` bridge + host globals (`screen`, `console`, timers), loads the app, and runs the SDL
frame loop (pump → commit → present). Mouse is forwarded as touch. The default demo is the
[thermostat](../../demos/thermostat/) (a draggable arc-dial climate UI). Press ESC to quit.

> The old hand-written C-API showcase has been removed — this project is "JSX on embedded", so the
> desktop demo is the JSX end-to-end path, not a separate C scene.

---

## Build & run

The desktop target runs whatever bundle `npm run build` produced. Build the JS app first, then the
example:

```sh
# 1. Bundle a demo + bake its assets  (from the repo)
cd bridges/quickjs/js && npm install && npm run build      # → dist/app.bundle.js + dist/assets.generated.c
cd ../../..

# 2. Build + run the desktop host
cmake -S examples/linux -B examples/linux/build
cmake --build examples/linux/build --target embedded-react-desktop
examples/linux/build/embedded-react-desktop                # boots the bundle by default
```

The build copies `app.bundle.js` next to the exe, precompiles it to `app.bundle.qbc` (bytecode —
preferred at boot), and compiles `dist/assets.generated.c` so the demo's `<Image>`/`<Text>` assets
resolve. App resolution: explicit CLI path → `app.bundle.qbc` → `app.bundle.js` → a small built-in JS
demo. Run with no args for the active bundle, or pass a path to iterate on a different one.

> Iterate: edit `demos/<name>/*` or the library `src/*` → `npm run build` → rebuild the target
> (re-copies bundle + assets) → run. Configure once with `npm run build` already done so the baked
> assets are present (otherwise the example builds text-only and prints a status note).

---

## Prerequisites (SDL2)

CMake 3.16+, a C compiler, and SDL2 2.0.6+ (`SDL_ComposeCustomBlendMode`).

**Windows (vcpkg):**

```bat
git clone https://github.com/microsoft/vcpkg C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
C:\vcpkg\vcpkg install sdl2:x64-windows
cmake -S examples\linux -B examples\linux\build -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
```

On Windows the build copies `SDL2.dll` next to the exe automatically.

**Linux / macOS:**

```sh
sudo apt install libsdl2-dev      # Debian/Ubuntu
brew install sdl2                 # macOS
cmake -S examples/linux -B examples/linux/build
```

---
