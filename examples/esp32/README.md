# examples/esp32 — ESP32-S3 host (Flow A – QuickJS)

Runs the embedded-react Flow A stack on an **ESP32-S3**: QuickJS interprets the precompiled
bytecode bundle, the C engine lays out and paints, and an LCD backend pushes pixels.

**Target board:** Waveshare **ESP32-S3-Touch-LCD-7** — 7" **800×480 RGB-parallel** IPS panel,
8 MB octal PSRAM, 16 MB flash, **GT911** capacitive touch (I²C), and a **CH422G** I²C IO-expander
that drives the LCD reset / backlight / touch-reset lines.

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
  main.c                  host: PSRAM JS heap, bytecode load, frame loop + present + touch
  board.c                 Waveshare 7" bring-up: CH422G expander + RGB panel + GT911 touch
  board.h                 board resolution + board_display_init() + board_touch_*()
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
- **GT911 touch.** Shares the panel's I²C bus (SDA=8/SCL=9); INT=GPIO4, RST=CH422G EXIO1. The reset
  sequence in `board.c` holds INT low across the RST rising edge to latch address **0x5D**. Point
  coordinates start at reg `0x8150` as `[xLo, xHi, yLo, yHi]` — the track-id is the *prior* register
  (`0x814F`), so reading from `0x8150` gives x/y directly (an off-by-one here yields wild coords). If
  taps land in the wrong place, dump the raw point bytes and check for an axis swap/mirror.
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

## Performance

The S3 runs the CPU at 240 MHz but the JS heap, engine pools, and framebuffers all live in 80 MHz
PSRAM, so the whole pipeline is memory-bound. What makes it smooth (~45 fps animation, tear-free):

- **Damage-clipped rendering** (engine). `er_commit()` scissors the paint walk to just the screen
  rects that actually changed — each changed/moved node's new rect ∪ the rect it was painted at last
  frame (so a moving box's trail is erased), plus any removed node's footprint. Both the software
  compositing and the panel flush shrink from 800×480 to the changed region. A full repaint only
  happens on the first frame or when the framebuffer is invalidated.
- **Identical-props gate** (engine). React re-renders allocate fresh inline-style objects, so the
  reconciler re-commits every node even when nothing changed. `er_node_set_props` hashes the incoming
  props and skips the layout/repaint invalidation when they're byte-identical — one leaf update no
  longer drags the whole screen into a repaint.
- **Double-buffered, tear-free flush** (`backends/esp32-lcd`). With `num_fbs=2` the backend converts
  the changed region straight into the **off-screen** framebuffer and lets `draw_bitmap` swap it in at
  vsync — the displayed buffer is never written mid-scanout. Because each buffer is two presents
  stale, the present pushes the union of this frame's and last frame's damage.
- **Bigger CPU caches** (`sdkconfig.defaults`). 64 KB data + 32 KB instruction cache (64 B lines) cut
  the PSRAM miss/stall rate ~2.4× across pump, layout, and flush. (PSRAM stays at 80 MHz — 120 MHz
  octal needs the flash at 120 MHz too, which this board isn't rated for.)
- **App patterns** (`bridges/quickjs/js/app/App.jsx`). The once-a-second uptime counter is its own
  leaf component (its `setState` re-renders only itself, not the whole tree) and static styles use
  `StyleSheet.create` (stable identity → the reconciler skips unchanged nodes). Standard React
  performance hygiene, and it matters a lot more when each JS op is a PSRAM round-trip.

## Possible next steps

1. Multitouch / gestures (the engine has `er_responder_query_set`), 
2. Asset/font loading (`loadImage`/`loadFont`, needs PNG decode), 
3. Shrinking the per-frame opacity-scratch clear, or factoring `board.c` into a reusable BSP so other ESP32 boards
can drop in.
