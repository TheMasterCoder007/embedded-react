# examples/stm32h7

STM32H7 + LTDC + LCD panel sample. Engine + `backends/dma2d/` + `bridges/quickjs/`
flashed to a Nucleo-H743ZI or similar dev board, driving an 800×480 panel.

First MCU bring-up target. Lands after `examples/linux/` proves the Flow A pipeline.

**Status:** Planned. No code yet.

## Writing an STM32 host today

The bridge is platform-neutral, so a Flow A STM32 host is `er_runtime_init` → `er_runtime_load_bytecode`
→ `er_runtime_pump`/`er_commit` per frame (see [`bridges/quickjs/er_runtime.h`](../../bridges/quickjs/er_runtime.h)).
One thing to get right on this class of board:

```c
const ErRuntimeConfig cfg = {
    .screen_width  = 800,
    .screen_height = 480,
    .log           = uart_log,
    .memory_limit  = 1024 * 1024, /* fail with a JS OOM instead of exhausting the system heap */
    /* .malloc_functions = NULL — leave it NULL unless the JS heap needs its own region. */
};
```

Leaving `malloc_functions` NULL is the safe default: the bridge installs an allocator that reports
real block sizes, which is what QuickJS uses to decide when to collect garbage and to enforce
`memory_limit`. If you *do* supply your own — a tlsf pool, SDRAM, a FreeRTOS heap — its
`js_malloc_usable_size` must return the block's actual size (e.g. `tlsf_block_size`). A stub returning
0 disables the garbage collector entirely: free heap then falls a few KB per re-render and never
recovers, which looks like a leak in the app. `er_runtime_init` warns through `log` when it detects
this, and `er_runtime_gc_accounting_ok()` reports it to firmware.
