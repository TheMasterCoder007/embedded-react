# RP2040-Touch-LCD-1.69 — round-corner watch example

Runs an embedded-react app on the **Waveshare RP2040-Touch-LCD-1.69** — an RP2040 board with a 1.69"
240×280 rounded-rectangle touchscreen. This is the smallest target in the example family: **264 KB
SRAM, no FPU, no PSRAM.**

Because there's no room for a JavaScript engine, this example uses **Flow B (AOT)**: your JSX is
compiled **ahead-of-time to C** on your computer and linked straight into the firmware. There's **no
QuickJS and no JavaScript on the device** — the compiled C app *is* the firmware.

```
App.jsx ──(npm run aot)──▶ app.gen.c ──▶ C engine ──▶ pico-spi-lcd backend ──▶ ST7789V2 screen
                                                                                     ▲
                                                                              CST816S touch
```

**Board:** RP2040, 1.69" **ST7789V2** panel (240×280, portrait, +20-row GRAM offset), **CST816S**
capacitive touch, 4 MB flash, no PSRAM. The demo it builds is the **watch-face** (a digital clock with
day/date, battery, a heart-rate card, and a live IMU step counter, plus a swipe-in bubble-level page).

> Have the **round 1.28"** sibling board (RP2040-Touch-LCD-1.28, GC9A01A 240×240)? Same pins and
> touch chip — swap the panel init to GC9A01A, drop the +20-row offset, and build the demo at
> `240×240` instead. See [Tuning for your board](#tuning-for-your-board).

## Quick start

You need the **Raspberry Pi Pico SDK**, the **Arm GNU toolchain** (`arm-none-eabi-gcc`), CMake, and
Ninja (or Make). Point the build at the SDK with `PICO_SDK_PATH`, or let it fetch its own copy with
`-DPICO_SDK_FETCH_FROM_GIT=ON`.

**1. Compile the app to C.** Run from the repo root. The `240×280` size selects the demo's layout:

```bash
cd bridges/quickjs/js
ER_AOT_SCREEN_W=240 ER_AOT_SCREEN_H=280 npm run aot -- watch-face   # writes dist/app.gen.c
cd ../../..
```

**2. Build the firmware.**

```bash
cd examples/rp2040/rp2040-touch-lcd-1.69
PICO_SDK_PATH=/path/to/pico-sdk cmake -S . -B build -G Ninja
cmake --build build                                                # → build/embedded-react-watch.uf2
```

**3. Flash it.** Two ways:

- **BOOTSEL drag-and-drop:** hold the board's **BOOT** button while plugging in USB; it mounts as an
  `RPI-RP2` drive. Copy `build/embedded-react-watch.uf2` onto it.
- **picotool** (no button press if the current firmware supports it):

  ```bash
  picotool load -f -x build/embedded-react-watch.uf2
  ```

**That's it.** The watch face appears: a big digital clock with weekday/date + battery up top and
heart-rate + live step-counter cards along the bottom. Walk with the board and the STEPS card counts
up (from the IMU). **Swipe right-to-left** to drag over the bubble-level page — tilt the board, and the
dot rolls toward the low side (green when level). Whenever you edit the JSX, re-run **step 1**, rebuild,
and reflash.

## How it works

- **`main.c`** boots the panel + touch + IMU (`board.c`), registers the render backend, builds the AOT
  app once (`er_app_build`), then runs the frame loop: poll touch → poll IMU/pedometer → `er_commit()`
  → present → tick. The 1 Hz clock "movement" is driven by `er_app_tick()`, which advances the app's
  `setInterval`.
- **`board.c`** owns the hardware: the ST7789V2 init sequence + windowing (with the +20-row GRAM
  offset) on SPI1, PWM backlight on GP25, the CST816S touch controller on I2C1, and the **QMI8658 IMU**
  (also on I2C1) — its accelerometer drives both the step counter and the bubble level. Touch is
  **INT-gated** — the controller auto-sleeps and NAKs I2C when idle, so a GPIO-edge ISR on the INT line
  tells the poll loop when a report is actually pending.
- **Real sensors → the UI via a small host→app bridge.** `pedometer.c` runs an accelerometer peak
  detector (gravity baseline + band-passed footfall peaks with hysteresis and refractory) for the step
  count; the level page maps a low-passed gravity vector to a dot offset. Both reach the app through
  **`useHostValue`**: the demo declares `const steps = useHostValue(0)` (and `dotx`/`doty`), which the
  AOT compiler lowers to a state field plus a generated public setter (**`er_app_set_steps(int)`**,
  `er_app_set_dotx/doty`); `main.c` calls them each frame when the value changes. (`useHostValue` is the
  general Flow B mechanism for feeding host/sensor data into a compiled app — no engine changes, no
  QuickJS.)
- **The swipe pager** needs no engine features beyond the gesture responder the engine already has: a
  480-wide track holds both pages and a dynamic `marginLeft` (state-driven margins are allowed) follows
  the finger, then a ~30 fps interval eases it to the settled page on release. A transparent
  full-screen overlay captures the swipe with a `PanResponder`, which the AOT lowers onto the engine's
  C responder negotiation — so travel and flick speed arrive worked out, with no JS on the board.
- The **render backend** (`backends/pico-spi-lcd`) keeps one 240×280 RGB565 framebuffer in SRAM
  (131 KB), composites the engine's fills/blits into it, and on present streams only the dirty
  rectangle to the panel.
- **`CMakeLists.txt`** compiles the engine as a static lib with RAM-tuned `ERUI_*` flags (shadows /
  gradients / keyboard off; node + vector pools sized to the watch face) so everything fits beside
  the framebuffer in 264 KB.

You should see this on the USB serial log at boot:

```
embedded-react RP2040-Touch-LCD-1.69 host — Flow B (AOT, no QuickJS)
board: ST7789V2 panel up (240x280, SPI 62.5 MHz)
board: CST816 touch ready (chip id 0xB5; ...)
board: QMI8658 IMU ready (addr 0x6B, accel ±4g @125Hz)
AOT app built at 240x280 (no QuickJS)
```

Build with `-DER_BOARD_DEBUG=1` for a 1 Hz heartbeat that logs the live accel sample and the running
step count — handy for confirming the IMU responds and tuning the pedometer thresholds in `pedometer.c`.

## Tuning for your board

| Symptom | Fix (in `board.c`) |
|---|---|
| Image shifted vertically / garbage strip at an edge | wrong `LCD_Y_OFFSET` — the 280 rows start at GRAM row 20 on this panel; a round 240×240 GC9A01A needs `0`. |
| Colors look photo-negative | toggle the `0x21` (inversion on) command in `lcd_init_regs()`. |
| Red/blue swapped | flip the RGB/BGR bit in MADCTL (`0x36`: `0x00` ↔ `0x08`). |
| Image mirrored / rotated | adjust the MADCTL MX/MY/MV bits in `lcd_init_regs()`. |
| Touch dead or wrong location | check the CST816S is detected in the boot log; the panel reports pixel coords directly (no calibration). |
| Steps never count / count too easily | tune `PEDO_HI` / `PEDO_LO` / `PEDO_MIN_STEP_MS` in `pedometer.c`; watch the `-DER_BOARD_DEBUG=1` heartbeat's `steps=` output while walking. |
| Sparkle / tearing / wrong pixels | drop `LCD_SPI_HZ` in `board.c` from `62500*1000` back to `31250*1000` — not every panel unit likes 62.5 MHz. |
| Round 1.28" GC9A01A board instead | replace `lcd_init_regs()` with the GC9A01A sequence, set `LCD_Y_OFFSET 0` + `BOARD_LCD_HEIGHT 240`, and AOT-compile the demo at `240×240`. |

## Bring-up debugging

Build with `-DER_BOARD_DEBUG=1` to wait briefly for a USB host at boot and print a 1 Hz heartbeat
(uptime and latest touch poll). `-DER_BOARD_DEBUG=2` also holds a **color-bar test pattern** for 3 s at
boot (`board_lcd_test_pattern()`) — four full-width R/G/B/W bands plus a centered black square. Clean
bands prove the panel driver (init + windowing + pixel streaming) end to end; anything upstream is
then the engine or the app. The default build (`0`) compiles all of this out.

## Pinout

| Function | RP2040 GPIO |
|---|---|
| LCD SCLK / MOSI | GP10 / GP11 (SPI1) |
| LCD DC / CS / RST | GP8 / GP9 / GP13 |
| LCD backlight (PWM) | GP25 |
| Touch SDA / SCL | GP6 / GP7 (I2C1) |
| Touch INT / RST | GP21 / GP22 |
| IMU (QMI8658) | GP6 / GP7 (I2C1, addr 0x6B) |
