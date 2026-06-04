# examples/esp32 — ESP32-S3 host (Flow A)

Runs the embedded-react Flow A stack on an **ESP32-S3**: QuickJS interprets the precompiled
bytecode bundle, the C engine lays out + paints, and (Phase 2+) an LCD backend pushes pixels.

**Target board:** Waveshare **ESP32-S3-Touch-LCD-7** — 7" **800×480 RGB-parallel** IPS panel,
8 MB octal PSRAM, 16 MB flash, **GT911** capacitive touch (I²C), and a **CH422G** I²C IO-expander
that drives the LCD reset / backlight / touch-reset lines.

## Bring-up is staged

| Phase | What runs | Status |
|-------|-----------|--------|
| **1. Headless** | QuickJS in PSRAM + bytecode + engine + bridge, **no-op backend**, output over UART | ✅ done |
| **2. Display** | RGB panel (CH422G init, 800×480 timings) + ARGB8888→RGB565 dirty-rect flush | ✅ done |
| **3. Touch** | GT911 over I²C → `embedded_renderer_touch` | ⏳ next |

Phase 1 proved the **whole JS stack runs on the S3** before any panel work — `console.log` and the
first-commit dirty rect over UART meant the hard part was done and the display was a contained
problem. **Phase 2** lights the 7" panel: the React UI (navy background, live uptime clock, animated
box, buttons) renders on real hardware.

## Self-contained via fetch

This example is a **standalone template**: its dependencies are **fetched from GitHub at configure
time** (CMake `FetchContent`, the same mechanism the desktop build uses) — nothing is vendored into
`components/`. Copy `examples/esp32/` to its own repo and `idf.py build` pulls everything it needs.
It's the seed a `create-embedded-react` scaffold would emit.

```
CMakeLists.txt            top-level IDF project — FetchContent of QuickJS + embedded-react
sdkconfig.defaults        ESP32-S3, octal PSRAM, 16MB flash, big task stack
partitions.csv            roomy factory partition (~1MB bytecode in the image)
main/
  main.c                  host: PSRAM JS heap, bytecode load, frame loop + present
  board.c                 Waveshare 7" bring-up: CH422G expander + RGB panel (pins/timings)
  board.h                 board resolution + board_display_init()
  app.bundle.qbc          precompiled bytecode of THE APP — GENERATED, not committed (see Build)
components/
  quickjs/                CMakeLists only — compiles the fetched QuickJS sources
  engine/                 CMakeLists only — compiles the fetched/local engine sources
  er-bridge-quickjs/      CMakeLists only — compiles the fetched/local native_ui_bridge.c
  esp32-lcd-backend/      CMakeLists only — compiles the fetched/local esp32-lcd renderer backend
```

What the top-level `CMakeLists.txt` fetches:

- **QuickJS-ng** — *always* fetched, **pinned to `v0.15.0`**. The pin matters: `JS_ReadObject` is
  bytecode-version-specific, so this must match the version that produced `main/app.bundle.qbc`.
- **embedded-react (engine + bridge)** — uses the **local monorepo checkout when building in-tree**
  (so development picks up your live edits with no sync step), otherwise **fetched from GitHub**
  (`github.com/TheMasterCoder007/react-embedded`, branch `master`). The components reference whichever
  source the top CMakeLists resolved.

The fetched sources land in the gitignored `build/` dir — they're build inputs, never committed.
**Nothing generated is committed** — including `main/app.bundle.qbc` (the precompiled app bytecode),
which you generate from the JS source before building (next section).

## Build

Prerequisites: **ESP-IDF v5.x** installed and exported (`. $IDF_PATH/export.sh`).

**1. Generate the app bytecode** (`main/app.bundle.qbc`) — it's not committed (it's a build artifact
of the JS app). From the repo root:

```bash
cd bridges/quickjs/js && npm run build                  # → dist/app.bundle.js
cmake --build ../../bridges/quickjs/build --target er-bridge-quickjs-compile   # build the precompiler once
bridges/quickjs/build/er-bridge-quickjs-compile \
    bridges/quickjs/js/dist/app.bundle.js \
    examples/esp32/main/app.bundle.qbc
```

(If you skip this, the build stops with a message telling you to do it.)

**2. Build + flash:**

```bash
cd examples/esp32
idf.py set-target esp32s3
idf.py build flash monitor      # picks the right port automatically, or pass -p <PORT>
```

Expected monitor output (roughly):

```
I (xxx) embedded-react: embedded-react ESP32-S3 host — Phase 2 (RGB display)
I (xxx) embedded-react: PSRAM free: 8……  internal free: …
I (xxx) board: RGB panel up: 800x480
I (xxx) embedded-react: display backend active
I (xxx) embedded-react: loading 940975 bytes of bytecode
I (xxx) embedded-react: first commit painted=1 dirty=0,0 800x480
I (xxx) embedded-react: alive: 120 frames, …
```

`first commit painted=1` plus a lit panel is the win: the React tree mounted, the engine laid it out
at panel size, paint walked it into the ARGB8888 framebuffer, and the backend flushed it to the LCD
as RGB565 — all from bytecode, with the heap in PSRAM. If the panel fails to init, the host logs a
warning and falls back to the headless no-op backend so the JS stack still runs.

Re-run **step 1** (regenerate `app.bundle.qbc`) whenever you change the JS app. The bytecode **must**
come from the same QuickJS version the top-level `CMakeLists.txt` fetches (`v0.15.0`) — the precompiler
links exactly that version, so it's automatic as long as both come from this repo.

## Known tuning points / gotchas

- **Host must be present.** The engine never auto-presents — `er_commit()` only paints into the backend
  framebuffer via fill/copy/blend. `main.c` calls `er_esp32_lcd_present()` after each commit to flush
  the painted (dirty) region to the panel. Forget it, and the screen stays black despite a working
  panel.
- **RGB panel timings.** Porches/PCLK live in `board.c`. If the image is shifted or rolling, tune the
  `hsync_*`/`vsync_*` porches and `pclk_hz`. The data-pin → color-channel map there is the Waveshare
  schematic; a wrong byte order shows as swapped red/blue.
- **CH422G expander.** The backlight/DISP, LCD-reset and touch-reset lines hang off the CH422G I²C
  IO expander (hand-driven in `board.c`, no maintained standalone IDF component). If the backlight
  stays off, the config byte / output mask there is what to check.
- **Task stack.** QuickJS + the React reconciler recurse deeply. The main task stack is bumped to
  64 KB (`CONFIG_ESP_MAIN_TASK_STACK_SIZE`) and `JS_SetMaxStackSize` to 48 KB. If you see a JS
  "stack overflow" or a FreeRTOS stack-overflow panic, raise both together.
- **PSRAM mode.** Defaults assume **octal** PSRAM at 80 MHz (this board). A quad-PSRAM board needs
  `CONFIG_SPIRAM_MODE_QUAD`.
- **Boot bytecode parse.** `JS_ReadObject` of ~940 KB takes a moment on first boot; that's the
  bytecode loader, not a hang.
- **First configure downloads deps.** `idf.py set-target` / first `build` runs FetchContent, which
  `git clone`s QuickJS-ng (and, when copied out, embedded-react) — so it needs network + `git` once.
  They cache in `build/`; later builds are offline. `idf.py fullclean` forces a re-fetch.
- **QuickJS pin vs bytecode.** The fetched QuickJS tag (`v0.15.0`, in the top `CMakeLists.txt`) must
  match `bridges/quickjs/CMakeLists.txt` and the version that compiled `app.bundle.qbc`. Bump all
  three together if you ever change it, or the bytecode won't load.

## How Phase 2 works (display)

- **`board.c`** (board-specific) brings up the Waveshare 7": the CH422G I²C expander releases LCD
  reset and enables the DISP/backlight line, then `esp_lcd_new_rgb_panel` configures the 800×480
  16-bit RGB panel (data/sync pins + porches per the Waveshare schematic) with its framebuffer in
  PSRAM. It returns an `esp_lcd_panel_handle_t`.
- **`backends/esp32-lcd/renderer_backend.c`** (board-agnostic) keeps an ARGB8888 framebuffer in PSRAM
  that the engine composites into via fill/copy/blend, tracking a dirty bounding box. `er_esp32_lcd_present()`
  converts just that dirty region to RGB565 and pushes it to the panel with `esp_lcd_panel_draw_bitmap`.
- **`main.c`** wires them together (`board_display_init` → `er_esp32_lcd_backend_init`) and calls
  `er_esp32_lcd_present()` after every `er_commit()`.

## Next (Phase 3 — touch)

Bring up the **GT911** capacitive controller over the shared I²C bus (its reset is the CH422G
`TP_RST` line, already de-asserted in `board.c`), poll touch points, and feed them to the engine via
`embedded_renderer_touch` so the on-screen buttons become interactive.
