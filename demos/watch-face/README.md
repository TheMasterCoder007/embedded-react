# watch-face

A two-page swipe pager for a small portrait touch display, built for the Waveshare
RP2040-Touch-LCD-1.69 (240×280): a digital watch face with the chip's name, its temperature and a step
count, and a bubble level that rolls with the board's tilt. They come from native modules the board's
firmware provides in C, imported from its device package, `example-device`, and the simulator runs the
package's mocks. Written to the AOT subset, so it runs with no JavaScript on the device.

In this repository the package is `examples/device`, linked by the root `npm install`. A project started
from the template has its own copy in `device/`.

**Docs:** [The demo apps](https://embedded-react.dev/guides/demos#watch-face) explains the swipe and the
AOT house rules it follows, and [Native modules](https://embedded-react.dev/guides/native-modules) how a
device package and the C behind it are written.

```bash
npm create embedded-react@latest my-watch -- --template watch-face   # start from this demo
cd my-watch

npm install
npm run dev          # the browser simulator with hot reload; set the size to 240×280
npm run dev:device   # hot reload on a board over USB
npm run build        # Flow A → dist/app.erpkg
npm run build:aot    # Flow B → dist/app.erpkg, for the RP2040 example
npm run typecheck    # tsc --noEmit
```
