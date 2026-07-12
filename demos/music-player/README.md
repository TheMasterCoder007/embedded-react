# music-player

Demo content still in progress...

This is a complete `embedded-react` app — the same JSX you'd write as a downstream user.

## Start from this demo

```bash
npm create embedded-react@latest my-player -- --template music-player
cd my-player
npm install
npm run dev          # WASM simulator with hot reload → http://localhost:3333
```

## Develop

```bash
npm install
npm run dev          # WASM simulator with hot reload → http://localhost:3333
npm run dev:device   # hot-reload on a real board over USB (pass -- <port> for non-ESP32 boards)
```

## Build for a device

```bash
npm run build        # Flow A → dist/app.erpkg   (QuickJS bytecode; PSRAM-class chips)
npm run build:aot    # Flow B → app.gen.c        (compiled to C; no-PSRAM boards)
```
