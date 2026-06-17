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

Flow B's whole point is fitting a no-PSRAM MCU. A full 240×320 **RGB565** framebuffer is 150 KB, which
does **not** fit the ESP32's fragmented internal DRAM (largest contiguous block ~110 KB). So the backend
renders in **horizontal strips through a small RGB565 band buffer** (`ER_LCD_BANDED`): the engine repaints
only the dirty rows, one ~40-row strip at a time, into a **~19 KB DMA-capable band buffer**, and the
ST7789's own GRAM retains the rest of the frame. The result is **full 16-bit color at less RAM than the
old RGB332 framebuffer** (75 KB) — crisp anti-aliased text, no 256-color banding. The engine's static
pools are also tuned down in `components/engine/CMakeLists.txt` (`ERUI_MAX_NODES`, tiny opacity scratch,
shadows/gradients off). `app_main` logs free internal RAM at boot — watch it.

> Tune the strip height with `ER_LCD_BANDED_ROWS` (default 40) in the backend component's CMakeLists: a
> taller strip costs more RAM (`240 × rows × 2` bytes) but flushes the screen in fewer DMA transfers.
> If RAM is still tight, lower `ERUI_MAX_NODES` and/or rebuild the app with a smaller `ER_AOT_LIST_CAP`.

## Build

Prerequisites: **ESP-IDF v5.3+** installed and exported (`. $IDF_PATH/export.sh`).

**1. Generate the AOT app.** The `ER_AOT_SCREEN_W/H` you pass selects the demo's responsive layout at
compile time — `240×320` folds the thermostat's **compact** branch (dial + steppers + modes, no weather),
which is exactly what fits this board. From the repo root:

```bash
cd bridges/quickjs/js
ER_AOT_SCREEN_W=240 ER_AOT_SCREEN_H=320 npm run aot -- thermostat    # → dist/app.gen.{c,h}
cd ../../..
```

> From **your own app project** (outside this monorepo), the consumer equivalent is
> `npx embedded-react build --aot`, which emits `app.gen.c` / `app.gen.h` (+ `assets.generated.c`) for the
> app in your project. `npm run aot` shown above is the in-repo form that compiles a demo from `demos/`.

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
I (xxx) er-spi-lcd: SPI LCD backend ready: 240x320, RGB565 BANDED (40-row band buffer, 18 KB DMA RAM) — panel GRAM retains the frame
I (xxx) board: XPT2046 touch ready
I (xxx) embedded-react: AOT app built at 240x320 (no QuickJS)
I (xxx) embedded-react: free internal RAM after boot: 1xxxxx bytes
```

The music player appears; tapping **Play**, **+ Add**, **- Remove** updates the screen — all from
compiled C. Re-run **step 1** whenever you change the JSX, then `idf.py flash`.

## Display / touch notes (verified on a v3 unit; tweak in `main/board.c`)

The defaults below are dialed in for the **v3 (dual-USB) CYD this was developed on**. A different unit
may need a tweak — they're all `#define`s at the top of `board.c`:

- **Banded RGB565 framebuffer.** A full 240×320 RGB565 fb (150 KB) does NOT fit the fragmented internal
  DRAM (largest block ~110 KB), so the backend renders in **horizontal strips through a ~19 KB RGB565
  band buffer** (`ER_LCD_BANDED=1`, `ER_LCD_BANDED_ROWS=40`) and lets the ST7789's GRAM retain the rest of
  the frame. Full 16-bit color (crisp text) at less RAM than the old RGB332 path. The engine repaints only
  the dirty rows, so a small change flushes one or two strips. (An earlier revision used a full RGB332 fb,
  `ER_SPI_LCD_FB8=1` — 256 colors, coarse anti-aliased text; banded RGB565 supersedes it.)
- **Pixel order — byte-swap + BGR (`ER_SPI_LCD_SWAP_BGR=1`).** This CYD's ST7789 displays a stored RGB565
  word as `BGR(byteswap(word))`: it wants the two color bytes swapped **and** red/blue in BGR order. The
  backend pre-compensates in `fb_store` when this flag is set (in the backend component's CMakeLists).
  Symptoms if it's wrong: gray renders pastel-green, blue renders pink, navy renders brown. Pure red/white
  still look right, which is why it hid in the old RGB332 build (mostly gray/white). A standard RGB565
  panel should leave this unset.
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
**fetched from GitHub** — but Flow B pulls in only the engine and SPI backend (no QuickJS). Copy this
folder out and `idf.py build` pulls what it needs. The AOT `app.gen.c` is generated separately (step 1).

**Alternative: the engine from the Component Registry.** Because Flow B needs only the engine (the app is
compiled C), you can pull it as a managed component instead of `FetchContent`:

```bash
idf.py add-dependency "TheMasterCoder007/embedded-react^0.3.0"
```

Then drop `app.gen.c` / `app.gen.h` into `main/` and provide a backend. Note the SPI backend
(`backends/esp32-spi-lcd`) is **not** on the registry — this example compiles it from the fetched/local
source via its component shim; with the registry route you'd vendor or fetch the backend yourself. This is
why the example defaults to `FetchContent` (one mechanism for both engine and backend).
