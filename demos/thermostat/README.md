# thermostat

A climate-control demo built around a draggable **270° arc dial** (a physical-thermostat metaphor).
Dragging the handle around the arc sets the target temperature; a −/+ stepper and a Heat/Cool/Auto/Off
mode selector round it out. On the wide layout a **weather panel** sits to the right — current
conditions plus a 4-day forecast, each day a baked `<Image>` weather icon.

It exercises three engine features: the **vector** dial (one `<Svg>` arc, updated imperatively during
the drag), **baked images** (the weather icons — imported PNGs, baked at build time), and the
**responsive layout**. Every dimension derives from the host-injected `screen` global, so one source
flexes between the 800×480 panel (ESP32-S3 / Flow A) and the 240×320 panel (no-PSRAM ESP32 / Flow B).
A `width < 400` breakpoint selects the compact layout (thermostat only).

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

Edit `App.jsx` and save — the simulator hot-reloads and your `useState` is preserved. The browser's
device toolbar drives the panel size (e.g. 800×480 or 240×320), pixel-accurate to a real display.

## Build for a device

```bash
npm run build        # Flow A → dist/app.erpkg   (QuickJS bytecode + baked assets; PSRAM-class chips)
npm run build:aot    # Flow B → app.gen.c        (compiled to C; no-PSRAM boards)
```

Flow A uploads `app.erpkg` to the device's config region (no reflash); Flow B compiles into firmware.
See the [embedded-react repo](https://github.com/TheMasterCoder007/embedded-react) for board wiring and
the on-device examples.

## Assets

`assets/` holds the weather-icon PNGs and the dial's baked `.svg` face. `App.jsx` imports them
(`import wxSun from './assets/wx_sun.png'`) and the build bakes each into the app artifact
(premultiplied ARGB8888, flash-resident, registered at boot) — no separate step, no committed generated
files. Drop a new PNG in `assets/`, import it, and rebuild.
