---
title: 'C engine'
description: "er_scene.h: the engine's public API for nodes, props, commit, events, animation and assets, plus the backend struct."
---

`er_scene.h` is the API both flows end at. Flow A reaches it through the bridge; Flow B's generated
C calls it directly; a firmware author calls a handful of it from the host loop. This page is a map
of the header, not a replacement for it: the header's doc comments are the reference for each
function's exact contract.

## A host in miniature

```c
#include "er_scene.h"
#include "native_renderer.h"

embedded_renderer_set_backend(&my_backend);   /* fill/copy/blend/wait/frame_ready */

ERNode* root = er_node_create(ER_NODE_VIEW);
ERProps p;
er_props_default(&p);
p.width = 240; p.height = 320; p.background_color = 0xFF07111F;
er_node_set_props(root, &p);
er_tree_set_root(root);

for (;;) {
    embedded_renderer_touch(id, phase, x, y);  /* from the panel driver */
    er_commit();                               /* layout + paint the dirty region */
    my_backend_present();                      /* flush it */
    embedded_renderer_tick(dt_ms);             /* advance the clock, run animations */
}
```

A Flow B firmware replaces the node building with the generated `er_app_build(w, h)` and adds
`er_app_tick(dt)` for the app's timers. A Flow A firmware replaces it with `er_runtime`, which
builds the tree from JavaScript.

## Nodes and the tree

| Function                                                                                    | Purpose                                                                                                                                                                                          |
| ------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `er_node_create(type)`                                                                      | Allocate a node from the fixed pool (`ERUI_MAX_NODES`). Types: view, text, image, scroll view, flat list, pressable, text input, activity indicator, switch, modal, vector (`Svg`), arc (`Dial`) |
| `er_node_destroy(node)`                                                                     | Return it to the pool, cancelling its animations                                                                                                                                                 |
| `er_tree_set_root`, `er_tree_append_child`, `er_tree_insert_before`, `er_tree_remove_child` | The tree                                                                                                                                                                                         |
| `er_node_first_child`, `er_node_next_sibling`, `er_node_get_type`                           | Traversal                                                                                                                                                                                        |
| `er_node_in_use_count()`                                                                    | Live nodes, for watching the pool                                                                                                                                                                |
| `er_reset()`                                                                                | Free every node and clear the root: a fresh scene                                                                                                                                                |

## Props

`ERProps` is one flat struct covering every node type; `er_props_default()` initialises it and
`er_node_set_props(node, &props)` applies it, marking the node dirty. Its fields follow the
header's groups: layout (the Yoga properties, with `_pct` companions for percentage width, height and
insets), view visuals (background, borders, radii, opacity), interaction (`pointer_events`, `display`),
text (`text`, `font_size`, `font_weight`, colour, alignment, ellipsis), image (name, resize mode,
tint), transform (the matrix and origin), shadow, and the per-widget groups for activity indicator,
switch, text input, modal, arc and gradient.

Two calls set what a prop bag cannot hold: `er_node_set_text_spans(node, spans, count)` for
per-run text styling, and `er_node_set_vector_ops(node, ops, paints, ...)` for an `Svg` node's shape
tape, with `er_node_set_vector_dirty_rect` to limit its next repaint to a sub-region.

## Commit and damage

`er_commit()` runs the layout pass and paints every dirty node through the backend. Afterwards,
`er_get_dirty_rect()` returns the bounding box of what was repainted and `er_get_dirty_rects(out,
max)` the up-to-16 disjoint rectangles, both reporting the last commit that actually painted, so a
host can flush one transfer window per region. `er_layout_pass_count()` and
`er_text_measure_count()` count work done, for tests and tuning.

A page-flipping display tells the engine how many buffers it rotates with
`er_set_display_buffer_count(n)` and reports each flip with `er_display_present()`; the engine then
replays damage into every buffer so none is stale.

## Events and input

`er_event_set(node, type, fn, user_data)` registers a handler for one event type on a node: press,
long press, press in/out, touch start/move/end/cancel, the responder lifecycle, layout, scroll,
value change (`Switch`, `Dial`), text input events. `er_responder_query_set(node, ...)` registers
the should-set predicate the engine asks during gesture negotiation. Touches arrive from the host
through `embedded_renderer_touch(id, phase, x, y)`; `er_touch_active_count()` reports fingers down.
`er_scroll_view_set_offset` scrolls programmatically; `er_text_input_focus`/`blur`/`get_text`/
`set_text` drive a text input; `er_keyboard_set_config` swaps the on-screen keyboard.

## Animation

The engine animates two things: a node property directly, and a standalone value bound to one or
more properties.

- `er_anim_start(node, prop, &cfg)` / `er_anim_cancel(node, prop)` animate a property in place.
- `er_anim_value_create()` makes a standalone float; `er_anim_value_bind(v, node, prop)` and
  `er_anim_value_bind_interpolated(v, node, prop, &map)` attach it, `er_anim_value_animate(v,
to, &cfg)` drives it, `er_anim_value_set`/`get` read and write it, `er_anim_value_unbind_all` and
  `er_anim_unbind_prop` detach, `er_anim_value_destroy` frees it. This is what an `Animated.Value`
  is.
- `er_anim_sequence`, `er_anim_parallel`, `er_anim_stagger` group animations; `er_anim_stop(handle)`
  stops any of them. `er_interpolate` maps a float through a breakpoint table.

`ERAnimConfig` selects the algorithm (`ER_ANIM_TIMING`, `ER_ANIM_SPRING`, `ER_ANIM_DECAY`), the
easing (`ER_EASE_LINEAR` through `ER_EASE_ELASTIC_OUT`, or `ER_EASE_BEZIER` with four control
points), `duration_ms` and `delay_ms`, the spring's `stiffness`, `damping`, `mass` and `velocity`,
decay's `deceleration`, `loop` and `loop_reverse`, and an `on_complete` callback. Layout
animations are armed with `er_layout_anim_configure_next(&cfg)` and take effect on the next commit.

## Assets

`er_image_load(name, ...)` and `er_image_load_rgb565(name, ...)` register an image under a name;
`er_font_register(family, size, ...)` registers one baked size of a font and `er_font_load` a font
blob. All reference the bytes in place, so flash-resident assets cost no RAM. The generated
`er_register_assets()` from a Flow B build calls these for you; a Flow A container registers its
pack on load. `er_text_measure` and `er_text_measure_spans` measure text the way layout does.

## Time

The engine has one clock, advanced by the host: `embedded_renderer_tick(dt_ms)` moves it and runs
due animations. `er_now_ms()` and `er_now_ms64()` read it; JavaScript's `performance.now()` and
`Date.now()` are the 64-bit one.

## The backend struct

`native_renderer.h` declares `EmbeddedRenderBackend`: `fill_rect`, `copy_rect`, `blend_rect`,
`wait`, `frame_ready` and a `ctx` pointer, plus the optional banded rendering (`band_height`,
`band_begin`, `band_flush`) and format-aware copy (`copy_rect_fmt`) extensions.
[Engine and backends](../concepts/engine-and-backends.md) explains the contract and
[Writing a backend](../internals/writing-a-backend.md) the conventions.

## Instrumentation

`er_perf.h` times each frame's layout and raster phases, keeps the worst frame seen with its full
split, and counts repainted pixels and pool usage; `perf_overlay.h` draws it on the panel. The
[Performance](../guides/performance.md#measuring-on-the-device) guide shows how to read it.

## Building the engine

```bash
cmake -S engine -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The engine is a CMake static library named `embedded-react` and needs only a C99 compiler and
`<math.h>`. Its feature flags are listed in [Memory](../guides/memory.md#the-engines-pools).
