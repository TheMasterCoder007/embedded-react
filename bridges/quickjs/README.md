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

Targets (CMake): `er-bridge-quickjs` (the bridge lib), `er-bridge-quickjs-smoke` (§0 link check),
`er-bridge-quickjs-runtest` (headless test harness; also runs `.qbc` bytecode), and
`er-bridge-quickjs-compile` (bytecode precompiler: JS bundle → QuickJS bytecode blob / C array for
MCU flash).

> The bytecode precompiler is a Flow A boot/RAM optimization — it skips the on-device parser, but
> the QuickJS VM still runs the bytecode. It is **not** the Flow B AOT compiler (which compiles JSX
> to C and drops QuickJS entirely; see `/PLAN.md`).
