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

Part of the [embedded-react monorepo](https://github.com/TheMasterCoder007/embedded-react).
