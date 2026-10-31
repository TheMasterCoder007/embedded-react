---
title: 'ESP32 Cheap Yellow Display'
sidebar_label: 'ESP32 CYD'
description: 'Flow B on the ESP32-2432S028R: a classic ESP32 with no PSRAM, driving a 240×320 SPI panel from a compiled package.'
---

The "Cheap Yellow Display" is a widely available ESP32 board with a 2.4" touchscreen and **no PSRAM**.
There is no room for a JavaScript engine, so this example uses Flow B: your JSX is compiled to a package
on your computer, and a small fixed runtime in the firmware checks it and runs it from flash. The example
project is at
[`examples/esp32/esp32-2432s028r`](https://github.com/TheMasterCoder007/embedded-react/tree/master/examples/esp32/esp32-2432s028r).

**Board:** ESP32-2432S028R **v3** (two USB ports, micro and USB-C). ESP32-WROOM-32, a 240×320
**ST7789** panel, an **XPT2046** resistive touch controller, 4 MB flash, no PSRAM. The older v1/v2
board with a single micro-USB port has an ILI9341 panel instead; see [Tuning](#tuning-for-your-board).

```text
App.jsx  →  build --aot  →  app.erpkg  →  flash slot  →  Flow B runtime  →  C engine  →  SPI backend  →  ST7789
                                                                                            ↑
                                                                                     XPT2046 touch
```

## Build and flash

You need **ESP-IDF v5.3 or newer** active in your shell (tested on v6.1).

**1. Build and flash the firmware.** You do this once. Plug in the **USB-C** port (it appears as a CH340 serial
device), then:

```bash
cd examples/esp32/esp32-2432s028r
idf.py set-target esp32            # first time only
idf.py -p PORT flash monitor       # e.g. /dev/ttyUSB0 or COM5; Ctrl-] leaves the monitor
```

With no package installed, the first boot shows the error screen, saying so.

:::note[Coming from the C output]
An `sdkconfig` from before the package slots keeps the old partition table. Delete it, and run `idf.py fullclean`.
:::

**2. Compile the app.** The panel size tells a responsive app which layout to fold to; the thermostat demo uses its
compact, dial-only layout at 240×320:

```bash
npx embedded-react build --aot --screen 240x320      # → dist/app.erpkg
```

**3. Write it to the board.**

```bash
npx embedded-react flash --board esp32-2432s028r --port PORT
```

The thermostat appears: drag the dial or tap − and +, tap the cog for °F/°C, and switch modes with
COOL / HEAT / AUTO / OFF. Each time you edit the JSX, repeat steps 2 and 3. The firmware doesn't change.

The log at boot:

```text
I (xxx) embedded-react: embedded-react ESP32-2432S028R (CYD) host — Flow B (no QuickJS)
I (xxx) embedded-react: free internal RAM: ~223000 bytes
I (xxx) board: ST7789 panel up: 240x320 (invert=0, bgr=0)
I (xxx) er-spi-lcd: SPI LCD backend ready: 240x320, RGB565 BANDED (2 x 40-row ping-pong buffers, 37 KB DMA RAM)
I (xxx) board: XPT2046 touch ready
I (xxx) embedded-react: package running from slot a; slot b is staging
I (xxx) embedded-react: free internal RAM after boot: ~181000 bytes
```

:::tip[Port already defined]
If `idf.py -p` complains that the port is already defined, `ESPPORT` is set in your environment; use
that instead of `-p`. If flashing wedges after many resets, unplug and replug the cable.
:::

**Orientation.** The example ships in the panel's native portrait, 240×320. `BOARD_ROTATE_90` in
`main/board.h` gives landscape 320×240, swapping the panel and touch axes together. The thermostat
lays landscape out differently, so build with `--screen 320x240` too; a package with no layout for the
panel is rejected at boot, with the reason on the error screen. If the
image is upside down, flip `BOARD_ROTATE_CW` in `main/board.c`.

## Package slots

`partitions.csv` gives the firmware 1.4 MB and each of two [package slots](../../internals/flow-b-packages.md#package-slots),
`erb_a` and `erb_b`, 1.25 MB. `embedded-react flash` writes the one that isn't running, so the last good package
stays in flash. If the new one fails verification, the board shows the reason for a few seconds, then runs the
last good one.

The arena is 2 KB (`ERB_ARENA_BYTES` in `main/main.c`), and the thermostat needs under 1 KB. The build prints what
a package needs. One that needs more than the firmware reserves is rejected at boot, with both amounts on the error
screen.

## How it fits without PSRAM

A full 240×320 framebuffer in 16-bit color is 150 KB, more than this ESP32's largest free block. The
backend instead draws the screen in **horizontal strips of about 40 rows** and lets the panel's own
memory hold the rest of the picture. Two strip buffers of about 19 KB each ping-pong: while the panel
receives one over SPI, the CPU draws the next. The result is full 16-bit colour with anti-aliased
text at a quarter of the RAM.

The engine's pools are trimmed for the board in `components/engine/CMakeLists.txt`: a smaller node
pool, a small opacity scratch buffer, shadows off. The firmware logs free RAM at boot and after
start-up. If you run short, in order: shrink the strip height with `ER_LCD_BANDED_ROWS` (less RAM,
more transfers), lower `ERUI_MAX_NODES`, or rebuild the app with a smaller `ER_AOT_LIST_CAP`.
[Memory](../memory.md) goes through the numbers.

A boot loop on `assert failed: vApplicationGetIdleTaskMemory` is the same shortage, earlier: until
`app_main` runs, FreeRTOS allocates its first tasks from the RAM that static data leaves free. Lower
`ERUI_MAX_NODES` to what the app needs; the thermostat uses 41.

The backend, `backends/esp32-spi-lcd`, works with any `esp_lcd` RGB565 SPI panel. The board brings
the panel up (SPI bus, panel and IO handle, reset, inversion, orientation, backlight) and passes both
handles to `er_esp32_spi_lcd_backend_init(panel, io, width, height)`; in banded mode there is nothing
to call per frame. A full-framebuffer mode exists (`ER_LCD_BANDED` unset) for a board with a big
enough block, with `er_esp32_spi_lcd_present()` called after each `er_commit()`.

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
Registry ([Installation](../../getting-started/installation.mdx) has the command), but the SPI backend is
not, so with the registry route you vendor or fetch the backend yourself; that is why the example
defaults to `FetchContent`, which covers both.
