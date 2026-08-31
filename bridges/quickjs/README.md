# bridges/quickjs

QuickJS bridge — the reference frontend. Hosts a React reconciler inside QuickJS and
maps React's host-config calls (`createInstance`, `appendChild`, `commitUpdate`, etc.) to
the engine's `er_scene.h` API.

This is what makes "write JSX, run it on an MCU" actually work in Flow A (see the
[root README](../../README.md) for how Flow A and Flow B relate).

**Status:** Working. `native_ui_bridge.c` publishes the full `NativeUI` surface (nodes, props,
events, Animated, timers/job-queue, text spans, LayoutAnimation) into QuickJS-ng (v0.15.0, via
FetchContent), and the React reconciler in `js/` drives it. See [`js/README.md`](js/README.md)
for the JS layer and its per-feature status.

**Lite JS profile:** the runtime creates its context with only the intrinsics the React runtime
needs — base objects, RegExp, JSON, Map/Set, Promise, plus a `performance.now` clock — and the same
set runs on device, desktop, simulator, and the test harnesses, so dev and hardware expose one JS
surface. Extras (Date, Proxy, typed arrays, WeakRef, BigInt) are opt-in per host via
`ErRuntimeConfig.extra_intrinsics`. Device firmware that only runs precompiled bytecode can also
drop the JS parser entirely with `-DER_BRIDGE_QUICKJS_LITE=ON` (~60 KB flash); the error overlay is
precompiled bytecode (`overlay/`) so it works there too. Release bytecode is stripped of source
text + debug tables (~8x smaller); the dev hot-reload loop keeps them for line numbers.

**`er_runtime` — the portable host core** (`er_runtime.{c,h}`): the few-function QuickJS host every
integration shares — create the runtime + context, install the bridge + host globals (console,
screen, and optional persist), load an app (bytecode or source), pump, reset for reload, report/overlay
errors. Backend-agnostic and platform-neutral (no SDL/IDF/filesystem): the caller owns the display
backend, where the app bytes come from, and the frame loop. This is how embedded-react drops into 
custom firmware with ~10 lines of glue (`er_runtime_init` → `er_runtime_load_bytecode` →
`er_runtime_pump`/`er_commit` per frame). The desktop demo + simulator's `examples/linux/host.c` is
just an SDL wrapper over it.

**JS heap + GC accounting** (`er_js_alloc.{c,h}`): QuickJS decides when to collect garbage — and
enforces `ErRuntimeConfig.memory_limit` — purely from what `js_malloc_usable_size()` reports for each
allocation. QuickJS's own default implements that for macOS, Windows, and the glibc/Linux/BSD family
and returns **0 everywhere else**, which includes bare-metal `arm-none-eabi`/newlib and Emscripten. A
zero there means the GC threshold is never crossed: the collector never runs, JS garbage accumulates
until the heap is exhausted, and the memory limit cannot cap anything — silently, and only on those
platforms, so it looks exactly like a leak in the app or the engine. The bridge therefore never uses
QuickJS's default allocator. When `ErRuntimeConfig.malloc_functions` is NULL it installs its own: the
platform's usable-size call where one exists, and a size-prefix allocator (one extra word per
allocation) on bare metal, where nothing can be assumed about the heap behind `malloc`. Override the
choice with `-DER_BRIDGE_JS_USABLE_SIZE=native|shim` if you know your target.

> **If you supply your own `malloc_functions`** (e.g., to put the JS heap in PSRAM — see
> `examples/esp32/esp32-s3/main/main.c`), its `js_malloc_usable_size` **must** return the real block
> size (`heap_caps_get_allocated_size`, `tlsf_block_size`, `malloc_usable_size`, …). `er_runtime_init`
> probes this at boot and logs a warning if it does not; `er_runtime_gc_accounting_ok()` exposes the
> same answer to firmware.

### Hosts with external RAM (PSRAM / SDRAM)

Pointing the JS heap at external RAM changes how the board behaves in two ways you can control — when
the collector walks it, and where the interpreter's call frames live.

**1. Tune when the collector runs.** `ErRuntimeConfig.gc_threshold` sets a floor under QuickJS's
automatic GC trigger. QuickJS starts that trigger at 256 KB and recomputes it to *live × 1.5* after
every collection, so a board with a small live set and a multi-MB arena mark-sweeps far more often than
it needs to — walking the whole object graph over a slow bus each time. The floor is re-asserted on
every `er_runtime_pump()`, which is what makes it survive the recompute. Keep it well under
`memory_limit` (`er_runtime_init` warns if it isn't), or the app hits the cap before the collector is
ever allowed to run. Because it is re-asserted per pump it holds across *frames* — how a React app
allocates — but not inside one synchronous call that churns past it without yielding. `er_runtime_gc_threshold()` reads back the live value, and `er_runtime_run_gc()`
collects on demand — set the floor to `SIZE_MAX` and call that at a screen change or an idle frame to
put the pause somewhere it doesn't show.

Worth knowing when you measure: QuickJS is ref counted first, so ordinary garbage is reclaimed the
moment the last reference drops. Mark sweep exists for reference *cycles*, and those are what the longer
GC interval lets accumulate.

**2. The interpreter's own stack is already fast RAM — keep it that way.** QuickJS has no separate JS
stack: `JS_CallInternal` recurses on the C stack of whatever task calls into JS, so every JS call frame,
local, and argument lives on your host task's stack, wherever you put it. Nothing in the config controls
 this because the host already does. On ESP-IDF, FreeRTOS task stacks are internal RAM by default
(external-memory stacks are opt-in) — the ESP32-S3 example runs JS on the main task and just sizes it
with `CONFIG_ESP_MAIN_TASK_STACK_SIZE`. On STM32, the default linker script puts the main stack in DTCM,
the fastest RAM on the part. Set `ErRuntimeConfig.max_stack_size` below whatever that stack really is, so
deep React recursion raises a JS stack-overflow error instead of quietly running off the end of it.

> **Why there is no size-tiered allocator.** One was built and measured for this section — small
> blocks routed to internal RAM, bulk to PSRAM — and rejected on the numbers (ESP32-S3, 800×480,
> 2026-08-31): with 160 KB of internal RAM lent to it, holding the hottest ~17% of the live object
> graph, mark-sweep improved only ~2%, and paths where collection is rare got a few percent *slower*
> from the per-allocation overhead. Cache-fronted external RAM (the S3's octal PSRAM behind its
> D-cache) already absorbs the penalty tiering targets, while `gc_threshold` cut 18% on the same
> workload — so the schedule is the lever, not placement. If a future host has uncached external RAM
> and profiling shows the mark pass stalling on it, that is the case to revisit.

Targets (CMake): `er-bridge-quickjs` (the bridge lib), `er-bridge-quickjs-smoke` (§0 link check),
`er-bridge-quickjs-runtest` (headless test harness; also runs `.qbc` bytecode),
`er-bridge-quickjs-gctest` (heap-accounting/GC regression test, also registered with ctest), and
`er-bridge-quickjs-compile` (bytecode precompiler: JS bundle → QuickJS bytecode blob / C array for
MCU flash).

> The bytecode precompiler is a Flow A boot/RAM optimization — it skips the on-device parser, but
> the QuickJS VM still runs the bytecode. It is **not** the Flow B AOT compiler (which compiles JSX
> to C and drops QuickJS entirely; that lives in [`js/aot/`](js/aot/)).
