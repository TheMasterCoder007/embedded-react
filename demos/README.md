# demos

Complete apps written against the public `embedded-react` API, the same JSX a downstream user would
write. Each folder is a self-contained consumer project (its own `package.json` depending on
`embedded-react`, wired to the CLI like a scaffolded app), and each builds through both flows from
one source. They are also the source of the `create-embedded-react` templates, staged into that
package by `npm run sync-templates`.

**Docs:** [The demo apps](https://embedded-react.dev/guides/demos): what each exercises, how it is
put together, and what it took to keep both compiling under Flow B.

| Demo | What it is |
|---|---|
| `thermostat/` | A thermostat with a native dial and a 14-day weather panel; three layouts from 1280×800 down to 240×320 |
| `watch-face/` | A two-page swipe pager, a digital watch face and a bubble level, for the RP2040 1.69" |

```bash
npm create embedded-react@latest my-app -- --template thermostat   # start from one, no checkout needed

cd demos/thermostat && npm install && npm run dev                   # or run one here, in the browser simulator

cd bridges/quickjs/js                                               # or by name, against the in-repo library
npm run sim   -- thermostat     # the SDL simulator
npm run pack  -- thermostat     # → dist/app.erpkg (Flow A)
npm run aot   -- thermostat     # → dist/app.gen.c (Flow B)
npm run create -- my-app        # scaffold a new in-repo demo at demos/my-app
```
