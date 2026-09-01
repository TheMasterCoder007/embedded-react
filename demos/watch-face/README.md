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

## Two pagers: hand-rolled vs `PanResponder`

The demo ships the swipe **twice**, and the pair is the point:

| | Entry | Swipe | Runs on |
|---|---|---|---|
| **`App.jsx`** (stock) | `index.jsx` | hand-rolled: records the touch-down `x` in a ref and subtracts it on every `onTouchMove` | Flow A **and** Flow B (AOT) — this is what the RP2040 compiles |
| **`App.pan.jsx`** | `index.pan.jsx` | `PanResponder` — the gesture arrives with travel (`g.dx`) and speed (`g.vx`) already worked out | **Flow A only** |

```bash
npm run dev:pan      # simulator, PanResponder variant
npm run build:pan    # Flow A → dist/app.erpkg (PSRAM-class board)
```

`App.pan.jsx` **cannot** be AOT-compiled: `PanResponder` is a JS module, and `{...pan.panHandlers}`
fails with *"AOT: a spread `{...}` on `<View>` is not supported"*. That is exactly why the stock
`App.jsx` stays hand-rolled — the RP2040 has 264 KB of SRAM and no room for QuickJS, so its app must
compile to C. The two share everything but the pager shell: `App.jsx` exports the pages, the styles,
and the page geometry, and `App.pan.jsx` imports them. (Those exports are invisible to the AOT — the
generated C is byte-identical with and without them.)

Two things the variant gets for free, which the raw-touch version cannot do:

- **Flick to turn the page.** A plain `onTouchMove` carries a point and no speed, so `App.jsx` can only
  latch on distance (drag 58 px). `App.pan.jsx` also latches on `g.vx`, so a short fast flick turns the
  page. Note the RN-inherited issue: velocity is measured at the last *move*, so a drag that stops dead
  and rests before the finger lifts still reads as a flick.
- **A canceled gesture doesn't wedge it.** `App.jsx` has no `onTouchCancel`, so if the engine abandons
  the sequence, its `dragging` flag stays latched at 1 and the settle animation freezes mid-drag.
  `onPanResponderTerminate` clears it.

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
