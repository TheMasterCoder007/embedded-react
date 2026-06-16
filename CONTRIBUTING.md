# Contributing

Contributions are welcome on any layer: the C engine, a backend, the Flow A QuickJS
bridge, or the Flow B AOT compiler. This file covers the project-wide rules. Each
top-level folder also has its own README with layer-specific guidance — start there for
the area you're touching:

- **Engine** — [`engine/README.md`](engine/README.md)
- **Backends** — [`backends/README.md`](backends/README.md)
- **Bridges (Flow A / Flow B)** — [`bridges/README.md`](bridges/README.md)
- **The npm/JS layer** — [`bridges/quickjs/js/README.md`](bridges/quickjs/js/README.md)

What's planned, what's known-broken, and the performance backlog live in
[`ROADMAP.md`](ROADMAP.md).

---

## Engine invariants

The engine is intentionally small and self-contained. These rules are non-negotiable —
they're what keep it portable to any MCU:

- **No platform headers.** Pure C99. No `stm32h7xx_hal.h`, no `esp_lcd.h`, no
  `<windows.h>`. The engine only needs `<stdint.h>`, `<string.h>`, `<stdlib.h>`, and
  `<math.h>`. Hardware specifics live in `backends/`.
- **No React (or any frontend) assumptions.** The engine does not import React. Bindings
  to React — or Lua, JSON, a visual editor, anything else — live in `bridges/`.
  `er_scene.h` is a pure C ABI; that neutrality is what makes the two-flow design work.
- **No heap during rendering.** All scratch buffers are static, sized at compile time via
  the `ERUI_*` flags. No allocation happens in a render pass.

---

## Documentation rules

### Section headers

Break C files into sections with this banner style:

```c
/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/
```

### Function documentation

Every function — static or public — is documented with a description, its parameters,
and its return value (when not `void`):

```c
/**
 * @brief This is a function description.
 *
 * @param[in] param1 This is a parameter description.
 * @param[in] param2 This is a parameter description.
 *
 * @return This is a return value description.
 */
void function_name(int param1, int param2) {}
```

- **Public APIs** carry the description in the **header**.
- **Static / private functions** carry the description in the **`.c` file** where they're
  implemented.

---

## Code style

The repo ships a `.clang-format`. The root `CMakeLists.txt` exposes a `format` target —
**run it before you commit**:

```
cmake -S . -B build
cmake --build build --target format        # rewrite in place
cmake --build build --target format-check  # CI-style dry-run (--Werror), no edits
```
