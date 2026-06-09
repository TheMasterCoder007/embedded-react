# examples/linux

The **desktop host** for Flow A — the analog of the iOS Simulator / Android emulator, and the on-desktop
peer of the ESP32 board. It mirrors the device's **uploaded-config model**: the exe is the "firmware"
(ships no app, no baked assets) and at boot loads a **config container** (`app.erpkg`) from a fixed
"config slot" next to the executable, exactly as the MCU loads its config from a flash partition. Same
QuickJS bridge + C engine the MCU runs, only the backend swapped to SDL. Build a config (`npm run pack`)
and run the *same* `.erpkg` here or on a device.

```
app.erpkg (bytecode + assets + CRC)  →  QuickJS + er_scene.h engine  →  backends/sdl  →  SDL2 window
```

**Status:** Implemented. One target, `embedded-react-desktop` — a thin `main.c` over the shared
desktop **host core** (`host.c` / `host.h`): the host inits QuickJS, installs the `NativeUI` bridge +
host globals (`screen`, `console`, timers), loads the config container, and runs the SDL frame loop
(pump → commit → present). Mouse is forwarded as touch. No config / a corrupt one shows an on-screen
panel (no built-in fallback), and the window stays up — just like firmware. The default demo is the
[thermostat](../../demos/thermostat/) (a draggable arc-dial climate UI). Press ESC to quit.

> The old handwritten C-API showcase has been removed — this project is "JSX on embedded", so the
> desktop demo is the JSX end-to-end path, not a separate C scene. The host core is factored out
> (`host.c`) so the upcoming **simulator** (`/SIMULATOR.md`) can reuse it — the demo drives it
> one-shot (`er_host_run`); the simulator will step it (`er_host_step`) around a watch + reload loop.

---

## Build & run

The desktop "firmware" builds standalone; you pack a config, and it gets copied into the slot:

```sh
# 1. Pack a demo into a config container  (from the repo root)
cd bridges/quickjs/js && npm install && npm run pack       # → dist/app.erpkg (bytecode + assets + CRC)
cd ../../..

# 2. Build + run the desktop host
cmake -S examples/linux -B examples/linux/build
cmake --build examples/linux/build --target embedded-react-desktop
examples/linux/build/embedded-react-desktop                # boots the config in the slot
```

The build copies `dist/app.erpkg` into the slot next to the exe (best-effort — the firmware builds
even with no config; it just shows a "No config loaded" panel until one is present). App resolution:
explicit CLI path (`.erpkg` container / `.qbc` bytecode / `.js` source) → otherwise the `app.erpkg`
slot. Run with no args for the slot config, or pass a path to test a specific one.

> Iterate: edit `demos/<name>/*` or the library `src/*` → `npm run pack` → rebuild the target
> (re-copies the container into the slot) → run. To "upload" a new config without rebuilding, drop a
> fresh `app.erpkg` next to the exe and restart — the same model as flashing a device's config
> partition. (For an instant edit-save-see loop instead, use the **simulator**: `npm run sim`.)

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
