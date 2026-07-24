# Cheap Yellow Display (ESP32-2432S028R) — no-PSRAM example

Runs an embedded-react app on the **DIYmall ESP32-2432S028R "Cheap Yellow Display" (CYD)** — a cheap,
widely available ESP32 board with a 2.4" touchscreen and **no PSRAM**.

Because there's no PSRAM to hold a JavaScript engine, this example uses **Flow B (AOT)**: your JSX is
compiled **ahead-of-time to C** on your computer and linked straight into the firmware. There's **no
QuickJS and no JavaScript on the device** — the compiled C app *is* the firmware.

```
App.jsx ──(npm run aot)──▶ app.gen.c ──▶ C engine ──▶ SPI backend ──▶ ST7789 screen
                                                                          ▲
                                                                   XPT2046 touch
```

**Board:** ESP32-2432S028R **v3** (two USB ports: micro + USB-C). ESP32-WROOM-32, 240×320 **ST7789**
panel, **XPT2046** resistive touch, 4 MB flash, no PSRAM. *Have the older v1/v2 (single micro-USB)? It
uses an **ILI9341** panel — see [Tuning for your board](#tuning-for-your-board).*

## Quick start

You need **ESP-IDF v5.3 or newer** installed and active in your shell (tested on v6.1). Activate it with
`. $IDF_PATH/export.sh` (macOS/Linux) or `%IDF_PATH%\export.bat` (Windows).

**1. Compile the app to C.** Run this from the repo root. The `240×320` size tells the demo to use its
**compact** layout (dial + steppers + mode buttons), which is what fits this screen:

```bash
cd bridges/quickjs/js
ER_AOT_SCREEN_W=240 ER_AOT_SCREEN_H=320 npm run aot -- thermostat   # writes dist/app.gen.c
cd ../../..
```

**2. Build and flash.** Plug the board into your computer via the **USB-C** port, then:

```bash
cd examples/esp32/esp32-2432s028r
idf.py set-target esp32            # first time only
idf.py -p <PORT> flash monitor    # e.g. -p /dev/ttyUSB0  or  -p COM5
```

Replace `<PORT>` with your board's serial port (the CYD's USB-C shows up as a CH340 serial device).
`monitor` opens the live log — press `Ctrl-]` to exit.

**That's it.** The thermostat appears on the screen: tap **−** / **+** to change the temperature, and
**Heat / Cool / Auto / Off** to switch mode (the dial recolors to match). Whenever you edit the JSX,
re-run **step 1** and `idf.py flash` again.

You should see this in the log:

```
I (xxx) embedded-react: embedded-react ESP32-2432S028R (CYD) host — Flow B (AOT, no QuickJS)
I (xxx) embedded-react: free internal RAM: ~223000 bytes
I (xxx) board: ST7789 panel up: 240x320 (invert=0, bgr=0)
I (xxx) er-spi-lcd: SPI LCD backend ready: 240x320, RGB565 BANDED (2 x 40-row ping-pong buffers, 37 KB DMA RAM)
I (xxx) board: XPT2046 touch ready
I (xxx) embedded-react: AOT app built at 240x320 (no QuickJS)
I (xxx) embedded-react: free internal RAM after boot: ~181000 bytes
```

> **Troubleshooting the port.** If `idf.py -p` complains the port is "already defined", you have `ESPPORT`
> set in your environment — use that instead of `-p` (`export ESPPORT=/dev/ttyUSB0`; on Windows,
> `set "ESPPORT=COM5"` — keep the quotes so no trailing space sneaks in). If flashing wedges after many
> resets, unplug and replug the USB-C cable.

## How it works: fitting a screen with no PSRAM

A full-screen 240×320 framebuffer in 16-bit color is **150 KB** — too big for this ESP32's fragmented
internal memory (the largest free block is only ~110 KB). Instead of one big buffer, the backend draws
the screen in **horizontal strips of ~40 rows** and lets the display's own memory hold the rest of the
picture. Only the strips being drawn ever live in RAM.

There are **two strip buffers** (~19 KB each, ~37 KB total). While the display is busy receiving one
strip over SPI, the CPU is already drawing the next one into the other buffer — so drawing and
transferring overlap instead of waiting on each other. The payoff: **full 16-bit color** (smooth,
anti-aliased text) using far less RAM than a whole framebuffer.

The engine's memory pools are also trimmed for this board in `components/engine/CMakeLists.txt`
(`ERUI_MAX_NODES`, a small opacity scratch buffer, shadows off). The firmware logs free RAM at boot and
after startup so you can keep an eye on headroom.

> **Running low on RAM?** Options, in order: shrink the strip height with `ER_LCD_BANDED_ROWS` (in the
> backend component's `CMakeLists.txt`; smaller = less RAM, more DMA transfers), lower `ERUI_MAX_NODES`,
> or rebuild the app with a smaller `ER_AOT_LIST_CAP`.

## Tuning for your board

The settings below are dialed in for the **v3 (dual-USB) CYD** this example was developed on. Other units
vary slightly — everything here is a `#define` at the top of [`main/board.c`](main/board.c) (or the
backend's `CMakeLists.txt`). Change one, rebuild, reflash.

| Symptom on screen | Fix |
|---|---|
| Colors look like a photo negative | Toggle `BOARD_LCD_INVERT` |
| Red and blue are swapped | Toggle `BOARD_LCD_BGR` |
| Text/image is mirrored | Toggle `BOARD_LCD_MIRROR_X` (or `_Y`) |
| Faint lines or glitches | Lower `LCD_PCLK_HZ` (default 40 MHz → try 30 or 26) |
| Colors subtly wrong (gray→green, blue→pink) | See *color byte order* below |
| Taps land in the wrong place | Recalibrate touch — see below |

- **Older v1/v2 board (ILI9341 panel).** In `board.c`, swap `esp_lcd_new_panel_st7789` for
  `esp_lcd_new_panel_ili9341`. The pins are identical.

- **Color byte order (`ER_SPI_LCD_SWAP_BGR`).** This particular CYD's panel wants its two color bytes
  swapped *and* red/blue in BGR order, so the backend enables `ER_SPI_LCD_SWAP_BGR=1`. If your panel
  shows gray as pastel-green or blue as pink (while red and white look fine), it needs this flag; a
  standard RGB565 panel does not. It lives in the backend component's `CMakeLists.txt`.

- **Touch calibration.** Touch is mapped with `TOUCH_X_MIN/MAX`, `TOUCH_Y_MIN/MAX`, and the
  `TOUCH_FLIP_X/Y` flags. To calibrate a new unit: uncomment the raw-value `ESP_LOGI` in
  `board_touch_read`, tap the four corners in order (top-left → top-right → bottom-right → bottom-left),
  read the raw numbers from the log, and set the min/max (and flips) so taps land where you touch.

## Pinout

| Bus | Pins (GPIO) |
|---|---|
| **Display** — ST7789 on HSPI | SCLK 14, MOSI 13, CS 15, DC 2, RST (software), Backlight 21 |
| **Touch** — XPT2046 on VSPI | CLK 25, MOSI 32, MISO 39, CS 33, IRQ 36 |

There's no official datasheet for these generic Sunton/DIYmall boards; this pin map comes from the
community CYD references (randomnerdtutorials, mischianti, and witnessmenow's *ESP32-Cheap-Yellow-Display*).

## Using this outside the monorepo

When you build in-tree, this example uses the engine and backend from the local checkout. Copy the folder
out on its own and `idf.py build` instead **fetches them from GitHub** — Flow B only needs the engine and
the SPI backend (no QuickJS). Generate `app.gen.c` separately with step 1 above.

You can also pull the engine as a managed component from the ESP-IDF Component Registry:

```bash
idf.py add-dependency "TheMasterCoder007/embedded-react^0.3.0"
```

Then drop `app.gen.c` / `app.gen.h` into `main/` and supply a backend. Note the SPI backend
(`backends/esp32-spi-lcd`) is **not** on the registry — this example compiles it from source, so with the
registry route you'd vendor or fetch the backend yourself. That's why the example defaults to
`FetchContent`: one mechanism covers both the engine and the backend.
