# thermostat

A climate-control demo built around a draggable **270° arc dial** (a physical-thermostat metaphor).
Dragging the handle around the arc sets the target temperature; a −/+ stepper and a Heat/Cool/Auto/Off
mode selector round it out. On the wide layout a **weather panel** sits to the right — current
conditions plus a 4-day forecast, each day a baked `<Image>` weather icon.

It exercises three engine features: the **vector** dial (one `<Svg>` arc, updated imperatively during
the drag), **baked images** (the weather icons — imported PNGs, baked at build time), and the
responsive layout.

It's the default demo and is **responsive**: every dimension derives from the host-injected `screen`
global, so one source flexes between the 800×480 panel (ESP32-S3 / Flow A) and the 240×320 panel
(no-PSRAM ESP32 / Flow B). A `width < 400` breakpoint selects the compact layout (thermostat only).

## Assets

`assets/` holds the weather-icon PNGs. `App.jsx` imports them (`import wxSun from './assets/wx_sun.png'`)
and `npm run build` bakes each into `dist/assets.generated.c` (premultiplied ARGB8888, flash-resident,
registered at boot via `er_register_assets()`) — no separate step, no committed generated files. Drop
a new PNG in `assets/`, import it, and rebuild. See [the JS package README](../../bridges/quickjs/js/README.md#assets-images--fonts)
for the full asset workflow (including fonts and `assets.config.js`).

## Build & run

From this folder:

```
npm run sim      # live-reload simulator (hot reload on save) — see tools/simulator/README.md for one-time setup
npm run build    # just bundle + bake assets -> bridges/quickjs/js/dist/app.bundle.js
```

The `sim`/`build` scripts delegate to the `embedded-react` package (`bridges/quickjs/js`); output always
lands in that package's `dist/`. After `npm run build` you can also run the desktop host
(`examples/linux`) or flash a device (`examples/esp32/esp32-s3`) — see their READMEs. The host injects
the globals the bundle expects (`NativeUI`, `screen`, `console`, timers).
