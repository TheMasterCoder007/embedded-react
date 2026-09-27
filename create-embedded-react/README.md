# create-embedded-react

Scaffold a new [Embedded React](https://embedded-react.dev) app: React Native for embedded MCUs.

```bash
npm create embedded-react@latest my-app          # add -- --ts for a TypeScript starter
cd my-app
npm install
npm run dev                                       # the browser simulator with hot reload → http://localhost:3333
```

The starter is a responsive card (a pulsing logo, a counter button and the panel size) laid out from
`screen.width`/`screen.height`, so it fits a 240×240 watch face and an 800×480 panel alike. Edit
`App.jsx` and save; it hot-reloads. The same code runs on hardware through the C engine.

## Start from a demo

```bash
npm create embedded-react@latest my-thermostat -- --template thermostat
npm create embedded-react@latest my-watch      -- --template watch-face
npm create embedded-react@latest -- --list
```

| Template | What you get |
|---|---|
| `starter` | The responsive starter (the default) |
| `starter-ts` | The starter in TypeScript (same as `--ts`) |
| `thermostat` | A thermostat with a native dial and a 14-day weather panel |
| `watch-face` | A digital watch face and a bubble level, sized for a 240×280 panel |

Every template scaffolds the same way: `npm run dev` for the simulator, `npm run dev:device` for hot
reload on a board, `npm run build` for the device artifact, `npm run export` for a shareable static
copy of the simulator.

**Docs:** [Installation](https://embedded-react.dev/getting-started/installation),
[the simulator](https://embedded-react.dev/getting-started/simulator),
[your first board](https://embedded-react.dev/getting-started/first-board), and
[the demo apps](https://embedded-react.dev/guides/demos). Part of the
[embedded-react monorepo](https://github.com/TheMasterCoder007/embedded-react).
