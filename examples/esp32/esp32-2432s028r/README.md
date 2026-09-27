# examples/esp32/esp32-2432s028r — Cheap Yellow Display (Flow B)

Runs an Embedded React app on the **ESP32-2432S028R "Cheap Yellow Display"**: a widely available
ESP32 board with a 2.4" touchscreen and **no PSRAM**. There is no room for a JavaScript engine, so
the app is compiled ahead of time to C and linked into the firmware.

**Board:** ESP32-2432S028R v3 (micro-USB and USB-C). ESP32-WROOM-32, 240×320 ST7789 panel, XPT2046
resistive touch, 4 MB flash. The older v1/v2 board has an ILI9341 panel; the guide says what to swap.

**Guide:** [embedded-react.dev/guides/boards/esp32-cyd](https://embedded-react.dev/guides/boards/esp32-cyd)
covers the build, how the banded backend fits without PSRAM, orientation, tuning for your unit, the
pinout, and building outside the repository.

## Build

ESP-IDF v5.3 or newer (tested on v6.1), exported.

```bash
# 1. compile the app to C at the panel size (from your app project, or the repo equivalent below)
npx embedded-react build --aot --screen 240x320
#    in-repo: cd bridges/quickjs/js && ER_AOT_SCREEN_W=240 ER_AOT_SCREEN_H=320 npm run aot -- thermostat

# 2. build and flash, over the USB-C port
cd examples/esp32/esp32-2432s028r
idf.py set-target esp32              # first time only
idf.py -p PORT flash monitor         # Ctrl-] leaves the monitor
```

After editing the JSX, repeat step 1 and flash again. `main/board.c` holds the board-specific
`#define`s (inversion, BGR, mirroring, touch calibration) and `main/board.h` holds `BOARD_ROTATE_90`; the engine's pools are
trimmed for the board in `components/engine/CMakeLists.txt`.
