# thermostat

A climate control built around a 240° arc dial: touch anywhere on the ring to set the target, with
HEAT / COOL / AUTO / OFF modes, a settings sheet for theme, units and the clock, and a 14-day weather
panel. One component, three layouts chosen from the panel size, so one source flexes from a 1280×800
tablet down to the 240×320 panel on a no-PSRAM ESP32. It exercises the native `<Dial>`, baked images,
`<Modal>` and responsive layout.

**Docs:** [The demo apps](https://embedded-react.dev/guides/demos#thermostat) explains how it is put
together and where the two flows differ.

```bash
npm create embedded-react@latest my-thermostat -- --template thermostat   # start from this demo
cd my-thermostat

npm install
npm run dev          # the browser simulator with hot reload → http://localhost:3333
npm run dev:device   # hot reload on a board over USB
npm run build        # Flow A → dist/app.erpkg
npm run build:aot    # Flow B → app.gen.c, baked at 240×320
```

Run `npm run dev` from this folder, not the repository root. `assets/` holds the weather icons and the
settings cog; import a new PNG and it is baked automatically.
