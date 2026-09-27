# backends/web

The present layer for the WebAssembly simulator: the engine renders through `../software` into an
ARGB8888 framebuffer, and this layer converts it to RGBA for a `<canvas>` and exposes the C ABI the
host page drives through `cwrap` (`web_backend.h`: `er_web_init`, `er_web_load_source`,
`er_web_load_pack`, `er_web_resize`, `er_web_pump`, `er_web_touch`, `er_web_framebuffer`,
`er_web_clear_persist`, …). Shipped as `npx embedded-react dev` and used by the site's playground.

**Docs:** [Run it in the simulator](https://embedded-react.dev/getting-started/simulator); the build
is in `tools/web-sim`.
