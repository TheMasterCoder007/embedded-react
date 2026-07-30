# watch-face

A **two-page swipe pager** for a small portrait touch display — built for the
[Waveshare RP2040-Touch-LCD-1.69](../../examples/rp2040/rp2040-touch-lcd-1.69) (240×280), and
runnable on any embedded-react target.

- **Page 0 — watch face**: weekday + date and a battery reading up top, a big 12-hour clock with an
  AM/PM label and a live seconds pill, and HEART / STEPS cards along the bottom.
- **Page 1 — bubble level**: a target with a crosshair and a dot that rolls with the board's tilt,
  turning green when it settles level (centered).

**Swipe right-to-left** to drag the level page over; release, and it eases to settle as the active page
(swipe back to return). The pager uses only AOT-supported primitives: a dynamic `marginLeft` on a
480-wide track follows the finger during the drag (a transparent overlay records the touch-down x and
derives the drag from `onTouchMove`'s `e.x`), and a ~30 fps interval eases it to the settled page on
release.

There is no RTC on the target board, so time is a second counter that **starts at 12:00 AM** — the
factory default of a device whose clock has never been set (with a matching unset weekday/date,
Wed · Jan 1 2020) — and is advanced by a 1 Hz `setInterval`. The heart rate is a small pseudo-random
walk in 61–68 bpm (deterministic, seeded off its own previous reading — there is no `Math.random` in
the AOT subset, and the interval closure can't see the live time).
**Steps and the level are real** on the RP2040 example: they read `useHostValue(0)`, host-fed values
the board's IMU writes each frame — the pedometer via `er_app_set_steps`, and the accelerometer tilt
(gravity direction) via `er_app_set_dotx` / `er_app_set_doty`. In the simulator (no IMU) `useHostValue`
just returns its initial, so those stay centered/at 0.

## Start from this demo

Scaffold your own copy with the toolchain — no repo checkout required:

```bash
npm create embedded-react@latest my-watch -- --template watch-face
cd my-watch
npm install
npm run dev          # WASM simulator with hot reload → http://localhost:3333
```

## Develop

```bash
npm install
npm run dev          # WASM simulator with hot reload → http://localhost:3333
npm run dev:device   # hot-reload on a real board over USB (pass -- <port> for non-ESP32 boards)
```

The browser's device toolbar drives the panel size — set it to 240×280 to match the target board.

## Build for a device

```bash
npm run build        # Flow A → dist/app.erpkg   (QuickJS bytecode; PSRAM-class chips)
npm run build:aot    # Flow B → app.gen.c        (compiled to C for the RP2040 example; targets 240×280)
```

`build:aot` bakes the 240×280 panel size in. Flash the generated C via the
[RP2040 example](../../examples/rp2040/rp2040-touch-lcd-1.69) — its `main.c` feeds the real step count
and accelerometer tilt into the app's `useHostValue` setters. For a desktop preview of the same compiled
app, build `examples/linux-aot` and run it with `ER_AOT_SCREEN_W=240 ER_AOT_SCREEN_H=280`.

## Design notes (the AOT house rules)

The demo is written to compile under **Flow B (AOT)** — the subset that runs with **no JS on the
device**:

- **Components live in `App.jsx`.** The AOT compiler inlines same-file function components
  (`StatCard`, `WatchFace`, `LevelPage`); it does not resolve local `.jsx` imports.
- **Pure `View` + `Text` — no `<Svg>`.** The whole pager is plain rounded-rect Views and Text (the
  level target/dot too), so the RP2040 example builds it with the vector rasterizer pools trimmed to
  the floor, which frees a good chunk of SRAM beside the framebuffer.
- **`useHostValue` is the host→app bridge.** Values the C host feeds (steps, the level dot offsets)
  are declared `const x = useHostValue(0)`, which the AOT lowers to a state field plus a generated
  public setter `er_app_set_x(int)`. See the RP2040 example's `main.c`.
- **Only some styles can be state-driven** — colors, opacity, sizes, and margins. That's why the level
  dot is positioned with dynamic `marginLeft`/`marginTop` (not `left`/`top`), and the swipe track with
  a dynamic `marginLeft`.
- **Module-scope consts fold to literals.** `Math.*` is only available inside dynamic expressions — the
  clock digits, AM/PM, and seconds are computed inline from the `t` state that way.
- **Font sizes come from the engine's baked Inter set** (10/12/16/20/24/32/48). The clock uses the
  largest, 48 — bump the baked font set if you want it larger.
