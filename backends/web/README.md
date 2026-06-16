# backends/web

The present layer for the **WASM simulator**: the engine is compiled to WebAssembly and renders through the
software (CPU) backend into an ARGB8888 framebuffer; this layer converts that to RGBA and shows it in a
`<canvas>`. Lets a React app authored against `embedded-react` run unmodified in a browser preview — the
shipped, zero-install dev loop for npm consumers.

It sits on top of [`backends/software`](../software/) (which does the actual compositing). The build/run
instructions and full design live in [`tools/web-sim`](../../tools/web-sim/README.md).

## Files

- [`web_backend.h`](web_backend.h) — the exported C ABI the host page drives via `cwrap`
  (`er_web_init` / `er_web_pump` / `er_web_touch` / `er_web_framebuffer` / …).
- [`renderer_backend.c`](renderer_backend.c) — the present layer: ARGB8888 → canvas RGBA swizzle, plus the
  ABI implementation and (W1) a static demo scene.

**Status:** Working. The `engine → WASM → canvas` pipeline (and the ARGB→RGBA swizzle) renders end to end,
verified pixel-for-pixel, and drives the full browser dev loop — interactive Flow A bundles
(`er_web_load_source`), baked asset packs, and Server-Sent hot reload. Shipped as `npx embedded-react dev`.
