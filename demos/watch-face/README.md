# watch-face

A two-page swipe pager for a small portrait touch display, built for the Waveshare
RP2040-Touch-LCD-1.69 (240×280): a digital watch face with heart-rate and step cards, and a bubble
level that rolls with the board's tilt. Steps and tilt are real on the RP2040, fed from its IMU
through `useHostValue`. Written to the AOT subset, so it runs with no JavaScript on the device.

**Docs:** [The demo apps](https://embedded-react.dev/guides/demos#watch-face) explains the swipe,
the host-fed values, and the AOT house rules it follows.

```bash
npm create embedded-react@latest my-watch -- --template watch-face   # start from this demo

npm install
npm run dev          # the browser simulator with hot reload; set the size to 240×280
npm run dev:device   # hot reload on a board over USB
npm run build        # Flow A → dist/app.erpkg
npm run build:aot    # Flow B → app.gen.c, baked at 240×280, for the RP2040 example
```
