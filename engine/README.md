# engine

The pure C99 runtime that does everything visible on screen: scene graph, layout,
rendering, text, animation, fonts. Runtime-agnostic by design — `er_scene.h` is the
public ABI any frontend (React-on-QuickJS, AOT-compiled React, future Lua / JSON / visual
editor) calls into.

Contributors working on the engine itself: this is your README. End users writing React
apps don't need to know about the layout here.

## Layout

| Folder | What lives here |
|---|---|
| `include/` | Public headers — `er_scene.h` (scene API) and `native_renderer.h` (backend interface). The only headers downstream code is allowed to include directly. |
| `core/` | Backend glue, frame tick, time advance. Everything that connects the engine to the hardware-blitting backend. |
| `scene/` | Node pool, parent/child/sibling tree, props, dirty tracking, render pass orchestration, hit-testing. |
| `layout/` | Yoga-compatible 7-pass flexbox. |
| `rendering/` | Painters for the renderable primitives — rounded rectangles, shadows, transforms, image scaling, canvas. |
| `text/` | UTF-8 decoder, glyph rasterizer, multi-line layout. |
| `animation/` | `Animated.Value` engine, timing/spring/decay curves, native driver. |
| `resources/` | Font registry, font blob loader, font bitmaps, built-in font data. Future home for image assets. |
| `platform/` | Platform-abstraction hooks the engine needs (time source, optional memory abstractions). Empty today. |
| `tests/` | Host-side CTest suites — layout, text. |

## Building

The engine is a CMake STATIC library; configure from the repo root, not from here:

```
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## Rules

- **No platform headers.** Pure C99. No `stm32h7xx_hal.h`, no `esp_lcd.h`, no
  `<windows.h>`. Hardware specifics live in `backends/`.
- **No React assumptions.** The engine does not import React. Bindings to React (or
  Lua, JSON, visual editors, anything else) live in `bridges/`.
- **No heap during rendering.** All scratch buffers are static, sized at compile time.
- **Section banners + JSDoc-style function docs** per the rules in the repo root
  `RULES.md`.
