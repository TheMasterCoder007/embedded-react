# examples/esp32/esp32-2432s028r — Cheap Yellow Display (Flow B / AOT)

Runs embedded-react on the **DIYmall ESP32-2432S028R "Cheap Yellow Display" (CYD)** via **Flow B**: the
JSX app is compiled **ahead-of-time to C** (`npm run aot`) and linked into the firmware — **no QuickJS,
no JS at runtime**. This is the path for a board with **no PSRAM**: the original ESP32-WROOM-32 can't
host the QuickJS heap, but the AOT-compiled C app needs no JS engine at all.

**Target board:** ESP32-2432S028R **v3** (two USB ports: micro + USB-C) — ESP32-WROOM-32, 240×320
**ST7789** SPI panel, **XPT2046** resistive touch, 4 MB flash, no PSRAM. (The v1/v2 single-micro-USB
boards use an **ILI9341** panel instead — see *Display quirks* below.)

```
App.jsx → AOT → app.gen.c → C engine (er_scene.h) → SPI backend → ST7789 panel
                                    ↑ XPT2046 touch
```

## How it fits in internal RAM

Flow B's whole point is fitting a no-PSRAM MCU. The only large allocation is the **150 KB RGB565
framebuffer** (`backends/esp32-spi-lcd`, internal RAM, full-width-band flush — no staging buffer). The
engine's static pools are tuned down in `components/engine/CMakeLists.txt` (`ERUI_MAX_NODES=96`, tiny
opacity scratch, shadows/gradients off). `app_main` logs free internal RAM at boot — watch it.

> If the framebuffer alloc fails at boot ("out of internal RAM"), free RAM by lowering `ERUI_MAX_NODES`
> and/or rebuilding the app with a smaller `ER_AOT_LIST_CAP` (fewer pooled list rows = fewer nodes).

## Build

Prerequisites: **ESP-IDF v5.3+** installed and exported (`. $IDF_PATH/export.sh`).

**1. Generate the AOT app** (sized for this board's RAM — a small list-pool cap keeps the node count
down). From the repo root:

```bash
cd bridges/quickjs/js
ER_AOT_LIST_CAP=8 npm run aot -- music-player    # → dist/app.gen.{c,h}
cd ../../..
```

**2. Build + flash:**

```bash
cd examples/esp32/esp32-2432s028r
idf.py set-target esp32                 # first time only
idf.py -p <PORT> flash monitor          # e.g. -p COM5 ; the CYD's USB-C is a CH340 serial port
```

> If `ESPPORT` is already set in your environment, `idf.py -p` errors ("port already defined") — set
> the env var instead: `ESPPORT=COM5` (Windows cmd: `set "ESPPORT=COM5"` — quote it, or a trailing
> space sneaks into the value and esptool can't open the port). `monitor` is the live serial console
> (`Ctrl-]` to exit). If the port wedges after many flashes/resets, unplug/replug the USB-C.

Expected monitor output:

```
I (xxx) embedded-react: embedded-react ESP32-2432S028R (CYD) host — Flow B (AOT, no QuickJS)
I (xxx) embedded-react: free internal RAM: 2xxxxx bytes
I (xxx) board: ST7789 panel up: 240x320 (invert=0, bgr=0)
I (xxx) er-spi-lcd: SPI LCD backend ready: 240x320, RGB332 fb in internal RAM (75 KB) + 11 KB DMA bounce
I (xxx) board: XPT2046 touch ready
I (xxx) embedded-react: AOT app built at 240x320 (no QuickJS)
I (xxx) embedded-react: free internal RAM after boot: 1xxxxx bytes
```

The music player appears; tapping **Play**, **+ Add**, **- Remove** updates the screen — all from
compiled C. Re-run **step 1** whenever you change the JSX, then `idf.py flash`.

## Display / touch notes (verified on a v3 unit; tweak in `main/board.c`)

The defaults below are dialed in for the **v3 (dual-USB) CYD this was developed on**. A different unit
may need a tweak — they're all `#define`s at the top of `board.c`:

- **RGB332 framebuffer.** The ESP32's internal RAM is fragmented (largest byte-addressable block ~110 KB),
  so a full 240×320 **RGB565** fb (150 KB) does NOT fit — the backend uses **RGB332** (75 KB,
  `ER_SPI_LCD_FB8=1`). Trade-off: 256 colors, so anti-aliased small text looks a bit coarse. Big text and
  flat fills look fine. (A PSRAM board could use RGB565.)
- **Display:** `BOARD_LCD_INVERT` (this unit = `false`; set `true` if the screen looks like a photo
  negative), `BOARD_LCD_BGR` (red/blue swapped), `BOARD_LCD_MIRROR_X` (this unit = `true`; flip if text
  is mirrored), `LCD_PCLK_HZ` (20 MHz — raising it can cause lines/glitches on CYD wiring). A **v1/v2
  (ILI9341)** board: swap `esp_lcd_new_panel_st7789` → `esp_lcd_new_panel_ili9341` (the SPI pins are
  identical).
- **Touch:** calibrated via `TOUCH_X_MIN/MAX`, `TOUCH_Y_MIN/MAX` and the `TOUCH_FLIP_X/Y` flags
  (`FLIP_X=true` here, to match the mirrored display). To recalibrate a different unit, re-enable the
  raw-value `ESP_LOGI` in `board_touch_read`, tap the four corners (TL→TR→BR→BL), and set the min/max +
  flips so taps land where you touch.

## Pinout (CYD, community-documented)

| | GPIO |
|---|---|
| **Display (ST7789, HSPI)** | SCLK 14, MOSI 13, CS 15, DC 2, RST sw, **Backlight 21** |
| **Touch (XPT2046, VSPI)** | CLK 25, MOSI 32, MISO 39, CS 33, IRQ 36 |

I couldn't find a manufacturer datasheet (generic Sunton/DIYmall board) but I was able to find several sources online
that helped me find what I needed; Pin map from the community CYD references (randomnerdtutorials / mischianti / 
witnessmenow's ESP32-Cheap-Yellow-Display)

## Self-contained via fetch

Like the esp32-s3 example, dependencies are the **local monorepo when building in-tree**, else
**fetched from GitHub** — but Flow B pulls in only the engine + SPI backend (no QuickJS). Copy this
folder out and `idf.py build` pulls what it needs. The AOT `app.gen.c` is generated separately (step 1).
