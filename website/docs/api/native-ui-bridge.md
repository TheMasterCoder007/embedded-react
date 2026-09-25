---
title: 'NativeUI bridge'
description: 'The NativeUI global the React reconciler drives, and the er_runtime host core a Flow A firmware calls.'
---

In Flow A, React's host config does not call the engine directly. It calls methods on a global
object, `NativeUI`, that the C bridge installs into the QuickJS context; each method forwards to
`er_scene.h`. Firmware never touches `NativeUI` (the library does), but knowing its shape explains
what a render costs and what the host has to provide.

```text
React reconciler  →  host-config.js  →  NativeUI.*  →  native_ui_bridge.c  →  er_scene.h
```

## The JavaScript side

Everything the library calls, grouped:

| Group            | Methods                                                                                                                                                                           |
| ---------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Tree             | `createNode(type)`, `destroyNode(h)`, `setRoot(h)`, `appendChild(parent, child)`, `insertBefore(parent, child, before)`, `removeChild(parent, child)`                             |
| Props            | `setProps(h, props)`, `setTextSpans(h, spans)`, `setVectorOps(h, ops, paints, gradients)`, `setEvent(h, name, handler)`                                                           |
| Frame            | `commit()`, `tick(dtMs)`, `now()`, `setBatcher(fn)`                                                                                                                               |
| Timers           | `setTimeout`, `setInterval`, `clearTimeout`, `clearInterval` (the web globals are these)                                                                                          |
| Animation        | `animValueCreate`, `animValueDestroy`, `animValueSet`, `animValueGet`, `animValueAnimate`, `animValueBind`, `animValueBindInterpolated`, `animUnbind`, `animStop`                 |
| Layout animation | `configureNextLayoutAnimation(config)`                                                                                                                                            |
| Keyboard         | `setKeyboardConfig(config)`                                                                                                                                                       |
| Limits           | `maxVectorOps`, `maxVectorPaints`, `maxVectorGrads`: the engine's compiled-in vector pool sizes, so the library can warn before the engine refuses                                |
| Instrumentation  | `perfCallbackBegin/End`, `perfRenderBegin/End`, `perfMarshalBegin/End`: the marks behind the JS sub-split in the [perf overlay](../guides/performance.md#measuring-on-the-device) |

`setProps` takes the flattened style plus top-level props as one bag. The bridge interns prop names
and caches the last parsed string per enum and colour prop, and hashes the bag to skip an unchanged
one, which is what made `setProps` cheap enough on the ESP32-S3: string identity, not parsing, is the
common case.

**One commit per frame.** `setBatcher` installs React's `batchedUpdates`, and the pump runs every
callback of a frame inside it, so however many timers and events fire, the frame ends in one render
and one `commit()`. A render outside a frame (the app's first) commits on the spot.

## The C side: `er_runtime`

A Flow A firmware does not install `NativeUI` itself either. It uses `er_runtime`, the portable host
core in `bridges/quickjs/er_runtime.h`, which owns the QuickJS runtime and context, installs the
bridge and the host globals (`console`, `screen`, the optional persist store), loads an app, pumps
it, and shows errors. It has no platform dependencies: the caller provides the display backend, the
app bytes and the frame loop.

```c
ErRuntimeConfig cfg = {
    .screen_width  = 800,
    .screen_height = 480,
    .log           = my_log,          /* one line at a time: console.* output and runtime errors */
    .memory_limit  = 1024 * 1024,     /* JS heap cap: an OOM error, not an exhausted system heap */
    .gc_threshold  = 256 * 1024,      /* floor under the GC trigger; matters with a big external heap */
};
er_runtime_init(&cfg);
er_runtime_load_container(bytes, len);  /* app.erpkg: verifies CRC + QuickJS version, registers assets */

for (;;) {
    poll_touch();                        /* embedded_renderer_touch(...) */
    er_runtime_pump();                   /* timers, promises, effects, React's render → er_commit() */
    present();                           /* backend flush of the dirty region */
    embedded_renderer_tick(dt_ms);       /* advance the engine clock */
}
```

| Function                                                                            | Purpose                                                                                                                                                    |
| ----------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `er_runtime_init(cfg)`                                                              | Creates the runtime and context with the lite JavaScript profile, installs the bridge and globals. Warns if a supplied allocator cannot report block sizes |
| `er_runtime_load_container(bytes, len)`                                             | Loads an `app.erpkg`: checks the CRC and QuickJS version, registers the asset pack, runs the bytecode                                                      |
| `er_runtime_load_bytecode(bytes, len)`, `er_runtime_load_source(src, len, name)`    | Load a bare bytecode blob, or source text (development hosts)                                                                                              |
| `er_runtime_pump()`                                                                 | Services the frame: microtasks, due timers, React's passive effects, all inside one batch scope                                                            |
| `er_runtime_reset()`                                                                | Tears down the context and rebuilds it, for a full reload                                                                                                  |
| `er_runtime_clear_persist()`                                                        | Forgets the persisted-state store                                                                                                                          |
| `er_runtime_set_wall_clock(ms)`                                                     | Sets what `Date.now()` returns, from an RTC or NTP                                                                                                         |
| `er_runtime_show_error()`                                                           | Draws the on-screen redbox for the last JavaScript error                                                                                                   |
| `er_runtime_run_gc()`, `er_runtime_gc_threshold()`, `er_runtime_gc_accounting_ok()` | Collect on demand, read the live threshold, and check that the allocator's size accounting works                                                           |

`ErRuntimeConfig` also takes `malloc_functions` (to place the heap in external RAM; its
`js_malloc_usable_size` must return real block sizes or the collector never runs), `max_stack_size`
(set below the host task's real stack so deep recursion is a JavaScript error rather than a crash),
`install_persist`, `install_host_globals` (a hook to add objects of your own, as the ESP32-S3
example does for WiFi), and `extra_intrinsics` (opt in to `Date` objects, `Proxy`, typed arrays,
`WeakRef`, `BigInt`, which the default profile leaves out).

Below `er_runtime`, for a host that drives QuickJS itself, the bridge exposes
`er_bridge_install(ctx)`, `er_bridge_pump(ctx)`, `er_bridge_run_bytecode(ctx, buf, len)`,
`er_bridge_now_ms()` and `er_bridge_release_runtime()`.

## Heap accounting

The bridge never uses QuickJS's default allocator. QuickJS decides when to collect garbage from what
`js_malloc_usable_size` reports, and its default returns 0 on bare-metal and Emscripten targets,
which silently disables collection. With `malloc_functions` left `NULL` the bridge installs an
allocator that reports real sizes (a size-prefix allocator on bare metal). The
[bridge README](https://github.com/TheMasterCoder007/embedded-react/blob/master/bridges/quickjs/README.md)
covers this and the external-RAM tuning in depth.
