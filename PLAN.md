# Plan: embedded-react — React Native for Embedded MCUs

## Overview

`embedded-react` is **React Native for embedded MCUs**. The developer writes the same
JSX components, the same `Animated` API, and the same flexbox styles they would write for
React Native on iOS or Android — and the app runs on an STM32, ESP32, RP2040, or any
microcontroller with a writable framebuffer.

Two officially supported developer flows compile that JSX onto hardware. They share the
same JSX source, the same component vocabulary, and the same C engine — they differ only
in how the React tree gets evaluated on the device.

- **Flow A — React on QuickJS** *(near-term, primary; in progress).* JSX is bundled to JS
    + QuickJS bytecode and flashed alongside the firmware. On boot, QuickJS executes a
      small React reconciler that calls into the pure-C `er_scene.h` runtime.

- **Flow B — React as a compile target** *(future, design only).* JSX is consumed by an
  AOT compiler that emits C code directly against `er_scene.h`. No JS at runtime, no JS
  engine in firmware, no garbage collector — at the cost of giving up some dynamic JS
  features. Same JSX source as Flow A.

The C engine is pure C99 with **zero platform dependencies** — no MCU SDK headers, no
RTOS includes, no DMA2D, no ESP-IDF. The library only needs `<stdint.h>`, `<string.h>`,
`<stdlib.h>`, and `<math.h>` from the standard library and a single backend struct with
function pointers for hardware blitting.

---

## Repo Layout & Architectural Layering

The repo is a monorepo with four top-level concerns. Each maps to a clean layer of the
architecture.

```
engine/      Pure C99 runtime. Scene graph, layout, rendering, text, animation.
             Knows nothing about React, JSX, JS, Lua, JSON, or any frontend.
             Exposes `er_scene.h` — a pure C ABI any caller can drive.

backends/    Hardware adapters. One folder per rendering API or peripheral.
             Implements five function pointers (fill / copy / blend / wait /
             frame_ready). Knows nothing about scene graphs — just blits pixels.

bridges/     Frontends that drive the engine. Each bridge maps a particular
             developer-facing concept (React components, Lua tables, JSON
             descriptors, GUI editor output) into er_scene.h calls. The
             supported bridge is `quickjs/` — a QuickJS host running a React
             reconciler.

examples/    End-to-end sample apps. Each example pins one engine + one
             backend + one bridge into a runnable artifact for a specific
             target board.
```

### Why the engine doesn't know about React

The engine deliberately keeps no React assumptions. The reconciler that translates
React's host config into `er_*` calls lives in `bridges/quickjs/`. The Flow B AOT
compiler that emits `er_*` calls at build time lives outside the engine entirely
(it's a Node-side build tool, not C code).

This separation pays for itself in three ways:

1. **`er_scene.h` is the stable runtime ABI.** Both Flow A (reconciler at runtime) and
   Flow B (compiler at build time) target the same C surface. An app written against
   Flow A can in principle be rebuilt as Flow B with no source changes — only the
   toolchain changes.

2. **The marketing identity (React Native for MCUs) is at the project level**, not the
   engine level. README.md tells React developers what they get; `engine/README.md`
   tells engine contributors what they're working on. Different audiences, different
   docs, the same project.

3. **The door stays open** for non-React frontends without forking the engine. Future
   bridges could include Lua UI, JSON UI loaders, a desktop visual editor, scripting
   APIs. None are roadmap items today, but the layering means they could land later
   without disrupting React app authors.

### What lives in each top-level folder

| Folder                     | Purpose                                                   | Status                                                              |
|----------------------------|-----------------------------------------------------------|---------------------------------------------------------------------|
| `engine/include/`          | Public headers (`er_scene.h`, `native_renderer.h`)        | Stable                                                              |
| `engine/core/`             | Backend glue, frame tick                                  | Working                                                             |
| `engine/scene/`            | Node pool, tree, render pass, hit-testing                 | Partial (scene tree, render, Pressable/touch events + zIndex done)  |
| `engine/layout/`           | Yoga 7-pass flexbox                                       | Working                                                             |
| `engine/rendering/`        | Rounded rects, shadows, transforms, image scaling, canvas | Partial (rrect done; shadows, transforms, image stubs)              |
| `engine/text/`             | UTF-8 decode + glyph blit                                 | Working                                                             |
| `engine/animation/`        | Animated.Value engine                                     | Partial (timing for existing color/opacity props)                   |
| `engine/resources/`        | Font registry, font blob loader, built-in font            | Working                                                             |
| `engine/platform/`         | Engine-side platform abstractions (time source, etc.)     | Empty (no abstractions needed yet)                                  |
| `engine/tests/`            | Host-side CTest suites                                    | Green (animation, input, layout, text, rrect)                       |
| `backends/dma2d/`          | STM32 DMA2D hardware blitter                              | Stub                                                                |
| `backends/sdl/`            | SDL2 desktop backend                                      | **Implemented** (`fill`, `copy`, `blend`; premultiplied blend mode) |
| `backends/esp32-lcd/`      | ESP32-S3 LCD peripheral + PSRAM                           | Stub                                                                |
| `backends/software/`       | Pure CPU blit (RP2040, low-end MCUs)                      | Stub                                                                |
| `backends/opengl/`         | OpenGL ES 2.0 (RPi, Android)                              | README only                                                         |
| `backends/framebuffer/`    | Linux `/dev/fb0`                                          | README only                                                         |
| `backends/web/`            | WebAssembly + Canvas/WebGL                                | README only                                                         |
| `bridges/quickjs/`         | React reconciler hosted on QuickJS                        | Stub (Flow A milestone)                                             |
| `examples/linux/`          | Desktop SDL demo                                          | Implemented as C-driver demo; Flow A JSX target planned             |
| `examples/stm32h7/`        | First MCU bring-up                                        | Planned                                                             |
| `examples/esp32/`          | ESP32-S3 bring-up                                         | Planned                                                             |
| `examples/raspberry-pi/`   | RPi reference app                                         | Planned                                                             |
| `examples/dashboard-demo/` | Cross-platform UI showcase                                | Planned                                                             |
| `examples/marine-display/` | Real-world reference app                                  | Planned                                                             |
| `tools/font-converter/`    | TTF → C font data generator                               | Working                                                             |

---

## Developer Workflow

### Flow A — React on QuickJS (primary, in progress)

The target experience:

```
$ npx create-embedded-react my-app
$ cd my-app && vim src/App.jsx
$ npm run build -- --target stm32h7
$ npm run flash
```

Under the hood:

1. The user writes JSX components that import from `'embedded-react'`.
2. A Metro-compatible bundler walks the dependency graph, compiles JSX with Babel, and
   emits a single bundle. That bundle is compiled to QuickJS bytecode and embedded in
   the firmware image as a binary blob.
3. On boot, the firmware initialises QuickJS, loads the bytecode, and starts the React
   reconciler. The reconciler (in `bridges/quickjs/`) is a small host config (ported
   from React Native's host config) that maps React's diff output to `er_*` calls.
4. Every layout / render / animation tick happens inside the C engine. JS is only
   re-entered when React state changes — animations driven by `useNativeDriver: true`
   never call into JS at all.

**What needs to be built for Flow A:**

- **Bundler** — Metro-compatible, capable of resolving `'embedded-react'` to the JS-side
  module shim that proxies React calls to `er_*` natives.
- **React reconciler** — ported from React Native's renderer. Lives entirely in JS.
- **QuickJS integration** — bytecode loader, bridge globals
  (`NativeUI.createNode`, `NativeUI.setProps`, etc.) wired through
  `bridges/quickjs/native_ui_bridge.c` to `er_scene.h`.
- **First end-to-end target** — `examples/linux/` with `backends/sdl/` (so the loop can
  be exercised without flashing hardware), then `examples/stm32h7/` with
  `backends/dma2d/`.

Flow A is not built yet. The runtime ABI it targets (`er_scene.h`) is, and
`examples/linux/` currently validates that ABI through a pure-C SDL demo.

### Flow B — React as a compile target (future, design only)

The same JSX source feeds an AOT compiler that emits C. The compiler walks the React
tree at build time and emits a series of `er_node_create` / `er_node_set_props` /
`er_tree_append_child` / `er_commit` calls — the same calls Flow A's reconciler would
make at runtime, but baked into the firmware binary.

Static analysis lets the AOT path skip a lot of work Flow A does at runtime:

- Component trees that never change can be emitted as a single setup block.
- Styles that don't depend on state become string-free `ERLayoutSpec` initialisers.
- Animations with constant `toValue` / duration can resolve to fixed-curve LUT calls.

Dynamic features lost: anything that requires JS evaluation at runtime (`eval`,
`Function`, dynamic imports, fetch). React state + hooks are preserved via a small
generated state machine in C.

Flow B is scheduled after Flow A is end-to-end working. Both flows target the same
`er_scene.h` ABI; a single app could in principle ship either way without source
changes.

---

## Component & Style API

What the React developer writes. All imports come from `'embedded-react'`.

### Components

| Component                            | React Native equivalent | Notes                                                               |
|--------------------------------------|-------------------------|---------------------------------------------------------------------|
| `View`                               | `View`                  | Flexbox container, all visual styles                                |
| `Text`                               | `Text`                  | Multi-line, nested `<Text>` spans, `numberOfLines`, `ellipsizeMode` |
| `Image`                              | `Image`                 | Premultiplied ARGB8888 source, `resizeMode`, `tintColor`            |
| `ScrollView`                         | `ScrollView`            | Horizontal and vertical, momentum, `onScroll`                       |
| `FlatList`                           | `FlatList`              | Virtualised — only renders visible rows + overscan                  |
| `Pressable`                          | `Pressable`             | `onPress`, `onLongPress`, `onPressIn`, `onPressOut`                 |
| `TouchableOpacity`                   | `TouchableOpacity`      | Opacity feedback on press                                           |
| `TextInput`                          | `TextInput`             | Soft keyboard or hardware keyboard input                            |
| `ActivityIndicator`                  | `ActivityIndicator`     | Animated spinner; driven by `Animated`                              |
| `Switch`                             | `Switch`                | Toggle; animated thumb slide                                        |
| `Modal`                              | `Modal`                 | Composited on top of root; `transparent`, `animationType`           |
| `Animated.View` / `.Text` / `.Image` | same                    | Accepts `Animated.Value` in style props                             |

### Hooks

`useState`, `useEffect`, `useRef`, `useMemo`, `useCallback`, `useContext`,
`useAnimatedValue`. Context + `NavigationContainer`/`Stack.Navigator` are supported for
theme and navigation state.

### Style props

A strict subset of React Native's StyleSheet. Unknown props are silently ignored.

**Layout (Yoga):** `flex`, `flexDirection`, `flexWrap`, `flexGrow`, `flexShrink`,
`flexBasis`, `justifyContent`, `alignItems`, `alignContent`, `alignSelf`, `position`
(relative | absolute), `top` / `right` / `bottom` / `left`, `width` / `height` /
`minWidth` / `maxWidth` / `minHeight` / `maxHeight`, `aspectRatio`, `margin` (+ per-edge,
`marginHorizontal`, `marginVertical`), `padding` (+ per-edge, `paddingHorizontal`,
`paddingVertical`), `gap` / `rowGap` / `columnGap`, `display` (flex | none), `overflow`
(visible | hidden | scroll).

**View visual:** `backgroundColor`, `opacity`, `borderRadius` (+ per-corner),
`borderWidth` (+ per-edge), `borderColor`, `borderStyle`, `shadowColor`, `shadowOffset`,
`shadowOpacity`, `shadowRadius`, `elevation`, `zIndex`.

**Text:** `color`, `fontSize`, `fontFamily`, `fontWeight`, `fontStyle`, `lineHeight`,
`letterSpacing`, `textAlign`, `textDecorationLine`, `numberOfLines`, `ellipsizeMode`.

**Image:** `resizeMode` (cover | contain | stretch | repeat | center), `tintColor`.

**Transforms:** `translateX`, `translateY`, `scaleX`, `scaleY`, `scale`, `rotateZ`,
`rotate` (always available); `rotateX`, `rotateY`, `perspective` (compile-flag gated —
see appendix); `transformOrigin`.

---

## Runtime Architecture

`er_scene.h` is the engine ABI that both Flow A and Flow B target. It is pure C99,
intentionally minimal, and stable.

```c
ERNode  *er_node_create(ERNodeType type);
void     er_node_destroy(ERNode *node);
void     er_node_set_props(ERNode *node, const ERProps *props);

void er_tree_append_child(ERNode *parent, ERNode *child);
void er_tree_remove_child(ERNode *parent, ERNode *child);
void er_tree_set_root(ERNode *root);

void     er_commit(void);
uint32_t er_now_ms(void);
void     er_font_load (const char *name, const void *buf, size_t len);
void     er_image_load(const char *name, const void *argb_buf, int w, int h);

void er_anim_start(ERNode *node, ERAnimProp prop, float value, const ERAnimConfig *cfg);
void er_anim_cancel(ERNode *node, ERAnimProp prop);

void er_event_set(ERNode *node, EREventType event, EREventFn fn, void *user_data);
```

The Flow A React reconciler in `bridges/quickjs/` calls these whenever React re-renders.
The Flow B AOT compiler emits these calls statically. App authors never call them
directly — they write JSX.

### The backend interface

The entire hardware surface is one struct with five function pointers:

```c
typedef struct {
    void (*fill_rect)(uint32_t argb, int x, int y, int w, int h, void *ctx);
    void (*copy_rect)(const void *src, int src_stride_bytes,
                      int x, int y, int w, int h, void *ctx);
    void (*blend_rect)(const void *src, int src_stride_bytes, uint8_t alpha,
                       int x, int y, int w, int h, void *ctx);
    void (*wait)(void *ctx);          /* may be NULL */
    void (*frame_ready)(void *ctx);   /* called each vsync; drives the frame loop */
    void *ctx;
} EmbeddedRenderBackend;

void embedded_renderer_set_backend(const EmbeddedRenderBackend *backend);
void embedded_renderer_tick(uint32_t delta_ms);
```

The engine never includes any platform header. It only calls through this struct.
Backends live in `backends/<api>/` and implement the five callbacks once per peripheral
or rendering API.

---

## Input & Events

Touch is fed into the library via one call:

```c
typedef enum { ER_TOUCH_DOWN, ER_TOUCH_MOVE, ER_TOUCH_UP, ER_TOUCH_CANCEL } ERTouchPhase;

void embedded_renderer_touch(uint8_t finger_id, ERTouchPhase phase, int x, int y);
```

The engine hit-tests against the scene graph (the deepest node first, z-order aware) and
dispatches the React events the developer wired up in JSX: `onPress`, `onLongPress`,
`onPressIn`, `onPressOut`, `onTouchStart`, `onTouchMove`, `onTouchEnd`. Multitouch up to
five fingers; `PanResponder` is supported for drag gestures.

`ScrollView` implements momentum scrolling in C with configurable deceleration and
snap-to-offset.

---

## Animation System

The React-facing API matches React Native's `Animated` exactly:

```jsx
const opacity = useRef(new Animated.Value(0)).current;

Animated.timing(opacity, {
    toValue: 1, duration: 300, easing: Easing.ease,
    useNativeDriver: true,
}).start();

<Animated.View style={{opacity}}/>
```

**Animation primitives:** `Animated.timing` (linear / easing), `Animated.spring` (damped
harmonic oscillator), `Animated.decay` (exponential friction), `Animated.delay`,
`Animated.sequence`, `Animated.parallel`, `Animated.loop`, `Animated.stagger`.
Interpolation supports numeric, color, and angle ranges.

**`useNativeDriver: true`** is what makes animations smooth on an MCU — the
`Animated.Value` is bound directly to a C-side float in the scene graph node. On each
`embedded_renderer_tick()` the C engine advances the value and marks the node dirty. JS
is **not** entered per frame. This is the default for transform / opacity / color
animations and is effectively mandatory for hitting display refresh rates on
microcontroller-class CPUs.

`LayoutAnimation` is supported for animated layout transitions (toggling components into
view), backed by `Animated.spring` / `Animated.timing` on computed Yoga deltas.

**Frame loop.** The backend drives it — it calls `embedded_renderer_tick(delta_ms)` from
its `frame_ready` callback, or from a bare-metal `while(1)`. The library does not spawn
threads or set timers.

---

## Hardware Support

A reference backend is one C file (~50 lines) plus an optional CMake snippet. Each
backend implements the five callbacks above. Backend authors write this once per
peripheral or rendering API; React app authors never touch it. Backends live in
`backends/<api>/` — the folder name describes how the backend paints, not which chip,
so multiple chips can share a backend.

### STM32 with DMA2D (`backends/dma2d/`)

```c
#include "bsp_dma2d.h"
#include "native_renderer.h"

static void fill(uint32_t argb, int x, int y, int w, int h, void *ctx) {
    BSP_DMA2D_FillRect(argb, x, y, w, h);
}
static void copy(const void *src, int stride, int x, int y, int w, int h, void *ctx) {
    BSP_DMA2D_CopyRect(src, stride, x, y, w, h);
}
static void blend(const void *src, int stride, uint8_t alpha, int x, int y, int w, int h, void *ctx) {
    BSP_DMA2D_BlendRect(src, stride, alpha, x, y, w, h);
}
static void wait_fn(void *ctx) { BSP_DMA2D_Wait(); }
static void on_vsync(void *ctx) { embedded_renderer_tick(16); }  /* LTDC vsync IRQ */

void renderer_backend_init(void) {
    static const EmbeddedRenderBackend b = { fill, copy, blend, wait_fn, on_vsync, NULL };
    embedded_renderer_set_backend(&b);
}
```

### ESP32-S3 with PSRAM framebuffer (`backends/esp32-lcd/`)

```c
#include "esp_lcd.h"
#include "native_renderer.h"

static uint16_t *fb;  /* RGB565 framebuffer in PSRAM */

static void fill(uint32_t argb, int x, int y, int w, int h, void *ctx) {
    uint16_t rgb565 = argb_to_rgb565(argb);
    for (int row = y; row < y + h; row++)
        for (int col = x; col < x + w; col++)
            fb[row * SCREEN_W + col] = rgb565;
}
/* copy / blend: walk premultiplied ARGB8888 rows → RGB565; see appendix for the blend formula */
static void on_frame(void *ctx) { embedded_renderer_tick(16); }

void renderer_backend_init(void) {
    fb = heap_caps_malloc(SCREEN_W * SCREEN_H * 2, MALLOC_CAP_SPIRAM);
    static const EmbeddedRenderBackend b = { fill, copy, blend, NULL, on_frame, NULL };
    embedded_renderer_set_backend(&b);
}
```

### SDL2 (`backends/sdl/`)

Same shape — the callbacks wrap SDL renderer operations. Used to exercise the engine
without flashing a board and intended to host the first Flow A desktop preview. This
backend is implemented today with fill, copy, and premultiplied-alpha blend support.

### Software (`backends/software/`)

Pure CPU-blit fallback for any board without a hardware accelerator — RP2040, low-end
Cortex-M, or anything else. Same shape; the inner loops just walk pixels.

### Support matrix

| Platform               | Backend                 | Status                         |
|------------------------|-------------------------|--------------------------------|
| Linux + SDL2           | `backends/sdl/`         | Dev/test (first Flow A target) |
| STM32H7 + DMA2D        | `backends/dma2d/`       | Reference (first MCU bring-up) |
| ESP32-S3 + LCD         | `backends/esp32-lcd/`   | Planned                        |
| RP2040 / bare-metal    | `backends/software/`    | Planned                        |
| Raspberry Pi / Android | `backends/opengl/`      | Planned                        |
| Embedded Linux SBC     | `backends/framebuffer/` | Planned                        |
| WebAssembly            | `backends/web/`         | Planned                        |
| i.MX RT + PXP          | community PXP backend   | Community                      |

---

## Roadmap

**Today (engine)**

- Scene graph + Yoga flexbox layout — implemented
- Text renderer (multi-byte UTF-8, run-length glyph blits) — implemented
- Pressable/touch event dispatch — implemented (press in/out, press, long press, cancel, bubbling, multitouch, zIndex)
- Timing animations for existing color/opacity props — implemented
- Built-in Inter font at 10/12/16/20/24/32/48 px + symbol set — implemented
- Rounded rectangle rasterizer — implemented (scanline fill, border ring, anti-aliased corners, `ERUI_BORDER_AA` flag)
- Backend interface — wired; `backends/sdl/` fully implemented
- Host-side CTest (animation + input + layout + text + rendering/rrect) — green
- `examples/linux/` — pure-C SDL demo implemented; no React/QuickJS bridge yet

**Next (Flow A)**

- Finish engine: shadows, transforms, full animation engine, image scaling
- Thin `NativeUI` bridge surface around `er_scene.h`
- Metro-compatible bundler
- React reconciler hosted in QuickJS (`bridges/quickjs/`)
- End-to-end: `examples/linux/` — JSX → SDL2 desktop preview
- First MCU: `examples/esp32/` with `backends/esp32-lcd/`
- Second MCU bring-up: `examples/stm32h7/` with `backends/dma2d/`

**Later (Flow B)**

- JSX → C source AOT compiler
- Static layout / static animation passes
- Benchmarks: Flow A vs. Flow B on the same app (binary size, RAM, frame time)

---

## Non-Goals

These are deliberately out of scope.

- **MCU SDK headers** (`stm32h7xx_hal.h`, `esp_lcd.h`, etc.) anywhere in `engine/`.
  Hardware specifics live in `backends/`.
- **QSPI flash tooling, OpenOCD scripts, deploy automation** — firmware tooling, not
  engine concerns.
- **RTOS setup or task creation** — caller's responsibility. Bare-metal, FreeRTOS, and
  Zephyr all work.
- **QuickJS heap sizing** — the firmware configures heap; the library runs inside it.
- **Display initialization** (SPI, MIPI DSI, LTDC config) — backend's responsibility.
- **Network / fetch** — provided independently by the firmware if needed.
- **Web-only React APIs** — DOM refs, portals, Suspense, Server Components.
- **Pitching as a general-purpose multi-runtime UI engine.** The engine is
  runtime-agnostic by construction — Lua / JSON / visual-editor frontends *could*
  later live in `bridges/` without touching `engine/` — but `embedded-react` is React
  Native for MCUs. React is the supported developer path.

---

## Appendix A: Engine Internals

This section is for contributors to the C engine — not for React app authors.

### Yoga implementation

The layout engine implements a 7-pass Yoga-compatible flexbox algorithm in
`engine/layout/layout_engine.c`. For each container node:

1. **Pass 1 — collect in-flow children** into a `FlexChild` scratch array with
   hypothetical main/cross sizes resolved from `flex_basis`, explicit width/height, or
   intrinsic size (text measurement).
2. **Pass 2 — wrap into lines** based on `flex_wrap` and main-axis available size.
3. **Pass 3 — resolve `flex_grow` / `flex_shrink`** against per-line free space.
4. **Pass 4 — compute per-line cross-axis size** from each line's tallest child.
5. **Pass 5 — assign main-axis positions** via `justify_content`, then cross-axis
   positions via per-child `align_self` (resolved from parent `align_items`).
6. **Pass 6 — write back resolved sizes and origins**, then recurse into each child.
7. **Pass 7 — lay out absolutely positioned children** against the parent's padding box.

The scratch arrays (`s_scratch[]`, `s_line_cross[]`) are sized to `ERUI_MAX_NODES` and
declared static at module scope. They're safe across recursion because each level
finishes writing results into the scene graph before descending.

### Pixel format

All bitmap pixel data — buffers passed to `copy_rect` / `blend_rect`, internal offscreen
buffers, and images loaded via `er_image_load` — uses **premultiplied ARGB8888** (memory
order A, R, G, B; big-endian word `0xAARRGGBB` with R, G, B already multiplied by
A/255).

`fill_rect`'s `argb` parameter is the only exception: it takes **straight-alpha**
`0xAARRGGBB` (CSS-friendly) and the library premultiplies the three channels once at
call time. Backends convert premultiplied ARGB8888 to their display's native format
(RGB565, BGR888, etc.) inside the callback.

Reference blend formula (per channel C, with `a = alpha/255`, `sA = src[A]/255`):

```
out.C = src.C * a + dst.C * (1 - sA * a)
```

`src.C` is already premultiplied, so no extra `src.C * sA` multiply is needed.

### Scratch buffer pool

A subtree with `opacity < 1`, or a transformed subtree, or a shadow blur is composited
into an offscreen premultiplied-ARGB8888 buffer before being blended into the parent
surface. The scratch pool is a single static array declared once at compile time — no
heap allocation occurs during rendering.

```
pool_bytes = ERUI_SCRATCH_W × ERUI_SCRATCH_H × ERUI_SCRATCH_POOL_DEPTH × 4
```

`ERUI_SCRATCH_W` / `ERUI_SCRATCH_H` are set to the framebuffer dimensions (the largest
surface the renderer is permitted to allocate). `ERUI_SCRATCH_POOL_DEPTH` is the peak
number of simultaneously live scratch slots (`ERUI_MAX_OPACITY_DEPTH + 2` by default —
the opacity-nesting stack plus one shadow-blur slot plus one transform-rasterization
slot). The pool is sliced into equal slots at library init; no further allocation
happens at runtime.

### Compile-time feature flags

Set these in CMake before `FetchContent_MakeAvailable` (or at the IDF component level).

| Flag                      | Default                      | Effect                                                               |
|---------------------------|------------------------------|----------------------------------------------------------------------|
| `ERUI_SHADOWS`            | 0                            | Box shadow rasteriser (two-pass horizontal + vertical box blur)      |
| `ERUI_BORDER_AA`          | 1                            | Anti-aliased border-radius edges                                     |
| `ERUI_3D_TRANSFORMS`      | 0                            | `rotateX` / `rotateY` / `perspective`                                |
| `ERUI_BILINEAR_SCALE`     | 0                            | Bilinear image scaling (vs. nearest-neighbour)                       |
| `ERUI_GRADIENT`           | 1                            | Gradient rasteriser (linear)                                         |
| `ERUI_GRADIENT_RADIAL`    | 1                            | Radial gradients (requires `ERUI_GRADIENT`)                          |
| `ERUI_TRANSFORMS`         | FULL                         | Set to `TRANSLATE_ONLY` to strip rasterisation paths                 |
| `ERUI_FONT_SIZES`         | 7                            | Number of pre-rasterised font sizes to include                       |
| `ERUI_MAX_NODES`          | 512                          | Scene graph node pool size                                           |
| `ERUI_MAX_OPACITY_DEPTH`  | 4                            | Maximum nested offscreen-composite layers                            |
| `ERUI_SCRATCH_W`          | *(framebuffer width)*        | Width in pixels of one scratch slot                                  |
| `ERUI_SCRATCH_H`          | *(framebuffer height)*       | Height in pixels of one scratch slot                                 |
| `ERUI_SCRATCH_POOL_DEPTH` | `ERUI_MAX_OPACITY_DEPTH + 2` | Number of live scratch slots                                         |
| `ERUI_FONT_POOL_BYTES`    | 0                            | Static byte pool for runtime-loaded fonts; 0 disables `er_font_load` |

### Platform requirements

| Requirement                   | Notes                                                             |
|-------------------------------|-------------------------------------------------------------------|
| C99 compiler + `<math.h>`     | arm-none-eabi, xtensa-esp32-elf, riscv, x86 — all work            |
| Writable linear framebuffer   | RGB565, ARGB8888, or any format the backend translates            |
| ~256 KB flash                 | Library + default font at seven sizes; configurable subset        |
| SDRAM / large internal RAM    | QuickJS heap (Flow A); framebuffer can live in PSRAM              |
| Scratch buffer RAM (optional) | Required for shadows, off-screen opacity, rotate/scale transforms |
| RTOS                          | None required — bare-metal, FreeRTOS, Zephyr, POSIX all work      |

The library is re-entrant as long as the caller serializes calls to
`embedded_renderer_tick()`.

---

## Appendix B: Decisions

1. **Source distribution.** Ship source and public headers only — no prebuilt binaries.
   CMake `FetchContent` is the primary integration path; `idf_component.yml` wraps the
   same CMake target for ESP-IDF users. No ABI concerns on embedded; source distribution
   lets firmware builds apply their own compiler flags and feature-flag `#define`s at
   configure time.

2. **Engine is runtime-agnostic; React lives in a bridge.** The engine in `engine/` has
   no React, JS, Lua, or any frontend assumptions. `er_scene.h` is a pure C ABI; the
   React reconciler that translates JSX into `er_*` calls lives in `bridges/quickjs/`.
   This keeps Flow A (runtime reconciler) and Flow B (build-time AOT compiler) cleanly
   layered against the same C surface, and leaves the door open for non-React frontends
   later (Lua UI, JSON UI, visual editor) without forking the engine. The project's
   identity is still "React Native for embedded MCUs" — React is the supported
   developer path; other frontends are an architectural possibility, not a roadmap
   item.

3. **Bitmap pixel format.** Premultiplied ARGB8888 (memory order A, R, G, B; big-endian
   word `0xAARRGGBB` with R, G, B already multiplied by A/255) is used for all bitmap
   pixel data — `copy_rect`, `blend_rect`, internal offscreen buffers, `er_image_load`.
   `fill_rect`'s `argb` parameter is straight-alpha (CSS-friendly); the library
   premultiplies once at call time. See Appendix A for the blend formula.

4. **Scratch buffer pool.** A single static array declared at compile time —
   `ERUI_SCRATCH_W × ERUI_SCRATCH_H × ERUI_SCRATCH_POOL_DEPTH × 4` bytes — sliced into
   equal slots at library init. No heap allocation during rendering. See Appendix A for
   the pool sizing model.

5. **Gradient support.** Gradients are a renderer-owned primitive, rasterized internally
   via the existing `fill_rect` / `copy_rect` / `blend_rect` callbacks — no new mandatory
   backend callback. Linear gradients always available; radial gradients excluded with
   `ERUI_GRADIENT_RADIAL=0`; entire feature excluded with `ERUI_GRADIENT=0`.

6. **Backend folder naming is by rendering API / peripheral, not by chip.** `dma2d/`,
   `sdl/`, `opengl/`, `software/`, etc. — describes *how* the backend paints, not
   *which* chip. Multiple chips can share a backend (any STM32 with DMA2D uses
   `dma2d/`; any board without a hardware accelerator uses `software/`). The one
   exception is `esp32-lcd/` where the rendering surface is genuinely entangled with
   the chip family.
