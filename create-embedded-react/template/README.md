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
