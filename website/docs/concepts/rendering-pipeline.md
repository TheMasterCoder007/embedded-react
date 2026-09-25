---
title: 'Rendering pipeline'
description: 'Commit, layout, damage tracking, the render pass, and how pixels reach the display.'
---

A frame is one call to `er_commit()`. This page follows what happens inside it, from a changed
prop to pixels on the panel, because the shape of that path explains most of what makes an app
fast or slow on a microcontroller.

## One commit per frame

Changes reach the engine as prop writes on nodes: a new text string, a new colour, a child
inserted. None of that draws anything. Drawing happens when the host calls `er_commit()`, and the
whole design pushes towards **one commit per frame** however many changes arrived.

In Flow A, the bridge runs a whole frame's JavaScript inside one React batch, so three animations
ticking and a timer firing on the same frame produce one render and one commit rather than four.
In Flow B the generated C writes props directly and the host commits once per frame. Either way
the engine sees the frame's changes together.

## Inside a commit

```text
prop writes → dirty flags
     ↓
layout      the flexbox solve, and text measurement, for the parts that changed
     ↓
damage      which rectangles of the screen must be repainted
     ↓
render      composite each damaged rectangle, back to front
     ↓
present     the backend pushes the changed rows to the panel
```

**Layout.** The solver walks only what is dirty. Changing a colour does not re-lay-out anything;
changing a size re-solves that node's container and whatever depends on it. Text measurement,
which is the expensive part of layout, is memoised per pass so a deep tree does not re-measure the
same string on the way down.

**Damage.** The engine tracks up to 16 **disjoint** dirty rectangles per commit (`ER_DAMAGE_RECTS_MAX`),
not one bounding box. A dial updating in the top-left corner and a clock in the bottom-right
repaint two small areas, not the span between them. Overlapping or touching rectangles merge on
insert; past the budget, the least wasteful pair merges, so coverage is never dropped, only
coarsened. Boards with few independent updaters set the budget to 4 and save the RAM.

Damage comes from several places: a prop that affects appearance dirties the node's rectangle;
a node that moved or vacated dirties where it _was_ as well as where it is; an animated value
dirties only what it drives, so a dial's ramp repaints the swept sliver and the knob rather than
the whole dial; a hidden subtree (`display: 'none'`) contributes nothing at all.

**Render.** Each damaged rectangle gets its own clipped pass. The engine walks the tree back to
front, skips subtrees whose bounds miss the rectangle, and skips layers that a fully opaque node
above them covers. Primitives are painted straight into the framebuffer through the backend's
`fill_rect`, `copy_rect` and `blend_rect`.

Three things need an offscreen buffer first: a subtree with `opacity` below 1, a transformed
subtree, and a shadow. Those composite into statically allocated scratch buffers, then blend onto
the frame. The buffers are sized at compile time (`ERUI_SCRATCH_W`/`H`, `ERUI_XFORM_W`/`H`), and a
translucent group taller than a strip is rendered in band passes rather than failing. A
transformed subtree is the one case that cannot be banded, because a rotation reads across its
whole source, so the transform buffer caps the largest node you can rotate or scale.

**Present.** The backend gets the damaged rows. A full-framebuffer backend flushes those
rectangles to the panel, one transfer window per rectangle on drivers that support it. A banded
backend never had a full framebuffer: the engine rendered the damage as full-width strips through
a small band buffer, and the panel's own memory kept the rest of the picture.

## What this means for an app

- **Repaint area is the cost.** Raster and present both scale with how many pixels changed, so
  an app that keeps its updates small stays fast on any board. Full-screen changes are fine, but
  not sixty times a second on a 240 MHz core.
- **Animate natively.** An `Animated` value the engine drives costs no JavaScript per frame and
  damages only what it moves. The same motion done with `setState` every 16 ms re-renders, re-diffs
  and re-commits each time.
- **Keep transformed and translucent subtrees small.** They are the only things that go through
  scratch memory, and a change inside a transformed subtree repaints the whole subtree.
- **Hide pages, don't unmount them.** `display: 'none'` drops a subtree out of layout, render and
  hit-testing while keeping its nodes and state. It costs nothing per frame, and switching back is a
  repaint rather than a rebuild.

[Performance](../guides/performance.md) has numbers from real boards and the instrumentation the
engine offers for finding where a frame's time goes.
