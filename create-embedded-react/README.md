# create-embedded-react

Scaffold a new [embedded-react](https://www.npmjs.com/package/embedded-react) app — React Native for embedded
MCUs.

```bash
npm create embedded-react@latest my-app
# or: npx create-embedded-react my-app

cd my-app
npm install
npm run dev          # WASM simulator with hot reload → http://localhost:3333
```

The generated project is a minimal starter — a styled card with a pulsing logo and a `count is N` button —
wired for the browser simulator (`npm run dev`) and a shareable static export (`npm run export`). Edit
`App.jsx` and save; it hot-reloads. The same code runs on real hardware through the embedded-react C engine.

## TypeScript

For a TypeScript starter, add `--ts` (or `--typescript`):

```bash
npm create embedded-react@latest my-app -- --ts
# or: npx create-embedded-react my-app --ts
```

The TypeScript template adds a `tsconfig.json`, ambient type declarations for asset imports (png/ttf/…),
and a `npm run typecheck` script.

## Start from a demo

Pass `--template <name>` (or `-t`) to scaffold from a full demo app instead of the minimal starter — a
ready-made starting point, or a way to try a real UI on your hardware:

```bash
npm create embedded-react@latest my-thermostat -- --template thermostat
npm create embedded-react@latest my-player     -- --template music-player
```

List everything available:

```bash
npm create embedded-react@latest -- --list
```

| Template       | What you get                                            |
| -------------- |---------------------------------------------------------|
| `starter`      | Minimal starter — pulsing logo + counter (the default). |
| `starter-ts`   | The starter in TypeScript (same as `--ts`).             |
| `thermostat`   | A thermostat with weather widget                        |
| `music-player` | A music player app                                      |

Every template scaffolds the same way — `npm install && npm run dev` for the browser simulator,
`npm run dev:device` to hot-reload on a board, `npm run build` for the device artifact. The demos are the
same JSX apps in the monorepo's [`demos/`](https://github.com/TheMasterCoder007/embedded-react/tree/master/demos).

Part of the [embedded-react monorepo](https://github.com/TheMasterCoder007/embedded-react).
