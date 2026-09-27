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

| Group            | Methods                                                                                                                                                                                                                     |
| ---------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Tree             | `createNode(type)`, `destroyNode(h)`, `setRoot(h)`, `appendChild(parent, child)`, `insertBefore(parent, child, before)`, `removeChild(parent, child)`                                                                       |
| Props            | `setProps(h, props)`, `setTextSpans(h, spans)`, `setVectorOps(h, ops, paints, gradients, dirtyRect)`, `setEvent(h, name, handler)`                                                                                          |
| Frame            | `commit()`, `tick(dtMs)`, `now()`, `setBatcher(fn)`                                                                                                                                                                         |
| Animation        | `animValueCreate`, `animValueDestroy`, `animValueSet`, `animValueGet`, `animValueAnimate`, `animValueBind`, `animValueBindInterpolated`, `animUnbind`, `animStop`                                                           |
| Layout animation | `configureNextLayoutAnimation(config)`, `hasPendingLayoutAnimation()`                                                                                                                                                       |
| Keyboard         | `setKeyboardConfig(config)`                                                                                                                                                                                                 |
| Limits           | `maxVectorOps`, `maxVectorPaints`, `maxVectorGrads`: the engine's compiled-in vector pool sizes, so the library can warn before the engine refuses                                                                          |
| Instrumentation  | `perfCallbackBegin/End`, `perfRenderBegin/End`, `perfMarshalBegin/End`: the marks behind the JS sub-split in the [perf overlay](../guides/performance.md#measuring-on-the-device), present only in an `ER_PERF_STATS` build |

`setProps` takes the flattened style plus top-level props as one bag. The bridge interns prop names
and caches the last parsed string per enum and colour prop, and hashes the bag to skip an unchanged
one, which is what made `setProps` cheap enough on the ESP32-S3: string identity, not parsing, is the
common case.

The web timers (`setTimeout`, `setInterval`, `clearTimeout`, `clearInterval`) are plain globals the
bridge installs beside `NativeUI`, run by the pump off the engine clock.

**One commit per frame.** `setBatcher` installs React's `batchedUpdates`, and the pump runs every
callback of a frame inside it, so however many timers and events fire, the frame ends in one render
and one `commit()`. A render outside a frame (the app's first) commits on the spot.

## The C side: `er_runtime`

A Flow A firmware does not install `NativeUI` itself either. It uses `er_runtime`, the portable host
core in `bridges/quickjs/er_runtime.h`, which owns the QuickJS runtime and context, installs the
bridge and the host globals (`console`, `screen`, the optional persist store), loads an app, pumps
it, and shows errors. It has no platform dependencies: the caller provides the display backend, the
app bytes, and the frame loop.

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

| Function                                                                            | Purpose                                                                                                                                                                                       |
| ----------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `er_runtime_init(cfg)`                                                              | Creates the runtime and context with the lite JavaScript profile, installs the bridge and globals. Warns if a supplied allocator cannot report block sizes                                    |
| `er_runtime_load_container(bytes, len)`                                             | Loads an `app.erpkg`: checks the CRC and QuickJS version, registers the asset pack, runs the bytecode. Returns an `ErContainerStatus`; `er_runtime_container_status_str()` names it for a log |
| `er_runtime_load_container_ex(bytes, len, copy_assets)`                             | The same; `copy_assets = true` copies the asset pack so a reusable staging buffer (a hot-reload upload) can be freed at once                                                                  |
| `er_runtime_load_bytecode(bytes, len)`, `er_runtime_load_source(src, len, name)`    | Load a bare bytecode blob, or source text (development hosts)                                                                                                                                 |
| `er_runtime_pump()`                                                                 | Services the frame: microtasks, due timers, React's passive effects, all inside one batch scope                                                                                               |
| `er_runtime_reset()`                                                                | Tears down the context and rebuilds it, for a full reload                                                                                                                                     |
| `er_runtime_clear_persist()`                                                        | Forgets the persisted-state store                                                                                                                                                             |
| `er_runtime_set_wall_clock(ms)`                                                     | Sets what `Date.now()` returns, from an RTC or NTP                                                                                                                                            |
| `er_runtime_show_error()`, `er_runtime_last_error()`                                | Draw the on-screen redbox for the last JavaScript error; return its message and stack                                                                                                         |
| `er_runtime_show_message(title, body, hint)`                                        | The same red panel with your own text, e.g. for a config that failed to load                                                                                                                  |
| `er_runtime_run_gc()`, `er_runtime_gc_threshold()`, `er_runtime_gc_accounting_ok()` | Collect on demand, read the live threshold, and check that the allocator's size accounting works                                                                                              |
| `er_runtime_set_gc_threshold(bytes)`                                                | Change the GC floor at runtime, e.g. higher while an animation runs                                                                                                                           |
| `er_runtime_shutdown()`                                                             | Frees the context and runtime; the engine and backend are left as they are                                                                                                                    |

`ErRuntimeConfig` also takes `screen_scale` (what `screen.scale` reports; 1.0 when unset),
`malloc_functions` (to place the heap in external RAM; its `js_malloc_usable_size` must return real
block sizes or the collector never runs), `max_stack_size`
(set below the host task's real stack so deep recursion is a JavaScript error rather than a crash),
`install_persist`, `install_host_globals` (a hook to add objects of your own, as the ESP32-S3
example does for WiFi), and `extra_intrinsics` (opt in to `Date` objects, `Proxy`, typed arrays,
`WeakRef`, `BigInt` or `eval`, which the default profile leaves out).

Below `er_runtime`, for a host that drives QuickJS itself, the bridge exposes
`er_bridge_install(ctx)`, `er_bridge_pump(ctx)`, `er_bridge_run_bytecode(ctx, buf, len)`,
`er_bridge_now_ms()` and `er_bridge_release_runtime()`.

## The lite JavaScript profile

The runtime creates its context with only the intrinsics React needs: base objects, `RegExp`,
`JSON`, `Map`/`Set`, `Promise`, plus `performance.now()` and `Date.now()` on the engine clock. The
same set runs on the device, the desktop, the simulator, and the test harnesses, so development and
hardware expose one JavaScript surface. `Date.now()` counts from boot until the host passes the real
time to `er_runtime_set_wall_clock()`, from an RTC or an SNTP sync. Extras (full `Date` objects,
`Proxy`, typed arrays, `WeakRef`, `BigInt`, and `eval`/`Function` in a build with the parser) are
opt-in per host through `ErRuntimeConfig.extra_intrinsics` (`ER_JS_INTRINSIC_DATE`, `_PROXY`,
`_TYPED_ARRAYS`, `_WEAK_REF`, `_BIGINT`, `_EVAL`).

Firmware that only runs precompiled bytecode can drop the JavaScript parser with
`-DER_BRIDGE_QUICKJS_LITE=ON`, about 60 KB of flash; the error overlay still works there.

## Heap accounting

QuickJS decides when to collect garbage, and enforces `memory_limit`, from what
`js_malloc_usable_size()` reports for each allocation, and its default returns **0 on bare metal and
Emscripten**. A zero there means the collector never runs and garbage accumulates until the heap is
exhausted, which looks exactly like a leak. So with `malloc_functions` left `NULL` the bridge installs
its own allocator, which reports real sizes everywhere (a size-prefix allocator on bare metal).

If you supply your own `malloc_functions`, to put the heap in PSRAM or SDRAM as the ESP32-S3 example
does, its `js_malloc_usable_size` **must** return the real block size
(`heap_caps_get_allocated_size`, `tlsf_block_size`, `malloc_usable_size`). `er_runtime_init` warns
at boot if it does not, and `er_runtime_gc_accounting_ok()` reports it to firmware.

## Hosts with external RAM

Two settings matter once the JavaScript heap is in PSRAM or SDRAM.

**`gc_threshold`** sets a floor under QuickJS's GC trigger. QuickJS recomputes that trigger to 1.5×
the live set after every collection, so a small app in a multi-megabyte arena mark-sweeps far more
often than it needs to, walking the object graph over a slow bus each time; a floor cut 18% off a
measured workload on the ESP32-S3. Keep it well under `memory_limit` (`er_runtime_init` warns
otherwise). `er_runtime_run_gc()` collects on demand if you would rather put the pause at a screen
change or an idle frame. (A tiered allocator that placed hot objects in internal RAM was measured at
about 2% and is not offered; the schedule is the lever, not placement.)

**`max_stack_size`.** QuickJS has no stack of its own: every JavaScript call frame lives on the C
stack of the task that calls into it. Keep that task's stack in internal RAM (the ESP32-S3 example
sizes the main task with `CONFIG_ESP_MAIN_TASK_STACK_SIZE`; an STM32's default linker script already
puts it in DTCM) and set `max_stack_size` below its real size, so deep recursion raises a JavaScript
stack-overflow error rather than running off the end.
