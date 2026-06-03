# examples/esp32 — ESP32-S3 host (Flow A)

Runs the embedded-react Flow A stack on an **ESP32-S3**: QuickJS interprets the precompiled
bytecode bundle, the C engine lays out + paints, and (Phase 2+) an LCD backend pushes pixels.

**Target board:** Waveshare **ESP32-S3-Touch-LCD-7** — 7" **800×480 RGB-parallel** IPS panel,
8 MB octal PSRAM, 16 MB flash, **GT911** capacitive touch (I²C), and a **CH422G** I²C IO-expander
that drives the LCD reset / backlight / touch-reset lines.

## Bring-up is staged

| Phase | What runs | Status |
|-------|-----------|--------|
| **1. Headless** | QuickJS in PSRAM + bytecode + engine + bridge, **no-op backend**, output over UART | ✅ this commit |
| **2. Display** | RGB panel (CH422G init, 800×480 timings) + ARGB8888→RGB565 flush | ⏳ next |
| **3. Touch** | GT911 over I²C → `embedded_renderer_touch` | ⏳ after display |

Phase 1 proves the **whole JS stack runs on the S3** before any panel work — if `console.log` and
the first-commit dirty rect appear in the monitor, the hard part is done and the display is a
contained problem.

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
  main.c                  Phase-1 host: PSRAM JS heap, bytecode load, frame loop
  app.bundle.qbc          precompiled bytecode of THE APP — GENERATED, not committed (see Build)
components/
  quickjs/                CMakeLists only — compiles the fetched QuickJS sources
  engine/                 CMakeLists only — compiles the fetched/local engine sources
  er-bridge-quickjs/      CMakeLists only — compiles the fetched/local native_ui_bridge.c
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

Expected Phase-1 monitor output (roughly):

```
I (xxx) embedded-react: embedded-react ESP32-S3 host — Phase 1 (headless)
I (xxx) embedded-react: PSRAM free: 8……  internal free: …
I (xxx) embedded-react: loading 940975 bytes of bytecode
I (xxx) embedded-react: first commit painted=1 dirty=0,0 800x480
I (xxx) embedded-react: alive: 120 frames, …
```

`first commit painted=1 dirty=0,0 800x480` is the win: the React tree mounted, the engine laid it
out at panel size, and paint walked it — all from bytecode, with the heap in PSRAM.

Re-run **step 1** (regenerate `app.bundle.qbc`) whenever you change the JS app. The bytecode **must**
come from the same QuickJS version the top-level `CMakeLists.txt` fetches (`v0.15.0`) — the precompiler
links exactly that version, so it's automatic as long as both come from this repo.

## Known tuning points / gotchas (Phase 1)

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

## Next (Phase 2 — display)

Implement `backends/esp32-lcd/renderer_backend.c`: init the RGB panel via `esp_lcd_new_rgb_panel`
(800×480 timings, data/sync pins per the Waveshare schematic) with its framebuffer in PSRAM, bring
up the CH422G expander to release LCD reset + enable backlight, and on `frame_ready` convert the
engine's ARGB8888 framebuffer to the panel's RGB565 (dirty-rect only, to stay within PSRAM
bandwidth). Then point `main.c` at that backend instead of the no-op one.
