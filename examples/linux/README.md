# examples/linux

Desktop sample app — engine + `backends/sdl/` inside an SDL2 window. No bridge yet;
the scene is driven directly via `er_scene.h` to validate the full render stack
(Yoga layout → rrect rasterizer → SDL2 blit) on a real display without flashing hardware.

**Status:** Implemented. C-driver path only (no React/QuickJS). Displays a dark-background
window with a title and two rounded-corner cards at 480 × 320. The second card is
clickable, draggable out/back in, and long-pressable through the engine touch-event path,
with its background color animated by the native timing animation engine. Two overlapping
zIndex badges in the top-right demonstrate matching render and hit-test stacking. Press
ESC to quit.

---

## Building on Windows

### Prerequisites

- [CMake](https://cmake.org/download/) 3.16 or later (add to PATH during install)
- A C compiler — Visual Studio 2019/2022 (MSVC) or [MinGW-w64](https://www.mingw-w64.org/)
- [SDL2](https://github.com/libsdl-org/SDL/releases) 2.0.6 or later

### Install SDL2 via vcpkg (recommended)

```bat
git clone https://github.com/microsoft/vcpkg C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
C:\vcpkg\vcpkg install sdl2:x64-windows
```

Then configure with the vcpkg toolchain file:

```bat
cd examples\linux
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release
build\Release\embedded-react-desktop.exe
```

### Install SDL2 manually

Download the `SDL2-devel-x.y.z-VC.zip` (MSVC) or `SDL2-devel-x.y.z-mingw.tar.gz`
(MinGW) development package from the SDL2 releases page. Extract it and pass the
`cmake/` subfolder as `SDL2_DIR`:

```bat
cmake -S . -B build -DSDL2_DIR=C:\SDL2-2.30.0\cmake
cmake --build build --config Release
```

Copy `SDL2.dll` next to the built executable before running:

```bat
copy C:\SDL2-2.30.0\lib\x64\SDL2.dll build\Release\
build\Release\embedded-react-desktop.exe
```

---

## Building on Linux / macOS

Install SDL2 from your package manager:

```sh
# Debian / Ubuntu
sudo apt install libsdl2-dev

# macOS (Homebrew)
brew install sdl2
```

Then build:

```sh
cd examples/linux
cmake -S . -B build
cmake --build build
./build/embedded-react-desktop
```

---

## What you should see

A 480 × 320 window titled **embedded-react** with:

- Dark navy background (`#1A1A2E`)
- White title text at the top
- A dark blue rounded card labelled *Scene graph · Yoga flexbox · Rounded rects*
- A red rounded card labelled *Click me: hit-testing + press events*
- Clicking the red card animates it to a green active state
- Holding the card triggers the long-press path
- Dragging out and back in exercises press-out / press-in transitions
- Clicking the overlap between the top-right badges hits the higher zIndex target

This is a pure C99 scene built with `er_scene.h`. React / QuickJS integration
comes in the next milestone (Flow A).
