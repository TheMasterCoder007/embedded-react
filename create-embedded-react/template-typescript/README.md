# __APP_NAME__

A new [embedded-react](https://github.com/TheMasterCoder007/embedded-react) app — React Native for embedded MCUs. **TypeScript** starter.

## Develop

```bash
npm install
npm run dev          # WASM simulator with hot reload → http://localhost:3333
npm run dev:device   # hot reload on a real board over USB (pass in -- <port> for non ESP32 devices) 
npm run typecheck    # tsc --noEmit
```

Edit `App.tsx` and save — the simulator hot-reloads and your `useState` is preserved. The canvas fills the
viewport, so the browser's device toolbar drives the board size (e.g., 240×320), pixel-accurate to a panel.

`npm run dev:device` streams the same edits to a connected board instead of the browser. The port is
**auto-detected for ESP32-S3/C3 boards** (they have a fixed USB id); for any other board — STM32, RP2040,
etc. — pass it explicitly: `npm run dev:device -- <port>`. Either way the board's firmware must have
on-device hot reload enabled.

## Share

```bash
npm run export       # → sim-export/  (a self-contained static playground, no server)
npx serve sim-export # serve it locally, or deploy the folder to any static host
```

## Build for a device

```bash
npm run build        # → dist/app.erpkg  (QuickJS bytecode + baked assets)
```

Upload `dist/app.erpkg` to your device's config region — your firmware loads it with
`er_runtime_load_container()` (Flow A; needs a PSRAM-class chip). For no-PSRAM boards, compile the app
ahead-of-time to C instead: `npx embedded-react build --aot` emits `app.gen.c` (+ `assets.generated.c`)
to compile into your firmware — note the AOT path supports a subset of the API (e.g., animation
composition like `Animated.loop`/`sequence` isn't supported yet). See the
[repo](https://github.com/TheMasterCoder007/embedded-react) for board wiring and examples.

## Layout

```
index.tsx          entry — registers <App/>
App.tsx            your UI: View / Text / Pressable / Image / Animated, styled with StyleSheet
types/assets.d.ts  ambient types for asset imports (png / ttf / …)
tsconfig.json      typecheck config (the dev/build pipeline uses esbuild and ignores types)
assets/            images & fonts — import them in code and they're baked automatically
```

Types for the `embedded-react` API ship with the package itself, so `import {View} from 'embedded-react'`
is typed out of the box.

## On real hardware

The same `App.tsx` runs on a device through the C engine — interpreted on QuickJS (Flow A) or compiled
ahead-of-time to C (Flow B). See the [embedded-react repo](https://github.com/TheMasterCoder007/embedded-react)
for the engine, the hardware backends (ESP32, STM32, …), and the on-device examples.
