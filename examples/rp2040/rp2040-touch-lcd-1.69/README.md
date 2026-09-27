# examples/rp2040/rp2040-touch-lcd-1.69 — RP2040 watch example (Flow B)

Runs the watch-face demo on the **Waveshare RP2040-Touch-LCD-1.69**, the smallest verified target:
264 KB SRAM, no FPU, no PSRAM. The app is compiled ahead of time to C and linked into the firmware.

**Board:** RP2040, 1.69" 240×280 ST7789V2 panel (portrait, +20-row GRAM offset), CST816S capacitive
touch, QMI8658 IMU, 4 MB flash. The round 1.28" sibling (GC9A01A, 240×240) shares the pins and touch
chip; the guide says what to change.

**Guide:** [embedded-react.dev/guides/boards/rp2040](https://embedded-react.dev/guides/boards/rp2040)
covers the build, how the sensors reach the app through `useHostValue`, tuning for your unit,
bring-up debugging, and the pinout.

## Build

The Raspberry Pi Pico SDK, the Arm GNU toolchain (`arm-none-eabi-gcc`), CMake, and Ninja or Make.

```bash
# 1. compile the app to C at the panel size
npx embedded-react build --aot --screen 240x280
#    in-repo: cd bridges/quickjs/js && ER_AOT_SCREEN_W=240 ER_AOT_SCREEN_H=280 npm run aot -- watch-face

# 2. build the firmware
cd examples/rp2040/rp2040-touch-lcd-1.69
PICO_SDK_PATH=/path/to/pico-sdk cmake -S . -B build -G Ninja     # or -DPICO_SDK_FETCH_FROM_GIT=ON
cmake --build build                                               # → build/embedded-react-watch.uf2

# 3. flash: hold BOOT while plugging in and copy the .uf2 onto the RPI-RP2 drive, or
picotool load -f -x build/embedded-react-watch.uf2
```

`main.c` runs the frame loop and feeds the pedometer and tilt into the app; `board.c` owns the panel,
touch, and IMU; `pedometer.c` turns accelerometer peaks into steps. `-DER_BOARD_DEBUG=1` prints a 1 Hz
heartbeat, `=2` also holds a color-bar test pattern at boot.
