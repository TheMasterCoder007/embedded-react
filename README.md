# embedded-react

Pure C99 scene graph, layout engine, and renderer for embedded systems — with full
**React Native API parity**.

Same component names, same style props, same `Animated` API, same flexbox layout as React
Native. A developer already familiar with React Native writes JSX and hooks; the library
handles compositing, text, images, animations, and input on any MCU that can provide a
framebuffer. Porting to a new platform means implementing four C callbacks — no renderer
logic to write.

Zero platform dependencies: pure C99, no MCU SDK headers, no RTOS, no DMA2D, no ESP-IDF.
Runs on STM32, ESP32-S3, RP2040, i.MX RT, bare-metal Linux, or any platform with a C99
compiler and a writable framebuffer.

The renderer exposes a stable pure-C scene graph API (`er_scene.h`) that can be driven
directly from C or through a thin language bridge. QuickJS is the reference bridge;
Lua, MicroPython, and bare-C are equally supported — no JS engine required.

---

## Features

### Layout
- Full Yoga-compatible flexbox — `flex`, `flexDirection`, `justifyContent`, `alignItems`,
  `alignSelf`, `flexWrap`, `position: 'absolute'`, percentage dimensions, `aspectRatio`
- `display: 'none'` — removes node from layout
- `overflow: 'hidden'` — hardware-friendly clip rect stack

### Visual Styling
- `backgroundColor`, `opacity`, `borderColor`, `borderWidth`, `borderStyle`
- **Border radius** — `borderRadius`, per-corner (`borderTopLeftRadius`, etc.),
  anti-aliased edge pixels
- **Shadows** — `shadowColor`, `shadowOffset`, `shadowRadius`, `shadowOpacity`,
  `elevation`; approximated with two-pass box blur; optional (`ERUI_SHADOWS=1`)
- **2D Transforms** — `transform: [{ translateX }, { translateY }, { scale }, { scaleX },`
  `{ scaleY }, { rotate }, { rotateZ }]`; 3D (`rotateX`, `rotateY`, `perspective`) optional
- `zIndex` — painter's algorithm sort per sibling group
- Per-node `opacity` with offscreen-buffer subtree compositing

### Animations
- `Animated.Value`, `Animated.timing`, `Animated.spring`, `Animated.decay`
- `Animated.sequence`, `Animated.parallel`, `Animated.loop`, `Animated.stagger`,
  `Animated.delay`
- `Animated.Value.interpolate` — numeric, color, and angle ranges
- `useNativeDriver: true` — runs entirely in C, zero JS per-frame overhead
- `LayoutAnimation` — animated layout transitions (spring / timing)
- Frame loop driven by backend `frame_ready` callback — no threads spawned

### Text
- Multi-line layout, `numberOfLines`, `ellipsizeMode: 'tail' | 'head' | 'middle' | 'clip'`
- Nested `<Text>` spans with mixed styles
- `fontSize`, `fontWeight`, `fontFamily`, `fontStyle`, `lineHeight`, `letterSpacing`,
  `textAlign`, `textDecorationLine`
- Built-in open-source font at sizes 10/12/16/20/24/32/48
- Custom fonts — load a TTF blob at runtime with `NativeUI.loadFont()`

### Images
- ARGB8888 bitmap source; `resizeMode: 'cover' | 'contain' | 'stretch' | 'repeat' | 'center'`
- `tintColor` colorization
- Nearest-neighbor scaling (default); bilinear via `ERUI_BILINEAR_SCALE=1`

### Components
`View`, `Text`, `Image`, `ScrollView`, `FlatList`, `Pressable`, `TouchableOpacity`,
`TextInput`, `ActivityIndicator`, `Switch`, `Modal`,
`Animated.View`, `Animated.Text`, `Animated.Image`

### Hooks
`useState`, `useEffect`, `useRef`, `useMemo`, `useCallback`, `useContext`

### Input
- Single and multitouch (up to 5 fingers)
- `onPress`, `onLongPress`, `onPressIn`, `onPressOut`
- `PanResponder` for drag gestures
- `ScrollView` momentum scrolling with configurable deceleration and snap-to-offset
- Feed touch events from any source: resistive, capacitive, or serial input

---

## Integration

```cmake
include(FetchContent)
FetchContent_Declare(embedded-react
    GIT_REPOSITORY https://github.com/TheMasterCoder007/embedded-react.git
    GIT_TAG master
)
FetchContent_MakeAvailable(embedded-react)

target_link_libraries(my_firmware embedded-react)
```

Optional feature flags (set before `FetchContent_MakeAvailable`):

```cmake
set(ERUI_SHADOWS        1)   # box shadow rasterizer
set(ERUI_BORDER_AA      1)   # anti-aliased border radius (default on)
set(ERUI_3D_TRANSFORMS  0)   # rotateX/rotateY/perspective
set(ERUI_BILINEAR_SCALE 0)   # bilinear image scaling
set(ERUI_MAX_NODES    512)   # scene graph node pool
```

---

## Backend

Implement four callbacks and one optional frame callback, then call
`embedded_renderer_set_backend()` once at startup:

```c
#include "native_renderer.h"

static void fill(uint32_t argb, int x, int y, int w, int h, void *ctx)  { /* ... */ }
static void copy(const void *src, int stride, int x, int y, int w, int h, void *ctx) { /* ... */ }
static void blend(const void *src, int stride, uint8_t alpha, int x, int y, int w, int h, void *ctx) { /* ... */ }
static void on_frame(void *ctx) { embedded_renderer_tick(16); }  // call each vsync or timer tick

void my_backend_init(void) {
    static const EmbeddedRenderBackend b = { fill, copy, blend, NULL, on_frame, NULL };
    embedded_renderer_set_backend(&b);
}
```

Reference backends for STM32/DMA2D, ESP32-S3/PSRAM, RP2040, and Linux/SDL2 are in
`backends/`.

---

## Usage Example (QuickJS bridge)

```jsx
import { View, Text, Animated, Easing, Pressable } from 'NativeUI';
import { useState, useRef } from 'NativeUI';

export default function Card() {
  const scale   = useRef(new Animated.Value(1)).current;
  const opacity = useRef(new Animated.Value(0)).current;

  // Fade in on mount
  useEffect(() => {
    Animated.timing(opacity, {
      toValue: 1, duration: 400, easing: Easing.out(Easing.quad),
      useNativeDriver: true,
    }).start();
  }, []);

  function handlePressIn() {
    Animated.spring(scale, { toValue: 0.95, useNativeDriver: true }).start();
  }
  function handlePressOut() {
    Animated.spring(scale, { toValue: 1, useNativeDriver: true }).start();
  }

  return (
    <Animated.View style={{
      opacity,
      transform: [{ scale }],
      backgroundColor: '#1a1a2e',
      borderRadius: 16,
      padding: 20,
      shadowColor: '#000',
      shadowOffset: { width: 0, height: 4 },
      shadowOpacity: 0.4,
      shadowRadius: 8,
    }}>
      <Text style={{ color: '#fff', fontSize: 20, fontWeight: 'bold' }}>
        Hello from embedded-react
      </Text>
      <Text style={{ color: '#aaa', fontSize: 14, marginTop: 8 }}>
        Border radius, shadows, and spring animations on a microcontroller.
      </Text>
      <Pressable
        onPressIn={handlePressIn}
        onPressOut={handlePressOut}
        style={{ marginTop: 16, backgroundColor: '#e94560',
                 borderRadius: 8, padding: 12, alignItems: 'center' }}
      >
        <Text style={{ color: '#fff', fontWeight: '600' }}>Press me</Text>
      </Pressable>
    </Animated.View>
  );
}
```

---

## Requirements

| | |
|---|---|
| Compiler | C99 + `<math.h>` — arm-none-eabi, xtensa-esp32-elf, riscv, x86 |
| Flash | ~256 KB for library + default font (configurable subset) |
| RAM | Language runtime heap, if using a bridge (QuickJS, Lua, MicroPython — all optional); scratch buffer for transforms/shadows/opacity |
| Framebuffer | Linear, writable; ARGB8888 or RGB565 (backend converts) |
| RTOS | None required — bare-metal, FreeRTOS, Zephyr all work |
