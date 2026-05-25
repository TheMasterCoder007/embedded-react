# bridges/quickjs

QuickJS bridge — the reference frontend. Hosts a React reconciler inside QuickJS and
maps React's host-config calls (`createInstance`, `appendChild`, `commitUpdate`, etc.) to
the engine's `er_scene.h` API.

This is what makes "write JSX, run it on an MCU" actually work in Flow A (see
`/PLAN.md`).

**Status:** Stub. Currently `native_ui_bridge.c` is a placeholder — no React reconciler,
no QuickJS integration. This is the next major milestone after the C runtime stabilizes.
