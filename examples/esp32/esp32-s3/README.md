# examples/esp32/esp32-s3 — ESP32-S3 host (Flow A – QuickJS)

Runs the embedded-react Flow A stack on an **ESP32-S3** using the **uploaded-config model**: the
firmware brings up the panel + QuickJS, then loads its UI + logic as a **config container** (`app.erpkg`)
from a dedicated flash partition — memory-mapped and verified (CRC + QuickJS version), **not embedded
in the firmware image**. QuickJS interprets the container's bytecode, the C engine lays out and paints,
and an LCD backend pushes pixels. The config carries its own images and fonts. This is the on-device peer
of the desktop demo (`examples/linux`): firmware and config ship and update independently — reflash a
new UI by writing one partition, no firmware rebuild.

**Target board:** Waveshare **ESP32-S3-Touch-LCD-7** — 7" **800×480 RGB-parallel** IPS panel,
8 MB octal PSRAM, 16 MB flash, **GT911** capacitive touch (I²C), and a **CH422G** I²C IO-expander
that drives the LCD reset / backlight / touch-reset lines.

## Self-contained via fetch

This example is a **standalone template**: its dependencies are **fetched from GitHub at configure
time** (CMake `FetchContent`, the same mechanism the desktop build uses) — nothing is vendored into
`components/`. Copy `examples/esp32/esp32-s3/` to its own repo and `idf.py build` pulls everything it needs.
It's the seed a `create-embedded-react` scaffold would emit.

**This is the canonical Flow A template** — Flow A uses `FetchContent`, *not* the ESP-IDF Component
Registry. The registry ships only the engine (which is all Flow B needs). Flow A additionally needs
QuickJS-ng, a plain CMake project that isn't an ESP-IDF component, so `idf.py add-dependency` can't manage
it — `FetchContent` can, and pulls the engine + bridge + QuickJS together. (For the AOT path that *does*
use the registry, see [examples/esp32/esp32-2432s028r](../esp32-2432s028r/README.md).)

```
CMakeLists.txt            top-level IDF project — FetchContent of QuickJS + embedded-react
sdkconfig.defaults        ESP32-S3, octal PSRAM, 16MB flash, big task stack
partitions.csv            factory (firmware) + a 'config' data partition for app.erpkg
main/
  main.c                  host: PSRAM JS heap, mmap + load config from flash, frame loop + touch
  board.c                 Waveshare 7" bring-up: CH422G expander + RGB panel + GT911 touch
  board.h                 board resolution + board_display_init() + board_touch_*()
components/
  quickjs/                CMakeLists only — compiles the fetched QuickJS sources
  engine/                 CMakeLists only — compiles the fetched/local engine sources
  er-bridge-quickjs/      CMakeLists only — compiles the fetched/local bridge + er_runtime + er_assets
  esp32-lcd-backend/      CMakeLists only — compiles the fetched/local esp32-lcd renderer backend
```

What the top-level `CMakeLists.txt` fetches:

- **QuickJS-ng** — *always* fetched, **pinned to `v0.15.0`**. The pin matters: `.qbc` bytecode is
  QuickJS-version-specific. The container stamps the tag it was built with, and the loader rejects a
  config built for a different version (`config rejected: built for a different QuickJS version`) —
  so a mismatch shows a panel instead of running garbage.
- **embedded-react (engine + bridge)** — uses the **local monorepo checkout when building in-tree**
  (so development picks up your live edits with no sync step), otherwise **fetched from GitHub**
  (`github.com/TheMasterCoder007/embedded-react`, pinned to tag `v0.3.0`). The components reference
  whichever source the top CMakeLists resolved.

The fetched sources land in the gitignored `build/` dir — they're build inputs, never committed. The
firmware builds standalone (no app required at build time); the config (`app.erpkg`) is a separate
artifact you flash into the `config` partition (next section).

**Adapting to another board.** Two board-specific pieces: `main/board.c` / `board.h` bring up *this*
panel + touch (the Waveshare 7" 800×480 RGB panel, GT911, CH422G), and the render backend
(`components/esp32-lcd-backend` → `backends/esp32-lcd`) flushes pixels over an `esp_lcd` panel handle. For
another **RGB / I80 parallel** panel, keep that backend and rewrite `board.c` for your panel and touch. For
an **SPI display** (ILI9341/ST7789 etc.), swap the backend to `backends/esp32-spi-lcd` (banded RGB565, the
one the [CYD example](../esp32-2432s028r/README.md) uses). The engine itself is backend-agnostic.

## Build

Prerequisites: **ESP-IDF v6.x** installed and exported (`. $IDF_PATH/export.sh`). Validated on
v6.0.1 (xtensa GCC).

Firmware and config are **two independent artifacts**: build/flash the firmware once, then flash a
config into the `config` partition (and re-flash just the config whenever the JS app changes).

**1. Build + flash the firmware:**

```bash
cd examples/esp32/esp32-s3
idf.py set-target esp32s3
idf.py build flash monitor      # picks the right port automatically, or pass -p <PORT>
```

The firmware builds with no app — until a config is flashed, it shows a **"No config loaded"** panel.

**2. Pack a config and flash it into the `config` partition.**

From **your own app project** (a `create-embedded-react` scaffold, or any project that depends on
`embedded-react`) — this is the path a consumer uses, outside this monorepo:

```bash
npx embedded-react build           # → ./dist/app.erpkg  (bytecode + assets + version + CRC)
parttool.py write_partition --partition-name=config --input dist/app.erpkg
```

From **inside this monorepo** (packing a demo from `demos/`), the equivalent is `npm run pack`:

```bash
# `npm run pack` packs the default demo (demos/thermostat); `npm run pack -- <name>` picks another.
cd bridges/quickjs/js && npm run pack            # → dist/app.erpkg
cd ../../..                                       # back to repo root

# Write it into the 'config' partition (parttool.py ships with ESP-IDF; auto-detects the port).
parttool.py write_partition --partition-name=config \
    --input bridges/quickjs/js/dist/app.erpkg
```

Reboot (or it's already running if you flashed before boot). To deploy a new UI later, re-run **step
2** only — no firmware rebuild.

Expected monitor output (roughly):

```
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

`React mounted` plus a lit panel is the win: the container verified (CRC + QuickJS version), its assets
registered, the React tree mounted, the engine laid it out at panel size, paint walked it into the
ARGB8888 framebuffer, and the backend flushed it to the LCD as RGB565 — all from a memory-mapped config
in flash, with the JS heap in PSRAM. (The `first commit painted=` count depends on the demo: the
thermostat paints its content on the frames after the initial mount/effects settle, so the very first
commit may report `painted=0` — the steady `alive:` frames confirm it's running.) If the panel fails to
init, the host falls back to the headless no-op backend so the JS stack still runs. If the config is
missing or rejected, the panel shows why (`No config loaded` / `Couldn't load config: <reason>`).

The container stamps the QuickJS version `npm run pack` built it against; it **must** match the tag the
top-level `CMakeLists.txt` fetches (`v0.15.0`). Both come from this repo, so it's automatic — and if it
ever mismatches, the loader rejects it with a panel rather than running garbage.

## Hot reload over USB (live, no reflash)

> **Opt-in, dev only — off by default.** The standard dev workflow is the **web simulator**
> (`npx embedded-react dev`); on-device hot reload is for when you specifically want to iterate on the
> real panel. The firmware does not carry the receiver unless you build it in: enable it with
> `idf.py -DER_HOTRELOAD=1 build flash` (or flip the default in `main/CMakeLists.txt`). A release
> firmware should be built **without** it. The rest of this section assumes a board flashed with
> `ER_HOTRELOAD=1`.

Once a hot-reload-enabled firmware is flashed, you don't have to re-pack + `parttool.py` for every JS
change. Point the dev loop at the board's **native USB-C port** and it streams a fresh config on every
save; the firmware swaps it in **live** — the same edit-and-see-it experience as the WASM simulator, on
the real panel.

This board has **two USB-C ports**: the **"UART" port** (a CH343 USB-UART bridge → UART0; use it for
`idf.py flash`) and the **"USB" port** (the chip's native USB-Serial-JTAG on GPIO19/20; hot reload rides
this one — it's fast). Plug in **both**:

```bash
# from your app project (or `cd bridges/quickjs/js` in-tree)
npm i serialport                       # one-time: optional native dep, only needed for device upload
npx embedded-react dev --device        # auto-detects the ESP32 USB-Serial-JTAG (USB 303a:1001)
# …or pin it explicitly:  npx embedded-react dev --device /dev/cu.usbmodemXXXX   (COMx on Windows)
```

`--device` with no port auto-detects the board by its fixed USB VID:PID, so you don't have to hunt for the
right `usbmodem` (the CH343 flashing port and the native-USB port look alike). A `create-embedded-react`
app ships an `npm run dev:device` script that wraps this. If the board isn't found, or you point it at 
firmware built **without** hot reload, the dev loop says so (no device connected → check the cable/native
port; connected but no reload ack → rebuild with `-DER_HOTRELOAD=1`) instead of hanging.

Edit a `.jsx`/`.tsx`, save, and the **running UI stays on screen** while the new app streams in, then
swaps to the updated version. No reboot, no blank "Reloading…" flicker.
Only your **changed app code** rides the wire: the framework half (react + reconciler + the lib, ~1 MB)
stays resident on the board, so each frame is just the app slice and reloads
stay quick as your app grows. Component state written with `usePersistentState` (and plain `useState`,
which this dev mode rewrites to persist) survives the reload. The dev loop also prints the device's own
logs (prefixed `▸`) over the same native-USB port.

How it works: the boot container is split into a **vendor** section (react + reconciler + lib bytecode,
run first) and a tiny **app** section (your bytecode + assets, run last) — see the `embedded-react build`
output (`vendor … + app …`). The vendor half is stable across edits, so the dev loop only ships the app:
the host wraps each app-only `.erpkg` in a tiny `ERHR` transport frame (magic + length + CRC) and writes
it to the native USB-Serial-JTAG. A background task (`main/hotreload_usb.c`) reads the bytes into the
portable parser (`er_hotreload.c`) — into one staging buffer, while the old app keeps running. On a
complete, CRC-valid frame the frame loop applies it (`er_hotreload_apply`). Because the frame carries **no
vendor section**, the runtime takes the **soft path**: it skips the full context teardown
(`er_runtime_reset`) and re-evaluates only the app into the resident context, where `AppRegistry` re-renders
into its persistent root and React reconciles old→new in place (a frame *with* a vendor section still does
the full reset — that's how a release boot loads). Assets are copied on load (`copy=true`), so nothing
references the staging buffer afterward — which is what lets the receiver reuse it for the next upload
*without* tearing the old app down first. A corrupt upload is simply rejected and never disturbs the
running app. The whole feature is compiled out unless you opt in with `ER_HOTRELOAD=1` (see the note at
the top of this section); a default build carries none of it.

Two board-specific bits make the native USB work (both already wired up here):
- **Console on USB-Serial-JTAG** (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`, sdkconfig.defaults). The native
  USB enumerates from ROM at boot, but the app drops it unless the console is bound to it; binding also
  installs the `usb_serial_jtag` driver the receiver reads from. (Flashing still works on the UART port.)
- **CH422G `USB_SEL` (EXIO5) driven low** (`main/board.c`). The native USB D−/D+ are muxed with a CAN
  transceiver via the CH422G IO-expander; the latch powers up all-high = CAN, so the port stays dark until
  the firmware selects USB. Without this the "USB" port never enumerates on the host — the gotcha that
  makes it look like native USB "isn't wired."

Notes / limits:
- **Single staging buffer, assets copied on load.** One staging buffer (not double-buffered) holds the
  incoming frame — sized for the boot container, though hot-reload frames are far smaller.
  Because the load copies the asset pack (~the app's image/font bytes — small) into its own block, the
  running app never references the buffer, so there's no teardown and no flicker. This copy is **opt-in and
  hot-reload-only**: production boot (`er_runtime_load_container`) still references assets in place from
  mmap'd flash at zero RAM cost — unchanged.
- **Dev loop only.** Hot reload is for development; ship a release by packing once and flashing the
  `config` partition (above).

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
- **Config partition.** `partitions.csv` defines a `config` data partition (label `config`); main.c
  finds it by label and `esp_partition_mmap`s it, passing the whole partition size as an upper bound
  (the loader walks the container to find its true length, so trailing partition bytes are ignored).
  Size it ≥ your `.erpkg` (default 2 MB). If you rename/resize it, keep the label `config`.
- **First configure downloads deps.** `idf.py set-target` / first `build` runs FetchContent, which
  `git clone`s QuickJS-ng (and, when copied out, embedded-react) — so it needs network + `git` once.
  They cache in `build/`; later builds are offline. `idf.py fullclean` forces a re-fetch.
- **QuickJS pin vs config.** The fetched QuickJS tag (`v0.15.0`, in the top `CMakeLists.txt`) must
  match `bridges/quickjs/CMakeLists.txt` and the version `npm run pack` stamped into the `.erpkg`. The
  loader enforces it: a mismatch is rejected (panel) rather than crashing. Bump all together if you
  ever change it.

## Performance overlay

A built-in LVGL-style metrics panel (FPS, CPU load, free PSRAM / internal RAM) can be drawn in the
bottom-right corner for on-device diagnostics. It's gated by the `ER_PERF_OVERLAY` preprocessor
define (in `engine/include/perf_overlay.h`), **off by default** — flip it to `1` (or build with
`-DER_PERF_OVERLAY=1`) and reflash:

```c
#define ER_PERF_OVERLAY 1   // engine/include/perf_overlay.h
```

## Frame pacing

The loop paces itself with an adaptive `vTaskDelay`: it sleeps only the time remaining up to a ~20 ms
target (≈50 fps, just over the panel's ~51 Hz), so a heavy frame isn't taxed by a fixed delay and runs
as fast as the work allows. It always blocks at least one tick so the idle task is fed.

## Possible next steps

1. Multitouch / gestures (the engine has `er_responder_query_set`),
2. Factoring `board.c` into a reusable BSP so other ESP32 boards can drop in.

3. ~~On-device config upload over USB + reload~~ — **done** (see [Hot reload over USB](#hot-reload-over-usb-live-no-reflash)).

(Images and custom fonts ride inside the config container — `npm run pack` bakes the demo's imported
assets into the `.erpkg`. See the
[JS package README](../../../bridges/quickjs/js/README.md#assets-images--fonts).)
