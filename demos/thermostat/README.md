# thermostat

> **Placeholder.** The previous arc-dial climate demo was removed and a new thermostat demo is being
> written in its place. Until `index.jsx` + `App.jsx` land here, this folder is not a runnable demo and
> is skipped by `sync-templates` (a demo is any `demos/<name>` with an `index.jsx`), so
> `--template thermostat` is unavailable and the toolchain's `thermostat` default-demo builds will fail.

The consumer-project wiring (`package.json` scripts → the `embedded-react` CLI, `.gitignore`) is kept
in place so the replacement drops straight in.

## Develop

```bash
npm install
npm run dev          # WASM simulator with hot reload → http://localhost:3333
npm run dev:device   # hot-reload on a real board over USB (pass -- <port> for non-ESP32 boards)
```

## Build for a device

```bash
npm run build        # Flow A → dist/app.erpkg   (QuickJS bytecode + baked assets; PSRAM-class chips)
npm run build:aot    # Flow B → app.gen.c        (compiled to C; no-PSRAM boards; targets 240x320)
```
