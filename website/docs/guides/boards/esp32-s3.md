---
title: 'ESP32-S3'
description: 'Flow A on the Waveshare ESP32-S3-Touch-LCD-7: build the firmware once, then flash your app as a config container.'
---

The ESP32-S3 example is the reference Flow A board: a real React reconciler on QuickJS, with the
JavaScript heap in PSRAM. Its example project lives at
[`examples/esp32/esp32-s3`](https://github.com/TheMasterCoder007/embedded-react/tree/master/examples/esp32/esp32-s3)
and is written to be copied out and adapted.

**Board:** Waveshare **ESP32-S3-Touch-LCD-7**. A 7" 800×480 RGB-parallel IPS panel, 8 MB octal
PSRAM, 16 MB flash, a GT911 capacitive touch controller on I²C, and a CH422G I²C expander that drives
the panel's reset, backlight and touch-reset lines. Two USB-C ports: "UART" (a CH343 bridge, used for
flashing) and "USB" (the chip's native USB, used for hot reload).

## How it is put together

The firmware and your app are **two independent artifacts**. The firmware brings up the panel, the
touch controller and QuickJS, then loads the app as a **config container** (`app.erpkg`) from a
dedicated flash partition: memory-mapped, checksum-verified, and stamped with the QuickJS version it
was built for. To ship a new UI you write one partition; the firmware is untouched.

```text
main/main.c        host: PSRAM JS heap, map + load the config partition, frame loop, touch
main/board.c       panel + touch bring-up for this board (CH422G, RGB panel, GT911)
CMakeLists.txt     top-level IDF project; fetches QuickJS-ng and, when copied out, embedded-react
partitions.csv     factory (firmware) + a 2 MB 'config' data partition for app.erpkg
sdkconfig.defaults ESP32-S3, octal PSRAM, 16 MB flash, a 64 KB main task stack
```

Dependencies are pulled by CMake `FetchContent` at configure time, nothing is vendored. Inside the
repository the example uses the local engine and bridge; copied out on its own, it fetches them from
GitHub at a pinned tag. QuickJS-ng is always fetched, pinned to `v0.15.0`, and that pin matters:
bytecode is specific to the QuickJS version, and the loader rejects a container built for another.

:::info[Why FetchContent and not the component registry]
Flow A needs QuickJS, which is a plain CMake project rather than an ESP-IDF component, so
`idf.py add-dependency` cannot manage it. `FetchContent` pulls the engine, the bridge and QuickJS
together. The registry route works for Flow B, which needs only the engine; see the
[CYD guide](./esp32-cyd.md).
:::

## Build and flash the firmware

You need **ESP-IDF v6.x** installed and exported (`. $IDF_PATH/export.sh`); the example is validated on
v6.0.1. The first configure downloads the dependencies, so it needs `git` and a network once.

```bash
cd examples/esp32/esp32-s3
idf.py set-target esp32s3
idf.py build flash monitor      # picks the port automatically, or pass -p PORT
```

Flash through the **UART** port. With no app loaded, the panel shows **"No config loaded"**. That is
the firmware working.

## Build and flash your app

From your own project (a `create-embedded-react` scaffold or anything that depends on
`embedded-react`):

```bash
npx embedded-react build                                                  # → dist/app.erpkg
parttool.py write_partition --partition-name=config --input dist/app.erpkg
```

`parttool.py` ships with ESP-IDF and finds the port itself. The board restarts into your app. From now
on, only this step is needed for a UI change.

The log should read roughly:

```text
I (xxx) embedded-react: embedded-react ESP32-S3 host
I (xxx) embedded-react: PSRAM free: 6……  internal free: …
I (xxx) board: RGB panel up: 800x480
I (xxx) embedded-react: display backend active
I (xxx) board: GT911 up: product id '911 '
I (xxx) embedded-react: config partition mapped (2097152 bytes) — loading container
I (xxx) embedded-react: config loaded
I (xxx) js: React mounted at 800x480
I (xxx) embedded-react: alive: 120 frames, …
```

`React mounted` plus a lit panel is the win. If the config is missing or rejected, the panel says
why (`No config loaded`, `Couldn't load config: <reason>`); if the panel itself fails to initialise,
the host falls back to a headless backend so the JavaScript side still runs and logs.

Inside the repository, `cd bridges/quickjs/js && npm run pack` packs one of the demos instead
(`npm run pack -- watch-face` picks another), and the container lands in
`bridges/quickjs/js/dist/app.erpkg`.

## Hot reload over USB

A firmware built with `idf.py -DER_HOTRELOAD=1 build flash` accepts a fresh app over the board's
**native USB** port on every save, with the running UI staying on screen until the new one is in and
component state preserved. Plug in both USB ports, then from your project:

```bash
npm i serialport                 # once: the optional native dependency for device upload
npx embedded-react dev --device  # auto-detects the ESP32's USB-Serial-JTAG
```

It is off by default and meant for development only; a release firmware should be built without it.
[Hot reload](../hot-reload.md) explains how it works and what it costs.

## Adapting the example to another board

Two pieces are board-specific. `main/board.c` and `board.h` bring up _this_ panel and touch
controller; the render backend (`components/esp32-lcd-backend`, which is `backends/esp32-lcd`) pushes
pixels through an `esp_lcd` panel handle.

- For another **RGB or I80 parallel** panel, keep the backend and rewrite `board.c` for your panel
  and touch chip.
- For an **SPI** panel (ILI9341, ST7789 and the like), swap the backend for `backends/esp32-spi-lcd`,
  the banded RGB565 backend the CYD example uses.

## Things to know

- **The host must present.** `er_commit()` paints into the backend's framebuffer and nothing more;
  `main.c` calls `er_esp32_lcd_present()` after each commit to flush the dirty region to the panel.
  Forget it and the screen stays black with a working panel.
- **Panel timings** (porches, pixel clock) and the data-pin to colour-channel map live in
  `board.c`. A shifted or rolling image means porch or clock; swapped red and blue means byte order.
- **The GT911** shares the panel's I²C bus. The reset sequence latches address `0x5D`, and point
  coordinates are read from register `0x8150`; taps in the wrong place usually mean an axis swap.
- **Task stack.** QuickJS and the reconciler recurse deeply; the main task stack is 64 KB and the
  JavaScript stack limit is set to three quarters of it. A JS "stack overflow" or a FreeRTOS stack
  panic means raise it.
- **PSRAM mode.** The defaults assume octal PSRAM at 80 MHz. A quad-PSRAM board needs
  `CONFIG_SPIRAM_MODE_QUAD`.
- **First boot pauses** while the bytecode loader reads about 940 KB; that is not a hang.
- **The config partition** is found by its label `config`; keep the label if you resize it, and
  size it at or above your `.erpkg` (2 MB by default).

## WiFi

The example has an optional WiFi build: a station the app can scan with and join, saved network and
time zone in encrypted NVS, and NTP keeping `Date.now()` on time. It is a build of its own, layered on
the defaults, because the WiFi stack needs internal RAM the board is short of:

```bash
idf.py -B build-wifi -D SDKCONFIG=sdkconfig.wifi -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.wifi" build flash
```

The thermostat demo's settings sheet gains a WiFi page and a time-zone page when the build provides
them. The costs are measured in the example's README: about 36 KB of internal RAM once WiFi is up, a
main task stack cut from 64 to 36 KB, and 615 KB of flash. The first boot of the WiFi build writes a
random key into eFuse `BLOCK_KEY5` for NVS encryption, which is permanent; set
`CONFIG_NVS_ENCRYPTION=n` before the first flash if you would rather keep settings in plain text.

## Measuring

Build with `-DER_PERF_OVERLAY=1` to draw the engine's frame instrumentation in the corner of the
panel: last and worst frame times, the split between JavaScript, layout, raster and present, and the
region the worst frame repainted. [Performance](../performance.md) explains how to read it.
