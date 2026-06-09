# bridges/quickjs

QuickJS bridge — the reference frontend. Hosts a React reconciler inside QuickJS and
maps React's host-config calls (`createInstance`, `appendChild`, `commitUpdate`, etc.) to
the engine's `er_scene.h` API.

This is what makes "write JSX, run it on an MCU" actually work in Flow A (see
`/PLAN.md`).

**Status:** Working. `native_ui_bridge.c` publishes the full `NativeUI` surface (nodes, props,
events, Animated, timers/job-queue, text spans, LayoutAnimation) into QuickJS-ng (v0.15.0, via
FetchContent), and the React reconciler in `js/` drives it. See `BRIDGE.md` for the per-feature
checklist and `js/README.md` for the JS layer.

**`er_runtime` — the portable host core** (`er_runtime.{c,h}`): the few-function QuickJS host every
integration shares — create the runtime + context, install the bridge + host globals (console,
screen, and optional persist), load an app (bytecode or source), pump, reset for reload, report/overlay
errors. Backend-agnostic and platform-neutral (no SDL/IDF/filesystem): the caller owns the display
backend, where the app bytes come from, and the frame loop. This is how embedded-react drops into 
custom firmware with ~10 lines of glue (`er_runtime_init` → `er_runtime_load_bytecode` →
`er_runtime_pump`/`er_commit` per frame). The desktop demo + simulator's `examples/linux/host.c` is
just an SDL wrapper over it.

Targets (CMake): `er-bridge-quickjs` (the bridge lib), `er-bridge-quickjs-smoke` (§0 link check),
`er-bridge-quickjs-runtest` (headless test harness; also runs `.qbc` bytecode), and
`er-bridge-quickjs-compile` (bytecode precompiler: JS bundle → QuickJS bytecode blob / C array for
MCU flash).

> The bytecode precompiler is a Flow A boot/RAM optimization — it skips the on-device parser, but
> the QuickJS VM still runs the bytecode. It is **not** the Flow B AOT compiler (which compiles JSX
> to C and drops QuickJS entirely; see `/PLAN.md`).
