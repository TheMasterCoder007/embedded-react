---
title: 'STM32H7'
description: 'Flow A on an STM32H7 with SDRAM and the Chrom-ART backend. The public example project is in progress.'
---

:::info[Example project in progress]
Embedded React runs Flow A on an STM32H7 with SDRAM today, through the `dma2d` (Chrom-ART) backend,
but the public example project has not landed yet; it will be built on an STM32H7 Discovery board.
Until then this page covers what is known to work and how a host is put together.
:::

## What exists

- **The backend.** `backends/dma2d` drives the STM32's Chrom-ART hardware blitter for the engine's
  fill, copy and blend operations, SDK-free. It implements the optional format-aware copy, so an
  opaque RGB565 image is one DMA2D transfer.
- **The runtime.** The QuickJS bridge is platform-neutral. A Flow A host on this class of board is
  the same handful of calls as anywhere else: `er_runtime_init`, `er_runtime_load_bytecode` (or
  `er_runtime_load_container`), then `er_runtime_pump` and `er_commit` each frame.
- **The RAM.** An H7 with external SDRAM has what Flow A needs: the JavaScript heap goes in SDRAM,
  the engine's buffers and the interpreter's stack stay in the fast internal RAM.

## Writing a host today

The runtime configuration is where an STM32 host differs from the ESP32 one:

```c
const ErRuntimeConfig cfg = {
    .screen_width  = 800,
    .screen_height = 480,
    .log           = uart_log,
    .memory_limit  = 1024 * 1024, /* a JS out-of-memory error instead of an exhausted system heap */
    /* .malloc_functions = NULL: leave it unless the JS heap needs its own region */
};
```

Three things to get right:

- **Heap accounting.** With `malloc_functions` left `NULL`, the bridge installs an allocator that
  reports real block sizes. QuickJS uses those sizes to decide when to collect garbage and to enforce
  `memory_limit`. If you supply your own allocator to put the heap in SDRAM (a TLSF pool, a FreeRTOS
  heap), its `js_malloc_usable_size` **must** return the block's actual size. A stub returning 0
  silently disables the garbage collector: free memory falls a few KB per re-render and never comes
  back, which looks exactly like a leak in the app. `er_runtime_init` warns through `log` if it
  detects this, and `er_runtime_gc_accounting_ok()` reports it to firmware.
- **Collection frequency in SDRAM.** QuickJS starts its GC trigger at 256 KB and recomputes it to
  1.5× the live set after each collection, so a small app in a multi-megabyte arena mark-sweeps far
  more often than it needs to, walking the object graph over a slow bus each time.
  `ErRuntimeConfig.gc_threshold` sets a floor under that trigger; keep it well under `memory_limit`.
  `er_runtime_run_gc()` collects on demand if you would rather put the pause at a screen change.
- **The stack.** QuickJS has no stack of its own; every JavaScript call frame lives on the C stack
  of the task that calls into it. The default STM32 linker script puts the main stack in DTCM, the
  fastest RAM on the part, which is where you want it. Set `ErRuntimeConfig.max_stack_size` below
  the real stack size so deep recursion raises a JavaScript stack-overflow error rather than running
  off the end.

The [bridge README](https://github.com/TheMasterCoder007/embedded-react/blob/master/bridges/quickjs/README.md)
has the full discussion of external-RAM hosts, including why the runtime deliberately has no
size-tiered allocator.

## Which flow

Flow A is the natural fit for an H7 with SDRAM, and it is what runs today. An STM32 without external
RAM (most F4 parts, for example) is a Flow B target: compile the app to C and link it in, as the
[CYD](./esp32-cyd.md) and [RP2040](./rp2040.md) examples do, with the `dma2d` backend underneath.
