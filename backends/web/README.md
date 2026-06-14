# backends/web

The present layer for the **WASM simulator**: the engine is compiled to WebAssembly and renders through the
software (CPU) backend into an ARGB8888 framebuffer; this layer converts that to RGBA and shows it in a
`<canvas>`. Lets a React app authored against `embedded-react` run unmodified in a browser preview — the
shipped, zero-install dev loop for npm consumers.

It sits on top of [`backends/software`](../software/) (which does the actual compositing) and is driven by
`er_runtime` inside the WASM module. See the full design — architecture, exported C ABI, build/packaging,
and phasing — in [**WASM_SIM.md**](../../WASM_SIM.md).

**Status:** Planned (design complete; implementation starts at phase W1). No code yet.
