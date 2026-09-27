# bridges/quickjs

The QuickJS bridge, the reference frontend for Flow A. `native_ui_bridge.c` publishes the `NativeUI`
object into a QuickJS context and forwards each call to `er_scene.h`; the React reconciler in
[`js/`](js/README.md) drives it. `er_runtime.{c,h}` is the portable host core a firmware calls:
create the runtime, install the bridge and host globals, load an app, pump it once per frame.
`er_js_alloc.c` is the allocator that keeps QuickJS's garbage collector working on bare metal;
`er_hotreload.c` parses the USB reload frames; `er_assets.c` registers an ERPK pack.

**Docs:** [NativeUI bridge](https://embedded-react.dev/api/native-ui-bridge) (the JavaScript surface,
`er_runtime`, heap accounting, hosts with external RAM), [Hot reload](https://embedded-react.dev/guides/hot-reload),
and [Memory](https://embedded-react.dev/guides/memory) for the Flow A heap.

## Building

```
cmake -S bridges/quickjs -B bridges/quickjs/build -DCMAKE_BUILD_TYPE=Release
cmake --build bridges/quickjs/build
ctest --test-dir bridges/quickjs/build --output-on-failure
```

QuickJS-ng is fetched at configure time (`FetchContent`, pinned to `v0.15.0`; bytecode is specific
to that version). Targets:

| Target | What it is |
|---|---|
| `er-bridge-quickjs` | The bridge library |
| `er-bridge-quickjs-smoke` | A link check |
| `er-bridge-quickjs-runtest` | The headless test harness the JS package's runtime tiers use; also runs `.qbc` bytecode |
| `er-bridge-quickjs-gctest` | The heap-accounting / GC regression test, registered with ctest |
| `er-bridge-quickjs-compile` | The bytecode precompiler (JS bundle → QuickJS bytecode); `npm run pack` looks for it, or set `ER_COMPILE_BIN` |

Options: `-DER_BRIDGE_QUICKJS_LITE=ON` drops the JavaScript parser for firmware that only loads
bytecode; `-DER_BRIDGE_JS_USABLE_SIZE=native|shim` overrides the allocator choice.
