# examples/esp32/esp32-2432s028r — Cheap Yellow Display (Flow B)

Runs an Embedded React app on the **ESP32-2432S028R "Cheap Yellow Display"**: a widely available
ESP32 board with a 2.4" touchscreen and **no PSRAM**. There is no room for a JavaScript engine, so
the app is compiled ahead of time to a package, and a small fixed runtime in the firmware checks it and
runs it from flash. Changing the UI needs no firmware rebuild.

**Board:** ESP32-2432S028R v3 (micro-USB and USB-C). ESP32-WROOM-32, 240×320 ST7789 panel, XPT2046
resistive touch, 4 MB flash. The older v1/v2 board has an ILI9341 panel; the guide says what to swap.

**Guide:** [embedded-react.dev/guides/boards/esp32-cyd](https://embedded-react.dev/guides/boards/esp32-cyd)
covers the build, how the banded backend fits without PSRAM, orientation, tuning for your unit, the
pinout, and building outside the repository.

## Build

ESP-IDF v5.3 or newer (tested on v6.1), exported.

```bash
# 1. build and flash the firmware, over the USB-C port. With no package yet, the panel says so.
cd examples/esp32/esp32-2432s028r
idf.py set-target esp32              # first time only
idf.py -p PORT flash monitor         # Ctrl-] leaves the monitor

# 2. compile the app at the panel size (from your app project, or demos/thermostat)
npx embedded-react build --aot --screen 240x320

# 3. write it to the board's staging slot
npx embedded-react flash --board esp32-2432s028r --port PORT
```

After editing the JSX, repeat steps 2 and 3. An `sdkconfig` from before the package slots keeps the old partition
table: delete it and run `idf.py fullclean`. `main/board.c` holds the board-specific `#define`s (inversion, BGR,
mirroring, touch calibration) and `main/board.h` holds `BOARD_ROTATE_90`; the engine's pools are trimmed for the
board in `components/engine/CMakeLists.txt`, and `ERB_ARENA_BYTES` is in `main/main.c`.
