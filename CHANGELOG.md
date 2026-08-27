# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Versioning is lockstep across all distribution channels (npm, GitHub Release,
ESP-IDF Component Registry, PlatformIO) — a single version drives every artifact.
See the README for the release process.

## [Unreleased]
### Added

- Added `<Dial>`, a native arc widget. Dials, gauges, and progress rings are now one engine node
  instead of a hand-built `<Svg>`. It draws a track, a value indicator, an optional backing band and
  segment gaps, and a knob — a circle, an image, or any child you want anchored to the value. The value
  is animatable, so a ramp runs in the engine with no JS per frame, and `adjustable` gives you
  drag-to-set handled entirely in C. Available in both flows.
- Dual-setpoint dials. `range` gives a dial two ends with a knob on each, for something like a
  thermostat's AUTO band. `minSpan` keeps the pair a set distance apart, pushing the far end along
  instead of stopping dead. `onChange` reports both values.
- `onValueChange` on `<Switch>` now works in Flow A. It had only ever fired in AOT builds.
- Per-phase raster timings. The perf overlay's RASTER number now splits into the four things it's
  made of — damage pre-pass, compositing, backend blits, and the dirty-flag sweep — so a slow frame
  names a culprit instead of a phase. A new `blit_px` counter shows how many pixels actually reached
  the backend, which against `dirty_px` gives you the frame's write amplification. Two new overlay
  lines (`RST` for the last frame, `PKR` for the worst), so `ER_PERF_OVERLAY_LINES` is now 7. All of it
  still compiles out with `ER_PERF_STATS=0`.

### Changed

- A rotated or scaled node that is too big to transform no longer costs trigonometry on every idle
  commit. The damage pass built the transform matrix and its bounding box first and only then asked
  whether the node could be transformed at all, throwing the work away when the answer was no.
- The engine no longer draws layers that something opaque covers. Painting was strictly bottom-up, so
  every layer inside a repaint region was drawn even where an opaque one buried it — a page background
  over a wallpaper over a root cost three fills where one would do. Measured on a 800x480 ESP32-S3
  with a page stacked over two full-screen layers: a full repaint drops from 95 ms to 33 ms, and a
  single row update went from 8.2 ms to 4.3 ms.
- Changing only a node's layout props no longer repaints its box. The node still looks the same, so
  the repaint now covers what the layout pass actually moved. A layout tweak on a full-screen container
  that lands every child back where it was used to damage the whole screen; on the same S3 panel that
  frame goes from 90 ms to 4.4 ms, and the screen updates 3.6x as often.
- Screens with many small independent updaters repaint far less. Damage tracking kept only four
  disjoint rects per frame, so a grid of dials cascade-merged toward one big box — and since a vector
  or arc node repaints wherever its background was erased, every dial then re-rasterized in full. The
  budget is now 16 (`ER_DAMAGE_RECTS_MAX`). Measured on an 800x480 ESP32-S3 with twelve dials all
  stepping at once: 12x fewer pixels repainted and 4.5x less rasterizing per frame. The two smallest
  no-PSRAM boards (CYD, RP2040) keep the old budget — they have a handful of updaters and no RAM to
  spare — so their builds are byte-for-byte unchanged.
- `<Svg>` shapes repaint much faster. Every render pass used to flatten, stroke, and sort a node's whole
  op-tape before the clip was applied — once per damage rect it straddled. Shapes now drop out against the
  clip before any of that, edges outside it never enter the list, the edge sort is a counting sort, and a
  stroke's smooth corners fold into the segments instead of each drawing its own join. Measured 17–35%
  faster on a full repaint and 35–85% on a damage-rect one, with output unchanged bar a subpixel corner.
- `<Svg>` arcs are much faster now. A plain `<Arc>` or `<Circle>` now goes through the same analytic
  rasterizer `<Dial>` uses rather than being tessellated, so the two render identically at a fraction of
  the cost. Shapes the fast path can't express exactly — a filled partial arc, a square cap, a gradient
  stroke, an arc joined to another segment — still take the old route, so nothing renders differently
  than it describes.
- Touch moves are coalesced to one per frame. Host report moves faster than frames, and under Flow A
  each one used to trigger a React render for a position already stale by the time it painted. The engine
  now dispatches only the newest move per finger, at the frame boundary. Down, up, and cancel are never
  coalesced, and a move that repeats the last position is dropped entirely, so a finger held still costs
  nothing. Nothing changes for a host — keep calling `embedded_renderer_touch()` for every sample. Two new
  entry points come with it: `embedded_renderer_flush_touch()` to dispatch pending moves immediately, and
  `embedded_renderer_set_touch_coalescing(false)` to get every point back for something like freehand
  drawing.
- The thermostat demo's dial is a `<Dial>` now, in both flows. That removed about 800 lines of op-tape
  building, per-touch repainting, and hand-expanded trig.

### Fixed

- A `<Modal>` with a scale or rotate transform scrimmed only its own box instead of the page behind it,
  and left that scrim behind when it closed. The backdrop covers the whole screen, but a transformed
  modal was measured down a path that never knew that. A zoom entrance settling at scale 1.0 was enough
  to hit it.
- A scaled or rotated view too big for the transform scratch — or sitting inside another transformed
  view — repainted its whole region on every commit, forever. It renders untransformed in that case,
  and damage tracking didn't know.
- A shadow on one of those views was left behind when it moved, and clipped at its new position. Only
  the view's own box was repainted, and a shadow is drawn outside it.
- A shadow was left on screen when its view was unmounted, hidden with `display: none`, or simply had
  its shadow turned off. Only the bare box was erased in each case; the shadow is drawn outside it.
- Taps on one of those views missed it. It draws untransformed, but touches were still mapped through
  the transform, so the area that answered didn't overlap the area you could see.
- `translateX`/`translateY` on an `<ActivityIndicator>` did nothing — the spinner stayed at its layout
  position while taps and repaints went to the offset one, and it repainted both spots on every frame
  forever.
- A spinning `<ActivityIndicator>` carrying any transform repainted the whole screen on every frame.
  Its spin angle lives in the same field as `rotate`, so damage tracking read it as a real rotation and
  gave up on bounding it. On an 800x480 panel that was ~10 fps — slow enough that the dots aliased
  against their own spacing and appeared to turn backwards.
- Unmounting a component mid-animation could hand its animation to an unrelated one. Node slots are
  recycled, and a destroyed node's animations kept running against whatever took its slot — an
  `<ActivityIndicator>` that disappeared while spinning left a plain `<View>` rotating on its own.
  Property animations, animation groups and `LayoutAnimation` are all cleaned up now.
- Taps on a view with `rotateX`, `rotateY` or `perspective` were matched against its flat layout box
  instead of the shape you see, so the parts leaning toward you didn't answer and empty space beside
  them did. (`ERUI_3D_TRANSFORMS` builds only.)
- A change to anything inside a rotated or scaled view didn't show up. The view's contents are drawn
  into a scratch buffer and mapped onto the screen. However, the damage tracker measured the changed child
  where it was laid out rather than where the mapping puts it, so the repaint missed the pixels
  entirely, and the change simply stayed invisible.
- Adding or removing a node under a see-through parent — a translucent background, a faded group, the
  soft edge of a rotated view — darkened it. Only the immediate parent was repainted, so it composited
  over the old pixels instead of a fresh background.
- Toggling `overflow` on a view left its children's overflowing pixels wrong. Hiding kept them on
  screen; showing never painted them back. Only the view's own box was ever repainted.
- A child that laid out past its faded parent's box vanished. `opacity` composited the subtree
  through a scratch region bounded by the parent's own rect, which cut the child out entirely — and
  the same child could later reappear at full opacity, unfaded, on a partial repaint.
- A view with a shadow that moved left its old shadow behind and had its new one clipped. Only the
  layout box was repainted, and a shadow is drawn outside it.
- An `<Svg>` path that closes without returning to its start left that closing side unstroked. The fill
  closed the shape, so a ring sector came out with one bare radial edge and a notch at the corner.
- A dial's conic gradient didn't re-color until you released the drag.
- A dial with a center readout couldn't be dragged; the readout swallowed the touch.
- A fast drag could jump to the wrong end of the dial.
- A press handler that unmounted its own node left the following `onPress` dispatch working from a stale
  node slot. The press chain now re-checks the node between callbacks.
- The Flow B AOT compiler no longer declares a C slot for a `useRef` the generated code never
  touches, so a ref that only holds a JS value (a callback, say) stops producing an
  `unused variable` warning in every consumer build.
- The shipped ESP32-S3 example built with an `unused variable` warning from the engine compositor. The
  3D-transform state was declared under a weaker `#if` than every use of it, so a build with
  `ERUI_3D_TRANSFORMS=0` never read it. (GCC device builds only — clang doesn't flag it.)
- `er_get_dirty_rects()` / `er_get_dirty_rect()` reported nothing on Flow A. React commits inside
  `er_runtime_pump()`, so the host's own `er_commit()` — a no-op — was wiping the answer before the
  host could read it. A commit that paints nothing now leaves the last painted rects in place.
- `er_get_dirty_rect()` reported nothing on a full repaint that no node had dirtied while `er_get_dirty_rects()` 
  reported the whole screen. Both now report the region actually painted.
- Adding or removing a transform on a view left a rotated or scaled child inside it half-drawn. Only
  the outermost transform is rendered through the scratch buffer, so a change up top silently moves
  the child between its transformed shape and its plain box — and the repaint, which never expected
  it to move, stayed on the old one. The frame did not recover on its own.

## [0.12.0] - 2026-08-19
### Added

- `display: 'none'` hides a subtree without unmounting it. The node and everything under it drop out
  of layout, rendering, and hit-testing. However, the native nodes stay allocated with their props intact,
  so showing the subtree again is a repaint rather than a rebuild (build each page once, then flip between 
  them, instead of tearing down and recreating a few hundred nodes in interpreted QuickJS on every page change). 
  `visible`, which was listed as a passthrough prop but did nothing outside `<Modal>`, is now an alias for it:
  `visible={false}` means `display: 'none'`, with an explicit `style.display` winning over it (on
  `<Modal>` it keeps its existing meaning). Works in both flows — Flow A through `style` or the prop,
  Flow B as a static or state-driven style key — and is declared in the TypeScript types.

### Fixed

- Hiding a subtree with `display: 'none'` left pixels on screen when any descendant painted outside
  the hidden node's own box (an absolutely positioned child of a small container, for example).
  Layout stops maintaining a hidden node's descendants, so the damage pre-pass read them as
  unchanged-and-in-place, and they contributed nothing to the repaint; their footprint is now banked
  when the subtree is hidden, the same way a removed node is.

- A hidden subtree is no longer processed every frame. Props set on a node the paint walk prunes —
  which happens continuously, since React keeps rendering a cached page — used to leave dirty flags
  that nothing could ever clear, so the page re-damaged its own rect on every commit for the rest of
  the run. A hidden page now costs nothing per frame and reports no dirty rect.

- `display: 'none'` collapses the hidden node's computed rect, so `onLayout` reports the empty box it
  actually occupies instead of the one it had when it was last visible.

- Pixels vacated by a removed, destroyed, or hidden node are now reported through
  `er_get_dirty_rect()` / `er_get_dirty_rects()` as well as repainted. The node that owned them is
  gone from the render walk, so nothing contributed them to the reported rect, and a host that
  transfers only that rect left the stale content on the panel.


## [0.11.1] - 2026-08-18
### Fixed

- The STM32 DMA2D backend did not compile in 0.11.0 — `wait_pending()` was missing the brace closing
  its polling branch, so every function after it parsed as a nested definition. The poll now waits on
  a named mask (`DMA2D_ISR_DONE`: transfer complete, transfer error, or configuration error) rather
  than on every interrupt flag, so a refused transfer still ends the wait and an unrelated flag
  cannot end it early.

## [0.11.0] - 2026-08-18
### Added

- Opaque images now render through a copy fast path instead of per-pixel blending. Every image is
  scanned once at registration; if no pixel is transparent, repaints replace destination pixels
  outright — no framebuffer read-modify-write and no blend math — which is the common case for
  full-screen backgrounds that previously paid the full ~4 B/px blend cost on every repaint.

- Images can be baked as 16-bit RGB565 (`er_image_load_rgb565()`, or per-image
  `images: { name: { format: 'rgb565' } }` in `assets.config.js` for the JS pipeline): half the
  flash footprint and half the source-read bandwidth of ARGB8888. RGB565 is for fully opaque art —
  the baker rejects sources with transparency — and always uses the opaque copy path. Asset packs
  that use it are written as ERPK version 2 (with 4-byte-aligned pixel records); packs without
  RGB565 images still emit version 1, so existing firmware keeps loading them.

- Backends can take opaque image blits in the image's native format: a new optional
  `copy_rect_fmt` callback on `EmbeddedRenderBackend` receives the source buffer, its pixel format
  (`ERImageFormat`, now part of the public backend header), and the engine's guarantee that every
  pixel is opaque — one whole-rect call, no per-pixel alpha inspection needed. On DMA2D-class
  hardware that is a single M2M_PFC transfer; the bundled ESP32 RGB LCD backend implements it as a
  straight row `memcpy` on its RGB565 framebuffer. Without the callback the engine keeps the CPU
  expansion fallback, so existing backends are unaffected. This closes the gap where a 1:1 RGB565
  background decayed into per-scanline CPU conversion plus one backend call per row — on a
  format-aware backend a full-screen 16-bit background now costs the same single transfer as the
  ARGB path, instead of ~1M conversions and one row blit per scanline.

- The STM32 DMA2D (Chrom-ART) backend is implemented — `backends/dma2d/` is no longer a stub.
  Opaque fills become register-to-memory transfers, opaque copies become memory-to-memory with
  pixel-format conversion (including `copy_rect_fmt`, so an RGB565 image is converted by the
  peripheral's PFC in a single whole-rect transfer), and everything with alpha stays on a CPU
  source-over compositor — DMA2D's blender expects straight-alpha foregrounds while the engine
  emits premultiplied ones, so hardware blending would darken every anti-aliased edge. The
  backend is SDK-free: it carries its own register map (identical on F4/F7/H7/U5) and takes the
  peripheral base address in its config, so it drops into bare-CMSIS, HAL, or service-owned
  firmware alike, with optional hooks for interrupt-driven start/wait and Cortex-M7 D-cache
  maintenance and a `min_dma_pixels` floor below which tiny ops stay on the CPU. Framebuffers may
  be ARGB8888, RGB888, or RGB565, tightly packed or row-padded. Link it as the CMake target
  `er-backend-dma2d`.

- For STM32 LTDC targets, `backends/dma2d/README.md` now documents how to keep static full-screen
  art on a second LTDC layer so the engine (and the bus) never re-blit it at all, plus the
  page-flip loop (`er_set_display_buffer_count` + `er_dma2d_backend_take_dirty`/`set_framebuffer`/
  `er_display_present`) for double- and triple-buffered panels.

- The `copy_rect_fmt` opaque native-format blit is now implemented in the `esp32-spi-lcd`,
  `pico-spi-lcd`, and `sdl` backends, so every example gets the fast path — not just the
  ESP32-S3 RGB board.

- Running out of vector storage slots (`ERUI_MAX_VECTOR_NODES`) now says so in release builds. A
  `<Svg>` that cannot get a slot holds no geometry and draws nothing. Because slots go out in
  mount order, *which* shapes vanish moves around as screens mount and unmount — on a panel that
  looks like random glitching rather than a pool that needs raising, and it previously cost a full
  debugging cycle to trace. The first refusal now prints one `stderr` line (once per process, even
  under `NDEBUG` — unlike the other vector pools, whose warnings are debug-only) and raises a flag
  the perf overlay shows as `!FULL` on its slot line (`VEC 8/8!FULL`), readable by hosts as
  `ERPerfFrame::vector_slots_overflow`. The marker appears only once a node has actually been
  turned away, since a screen that exactly fills the pool renders fine. `-DERUI_VECTOR_STORE_WARN=0`
  drops the `stderr` line (and with it any `<stdio.h>` dependency) while keeping the flag.

### Changed

- The ESP32-S3 example's on-screen perf overlay now shows only the four host metrics (FPS / CPU /
  PSRAM / IRAM) by default. The engine's frame diagnostics — the FRM/PK timing split, PKDRT repaint
  region, and VEC/IMG slot counters — are a debugging aid and are compiled out with the
  instrumentation that feeds them (`ER_PERF_STATS=0`) unless the firmware is built with
  `idf.py -DER_PERF_DETAIL=1`.

### Fixed

- A `<Modal>` whose style did not cover the whole screen drew its backdrop only behind itself: the
  rest of the page was never dimmed, and closing the modal left the dimming that had been drawn.
  The backdrop always covers the screen, so it is now tracked that way. Modals styled to fill the
  screen — which is what the demos and the docs show — are unaffected.

- Borders with a per-edge width or color, and dashed or dotted borders, ignored `borderRadius` and
  squared off every corner: they were drawn as four straight rectangles, which then overhung the
  rounded background. They now follow the same corners as the rest of the node, and a dash pattern
  is stepped around the perimeter, so it flows through the corners as one continuous run instead of
  stopping at each one.

- A border with a different color per edge changed color on a hard horizontal step in each corner,
  because the top and bottom edges claimed their full width. Adjacent colors now meet on a miter
  through the corner, the way they do on the web. The seam is anti-aliased like every other edge
  the renderer draws.

- A View with a border and no `backgroundColor` rendered as a solid block of the border color
  instead of an outline. The border was painted by filling the whole shape and covering the middle
  back up with the background — which does nothing when there is no background to paint. Borders are
  now stroked as a ring whenever the background can't hide them, so transparent, translucent, and
  gradient-backed nodes all keep what's behind them. Opaque backgrounds are unaffected.

- Gradient backgrounds are now clipped by `borderRadius`. A gradient painted its full rectangle, so
  its square corners poked out past the node's rounded ones — unnoticeable at a radius of 4, plainly
  visible by 16. It now stops at exactly the edge a solid `backgroundColor` would, anti-aliased
  corner fringe included.

### Added

- Frame instrumentation in the perf overlay (`engine/include/er_perf.h`). Each frame is now split
  into JS/commit, layout, raster, and present, alongside counters for dirty-rect area and vector /
  image slot usage — so a frame that spikes can be blamed on the right subsystem instead of guessed
  at. The worst frame is kept with its full breakdown until you reset it, which is what makes a
  one-off spike diagnosable after the fact. Wired up in the ESP32-S3 example and the desktop
  simulator; gated by `ER_PERF_STATS` (defaults to `ER_PERF_OVERLAY` unless overridden by the build).

### Changed

- The image registry is now sized by `ERUI_IMAGE_REGISTRY_MAX` (`-DERUI_IMAGE_REGISTRY_MAX=n`, or
  appended to `COMPILE_DEFINITIONS` on the ESP-IDF path), and the default rises from 32 to 128. It
  was a bare `#define` with no way to change it from a build, so an icon-heavy app that baked more
  than 32 images simply lost the ones registered last — no crash, no layout change, just a hole
  where the art should be. Slots cost ~80 B each on a 32-bit target, so the new default is ~10 KB of
  `.bss`; nothing scales with it per frame, and the two RAM-tight examples pin it lower rather than
  absorb that. The macro was renamed `IMAGE_REGISTRY_MAX` → `ERUI_IMAGE_REGISTRY_MAX` to match every
  other tunable; it lives in a private engine header, so no public API changes.

- Damage tracking now keeps up to `ER_DAMAGE_RECTS_MAX` disjoint dirty rects per commit instead of
  one bounding box, so two widgets updating in opposite corners repaint two small areas rather than
  the whole span between them. Display drivers that can flush multiple windows can read the rects
  via the new `er_get_dirty_rects()`; `er_get_dirty_rect()` still returns the covering box.

## [0.10.2] - 2026-08-03
### Fixed

- `native_ui_bridge.c`'s `apply_props` no longer probes all ~90 known prop names (`JS_GetPropertyStr`
  per name — a C-string hash + atom intern + lookup) against every `setProps` object regardless of
  how many keys it actually has. It now walks the object's own enumerable keys once via
  `JS_GetOwnPropertyNames` and dispatches each into a pre-interned atom table, so cost scales with
  the object's own key count instead of the full known-prop surface. A host microbenchmark
  (200k calls) measured ~7.6µs → ~3.0µs per `setProps` call for a typical 5-key style object (~2.5x),
  and ~7.1µs → ~3.7µs for an 18-key object (~1.9x) — narrower than the sparse case since the atom
  lookup cost still scales with the object's own key count, not the constant 90-name probe it
  replaced.

- `commitUpdate`'s JS-side prop marshaling no longer flattens a node's `style` redundantly. A
  `<Text>` update used to flatten the same style object up to four times and walk its children tree
  twice (once each in `splitAnimatedStyle`, `buildProps`'s prop bag, `buildProps`'s text-content
  build, and `buildTextSpans`) before a single value ever reached the bridge — costly JS/bytecode
  work on an interpreted MCU target, separate from and in addition to the prop re-serialization
  fixed below. The style is now flattened once per commit and threaded through to every consumer
  that needs it.

- Re-rendering a component no longer re-serializes every host node's props across the JS→C bridge
  when nothing actually changed. The reconciler's `prepareUpdate` used to rubber-stamp every update
  (`return true`), so a single state change re-marshaled the style/props of every re-rendered node —
  the dominant per-node cost of a Flow A update (~10 ms/node on an ESP32-S3) — even when the values
  were identical. It now diffs the old and new props by value and returns `null` when nothing
  observable changed, which makes React skip the commit for that node entirely. When something did
  change, a section-flag payload re-applies only the affected part: a new inline event handler
  re-registers the handler without re-serializing props, a `<Text>` re-uploads spans only when its
  content or style changed, and an `<Svg>` re-uploads its vector op-tape only when the tape's real
  inputs changed (shape children, `viewBox`, `width`/`height`, or the `source` artifact/box) — so a
  position-only style move, the interactive-drag hot path, no longer re-flattens declarative shapes
  every frame.

- Any state change that touched layout (a resized View, new text, a mounted/unmounted node) cost far
  more than a plain repaint, and the gap grew with how deeply nested the UI was. The flex engine
  measures each node's natural content size (a Text node's glyph run, or a container's summed
  children) before laying it out. However, that measurement was neither cached nor skipped when unneeded —
  a node D levels deep in the tree got its content remeasured D times as the layout pass descended one
  level at a time, and even a flat row of auto-sized text siblings was measured twice per commit. The
  layout pass now measures each node's content at most once per pass. It skips the measurement
  entirely when a node's size is already fully pinned (explicit width and height, or driven by
  `flexBasis`/percentage/`aspectRatio`) — for the common case of mostly fixed-size embedded layouts,
  the text-measuring work drops to zero.

## [0.10.1] - 2026-07-31
### Fixed

- Apps running the JavaScript runtime on a bare-metal board could run out of memory over time, even
  when the app itself was correct. QuickJS only cleans up unused JavaScript objects once it believes a
  certain number of bytes are in use, and it works that number out by asking the system how big each
  allocation was. On Mac, Windows, and Linux it gets a real answer; on a bare-metal chip (and inside the
  browser simulator) it silently got zero, so the cleanup never ran and memory use only ever grew —
  on one STM32 board, a few kilobytes per screen redraw until the board ran out. The same gap also
  meant the `memory_limit` setting could not actually cap the JavaScript heap. The bridge now supplies
  its own allocator that always reports real sizes, so cleanup and the memory cap work on every
  board without any host changes. Boards that supply their own allocator are unaffected and are now
  checked at start-up: if theirs cannot report sizes, a clear warning is logged instead of the problem
  going unnoticed.

### Added

- `er_runtime_gc_accounting_ok()`, so firmware can check at boot whether its JavaScript heap
  cleanup and memory cap are working, and refuse to start or show it on screen if not.

## [0.10.0] - 2026-07-30
### Added

- Gradients on hand-written `<Svg>` shapes. Previously only an imported .svg artwork file could carry
  one, so a shape you wrote yourself was always a flat color. A `fillGrad` / `strokeGrad` prop now works
  on `<Arc>`, `<Path>`, `<Circle>` and the rest, in both the JavaScript runtime and the compiled-to-C
  build, and inherits through `<G>`. In the compiled build the gradient may be driven by state — the
  thermostat's Auto band uses that for its warm-to-cool sweep, which follows the two setpoints as you
  drag them — and folds to a constant table when nothing about it changes. It may also be applied
  conditionally (`cond ? { … } : null`), so a shape can be gradient-filled in some states and a flat
  color in others.
- Redesigned the thermostat demo with a new cleaner, fully responsive design
- New example for the Waveshare RP2040-Touch-LCD-1.69, a small round-corner touch board. This is the
  smallest board supported so far: an RP2040 with 264 KB of memory and no PSRAM. It drives the 240 by
  280 screen and the capacitive touch panel, and runs the new watch-face demo. Tested building,
  flashing, and running on real hardware.
- New display backend for small microcontrollers that drive an SPI screen. It keeps one screen-sized
  image in memory and sends only the part that changed to the screen each frame. It is plain, portable
  C, so any board with an SPI display can reuse it by supplying two small functions.
- New watch-face demo with two pages you swipe between: a digital watch face (clock, day and date,
  battery, heart rate, and a live step counter) and a bubble level (a dot that rolls as you tilt the
  board, turning green when the board is level). It compiles ahead of time to C, so it runs on boards
  that have no JavaScript runtime.
- A new way to feed live device data into an app that is compiled ahead of time. You mark a value in
  the app with useHostValue, and the build generates a small setter function your device code calls to
  update it, for example, a step count or a sensor reading. This needs no engine changes and no
  JavaScript runtime.
- Real motion sensing on the RP2040-Touch-LCD-1.69 example. The board's accelerometer now drives a real
  step counter (using a simple step-detection algorithm) and the bubble level (using the direction of
  gravity). Both values reach the screen through useHostValue.
- Start a new project from a demo. `npm create embedded-react@latest my-app -- --template <name>` scaffolds
  a full example app (e.g. `thermostat`, `watch-face`) instead of the minimal starter, so you can build on
  a real UI or try one on your hardware right away; `--list` shows what's available. The demo apps are now
  self-contained projects wired to the `embedded-react` CLI.
- `embedded-react build --aot --screen <WxH>` bakes a target panel size into the ahead-of-time build, so a
  responsive app compiles to the layout that board actually renders (e.g. `--screen 240x320` for a small
  no-PSRAM display).

### Changed

- The Waveshare 7-inch ESP32-S3 example now keeps a single screen buffer instead of rotating three, which
  makes dragging noticeably smoother — on the thermostat dial, 15-16 frames per second before and 20-26
  after. Rotating buffers were costing more than they saved: each one remembers separately what it still
  needs redrawn, so a frame had to repaint its own changes plus the last two frames' as one combined area,
  and it had to rebuild that whole area from scratch rather than blending just what changed. With one
  buffer the area is smaller and the work per pixel is lower. The tradeoff is that drawing now goes into
  the buffer being shown, so a very large, very fast redraw could show a torn edge; dragging, theme
  switches, and opening and closing the settings sheet were all checked on hardware and are clean. A board
  that does tear can set the buffer count back to three, documented where it is set.
- Updated the examples and backends README tables to list what is actually implemented, and added rows
  for the new RP2040 example and its display backend.
- Rewrote the ESP32-2432S028R (Cheap Yellow Display) example README for clarity. A short intro and a
  quick start come first, followed by a plain-language how-it-works section, a tuning table that maps
  symptoms to fixes, and the pin list. The deeper detail moved below the get-it-running steps, and a
  duplicated explanation was merged.

### Removed

- Removed the music-player demo. It was an early bring-up test for the Cheap Yellow Display and never
  grew into a real example. The thermostat and watch-face demos now cover the same ground — dynamic
  layout, animation, and a full app — and there are the two starting templates going forward.
- Removed the old thermostat demo's app code (the arc-dial climate UI, its weather panel, and their
  artwork) to make room for a redesigned thermostat demo.

### Fixed

- Setting a text label's content directly from code (the `updateText` escape hatch) no longer re-measures
  every piece of text on the screen when it cannot possibly change the layout. A label whose width is fixed
  and that is limited to one line always occupies the same space whatever it says, so only that label is
  redrawn now. Live readouts driven this way get cheaper: on the 800 by 480 thermostat, dragging the dial
  updates the center temperature every few pixels, and that was triggering a full re-measure of the screen
  on about nine of every ten frames, roughly 5.7 ms each. Labels that can still change size — content-sized
  ones, or fixed-width ones allowed to wrap onto more lines — are unaffected and re-measure as before. The
  thermostat's readouts were given fixed boxes so they benefit.
- Fixed a permanent slowdown after scrolling. Once a list had been scrolled, every later frame stayed
  expensive for the rest of the run — nothing recovered it except restarting the device, not switching
  screens, not scrolling back. Rows scrolled out of view kept asking to be redrawn where they used to be,
  forever: scrolling moves a row, so it is reported as having moved, but a row that is now out of sight is
  skipped when drawing, and only drawing updates the record of where it was last drawn. So it stayed
  "moved" and kept the changed region of every frame propped open, which in turn defeated the shortcut
  that normally lets the engine skip untouched parts of the screen. A row with nothing left on screen now 
  settles after its old area is cleaned up once and is still redrawn normally when scrolled back into view. 
  Affects every app with a ScrollView or FlatList.
- Fixed division in apps compiled ahead of time to C. JavaScript always divides as decimals, but the
  compiler emitted plain C division, which discards the fraction when both sides are whole numbers. Any
  ratio built from the whole-number state stayed at 0 until the two sides were equal and then jumped to 1 —
  on the thermostat's small-screen dial, a fill and handle that sat at the bottom of the range until the
  setpoint reached maximum. Affects every ahead-of-time app.
- Fixed vector graphics silently disappearing when a drawing is wider than the buffer the renderer
  reserves for one row of it. It drew nothing and said nothing, because the warning behind that limit is
  compiled out of release builds — the enlarged thermostat dial vanished on the Cheap Yellow Display
  while its drag still worked. The board's buffer now covers the dial, and the limit is documented where
  it is set.
- The Cheap Yellow Display example can now run rotated. A single switch in the board header turns it to
  landscape, swapping the screen size, the panel axes, and the touch axes together so they cannot end up
  disagreeing. The example still ships in the panel's native portrait.
- Fixed a blank screen in the simulator's dev server when a demo or scaffolded app carries its own
  `node_modules`. The app's React and the library's React were resolved from two different places, giving
  two copies of React — every component then failed with "cannot read property 'useRef' of null". The dev
  server now pins React to a single copy, the same way the production bundler already did. Apps installed
  normally, where there is only ever one copy, are unaffected.
- Corrected the ESP32-2432S028R example docs: fixed the display backend description to match the
  current design, fixed the documented pixel-clock speed (40 MHz, was 20), fixed the expected startup
  log line, and replaced the stale music-player description with the thermostat dial the example
  actually builds. Verified the example builds, flashes, and starts cleanly on a real board.
- Fixed the ahead-of-time (AOT) compiler so a repeating timer started from inside an effect that has
  dependencies — or from an event handler — generates C that compiles. The timer's callback is now
  declared before it is used, which the watch-face demo needs for its swipe-to-settle animation.

## [0.9.0] - 2026-07-23
### Added

- Multicore rendering (opt-in). On builds made with `ERUI_RENDER_WORKERS` above 1, a host can
  hand the engine extra render workers (`embedded_renderer_set_workers`) — the engine never
  creates threads itself — and each frame's repaint is then split into horizontal slices
  rendered concurrently, one worker per core. Scenes using vector graphics or shadows fall back
  to single-core automatically, the fade cache keeps working (reads happen in parallel; a
  refresh borrows one single-core frame), and everything else — nested fades, transforms,
  text, gradients, images — renders in parallel. The default build is unchanged and stays
  exactly single-core.

- The ESP32-S3 example renders on both cores, and its memory system now runs PSRAM and flash at
  120 MHz (up from 80). The faster bus matters beyond speed: with both cores drawing, the RGB
  panel's continuous refresh from PSRAM was starved of bandwidth, and the picture visibly
  drifted — at 120 MHz there is headroom for both. 120 MHz PSRAM is an ESP-IDF "experimental"
  feature; the example also enables vsync-based panel-sync recovery.

- Ordered dithering on RGB565 panels (`ER_LCD_DITHER`, on by default with SIMD blending). Fading
  translucent or anti-aliased content used to shimmer as pixels flickered between color levels
  frame to frame; the quantization now lands as a fine, stationary checkerboard instead, which
  also softens gradient banding. The startup self-test covers the dithered math per pixel lane.

- SIMD blending on the ESP32-S3 (`ER_LCD_PIE`, on by default). The hottest drawing operation —
  blending translucent content onto the screen — now uses the chip's 128-bit vector unit, eight
  pixels at a time, roughly halving the cost of fades on a device. A self-test at startup verifies
  the SIMD output against the plain C version and falls back automatically if it ever disagrees.
- The ESP32 LCD backend can now draw directly into the panel's own framebuffers (`ER_LCD_DIRECT`,
  on by default for unrotated RGB565 panels with 2–3 framebuffers when `on_frame_buf_complete` callbacks are available). The intermediate framebuffer
  and the per-frame copy to the panel are gone — pushing a large update dropped from 20–35 ms to
  ~2 ms on the ESP32-S3 example. With three panel framebuffers (the example's new default) there
  is always a free buffer to draw into, so frames never wait on the display. Rotated and
  single-framebuffer panels keep the previous path.

- Fades now work at any size. A translucent group bigger than the scratch buffer used to silently
  render fully opaque; the engine now composites large groups in horizontal strips, so even
  full-screen fades render correctly — using far less reserved RAM than before.
- When a group truly can't be composited (very deep nesting, or a board with compositing turned
  off), each element is now dimmed individually instead of the transparency being dropped entirely.
- New board-tuning flags: `ERUI_SCRATCH_BAND_H` (strip height — smaller means less RAM) and
  `ERUI_XFORM_W`/`ERUI_XFORM_H` (the largest element that can be rotated or scaled). Defaults leave
  existing configurations unchanged.
- A fade cache (`ERUI_FADE_CACHE_W/H`, off by default). During an opacity animation the faded
  content doesn't change — only how transparent it is — so the engine now keeps the composited
  result and re-blends it each frame instead of redrawing everything. Measured 1.3–2.8× fade
  frame rate on device (nested fades gain the most); any change to the content invalidates the
  cache automatically.

### Changed

- Compositing needs much less memory. One of the two full-size transform buffers is gone, and the
  strip pool replaces four full-size opacity buffers — on the ESP32-S3 example the compositing
  buffers shrink from ~1.35 MB to under 300 KB.
- Rendering is faster on microcontrollers: the hottest pixel loops were reworked, and the ESP32-S3
  example keeps its compositing strips in fast internal RAM.

### Fixed

- A rotating or growing element could vanish mid-animation once its on-screen footprint outgrew the
  scratch buffer. Any on-screen size now renders; only the element's own size is limited.

## [0.8.0] - 2026-07-21
### Added

- A lite JavaScript profile. The runtime starts with only the built-ins the React runtime actually
  needs, and the same set runs everywhere — device, desktop, and simulator — so an app that works in
  development works on hardware. Anything extra an app needs (Date, Proxy, typed arrays, …) can be
  opted back in per host.
- Parser-less device builds. Firmware that only runs precompiled bytecode can drop the JavaScript
  parser entirely, saving about 60 KB of flash. The error overlay still works on such builds.
- Smaller release bundles. Built apps no longer embed their source text and debug tables — the
  thermostat demo shrinks from 1.14 MB to 260 KB. Development builds keep debug info so stack traces
  keep their line numbers.
- An optional hard cap on the JS heap: a runaway app fails with a catchable out-of-memory error
  instead of exhausting the shared system heap.
- The ESP32-S3 example uses all of the above: lite profile, no parser, and a 4 MB heap cap.

### Fixed

- Demo bundles no longer break when a demo folder carries its own `node_modules`. A second copy of
  React could sneak into the bundle and make every component throw at mount; the bundler now always
  uses the package's own copy.

## [0.7.0] - 2026-07-04
### Added

- Support for page-flipped / multi-buffer displays. Some panels rotate two or more framebuffers and flip
  between them in hardware, so the buffer being drawn into is a frame or two stale — with the standard
  single-buffer assumption, moved elements ghosted and some changes never appeared. Tell the engine how many
  buffers rotate (`er_set_display_buffer_count`) and signal each hardware flip (`er_display_present`), and it
  repaints enough per buffer to keep every one correct, with no extra full-screen buffer or host-side
  copying. The default is unchanged for single-buffer displays.

## [0.6.0] - 2026-07-01
### Added

- On-device hot reload over USB. Save an edit, and it streams to a connected board and swaps in live — no
  reflashing, no rebooting, and component state is kept across reloads. It's a development feature, turned
  off by default; the web simulator stays the standard way to develop, and release builds are unaffected.
- Automatic device detection for on-device hot reload. You can start it without naming a port: ESP32 boards
  are found automatically, and any other board is given an explicit port. New projects created with the
  scaffolder include a script for it, and the tool clearly explains what to do when it can't connect.

## [0.5.3] - 2026-06-29
### Fixed

- SVG gradients render again. Linear, radial, and conic gradients on vector shapes had regressed to drawing
  as transparent in the simulators; they now paint correctly.
- The simulator dev server no longer crashes when a requested file is missing. It returns a normal
  "not found" response and stays running instead of shutting down.

## [0.5.2] - 2026-06-28
### Fixed

- The simulators now enable every engine feature at full capacity. They previously left some limits at the
  lean device defaults, so features could silently disappear (for example, gradients dropping out past a
  certain count). Real device builds still use the lean defaults.

## [0.5.1] - 2026-06-28
### Changed

- The JavaScript and TypeScript starter templates ship an updated app icon.

## [0.5.0] - 2026-06-28
### Added

- TypeScript support, end to end. You can write apps in TypeScript, scaffold a TypeScript starter, and use
  it across every workflow — the simulator (with state kept across reloads), the static export, and both
  device build paths.
- Bundled type declarations. The package ships its own types for the public API, so imports are typed out
  of the box with no extra type packages to install.

## [0.4.1] - 2026-06-27
### Changed

- Animated scaling and rotation do less work per frame, with no change to how they look.

### Fixed

- Animated 2D and 3D transforms no longer repaint the whole screen every frame; only the area that actually
  changed is redrawn.

## [0.4.0] - 2026-06-26
### Added

- Import an SVG file and render it as a live vector graphic. It works on both device paths and supports the
  common SVG shapes and nesting.
- Gradients for vector shapes — linear, radial, and conic — on both fill and stroke.
- Automatic fallback for SVG features the vector engine can't draw directly. Those files are turned into a
  baked image at build time instead of losing content, with a build warning naming what triggered it.
- Configurable memory limits for vectors, with clearer warnings when a graphic exceeds them.

### Changed

- Conic gradients render faster on the device, with no visible change to output.

## [0.3.0] - 2026-06-16
### Added

- A build command that produces the device artifact from your own project — either a bytecode bundle for
  PSRAM-class boards or ahead-of-time C for boards without PSRAM. It needs no native toolchain, and the
  scaffolder template gains a build script.

### Changed

- The build command compiles device bytecode without needing a native compiler.

## [0.2.3] - 2026-06-15
### Fixed

- Fixed a crash ("maximum call stack size exceeded") that hit newly scaffolded apps on the first run of the
  dev simulator. The state-preservation transform was being applied to the installed library as well as
  your app; it's now limited to your own source.

## [0.2.2] - 2026-06-15
### Fixed

- Hardened looping animations so an animation that finishes instantly can't recurse into its next iteration.

## [0.2.1] - 2026-06-15

Maintenance release — dependency, security, and release-pipeline fixes. No changes to the component API,
the engine, or runtime behavior.

### Security

- Updated the build-time bundler dependency to clear two security advisories. embedded-react doesn't expose
  the affected functionality, so real-world impact was low, but the dependency is now patched.

### Changed

- Pinned and adjusted development dependencies, so security audits report clean and installs are reproducible
  across platforms.

### Fixed

- Fixed the release workflow so automated npm publishing completes reliably.

## [0.2.0] - 2026-06-15

First beta.

### Added

- A browser-based simulator that runs your app with hot reload, so you can develop without any hardware.
  The prebuilt simulator ships in the package, so there's nothing extra to install.
- The embedded-react command-line tool: run your project in the simulator with hot reload, or export a
  self-contained static playground to share.
- The create-embedded-react scaffolder for starting a fresh project, published as a companion package.

### Changed

- Documentation restructured for the beta — the project now carries READMEs plus a single roadmap and a
  contributing guide.

### Fixed

- Fixed looping animations built from a sequence (the ping-pong pattern), which could overflow the stack on
  the second pass; sequences now reset correctly each time they start.

## [0.1.1] - 2026-06-14
### Fixed

- Apps that use Math.PI in the ahead-of-time build now compile on all toolchains.

### Added

- Continuous integration and one-step release automation.
- Distribution through PlatformIO and a finalized ESP-IDF component.
- License and notice files are included in every distributed package, and Install and Releasing sections in the
  README.

### Changed

- Rewrote the npm-facing README and fixed links that broke on the package page.

## [0.1.0] - 2026-06-14

Initial public release.

### Added

- The engine: a React Native-style renderer written in C, with layout, text, images, vector graphics, and
  animation.
- Flow A: run React apps directly on-device for instant iteration on PSRAM-class microcontrollers.
- Flow B: compile apps ahead-of-time to C for microcontrollers without PSRAM.
- Backends for desktop development and for ESP32 SPI displays.
- A desktop hot-reload simulator.
- Versioning foundation with a single source of truth propagated to every artifact.
- The first publish to npm as embedded-react.

[Unreleased]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.12.0...HEAD
[0.12.0]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.11.1...v0.12.0
[0.11.1]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.11.0...v0.11.1
[0.11.0]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.10.2...v0.11.0
[0.10.2]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.10.1...v0.10.2
[0.10.1]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.10.0...v0.10.1
[0.10.0]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.9.0...v0.10.0
[0.9.0]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.8.0...v0.9.0
[0.8.0]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.7.0...v0.8.0
[0.7.0]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.6.0...v0.7.0
[0.6.0]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.5.3...v0.6.0
[0.5.3]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.5.2...v0.5.3
[0.5.2]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.5.1...v0.5.2
[0.5.1]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.5.0...v0.5.1
[0.5.0]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.4.1...v0.5.0
[0.4.1]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.4.0...v0.4.1
[0.4.0]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.2.3...v0.3.0
[0.2.3]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.2.2...v0.2.3
[0.2.2]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.2.1...v0.2.2
[0.2.1]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.2.0...v0.2.1
[0.2.0]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.1.1...v0.2.0
[0.1.1]: https://github.com/TheMasterCoder007/embedded-react/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/TheMasterCoder007/embedded-react/releases/tag/v0.1.0
