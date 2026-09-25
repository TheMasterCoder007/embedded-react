---
title: 'Getting started'
description: 'Install the tooling, run an app in the browser simulator, then flash it to a board.'
---

You do not need any hardware to start. The engine also compiles to WebAssembly, so the first thing
you will see is your app running in a browser tab, pixel for pixel as a device would draw it.

1. **[Installation](./installation.mdx).** Scaffold a project with one command. All you
   need is Node.js.
2. **[Run it in the simulator](./simulator.md).** Edit `App.jsx`, save, and watch it
   hot-reload at your panel's exact resolution.
3. **[Your first board](./first-board.md).** Build the app for a real device and flash
   it. This is where you pick a flow and install a firmware toolchain.

```bash
npm create embedded-react@latest my-app
cd my-app
npm install
npm run dev
```

If you would rather look before installing anything, open the [playground](/playground).
