# __APP_NAME__

A new [embedded-react](https://github.com/TheMasterCoder007/embedded-react) app — React Native for embedded MCUs.

## Develop

```bash
npm install
npm run dev          # WASM simulator with hot reload → http://localhost:3333
```

Edit `App.jsx` and save — the simulator hot-reloads and your `useState` is preserved. The canvas fills the
viewport, so the browser's device toolbar drives the board size (e.g., 240×320), pixel-accurate to a panel.

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
index.jsx     entry — registers <App/>
App.jsx       your UI: View / Text / Pressable / Image / Animated, styled with StyleSheet
assets/       images & fonts — import them in code and they're baked automatically
```

## On real hardware

The same `App.jsx` runs on a device through the C engine — interpreted on QuickJS (Flow A) or compiled
ahead-of-time to C (Flow B). See the [embedded-react repo](https://github.com/TheMasterCoder007/embedded-react)
for the engine, the hardware backends (ESP32, STM32, …), and the on-device examples.
