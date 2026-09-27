# engine

The pure C99 runtime that does everything visible on screen: scene graph, layout, rendering, text,
animation, fonts. Runtime-agnostic by design: `er_scene.h` is the public ABI any frontend calls
into, and the engine never includes a platform header.

**Docs:** [Engine and backends](https://embedded-react.dev/concepts/engine-and-backends) (the
outside view), the [C engine reference](https://embedded-react.dev/api/c-engine), and
[Engine internals](https://embedded-react.dev/internals/engine-internals) (the layout solver, scratch
buffers, damage tracking, the arc widget, frame instrumentation, and every `ERUI_*` flag).

## Layout

| Folder       | What lives here                                                                                                                                                                                                           |
| ------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `include/`   | Public headers: `er_scene.h` (scene API), `native_renderer.h` (backend interface), `er_perf.h` / `perf_overlay.h` (instrumentation), `font_bitmap.h`, `er_version.h`. The only headers downstream code includes directly. |
| `core/`      | Backend glue, frame tick, time advance.                                                                                                                                                                                   |
| `scene/`     | Node pool, tree, props, dirty tracking, render-pass orchestration, hit-testing.                                                                                                                                           |
| `layout/`    | Yoga-compatible flexbox.                                                                                                                                                                                                  |
| `rendering/` | Painters: rounded rectangles, shadows, transforms, gradients, image scaling, vectors, the arc rasteriser, the perf overlay.                                                                                               |
| `text/`      | UTF-8 decoder, glyph rasteriser, multi-line layout.                                                                                                                                                                       |
| `animation/` | `Animated.Value` engine, timing/spring/decay curves, native driver.                                                                                                                                                       |
| `font/`      | Font registry, font blob loader, font bitmaps, built-in font data.                                                                                                                                                        |
| `platform/`  | Reserved for platform-abstraction hooks. Empty today.                                                                                                                                                                     |
| `tests/`     | Host-side CTest suites.                                                                                                                                                                                                   |

## Building

The engine is a CMake static library named `embedded-react`. It pulls in nothing else; from the
repository root:

```
cmake -S engine -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Feature and pool sizes are `ERUI_*` CMake options: pools default desktop-sized (a board turns them
down) and optional features default off. [Memory](https://embedded-react.dev/guides/memory) walks
through sizing them.

## Rules

- **No platform headers.** Pure C99. Hardware specifics live in `backends/`.
- **No React assumptions.** Bindings to React, or anything else, live in `bridges/`.
- **No heap during rendering.** All scratch buffers are static, sized at compile time.
- **Section banners and documented functions**, per [Contributing](https://embedded-react.dev/internals/contributing).
