---
title: "Run it in the simulator"
sidebar_label: "Simulator"
description: "Run your app in the browser with npx embedded-react dev, with hot reload and a device frame."
---

The simulator is the real C engine, compiled to WebAssembly and drawing into a `<canvas>`. Your app
runs unmodified, pixel-accurate to what a device draws. It is where most development happens, and it
needs no hardware and no native toolchain.

```bash
npm run dev
```

This runs `embedded-react dev`, which serves [http://localhost:3333](http://localhost:3333). Pick
another port with `npm run dev -- --port 4000`.

## Hot reload

Edit `App.jsx` and save. The page updates in place, and **component state survives**: press the
starter's counter a few times, change the title text, save, and the count is still there.

The same goes for assets. Change an imported image or font and it is re-baked and reloaded without
restarting anything.

## Match your panel

The canvas renders 1:1, with no scaling, and by default fills the browser window. To see your app
the way a specific display will show it, open the **gear button** in the bottom-left corner:

- **Size** offers common panel resolutions (800×480, 1024×600, 480×320, 320×240, 240×320) or a
  custom width and height. **Lock** pins the canvas to that size.
- **Frame** draws a device bezel around a locked canvas. It is cosmetic only.

You can also set the size from the URL, which is handy for bookmarks:

```text
http://localhost:3333/?screen=240x320
```

Because the canvas follows the window when it is not locked, the browser's own responsive-design
mode works too.

:::tip[Build responsive layouts]
An app can read the screen size and choose a layout, so one codebase serves an 800×480 panel and a
240×320 one. The `thermostat` template does this. Resize the simulator to watch it switch.
:::

## Baked font sizes

Fonts are baked to bitmaps at build time, so only the sizes that were baked exist on the device. The
built-in font comes in seven sizes: 10, 12, 16, 20, 24, 32, and 48. Use any other size, and the build
tells you what will happen instead:

```text
embedded-react: 1 font size(s) the app uses are not baked and will render at the nearest baked size:
  15px → renders at 16px in the built-in font (baked: 10, 12, 16, 20, 24, 32, 48)
```

Either use a baked size, or import your own `.ttf`/`.otf` and list the sizes you need.
[Assets](/concepts/assets) explains how.

## Share a build

```bash
npm run export
```

This writes `sim-export/`: a self-contained static copy of the simulator with your app bundled in.
It has no server component, so it can go on any static host (GitHub Pages, an S3 bucket, a docs
site). Preview it locally with `npx serve sim-export`.

## What the simulator does not tell you

The simulator runs **Flow A** on your computer's CPU. It is exact about layout and pixels, and says
nothing about two things:

- **Speed and memory.** A laptop is orders of magnitude faster than a microcontroller. Measure on
  the board; see [Performance](/guides/performance) and [Memory](/guides/memory).
- **Flow B compatibility.** The ahead-of-time compiler accepts a subset of what runs here. If you
  are targeting a board without PSRAM, run `npx embedded-react build --aot` early and often. See
  [The AOT subset](/guides/aot-subset).

When the app looks right, [put it on a board](/getting-started/first-board).
