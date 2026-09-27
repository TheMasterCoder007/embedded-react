---
title: 'The demo apps'
sidebar_label: 'Demo apps'
description: 'The thermostat and the watch face: what each exercises, how it is put together, and what it took to keep both compiling under Flow B.'
---

Two complete apps live in the repository's `demos/` folder, written against the public API exactly
as your app would be. They are the `create-embedded-react` templates, the apps the board examples
flash, and the inputs to the parity harness, so each compiles through both flows from one source.

```bash
npm create embedded-react@latest my-thermostat -- --template thermostat
npm create embedded-react@latest my-watch -- --template watch-face
```

A scaffolded copy has `npm run dev` (the browser simulator), `npm run dev:device` (hot reload on a
board), `npm run build` (Flow A, `dist/app.erpkg`) and `npm run build:aot` (Flow B, `app.gen.c` at
the panel size the demo targets). Inside the repository, `npm run sim -- thermostat` and
`npm run pack -- …` from `bridges/quickjs/js` build a demo against the in-repo library with no
install. `npm run aot` does too, but reads the panel size from the environment and otherwise assumes
800×480, where the thermostat does not compile:
`ER_AOT_SCREEN_W=240 ER_AOT_SCREEN_H=320 npm run aot -- thermostat`.

## Thermostat

A climate control around a 240° dial: touch the ring to set the target, with HEAT, COOL, AUTO, and
OFF modes (AUTO is a two-handle low/high band), a settings sheet for theme, units, and the clock, and
a 14-day weather panel. It exercises the native `<Dial>`, baked images, `<Modal>` and responsive
layout.

**One component, three layouts**, chosen from `screen.width`/`screen.height`, so one source runs
from a 1280×800 tablet down to the 240×320 panel on a no-PSRAM ESP32:

| Layout  | When                            | Contents                                                                          |
| ------- | ------------------------------- | --------------------------------------------------------------------------------- |
| `split` | ≥ 760 px wide and landscape     | dial and weather, side by side                                                    |
| `stack` | ≥ 600 px tall and ≥ 330 px wide | the two cards stacked                                                             |
| `solo`  | anything smaller                | the dial alone; buttons in a row on a portrait panel, a column on a landscape one |

**How it is put together:**

- `App.jsx` picks the layout and owns the model. COOL and HEAT each keep their own setpoint; AUTO's
  pair is held 4 °F apart by pushing the far end along.
- `components/dial.jsx` is one `<Dial>` node. The engine tracks the finger, quantises to `step`,
  latches the nearer setpoint in AUTO, and repaints only what moved. The centre number is updated
  imperatively with `updateText`, so a move costs no React work; state is committed on release.
- `components/weather.jsx` is static, so it never re-renders during a drag.
- `components/clock.jsx` and `components/network.jsx` (Flow A only) are the clock and the WI-FI and
  TIME ZONE pages. The latter drive the `__erWifi` and `__erClock` objects the
  [ESP32-S3 WiFi build](./boards/esp32-s3.md#wifi) installs, and appear only where those exist.

**Where the two flows differ.** `split` and `stack` run under Flow A. `solo` also compiles ahead of
time, which keeps that branch simpler:

- The settings sheet offers units only. A live theme switch would require every colour in the tree to
  be a ternary of literals, so the theme is baked at build time.
- AUTO's pair renders as one `<Text>` (`59°-76°`) rather than two tappable numbers: a `<Text>` lowers
  to a single `snprintf`, so that is one node against four, and the CYD has little node headroom (40
  of 44).
- The centre readout is state-driven rather than imperative. The drag itself is native in both
  flows.

The dial, its band, and its conic gradient are not differences; `<Dial>` lowers to the same engine
node in both flows.

## Watch face

A two-page swipe pager for a small portrait display, built for the
[RP2040-Touch-LCD-1.69](./boards/rp2040.md) (240×280): a watch face with a 12-hour clock, date,
battery, and HEART and STEPS cards; and a bubble level whose dot rolls with the board's tilt. Swipe
right-to-left, or flick, to change page. With no RTC on the board, the clock starts at 12:00 AM and
counts from a 1 Hz `setInterval`. **Steps and the level are real** on the RP2040: they read
`useHostValue(0)`, which the board's IMU writes through the generated `er_app_set_steps`,
`er_app_set_dotx` and `er_app_set_doty` setters. In the simulator they stay at their initial values.

**The swipe** is a `PanResponder` on a transparent overlay, driving a dynamic `marginLeft` on a
480-wide track, with a ~30 fps interval easing to the settled page on release. It works in both
flows: the AOT lowers `PanResponder.create` and the `{...pan.panHandlers}` spread onto the engine's
C gesture responder, so the RP2040 gets it with no JavaScript. Release commits on a long enough drag
(58 px) or a fast enough throw (`g.vx` ≥ 0.4 px/ms); `onPanResponderTerminate` clears the drag flag
if the panel driver abandons the touch; and `g.dx` is travel since the grant, so nothing records a
touch-down position by hand. `PanResponder.create` runs once, in a ref, so its callbacks close over
the first render's state, hence the `pageRef` mirror.

**The AOT house rules** the demo is written to:

- Components live in `App.jsx`: the compiler inlines same-file components and does not resolve
  local `.jsx` imports.
- Pure `View` and `Text`, no `<Svg>`, so the RP2040 build trims the vector pools to the floor.
- `left` and `top` cannot be state-driven, while margins can, which is why the level dot and the
  swipe track move with `marginLeft`/`marginTop`.
- Module-scope constants fold to literals; `Math.*` is only available inside dynamic expressions,
  so the clock digits are computed inline from the `t` state.
- A handler can call a helper as a statement but not read one mid-expression, which is why the
  clamp is written out at both use sites rather than factored into a `clamp()`.
- Font sizes come from the built-in font's baked set; the clock uses the largest, 48.

[The AOT subset](./aot-subset.md) is the full list of rules.

## Adding a demo

```bash
cd bridges/quickjs/js
npm run create -- my-app     # creates demos/my-app, wired to the in-repo build tools
cd ../../../demos/my-app
npm run sim
```

To ship it as a `create-embedded-react` template too, give it the consumer-form `package.json` the
other demos have and re-run `npm run sync-templates` in `create-embedded-react/`.
