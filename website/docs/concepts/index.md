---
title: 'Concepts'
description: 'The mental model: one engine, two flows, backends underneath.'
---

Embedded React is three layers. Knowing what each one owns makes everything else in these docs
easier to place.

1. **Your app**, written as React Native-style JSX: `<View>`, `<Text>`, `<Pressable>`, hooks,
   `StyleSheet`, `Animated`.
2. **One C99 engine** that owns the node tree, flexbox layout, damage tracking, rendering and touch.
   It has no opinion about who calls it.
3. **A backend** per display: the handful of pixel operations the engine needs from your hardware,
   and the code that pushes a finished frame to the panel.

Between your app and the engine sits the question this section spends the most time on: does the
app run as JavaScript on the chip, or is it compiled to C before it gets there?

- [Two flows](./two-flows.md): React at runtime on QuickJS, or JSX compiled ahead of time to C.
  Same engine, same app, different trade-offs.
- [Engine and backends](./engine-and-backends.md): what the engine owns and what a backend has to
  provide.
- [Rendering pipeline](./rendering-pipeline.md): from a state change to pixels on the panel:
  commit, layout, damage rectangles, the render pass, present.
- [Layout](./layout.md): the Yoga flexbox subset the engine implements, and where it diverges.
- [Assets](./assets.md): how images and fonts are baked at build time and found on the device.
