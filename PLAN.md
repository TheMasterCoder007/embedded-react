# Plan: embedded-react — Embedded Renderer C Library

## Overview

A pure C99 scene graph, layout engine, and renderer as a standalone open-source library with
**full React Native API parity** — the same component names, the same style props, the same
animation APIs, the same lifecycle — so a developer already familiar with React Native can
target embedded hardware without learning a new UI toolkit.

The library has **zero platform dependencies** — pure C99, no MCU SDK headers, no RTOS
includes, no DMA2D, no ESP-IDF. It works on any microcontroller or embedded Linux board
that can give it a pointer to a writable framebuffer. The caller implements four function
pointers that map to however that particular platform blits its pixels: DMA2D on STM32,
PXP on i.MX RT, 2D engine on ESP32-S3, or plain `memcpy`/`memset` on anything else.

---

## What Goes in the Library

### C library (static lib, pure C99)

- Scene graph with parent/child/sibling nodes
- Yoga-compatible flexbox layout engine
- Rounded-rectangle rasterizer (border radius, anti-aliased)
- Shadow rasterizer (box shadows, approximated with horizontal + vertical box blur)
- 2D transform compositing (translate, scale, rotate via software matrix)
- Clipping / overflow:hidden via a stack-based clip rect
- Z-index painter's-algorithm sort during compositing
- Alpha compositing — per-node opacity with offscreen-buffer subtree compositing
- Image scaling — nearest-neighbor (fast) and bilinear (quality flag)
- Language bridge — thin adapter over the stable pure-C scene graph API; QuickJS is the reference implementation (`src/bridges/quickjs/native_ui_bridge.c`); other runtimes (Lua, MicroPython, bare C) provide their own adapter without forking the renderer
- Glyph rasterization, multi-line text layout, ellipsis truncation
- Default font (Inter or Roboto, open source), sizes 10/12/16/20/24/32/48
- Custom font support — pass a TTF blob at runtime via the bridge
- Animation engine — Animated.Value, timing, spring, decay, interpolation, driven by vsync

The C library includes **no platform headers**. It only uses `<stdint.h>`, `<string.h>`,
`<stdlib.h>`, and `<math.h>` from the C standard library — which are available on every
toolchain (arm-none-eabi-gcc, xtensa-esp32-elf-gcc, riscv32-esp-elf-gcc, etc.).

---

## Key Design: Render Backend Abstraction

The entire hardware interface is a single struct with four function pointers plus one
optional frame callback. The library never includes any platform header — it only calls
through this struct.

```c
// include/native_renderer.h  (public API)

typedef struct {
    // Write a solid color rectangle into the framebuffer.
    // argb: straight-alpha 0xAARRGGBB (CSS-style); the library premultiplies internally.
    // x/y/w/h are in pixels relative to framebuffer origin.
    void (*fill_rect)(uint32_t argb, int x, int y, int w, int h, void *ctx);

    // Copy a premultiplied-ARGB8888 bitmap into the framebuffer, no blending.
    // All bytes are stored in memory order A, R, G, B (big-endian word: 0xAARRGGBB).
    // src_stride_bytes: bytes per row in the source bitmap (>= w * 4).
    void (*copy_rect)(const void *src, int src_stride_bytes,
                      int x, int y, int w, int h, void *ctx);

    // Alpha-blend a premultiplied-ARGB8888 bitmap into the framebuffer.
    // All bytes are stored in memory order A, R, G, B (big-endian word: 0xAARRGGBB).
    // alpha: global opacity 0–255 applied on top of each pixel's stored alpha channel.
    // Reference blend formula (per channel C, with a = alpha/255, sA = src[A]/255):
    //   out.C = src.C * a + dst.C * (1 - sA * a)
    // src.C is already premultiplied, so no extra src.C * sA multiply is needed.
    void (*blend_rect)(const void *src, int src_stride_bytes, uint8_t alpha,
                       int x, int y, int w, int h, void *ctx);

    // Optional: block until any in-flight hardware blit finishes.
    // Pass NULL for CPU-only backends where blits are synchronous.
    void (*wait)(void *ctx);

    // Called by the backend on each vsync (or at a fixed rate for non-vsync displays).
    // The library calls embedded_renderer_tick() inside this callback to step animations
    // and redraw dirty nodes. The backend drives the frame loop — the library does not
    // spawn threads or set timers.
    void (*frame_ready)(void *ctx);

    // Passed back as the last argument of every callback.
    // Typically a pointer to your framebuffer or display driver handle.
    void *ctx;
} EmbeddedRenderBackend;

// Call once at startup, before the JS runtime starts.
void embedded_renderer_set_backend(const EmbeddedRenderBackend *backend);

// Call from inside frame_ready (or from a bare-metal loop) to advance
// animations, run the layout pass, and repaint dirty nodes.
// delta_ms: milliseconds elapsed since the last call.
void embedded_renderer_tick(uint32_t delta_ms);
```

---

## Key Design: Pure-C Scene Graph API

The scene graph is exposed as a stable, engine-agnostic C API in `include/er_scene.h`.
This is the primary integration surface. Any language runtime — QuickJS, Lua, MicroPython,
or bare C — drives the renderer by calling these functions directly.
`src/bridges/quickjs/native_ui_bridge.c` is a thin (~300-line) adapter that maps
`NativeUI.*` QuickJS globals to this C API. Other bridges do the same for their runtime.

```c
// include/er_scene.h  (stable public API — engine-agnostic)

typedef struct ERNode   ERNode;       // opaque scene graph node
typedef uint32_t        ERNodeType;   // ER_NODE_VIEW, ER_NODE_TEXT, ER_NODE_IMAGE, …
typedef uint32_t        ERAnimProp;   // ER_PROP_OPACITY, ER_PROP_TRANSLATE_X, …
typedef uint32_t        EREventType;  // ER_EVENT_PRESS, ER_EVENT_LONG_PRESS, …

typedef void (*EREventFn)(ERNode *node, const EREventData *data, void *user_data);

// Node lifecycle
ERNode  *er_node_create(ERNodeType type);
void     er_node_destroy(ERNode *node);
void     er_node_set_props(ERNode *node, const ERProps *props);

// Tree mutations — batch before calling er_commit()
void er_tree_append_child(ERNode *parent, ERNode *child);
void er_tree_remove_child(ERNode *parent, ERNode *child);
void er_tree_set_root(ERNode *root);

// Commit a batch of mutations: run Yoga layout + mark dirty nodes for next tick
void er_commit(void);

// Utilities
uint32_t er_now_ms(void);
void     er_font_load(const char *name, const void *ttf_buf, size_t len);
void     er_image_load(const char *name, const void *argb_buf, int w, int h);

// Animations (native driver — no per-frame call into the language runtime)
void er_anim_start(ERNode *node, ERAnimProp prop, float value, const ERAnimConfig *cfg);
void er_anim_cancel(ERNode *node, ERAnimProp prop);

// Events
void er_event_set(ERNode *node, EREventType event, EREventFn fn, void *user_data);
```

A minimal bare-C usage (no language runtime at all):

```c
ERNode *root  = er_node_create(ER_NODE_VIEW);
ERNode *label = er_node_create(ER_NODE_TEXT);

ERProps p = { .text = "Hello world", .fontSize = 20, .color = 0xFFFFFFFF };
er_node_set_props(label, &p);
er_tree_append_child(root, label);
er_tree_set_root(root);
er_commit();
```

Language bridge adapters map whatever calling convention their runtime requires to these
C functions:

| Runtime | Adapter | Status |
|---|---|---|
| QuickJS | `src/bridges/quickjs/native_ui_bridge.c` | Reference |
| Lua | `src/bridges/lua/er_lua.c` | Community |
| MicroPython | `src/bridges/micropython/er_upy.c` | Community |
| Bare C | — (call `er_scene.h` directly) | Always supported |

---

## Rendering Features

### Border Radius

Rounded rectangles are a first-class primitive. The rasterizer computes per-row x-extents
using the circle equation for each corner, then issues `fill_rect` / `blend_rect` calls
at pixel granularity. Per-corner radii are supported:
`borderRadius`, `borderTopLeftRadius`, `borderTopRightRadius`,
`borderBottomLeftRadius`, `borderBottomRightRadius`.

Anti-aliasing is controlled by a compile flag (`ERUI_BORDER_AA=1`). When enabled, the
edge pixels are alpha-blended using coverage estimation — no additional hardware needed.

### Shadows

Box shadows are approximated with a two-pass (horizontal + vertical) box blur over a solid
rectangle of `shadowColor`. Blur radius maps to `shadowRadius`; offset maps to
`shadowOffset: { width, height }`. `shadowOpacity` controls the pre-blur alpha fill.

On devices without spare RAM for a full-framebuffer scratch buffer, shadows can be disabled
at compile time (`ERUI_SHADOWS=0`) or limited to a reduced-resolution approximation.
`elevation` (Android-style) maps to a preset `shadowRadius` + `shadowOpacity`.

### Opacity & Alpha Compositing

Per-node `opacity` (0.0–1.0) multiplies down the subtree. A subtree with `opacity < 1`
is composited off-screen into a temporary premultiplied-ARGB8888 buffer, then blended into the parent
surface with the final opacity.

The scratch pool is a single static array declared once at compile time — no heap allocation
occurs during rendering. Its size is:

```
pool_bytes = ERUI_SCRATCH_W × ERUI_SCRATCH_H × ERUI_SCRATCH_POOL_DEPTH × 4
```

`ERUI_SCRATCH_W` / `ERUI_SCRATCH_H` are the dimensions of the largest surface the renderer
is permitted to allocate (set these to your framebuffer dimensions). `ERUI_SCRATCH_POOL_DEPTH`
is the number of simultaneously live scratch slots: opacity nesting (`ERUI_MAX_OPACITY_DEPTH`)
plus one slot per concurrently active shadow blur and one per transform rasterization. The pool
is sliced into `ERUI_SCRATCH_POOL_DEPTH` equal slots at library init; no further allocation
happens at runtime.

### Transforms

2D transforms are applied via a 3×3 matrix accumulated top-down through the scene graph.

| Transform | Cost | Notes |
|---|---|---|
| `translateX` / `translateY` | Zero — shifts blit destination | Fast path; no pixel math |
| `scaleX` / `scaleY` / `scale` | O(pixels) — nearest-neighbor resample | Moderate; flag for bilinear |
| `rotate` / `rotateZ` | O(pixels) — scanline affine | Expensive; use sparingly on slow MCUs |
| `rotateX` / `rotateY` | O(pixels) — perspective divide | Very expensive; disable with `ERUI_3D_TRANSFORMS=0` |
| `perspective` | Enables 3D matrix stack | Paired with rotateX/rotateY |

Translation is a free fast-path (just adjusts the blit target coordinates). Rotation and
scale rasterize the transformed node into a scratch buffer, then blit the result.

Platforms without spare RAM for scratch buffers can set `ERUI_TRANSFORMS=TRANSLATE_ONLY`
to compile out the rasterization paths.

### Clipping & Overflow

Each `View` with `overflow: 'hidden'` pushes a clip rect onto a stack during compositing.
All child draw calls are scissored to the intersection of the clip stack. `overflow: 'scroll'`
combines clipping with a scroll offset (see ScrollView).

### Z-Index

Sibling nodes are sorted by their computed z-index before each paint pass (painter's
algorithm). Negative z-index is supported. Same-z siblings paint in tree order (first child
behind, last child in front).

---

## Animation System

### Frame Loop

The backend drives the frame loop by calling `embedded_renderer_tick(delta_ms)` from the
`frame_ready` callback or from a bare-metal `while(1)`. The library does not create threads.

For vsync-locked hardware (LTDC, MIPI DSI with TE), call `frame_ready` in the vsync ISR
(or from the RTOS task that waits on the vsync semaphore). For polling displays, call at a
fixed interval (e.g., every 16 ms for ~60 fps).

### Animated API (JS)

The JS side mirrors the React Native `Animated` API exactly:

```js
import { Animated } from 'NativeUI';

const opacity = new Animated.Value(0);

Animated.timing(opacity, {
  toValue: 1,
  duration: 300,
  easing: Easing.ease,
  useNativeDriver: true,   // runs entirely in C, no JS per-frame call
}).start();

// Usage in style:
<Animated.View style={{ opacity }} />
```

**Animation types:**

| API | Algorithm | Notes |
|---|---|---|
| `Animated.timing` | Linear / easing curve | Bezier easing via `Easing.*` |
| `Animated.spring` | Damped harmonic oscillator | `stiffness`, `damping`, `mass` |
| `Animated.decay` | Exponential friction decay | `velocity`, `deceleration` |
| `Animated.delay` | Time-shifted start | Composable |
| `Animated.sequence` | Serial composition | |
| `Animated.parallel` | Parallel composition | |
| `Animated.loop` | Repeat count or infinite | `resetBeforeIteration` |
| `Animated.stagger` | Staggered parallel | |

**Interpolation:**

```js
const rotation = opacity.interpolate({
  inputRange:  [0, 1],
  outputRange: ['0deg', '360deg'],
  extrapolate: 'clamp',
});
```

Supports numeric ranges, color strings (`'#rrggbb'`, `'rgba(r,g,b,a)'`), and angle strings
(`'Ndeg'`, `'Nrad'`). Color interpolation uses linear RGB.

**`useNativeDriver: true`** binds the `Animated.Value` directly to a C-side float in the
scene graph node. On each `embedded_renderer_tick()` the C animation engine advances the
value and marks the node dirty — no JS call is needed per frame. This is the default for
all transform, opacity, and color animations and must be used for smooth performance on MCUs.

**`LayoutAnimation`** is supported for animated layout changes (e.g., toggling a component
into view). Uses `Animated.spring` or `Animated.timing` on computed Yoga layout deltas.

---

## Component Library (JS API)

All components are exposed via `import { ... } from 'NativeUI'`. The JS module is the
thin QuickJS bridge; the layout and rendering logic lives entirely in C.

### Core Components

| Component | React Native Equivalent | Notes |
|---|---|---|
| `View` | `View` | Flexbox container, all visual styles |
| `Text` | `Text` | Multi-line, nested `<Text>` spans, `numberOfLines`, `ellipsizeMode` |
| `Image` | `Image` | `source` buffer or URI hook, `resizeMode`, `tintColor` |
| `ScrollView` | `ScrollView` | Horizontal and vertical, momentum, `onScroll` |
| `FlatList` | `FlatList` | Virtualized — only renders visible rows + overscan |
| `Pressable` | `Pressable` | `onPress`, `onLongPress`, `onPressIn`, `onPressOut` |
| `TouchableOpacity` | `TouchableOpacity` | Opacity feedback on press |
| `TextInput` | `TextInput` | Software keyboard or serial/hardware keyboard input |
| `ActivityIndicator` | `ActivityIndicator` | Animated spinner; driven by `Animated` |
| `Switch` | `Switch` | Toggle; animated thumb slide |
| `Modal` | `Modal` | Overlay composited on top of root; `transparent`, `animationType` |
| `Animated.View` | `Animated.View` | Accepts `Animated.Value` in style props |
| `Animated.Text` | `Animated.Text` | Same |
| `Animated.Image` | `Animated.Image` | Same |

### Hooks

| Hook | Equivalent | Notes |
|---|---|---|
| `useState` | `useState` | Triggers layout + paint when state changes |
| `useEffect` | `useEffect` | Runs after paint; cleanup on unmount |
| `useRef` | `useRef` | Mutable ref; also used for `Animated.Value` |
| `useMemo` | `useMemo` | Memoized computation; skips re-render |
| `useCallback` | `useCallback` | Stable function reference |
| `useAnimatedValue` | n/a (helper) | Shorthand for `useRef(new Animated.Value(v))` |

### Context & Navigation

`createContext` / `useContext` are supported for theme and navigation state. A lightweight
`NavigationContainer` + `Stack.Navigator` is provided for push/pop screen transitions
(animated with `Animated.spring` by default).

---

## Style System

All style props are a strict subset of React Native's StyleSheet. Unknown props are silently
ignored. Props marked ⚠️ are supported only when the relevant compile flag is enabled.

### Layout (Yoga)

```
flex, flexDirection, flexWrap, flexGrow, flexShrink, flexBasis
justifyContent, alignItems, alignContent, alignSelf
position (relative | absolute)
top, right, bottom, left
width, height, minWidth, maxWidth, minHeight, maxHeight
aspectRatio
margin, marginTop, marginRight, marginBottom, marginLeft
marginHorizontal, marginVertical
padding, paddingTop, paddingRight, paddingBottom, paddingLeft
paddingHorizontal, paddingVertical
display (flex | none)
overflow (visible | hidden | scroll)
```

### Visual — View

```
backgroundColor                  # 0xAARRGGBB or CSS color string
opacity                          # 0.0–1.0
borderRadius                     # all corners
borderTopLeftRadius
borderTopRightRadius
borderBottomLeftRadius
borderBottomRightRadius
borderWidth                      # uniform
borderTopWidth, borderRightWidth
borderBottomWidth, borderLeftWidth
borderColor
borderStyle (solid | dashed | dotted)
shadowColor                      # ⚠️ ERUI_SHADOWS=1
shadowOffset                     # ⚠️ { width, height }
shadowOpacity                    # ⚠️ 0.0–1.0
shadowRadius                     # ⚠️ blur radius in pixels
elevation                        # ⚠️ maps to preset shadow
zIndex
```

### Visual — Text

```
color
fontSize
fontFamily
fontWeight (100–900 | normal | bold)
fontStyle (normal | italic)
lineHeight
letterSpacing
textAlign (left | center | right | justify)
textDecorationLine (none | underline | line-through)
numberOfLines
ellipsizeMode (head | middle | tail | clip)
```

### Visual — Image

```
resizeMode (cover | contain | stretch | repeat | center)
tintColor
```

### Transforms

```
transform: [
  { translateX: N },
  { translateY: N },
  { scaleX: N },    { scaleY: N },    { scale: N },
  { rotateZ: 'Ndeg' },
  { rotate: 'Ndeg' },
  { rotateX: 'Ndeg' },  # ⚠️ ERUI_3D_TRANSFORMS=1
  { rotateY: 'Ndeg' },  # ⚠️ ERUI_3D_TRANSFORMS=1
  { perspective: N },   # ⚠️ ERUI_3D_TRANSFORMS=1
]
transformOrigin    # default 'center center'
```

---

## Input & Events

Touch input is fed into the library via a single call:

```c
// include/native_renderer.h
typedef enum { ER_TOUCH_DOWN, ER_TOUCH_MOVE, ER_TOUCH_UP, ER_TOUCH_CANCEL } ERTouchPhase;

void embedded_renderer_touch(uint8_t finger_id, ERTouchPhase phase, int x, int y);
```

The library hit-tests the touch against the scene graph (deepest node first, z-order aware)
and dispatches the appropriate JS events: `onPressIn`, `onPress`, `onLongPress`,
`onPressOut`, `onTouchStart`, `onTouchMove`, `onTouchEnd`.

Multi-touch (up to 5 fingers) is tracked simultaneously. `PanResponder` is supported for
drag gestures, with `onMoveShouldSetPanResponder`, `onPanResponderMove`, etc.

`ScrollView` implements momentum scrolling in C with configurable deceleration and
snap-to-offset support.

---

## Language Bridge Layer

### QuickJS Bridge (reference adapter)

`src/bridges/quickjs/native_ui_bridge.c` is a thin (~300-line) QuickJS adapter over the
pure-C scene graph API. It registers the `NativeUI` global object inside a QuickJS context
and maps each call through to `er_scene.h`:

```js
NativeUI.createNode(type)                      // → er_node_create()
NativeUI.setProps(id, props)                   // → er_node_set_props()
NativeUI.appendChild(parent, child)            // → er_tree_append_child()
NativeUI.removeChild(parent, child)            // → er_tree_remove_child()
NativeUI.setRoot(id)                           // → er_tree_set_root()
NativeUI.commitLayout()                        // → er_commit()
NativeUI.now()                                 // → er_now_ms()
NativeUI.loadFont(name, buffer)                // → er_font_load()
NativeUI.loadImage(name, buffer)               // → er_image_load()
NativeUI.animateValue(id, prop, value, config) // → er_anim_start()
NativeUI.cancelAnimation(id, prop)             // → er_anim_cancel()
NativeUI.setEventHandler(id, event, fn)        // → er_event_set()
```

The React-side wrapper (`NativeUI.js`, distributed separately) translates the React
component tree into these calls. Application developers never call `NativeUI.*` directly —
they write JSX.

### Other Adapters

Any runtime that can call C functions can drive the renderer. See the bridge adapter table
in the **Pure-C Scene Graph API** section above. Each adapter is a single file that
translates the runtime's calling convention to `er_scene.h` — the renderer itself is
unchanged. Compile `ERUI_JS_BRIDGE=0` to exclude the QuickJS adapter when using a
different bridge or bare C.

---

## Platform Requirements

A platform can use this library if it provides:

| Requirement | Notes |
|---|---|
| A writable linear framebuffer | RGB565, ARGB8888, or any format the backend translates |
| C99 compiler + math.h | arm-none-eabi, xtensa-esp32-elf, riscv, x86 all work |
| Language runtime (optional, engine-agnostic) | Only needed for a language bridge; the pure-C scene graph (`er_scene.h`) works without any runtime. QuickJS is the reference bridge; Lua, MicroPython, or bare-C call `er_scene.h` directly |
| ~256 KB flash for C library + fonts | Default font at seven sizes; configurable subset |
| SDRAM or large internal RAM | QuickJS heap (configurable); framebuffer can live in PSRAM |
| Scratch buffer RAM (optional) | Required for shadows, off-screen opacity, rotate/scale transforms |

There is **no RTOS requirement**. The library is re-entrant as long as the caller
serializes calls to `embedded_renderer_tick()`. FreeRTOS, Zephyr, bare-metal, and
POSIX all work.

### Compile-Time Feature Flags

| Flag | Default | Effect |
|---|---|---|
| `ERUI_SHADOWS` | 0 | Enable box shadow rasterizer |
| `ERUI_BORDER_AA` | 1 | Anti-aliased border radius edges |
| `ERUI_3D_TRANSFORMS` | 0 | Enable rotateX/rotateY/perspective |
| `ERUI_BILINEAR_SCALE` | 0 | Bilinear image scaling (vs nearest-neighbor) |
| `ERUI_TRANSFORMS` | FULL | Set to TRANSLATE_ONLY to strip rasterization paths |
| `ERUI_FONT_SIZES` | 7 | Number of pre-rasterized font sizes to include |
| `ERUI_MAX_NODES` | 512 | Scene graph node pool size |
| `ERUI_MAX_OPACITY_DEPTH` | 4 | Maximum nested offscreen-composite layers |
| `ERUI_SCRATCH_W` | *(framebuffer width)* | Width in pixels of one scratch slot; set to your display width |
| `ERUI_SCRATCH_H` | *(framebuffer height)* | Height in pixels of one scratch slot; set to your display height |
| `ERUI_SCRATCH_POOL_DEPTH` | `ERUI_MAX_OPACITY_DEPTH + 2` | Number of simultaneously live scratch slots (opacity nesting + 1 shadow + 1 transform) |
| `ERUI_JS_BRIDGE` | 1 | Include QuickJS bridge adapter (`src/bridges/quickjs/`); set 0 when using a different language runtime or bare-C |

---

## Platform Porting Examples

Each platform provides one C file (~50 lines) and optionally a `CMakeLists.txt` snippet.

### STM32 with DMA2D
```c
// backends/stm32-dma2d/renderer_backend.c
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
static void on_vsync(void *ctx) { embedded_renderer_tick(16); }  // called from LTDC vsync IRQ

void renderer_backend_init(void) {
    static const EmbeddedRenderBackend b = { fill, copy, blend, wait_fn, on_vsync, NULL };
    embedded_renderer_set_backend(&b);
}
```

### ESP32-S3 with PSRAM framebuffer (software blit)
```c
// backends/esp32s3-psram/renderer_backend.c
#include "esp_lcd.h"
#include "native_renderer.h"

static uint16_t *fb;  // RGB565 framebuffer in PSRAM

static void fill(uint32_t argb, int x, int y, int w, int h, void *ctx) {
    uint16_t rgb565 = argb_to_rgb565(argb);
    for (int row = y; row < y + h; row++)
        for (int col = x; col < x + w; col++)
            fb[row * SCREEN_W + col] = rgb565;
}
static void copy(const void *src, int stride, int x, int y, int w, int h, void *ctx) {
    // blit premultiplied-ARGB8888 rows → RGB565 framebuffer (drop alpha, pack channels)
    const uint8_t *s = (const uint8_t *)src;
    for (int row = y; row < y + h; row++, s += stride) {
        const uint8_t *p = s;
        for (int col = x; col < x + w; col++, p += 4)
            fb[row * SCREEN_W + col] = argb_to_rgb565(p[0], p[1], p[2], p[3]);
    }
}
static void blend(const void *src, int stride, uint8_t alpha, int x, int y, int w, int h, void *ctx) {
    // blend premultiplied-ARGB8888 src over RGB565 framebuffer
    // formula: out.C = src.C * a + dst.C * (1 - sA * a)  (src.C is premultiplied)
    const uint8_t *s = (const uint8_t *)src;
    for (int row = y; row < y + h; row++, s += stride) {
        const uint8_t *p = s;
        for (int col = x; col < x + w; col++, p += 4) {
            uint32_t sA = p[0], sR = p[1], sG = p[2], sB = p[3];
            uint32_t a  = alpha;
            uint32_t cover = sA * a / 255;          // combined coverage
            uint16_t dst = fb[row * SCREEN_W + col];
            uint32_t dR = (dst >> 11) & 0x1F, dG = (dst >> 5) & 0x3F, dB = dst & 0x1F;
            dR = dR * 255 / 31; dG = dG * 255 / 63; dB = dB * 255 / 31;
            uint32_t oR = sR * a / 255 + dR * (255 - cover) / 255;
            uint32_t oG = sG * a / 255 + dG * (255 - cover) / 255;
            uint32_t oB = sB * a / 255 + dB * (255 - cover) / 255;
            fb[row * SCREEN_W + col] = ((oR * 31 / 255) << 11) |
                                       ((oG * 63 / 255) << 5)  |
                                        (oB * 31 / 255);
        }
    }
}
static void on_frame(void *ctx) { embedded_renderer_tick(16); }  // called from timer task

void renderer_backend_init(void) {
    fb = heap_caps_malloc(SCREEN_W * SCREEN_H * 2, MALLOC_CAP_SPIRAM);
    static const EmbeddedRenderBackend b = { fill, copy, blend, NULL, on_frame, NULL };
    embedded_renderer_set_backend(&b);
}
```

### RP2040 / any bare-metal MCU (software blit)
Same pattern as ESP32-S3. The `wait` callback can be `NULL` because CPU blits complete
synchronously. The `frame_ready` callback is invoked from a repeating timer or the main
loop.

### Linux / SDL2 (development and testing)
```c
static void fill(uint32_t argb, int x, int y, int w, int h, void *ctx) {
    SDL_Surface *surface = ctx;
    SDL_FillRect(surface, &(SDL_Rect){x, y, w, h}, SDL_MapRGBA(...));
}
static void on_frame(void *ctx) { embedded_renderer_tick(16); }
// ...
```
Lets the C library be exercised in unit tests and a desktop preview entirely
outside any embedded toolchain.

---

## Repository Structure

```
embedded-react/
├── include/
│   ├── native_renderer.h          # EmbeddedRenderBackend, tick/touch API (public)
│   └── er_scene.h                 # Pure-C scene graph API — er_node_*, er_tree_*, er_commit (public)
├── src/
│   ├── native_renderer.c          # Scene graph, layout, compositing, clipping
│   ├── layout_engine.c            # Yoga-compatible flexbox layout
│   ├── text_renderer.c            # Glyph rasterization, multi-line layout
│   ├── font_registry.c            # Font registration and size lookup
│   ├── font_blob.c                # Built-in font blob loader
│   ├── font_data.c                # Default built-in font data
│   ├── image_scaler.c             # Nearest-neighbor and bilinear scaling
│   ├── rrect.c                    # Rounded-rectangle rasterizer
│   ├── shadow.c                   # Box shadow rasterizer (ERUI_SHADOWS=1)
│   ├── transform.c                # 2D/3D transform matrix & rasterization
│   ├── animation.c                # Animated.Value, timing, spring, decay engine
│   ├── compositor.c               # Z-index sort, opacity, offscreen compositing
│   ├── hit_test.c                 # Touch hit-testing and event dispatch
│   ├── canvas_bindings.c          # Canvas-level draw primitives
│   └── bridges/                   # Language runtime adapters (each is ~300 lines)
│       ├── quickjs/
│       │   └── native_ui_bridge.c  # QuickJS adapter: NativeUI.* → er_scene.h (ERUI_JS_BRIDGE=1)
│       ├── lua/
│       │   └── er_lua.c            # Lua C module adapter (community)
│       └── micropython/
│           └── er_upy.c            # MicroPython C extension adapter (community)
├── CMakeLists.txt                 # STATIC target, no platform flags
├── idf_component.yml              # ESP-IDF component manifest (wraps CMakeLists.txt)
├── backends/                      # Reference backend implementations
│   ├── stm32-dma2d/
│   │   ├── renderer_backend.c
│   │   └── README.md
│   ├── esp32s3-psram/
│   │   ├── renderer_backend.c
│   │   └── README.md
│   ├── rp2040-software/
│   │   ├── renderer_backend.c
│   │   └── README.md
│   └── linux-sdl2/                # Desktop testing/dev backend
│       ├── renderer_backend.c
│       └── README.md
├── tools/
│   └── font-converter/            # TTF → font_blob.h conversion tool
├── tests/
│   ├── layout/                    # Yoga layout unit tests
│   ├── animation/                 # Animation engine unit tests
│   ├── rendering/                 # Pixel-exact rendering regression tests (Linux backend)
│   └── touch/                     # Hit-test and gesture unit tests
└── README.md
```

---

## Platform Dependency Matrix

| Platform | fill/copy/blend | wait | frame_ready | Language bridge | Status |
|---|---|---|---|---|---|
| STM32H7 + DMA2D | DMA2D hardware | DMA2D IRQ | LTDC vsync ISR | QuickJS (reference) | Reference backend |
| ESP32-S3 + PSRAM | Software (CPU) | NULL (sync) | FreeRTOS timer | QuickJS ESP port, Lua, or bare-C | Planned |
| RP2040 | Software (CPU) | NULL (sync) | Repeating timer | QuickJS, Lua, MicroPython, or bare-C | Planned |
| i.MX RT + PXP | PXP hardware | PXP IRQ | LCDIF vsync | QuickJS or bare-C | Community |
| Linux + SDL2 | SDL2 | NULL (sync) | SDL timer | QuickJS host build | Dev/test |

---

## Non-Goals (stay out of the library)

- Any MCU SDK header (`stm32h7xx_hal.h`, `esp_lcd.h`, etc.) — zero platform includes
- QSPI flash, OpenOCD, or deploy scripts — firmware tooling
- RTOS setup or task creation — caller's responsibility
- QuickJS heap sizing — firmware configures heap, library runs inside it
- Display initialization (SPI, MIPI DSI, LTDC config) — backend's responsibility
- React/TypeScript components or npm packages — separate repo
- Web-only React APIs (DOM refs, portals, Suspense, Server Components)
- Network / fetch — the JS runtime can provide this independently

---

## Decisions

1. **C library distribution**: Ship source + public headers only — no prebuilt binaries.
   CMake `FetchContent` is the primary integration path. An `idf_component.yml` wrapper
   is provided for ESP-IDF users. Rationale: no ABI concerns on embedded; source distribution
   lets firmware builds apply their own compiler flags and feature-flag `#define`s at
   configure time.

2. **Engine-agnostic bridge layer**: Expose a stable pure-C scene graph API (`include/er_scene.h`).
   All language runtimes — QuickJS, Lua, MicroPython, or bare C — drive the renderer through
   this API. `src/bridges/quickjs/native_ui_bridge.c` is the reference adapter (~300 lines)
   that maps `NativeUI.*` QuickJS globals to `er_scene.h`. Other runtimes implement their own
   thin adapter without forking the renderer. QuickJS is the default; exclude it with
   `ERUI_JS_BRIDGE=0` when using a different bridge or plain C.

3. **Bitmap pixel format**: All bitmap pixel data — bitmaps passed to `copy_rect` /
   `blend_rect`, internal offscreen buffers, and image data loaded via `er_image_load` —
   uses **premultiplied ARGB8888** (memory order: A, R, G, B; big-endian word 0xAARRGGBB
   with R, G, B already multiplied by A/255). The `fill_rect` color parameter is the sole
   exception: it takes **straight-alpha** `0xAARRGGBB` (CSS-friendly); the library
   premultiplies the three channels once at call time. Every backend converts premultiplied
   ARGB8888 → its display's native format (RGB565, BGR888, etc.).

4. **Scratch buffer pool**: A single static array declared at compile time — no heap
   allocation during rendering. Size = `ERUI_SCRATCH_W × ERUI_SCRATCH_H × ERUI_SCRATCH_POOL_DEPTH × 4`
   bytes. `ERUI_SCRATCH_W` / `ERUI_SCRATCH_H` are set to the framebuffer dimensions (the
   largest surface the renderer is permitted to allocate). `ERUI_SCRATCH_POOL_DEPTH` is the
   peak number of simultaneously live scratch slots (`ERUI_MAX_OPACITY_DEPTH + 2` by default:
   the opacity nesting stack plus one shadow-blur slot plus one transform-rasterization slot).
   The pool is sliced into equal slots at library init; no further allocation happens at
   runtime. See *Opacity & Alpha Compositing* for the full model.

5. **Gradient support**: Gradients are included in the core C library as an optional
   compile-time feature. The library rasterizes gradients internally using the existing
   `fill_rect` / `copy_rect` / `blend_rect` draw model — no new mandatory backend callback
   is required. Backends require no changes to support gradients.

   Supported:
   - Linear gradients
   - Radial gradients (optional; excluded with `ERUI_GRADIENT_RADIAL=0`)
   - Gradient stops: each stop is a straight-alpha `0xAARRGGBB` color + `[0.0, 1.0]` position
   - Clipping to rounded rectangles (re-uses the existing border-radius mask path)
   - Output is premultiplied ARGB8888, consistent with the rest of the pipeline
   - Entire feature excluded with `ERUI_GRADIENT=0`

   Gradients are a **renderer-owned primitive**, not a backend requirement. Third-party
   backends do not need to implement gradient logic.
