---
title: 'ESP32 Cheap Yellow Display'
sidebar_label: 'ESP32 CYD'
description: 'Flow B on the ESP32-2432S028R: a classic ESP32 with no PSRAM, driving a 240×320 SPI panel from compiled C.'
---

The "Cheap Yellow Display" is a widely available ESP32 board with a 2.4" touchscreen and **no PSRAM**.
There is no room for a JavaScript engine, so this example uses Flow B: your JSX is compiled to C on
your computer and linked into the firmware. Nothing on the board interprets anything. The example
project is at
[`examples/esp32/esp32-2432s028r`](https://github.com/TheMasterCoder007/embedded-react/tree/master/examples/esp32/esp32-2432s028r).

**Board:** ESP32-2432S028R **v3** (two USB ports, micro and USB-C). ESP32-WROOM-32, a 240×320
**ST7789** panel, an **XPT2046** resistive touch controller, 4 MB flash, no PSRAM. The older v1/v2
board with a single micro-USB port has an ILI9341 panel instead; see [Tuning](#tuning-for-your-board).

```text
App.jsx  →  build --aot  →  app.gen.c  →  C engine  →  SPI backend  →  ST7789
                                                          ↑
                                                   XPT2046 touch
```

## Build and flash

You need **ESP-IDF v5.3 or newer** active in your shell (tested on v6.1).

**1. Compile the app to C.** The panel size tells a responsive app which layout to fold to; the
thermostat demo uses its compact, dial-only layout at 240×320:

```bash
npx embedded-react build --aot --screen 240x320      # → dist/app.gen.c, app.gen.h, assets.generated.c
```

Inside the repository the equivalent is `cd bridges/quickjs/js && ER_AOT_SCREEN_W=240 ER_AOT_SCREEN_H=320 npm run aot -- thermostat`.

**2. Build and flash.** Plug in the **USB-C** port (it appears as a CH340 serial device), then:

```bash
cd examples/esp32/esp32-2432s028r
idf.py set-target esp32            # first time only
idf.py -p PORT flash monitor       # e.g. /dev/ttyUSB0 or COM5; Ctrl-] leaves the monitor
```

The thermostat appears: drag the dial or tap − and +, tap the cog for °F/°C, and switch modes with
COOL / HEAT / AUTO / OFF. Each time you edit the JSX, repeat step 1 and flash again.

The log at boot:

```text
I (xxx) embedded-react: embedded-react ESP32-2432S028R (CYD) host — Flow B (AOT, no QuickJS)
I (xxx) embedded-react: free internal RAM: ~223000 bytes
I (xxx) board: ST7789 panel up: 240x320 (invert=0, bgr=0)
I (xxx) er-spi-lcd: SPI LCD backend ready: 240x320, RGB565 BANDED (2 x 40-row ping-pong buffers, 37 KB DMA RAM)
I (xxx) board: XPT2046 touch ready
I (xxx) embedded-react: AOT app built at 240x320 (no QuickJS)
I (xxx) embedded-react: free internal RAM after boot: ~181000 bytes
```

:::tip[Port already defined]
If `idf.py -p` complains that the port is already defined, `ESPPORT` is set in your environment; use
that instead of `-p`. If flashing wedges after many resets, unplug and replug the cable.
:::

**Orientation.** The example ships in the panel's native portrait, 240×320. `BOARD_ROTATE_90` in
`main/board.h` gives landscape 320×240, swapping the panel and touch axes together; the `--screen`
size must match, or a responsive app folds to the wrong layout on a correctly rotated screen. If the
image is upside down, flip `BOARD_ROTATE_CW` in `main/board.c`.

## How it fits without PSRAM

A full 240×320 framebuffer in 16-bit colour is 150 KB, more than this ESP32's largest free block. The
backend instead draws the screen in **horizontal strips of about 40 rows** and lets the panel's own
memory hold the rest of the picture. Two strip buffers of about 19 KB each ping-pong: while the panel
receives one over SPI, the CPU draws the next. The result is full 16-bit colour with anti-aliased
text at a quarter of the RAM.

The engine's pools are trimmed for the board in `components/engine/CMakeLists.txt`: a smaller node
pool, a small opacity scratch buffer, shadows off. The firmware logs free RAM at boot and after
start-up. If you run short, in order: shrink the strip height with `ER_LCD_BANDED_ROWS` (less RAM,
more transfers), lower `ERUI_MAX_NODES`, or rebuild the app with a smaller `ER_AOT_LIST_CAP`.
[Memory](../memory.md) goes through the numbers.

## Tuning for your board

These generic boards vary unit to unit. Every setting is a `#define` at the top of `main/board.c` or
in the backend component's `CMakeLists.txt`; change one, rebuild, reflash.

| Symptom                               | Fix                                                                                                                                                |
| ------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------- |
| Colours look like a photo negative    | Toggle `BOARD_LCD_INVERT`                                                                                                                          |
| Red and blue are swapped              | Toggle `BOARD_LCD_BGR`                                                                                                                             |
| Text or image is mirrored             | Toggle `BOARD_LCD_MIRROR_X` or `_Y`                                                                                                                |
| Faint lines or glitches               | Lower `LCD_PCLK_HZ` (40 MHz by default; try 30 or 26)                                                                                              |
| Grey renders green, blue renders pink | The panel wants its colour bytes swapped: `ER_SPI_LCD_SWAP_BGR=1` in the backend's `CMakeLists.txt` (already on for this unit)                     |
| Taps land in the wrong place          | Recalibrate: log the raw values in `board_touch_read`, tap the four corners, set `TOUCH_X_MIN/MAX`, `TOUCH_Y_MIN/MAX` and the `TOUCH_FLIP_*` flags |
| Older v1/v2 board (ILI9341)           | Swap `esp_lcd_new_panel_st7789` for `esp_lcd_new_panel_ili9341` in `board.c`; the pins are identical                                               |

## Pinout

| Bus                     | GPIO                                                      |
| ----------------------- | --------------------------------------------------------- |
| Display, ST7789 on HSPI | SCLK 14, MOSI 13, CS 15, DC 2, RST software, backlight 21 |
| Touch, XPT2046 on VSPI  | CLK 25, MOSI 32, MISO 39, CS 33, IRQ 36                   |

There is no official datasheet for these boards; the map comes from the community references
(randomnerdtutorials, mischianti, witnessmenow's _ESP32-Cheap-Yellow-Display_).

## Outside the repository

Copy the example folder out on its own and `idf.py build` fetches the engine and the SPI backend
from GitHub. Flow B needs nothing else, no QuickJS. The engine is also on the ESP-IDF Component
Registry (`idf.py add-dependency "TheMasterCoder007/embedded-react^0.14.1"`), but the SPI backend is
not, so with the registry route you vendor or fetch the backend yourself; that is why the example
defaults to `FetchContent`, which covers both.
