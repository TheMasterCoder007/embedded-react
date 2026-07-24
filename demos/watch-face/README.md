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
walk in 61–68 bpm (deterministic, seeded off the time — there is no `Math.random` in the AOT subset).
**Steps and the level are real** on the RP2040 example: they read `useHostValue(0)`, host-fed values
the board's IMU writes each frame — the pedometer via `er_app_set_steps`, and the accelerometer tilt
(gravity direction) via `er_app_set_dotx` / `er_app_set_doty`. In the simulator (no IMU) `useHostValue`
just returns its initial, so those stay centered/at 0.

## Running it

```bash
# Flow B (AOT) — compile to C for the RP2040 example (240×280):
cd bridges/quickjs/js
ER_AOT_SCREEN_W=240 ER_AOT_SCREEN_H=280 npm run aot -- watch-face    # → dist/app.gen.{c,h}

# Desktop preview of the SAME compiled app (examples/linux-aot):
cd ../../../examples/linux-aot && cmake -S . -B build && cmake --build build
ER_AOT_SCREEN_W=240 ER_AOT_SCREEN_H=280 ./build/embedded-react-desktop-aot

# Or the hot-reload simulator (Flow A):
cd demos/watch-face && npm run sim
```

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
