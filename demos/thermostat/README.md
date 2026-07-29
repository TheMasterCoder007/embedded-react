# thermostat

A climate control built around a **240° arc dial** — a solid arc that fills from the bottom of the range
up to the setpoint, with a radial handle riding its leading edge. Touch anywhere on
the ring to set the target. **HEAT / COOL / AUTO / OFF** each carries their own accent, and AUTO splits the
readout into a low/high pair with two handles and a warm→cool ramp between them. A settings sheet switches
theme and units.

One component, three layouts, chosen from the panel size:

| Layout  | When                                 | Contents                              |
| ------- | ------------------------------------ | ------------------------------------- |
| `split` | ≥ 760 px wide and landscape          | dial + 14-day weather, side by side   |
| `stack` | ≥ 600 px tall and ≥ 330 px wide      | the two cards stacked                 |
| `solo`  | anything smaller (e.g. 240×320)      | the dial alone                        |

Every dimension derives from the host-injected `screen` global, so one source flexes from a 1280×800 wall
tablet down to the 240×320 panel on a no-PSRAM ESP32.

It exercises four engine features: the **vector** dial (stroked `<Arc>` coils and `<Line>` handles, moved
imperatively during a drag), **baked images** (the weather icons — imported PNGs, baked at build time), a
**`<Modal>`** settings sheet, and the **responsive layout**.

This is a complete `embedded-react` app — the same JSX you'd write as a downstream user. It imports from
`react` (hooks) and `'embedded-react'` (everything else), exactly like a React Native screen.

## Start from this demo

Scaffold your own copy with the toolchain — no repo checkout required:

```bash
npm create embedded-react@latest my-thermostat -- --template thermostat
cd my-thermostat
npm install
npm run dev          # WASM simulator with hot reload → http://localhost:3333
```

## Develop

```bash
npm install
npm run dev          # WASM simulator with hot reload → http://localhost:3333
npm run dev:device   # hot-reload on a real board over USB (pass -- <port> for non-ESP32 boards)
```

Edit `App.jsx` and save — the simulator hot-reloads and your `useState` is preserved. The browser's device
toolbar drives the panel size (e.g., 1280×800, 480×800, or 240×320), pixel-accurate to a real display.

Run `npm run dev` **from this folder**. The dev server scopes its `useState`→persist transform to the
project root, so starting it from the repo root would rewrite the library's own hooks too.

## Build for a device

```bash
npm run build        # Flow A → dist/app.erpkg   (QuickJS bytecode + baked assets; PSRAM-class chips)
npm run build:aot    # Flow B → app.gen.c        (compiled to C; no-PSRAM boards, baked at 240×320)
```

Flow A uploads `app.erpkg` to the device's config region (no reflash); Flow B compiles into firmware. See
the [embedded-react repo](https://github.com/TheMasterCoder007/embedded-react) for board wiring and the
on-device examples.

## How it is put together

- **`App.jsx`** picks the layout, owns the model, and contains the whole `solo` branch inline. COOL and
  HEAT each keep their own setpoint, so switching between them restores what you last set; AUTO has its
  own low/high pair, held 4 °F apart by pushing the far end along rather than blocking.
- **`components/dial.jsx`** is the rich Flow A dial. A drag repaints the coils, the handle, and the centre
  number *imperatively* (`updateVector` / `updateText`) and commits to React state only on release, so the
  app never reconciles mid-drag. The ring is a static full-sweep track plus ONE filled arc (AUTO
  subdivides into 8 to blend amber→cool, since imperative shapes carry solid paints only), and a move
  repaints just the sector the arc swept.
- **`components/weather.jsx`** is the current conditions plus a scrolling 14-day outlook, each row a baked
  `<Image>` icon and a hi/lo range bar. Static, so it never re-renders during a drag.

### Where the two flows differ

`split` and `stack` run the real JS engine (Flow A). `solo` compiles ahead of time to C for boards with no
JS runtime (Flow B), which constrains it to the AOT subset. That branch is therefore deliberately simpler:

- The settings sheet offers **units only**. A live theme switch would require every color in the tree to
  be a ternary of literals, so the theme is baked at build time.

A few constraints that are worth knowing if you edit this demo:

- Module constants in the `solo` path must fold with `+ - * /` and `?:` only — the AOT's static evaluator
  has no `Math.min/max/floor`.
- `<Svg>` flattens its children **one level deep**. Two sibling `.map()` calls produce an array of arrays
  and every shape is silently dropped; build one flat array instead.
- The center readout is bounded on both axes, so it never lies across the ring — it sits above the dial in
  hit-test order, so anywhere it covers is a place the drag cannot start.
- Text can only use glyphs the built-in font bakes (printable ASCII plus a fixed symbol set), which is why
  the close button is `×` and the status separator is `•`.

## Assets

`assets/` holds the weather-icon PNGs and the settings cog. `App.jsx` and `components/weather.jsx` import
them (`import wxSun from './assets/wx_sun.png'`) and the build bakes each into the app artifact
(premultiplied ARGB8888, flash-resident, registered at boot) — no separate step, no committed generated
files. Drop a new PNG in `assets/`, import it, and rebuild.

The icons are raster, so one set serves both themes; their gray is the midpoint of the design's dark and
light cloud colors rather than either one.
