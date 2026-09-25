---
title: 'Performance'
description: "Where a frame's time goes on a microcontroller, the habits that keep an app fast, and the on-device overlay that shows which subsystem to blame."
---

A 240 MHz microcontroller with a few hundred kilobytes of fast RAM is a different budget from a
phone, but the same rules apply: know where the time goes, and spend it on pixels that changed.

## Where a frame goes

A frame has four phases, and the engine's instrumentation times each one:

| Phase   | Who        | What                                                                                                                              |
| ------- | ---------- | --------------------------------------------------------------------------------------------------------------------------------- |
| JS      | the host   | The pump: delivering touches and timers, React's render and diff, the bridge calls that push changes into the engine. Flow A only |
| Layout  | the engine | The flexbox solve and text measurement, for the parts that changed                                                                |
| Raster  | the engine | The damage pre-pass, compositing each damaged rectangle, the blits into the framebuffer                                           |
| Present | the host   | Pushing the changed rows to the panel                                                                                             |

Raster and present scale with the **area repainted**. Layout scales with what moved. JS scales with
how many components re-render and how many nodes the render changed, and on a Flow A board it is
usually the slowest phase when something is slow.

## Habits that keep an app fast

**Animate natively.** An `Animated.Value` driven by `timing`, `spring` or `decay` costs no
JavaScript per frame once started, and damages only what it moves. The same motion done with
`setState` on a 16 ms interval re-renders, re-diffs and re-commits every tick. `LayoutAnimation` for
moves, `<Dial value={animatedValue}>` for gauges, and `Animated.View`/`Image`/`Text` for everything
else.

**Keep drags out of React.** A finger on a dial produces a move event every few milliseconds. If
each one sets state, the whole component re-renders per move, and any `<Svg>` in it rebuilds its
shape tape. Measured on the ESP32-S3, re-parsing a `<Path d>` string on every move was the dominant
cost of a janky dial. The fixes, in order of effect: `<Dial adjustable>` (the drag is handled in C
and only the quantised value reaches JavaScript), `updateVector`/`updateText` for imperative updates
without a render, and local drag state with a `React.memo`'d static face.

**Make updates small.** Every pixel repainted is paid for twice, in raster and in present. A full-
width dim behind a modal costs several times an idle commit on the S3, while the same dim over just
the button is nearly free; dim the button, not the page. A `ScrollView` step repaints its whole
viewport, about 80 ms per step for a 556×326 list on the S3, so keep scrolling regions to the size
they need.

**Hide pages instead of unmounting them.** `display: 'none'` (or `visible={false}`) drops a subtree
out of layout, render and hit-testing while keeping its nodes and state. Switching back is a
repaint; a conditional render rebuilds every node in the interpreter, which is the dominant cost of
a page change in Flow A.

**Keep transformed and translucent subtrees small.** They are the only things that go through
scratch memory, and a change anywhere inside a transformed subtree repaints the subtree's whole
transformed box.

**Prefer engine widgets to re-tessellated vectors.** A `<Dial>` is one closed-form node; a stroked
`<Svg>` arc is flattened, stroked and scan-converted every time it changes. A static `<Svg>` is
cached after its first draw; a state-driven one is not.

**Mind the damage budget.** The engine tracks up to 16 disjoint dirty rectangles. A screen with
more independent updaters than that (a grid of dials) merges rectangles, and every vector or arc in
a merged rectangle re-rasterises in full. Boards with few updaters set the budget to 4 to save RAM;
a dashboard wants it at the default.

## Flow A specifics

- **One commit per frame.** The bridge batches a whole pump into one React render and one commit,
  so several animations and a timer on the same frame do not pay for several commits. This is
  automatic; the thing to avoid is doing work _between_ frames that forces extra renders.
- **The garbage collector.** QuickJS is reference-counted first, so ordinary garbage goes at once;
  mark-sweep runs for cycles, and its trigger recomputes to 1.5× the live set after each pass. On a
  board with a large external heap that means frequent walks of the object graph over a slow bus;
  `ErRuntimeConfig.gc_threshold` puts a floor under the trigger, which cut 18% off a measured
  workload on the S3. Placement (a tiered allocator) was tried and measured at about 2%, and is not
  offered.
- **Bytecode, not source.** `embedded-react build` ships QuickJS bytecode with source text and
  debug tables stripped, about 8× smaller than source, and the device never runs the parser. A
  firmware that only ever loads bytecode can drop the parser entirely with
  `-DER_BRIDGE_QUICKJS_LITE=ON`, saving about 60 KB of flash.

## Flow B specifics

There is no JS phase: a state change is a C assignment and a prop write. What remains is the
engine, so layout, raster and present are the whole frame, and the habits above about repaint area
and animation are the entire story. Timers are advanced by `er_app_tick()`, which the host calls
once per frame.

## Measuring on the device

The engine keeps a per-frame timing split and resource counters, retains the **worst frame seen**
with its whole split, and can draw it in the corner of the panel. Build the host with
`-DER_PERF_OVERLAY=1` (the ESP32-S3 example supports it directly):

```text
FRM 18.4 PK 2013.1      last frame / worst frame, ms
J6.2 L0.3 R9.1 P2.4     last frame: JS, layout, raster, present
PK J1900 L12 R80 P9     the WORST frame's split: what to blame the spike on
PKDRT 800x40 32k        the WORST frame's repainted region
VEC 3/8 IMG 5/32        vector and image slots in use, out of the pool
RST P0.4 C7.2 B22.1 S0.9 W96k   raster split: pre-pass, composite, blit, sweep + pixels written
JSS D2.1 R7.4 M3.8 C9.0 JS split: dispatch, reconcile, marshal, commit
```

The `PK` lines are the point. A frame that spikes once and recovers is invisible to an FPS counter,
but its full split and the region it repainted are retained until `er_perf_reset()`, so minutes
later you can read whether the two seconds went into JavaScript, layout, raster or the panel
transfer, and whether it went full-screen doing it. The raster line splits further into the
pre-pass (scales with the node pool), the composite (scales with damage area), the blits (write
bandwidth) and the sweep; the JS line into delivering events, React's render, marshalling into the
engine, and the commit the pump drove.

A host without the overlay can still collect the numbers: `ER_PERF_STATS=1` compiles the
instrumentation in without drawing, and `er_perf_get_last()`/`er_perf_get_worst()` read it. The
engine has no clock of its own; hand it one with `er_perf_set_clock()` or the phase times read 0.

Two other ways to see cost:

- The ESP32-S3 log prints `alive:` lines with frame counts, and both ESP32 examples log free RAM
  at boot and after start-up.
- The simulator is exact about layout and pixels but not about speed: a laptop is orders of
  magnitude faster than the target. Measure on the board.

## What is fast already

Some things that are expensive elsewhere are cheap here and worth using freely: anti-aliased
rounded rectangles and borders, gradients, opaque images (copied with no per-pixel blending, one
DMA transfer on hardware with a blitter), text in the built-in font, and any animation the engine
drives. An idle frame with nothing dirty costs a few microseconds.
