---
title: 'Memory'
description: "The engine's compile-time pools, the JavaScript heap in Flow A, and how the three board examples were sized to fit."
---

Everything the engine needs is allocated before the first frame, sized by compile-time flags, and
nothing is allocated while rendering. That makes a board's memory budget a list of numbers you
choose rather than a thing you discover under load. This page is the list.

## The engine's pools

Set these as CMake options before the engine is added (or at the ESP-IDF component level). The
defaults are desktop-sized.

| Flag                                                                         | Default          | What it sizes                                                                                                                          |
| ---------------------------------------------------------------------------- | ---------------- | -------------------------------------------------------------------------------------------------------------------------------------- |
| `ERUI_MAX_NODES`                                                             | 512              | The scene-graph node pool. Every `<View>`, `<Text>` and shape is one node, and a hidden page keeps its nodes                           |
| `ERUI_SCRATCH_W`, `ERUI_SCRATCH_H`                                           | 240, 240         | The offscreen buffer for opacity groups, transforms and shadows: the largest node that can fade or rotate. `W × H × 4` bytes per strip |
| `ERUI_MAX_OPACITY_DEPTH`                                                     | 4                | Nested translucent groups, one strip each                                                                                              |
| `ERUI_SCRATCH_BAND_H`                                                        | `ERUI_SCRATCH_H` | Shrink to render tall fades in more, smaller passes                                                                                    |
| `ERUI_XFORM_W`, `ERUI_XFORM_H`                                               | scratch size     | The transform source, the one buffer that cannot be banded; decouple it when strips are screen-wide but only small widgets rotate      |
| `ERUI_FADE_CACHE_W/H`                                                        | 0 (off)          | Caches a translucent subtree during a pure opacity animation, roughly doubling fade frame rates; put it in external RAM                |
| `ER_DAMAGE_RECTS_MAX`                                                        | 16               | Disjoint dirty rectangles per commit; 4 → 16 costs about 1.1 KB of `.bss`                                                              |
| `ERUI_IMAGE_REGISTRY_MAX`                                                    | 128              | Registered images, about 80 bytes each. Past the limit an image is refused and does not draw, so keep it at or above the asset count   |
| `ERUI_FONT_SIZES`                                                            | 7                | Pre-rasterised sizes of the built-in font                                                                                              |
| `ERUI_FONT_POOL_BYTES`                                                       | 0                | A static pool for fonts loaded at runtime; 0 disables `er_font_load`                                                                   |
| `ERUI_SHADOWS`, `ERUI_3D_TRANSFORMS`, `ERUI_GRADIENT`, `ERUI_BILINEAR_SCALE` | varies           | Features that cost code and scratch; off is free                                                                                       |

The vector rasteriser has its own pools, and unlike the pixel scratch they must live in **internal
RAM** on a PSRAM board, because the scanline loops touch them per pixel:

| Flag                                           | Default | Bounds                                                         |
| ---------------------------------------------- | ------- | -------------------------------------------------------------- |
| `ERUI_MAX_VECTOR_NODES`                        | 8       | `<Svg>` nodes with geometry at once                            |
| `ERUI_VECTOR_PAINTS_MAX`                       | 16      | Shapes per `<Svg>`                                             |
| `ERUI_VECTOR_MAX_PTS`, `ERUI_VECTOR_MAX_EDGES` | 2048    | Flattened points, and edges, in one shape                      |
| `ERUI_VECTOR_MAX_ROW`                          | 1024    | The widest vector node, in pixels                              |
| `ERUI_ARC_MAX_RADIUS`, `ERUI_ARC_SPAN_CACHE`   |         | The arc widget's shared span cache, about 4 KB at the defaults |

Turn the perf overlay on and its `VEC n/8 IMG n/128` line shows the pools filling; a screen missing an
asset reads as a full image pool.

## The framebuffer

The biggest single number on most boards, and the backend's to choose:

- **Full framebuffer.** The ESP32-S3 example keeps an 800×480 framebuffer in PSRAM; the RP2040
  example keeps a 240×280 RGB565 framebuffer in SRAM, 131 KB of its 264 KB.
- **Banded.** A backend with no room for a framebuffer keeps a band of `band_height` rows and lets
  the panel's memory retain the rest. The CYD example's two 40-row RGB565 strips are about 19 KB
  each, in place of a 150 KB framebuffer, and they ping-pong so drawing overlaps the SPI transfer.
  `ER_LCD_BANDED_ROWS` sets the height: smaller is less RAM and more transfers.

## Flow A: the JavaScript heap

Flow A adds QuickJS, React, the reconciler and your app's live objects. Three settings in
`ErRuntimeConfig` control it:

- **`memory_limit`** caps the heap so an app that leaks gets a JavaScript out-of-memory error rather
  than exhausting the system heap under the display driver.
- **`malloc_functions`** decides where the heap lives. Left `NULL`, the bridge's own allocator is
  used and reports real block sizes. If you supply one to put the heap in PSRAM or SDRAM (the ESP32-S3
  example does), its `js_malloc_usable_size` **must** return the true block size
  (`heap_caps_get_allocated_size`, `tlsf_block_size`, `malloc_usable_size`). A stub returning 0
  silently disables garbage collection, and the heap falls a few KB per re-render forever;
  `er_runtime_init` warns when it detects this.
- **`gc_threshold`** sets a floor under QuickJS's collection trigger. With a large external heap the
  default trigger recomputes to 1.5× the live set and mark-sweeps far too often; a floor cut 18% off
  a measured workload on the S3.

The interpreter's **stack** is the C stack of the task that calls into it: every JavaScript frame
lives there. Keep that task's stack in internal RAM and size it generously; the ESP32-S3 example uses
a 64 KB main task stack and gives JavaScript three quarters of it. A JS "stack overflow" or an RTOS
stack panic means raise it.

Sizes to plan around, measured on the ESP32-S3 with the thermostat: the vendor bytecode (React, the
reconciler and the library) is about 140 KB in the container, and the QuickJS bridge's own tables
are 22 KB, which sit in PSRAM at no measurable cost. Flow B has none of this.

## How the examples are sized

|                    | ESP32-S3                     | ESP32 CYD                                             | RP2040                              |
| ------------------ | ---------------------------- | ----------------------------------------------------- | ----------------------------------- |
| RAM                | 512 KB internal + 8 MB PSRAM | ~223 KB free at boot, largest block ~110 KB; no PSRAM | 264 KB SRAM                         |
| Flow               | A                            | B                                                     | B                                   |
| Framebuffer        | 800×480 in PSRAM             | Two 40-row RGB565 bands, ~37 KB                       | 240×280 RGB565 in SRAM, 131 KB      |
| JS heap            | In PSRAM                     | none                                                  | none                                |
| Node pool          | default                      | lowered                                               | sized to the watch face             |
| Image registry     | default                      | 16                                                    | 8                                   |
| Damage rectangles  | 16                           | 4                                                     | 4                                   |
| Shadows, gradients | on                           | shadows off                                           | shadows, gradients and keyboard off |
| Free after boot    | logged at boot               | ~181 KB internal                                      | fits with the framebuffer           |

The two ESP32 examples log free RAM at boot and again after start-up; watch the second number as you
add features. The three `CMakeLists.txt` files are the worked examples of the flags above.

## Symptoms and causes

| Symptom                                                            | Likely cause                                                                                                 |
| ------------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------ |
| An image draws as a hole; `IMG` reads full in the overlay          | The image registry is full: raise `ERUI_IMAGE_REGISTRY_MAX`                                                  |
| A `<Svg>` stops drawing; `VEC` shows `!FULL`                       | More vector nodes than `ERUI_MAX_VECTOR_NODES`, or more shapes than `ERUI_VECTOR_PAINTS_MAX` in one          |
| A fade or rotation is clipped, or a node will not fade at all      | The node is larger than the scratch buffer: raise `ERUI_SCRATCH_W/H` or `ERUI_XFORM_W/H`, or shrink the node |
| JS "stack overflow", or an RTOS stack-overflow panic               | The host task's stack is too small for the reconciler's recursion                                            |
| Free heap falls a few KB per re-render and never recovers (Flow A) | The custom allocator's `js_malloc_usable_size` returns 0, so the collector never runs                        |
| A long list stops adding rows                                      | The node pool is full (`ERUI_MAX_NODES`), or in Flow B the list is past `ER_AOT_LIST_CAP`                    |
| A no-PSRAM ESP32 fails to allocate its band buffers                | Internal RAM is fragmented: shrink `ER_LCD_BANDED_ROWS`, or free DMA-capable RAM elsewhere                   |
