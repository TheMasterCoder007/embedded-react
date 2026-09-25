---
title: 'Layout'
description: 'The Yoga flexbox subset the engine implements, and where it diverges.'
---

The engine lays out with flexbox, the same model React Native uses through Yoga. If you know how
`flex: 1`, `flexDirection: 'row'` and `justifyContent: 'space-between'` behave on a phone, you
know how they behave on a panel. This page is about the edges: what the solver implements, what
it leaves out, and what is different about a screen that is 240 pixels wide.

## The solver

`layout/layout_engine.c` is a Yoga-compatible solve per container, in the same passes Yoga
makes: collect the in-flow children with their hypothetical sizes, wrap them into lines, resolve
`flexGrow` and `flexShrink` against the free space (iteratively, so a child frozen at its `minWidth`
or `maxWidth` hands its share back to the others), compute each line's cross size, place along the
main axis for `justifyContent` and the cross axis for `alignItems`/`alignSelf`, write the results
back and recurse, then lay out absolutely positioned children against the parent's padding box.

The properties it takes:

| Group      | Properties                                                                                |
| ---------- | ----------------------------------------------------------------------------------------- |
| Size       | `width`, `height`, `minWidth`, `minHeight`, `maxWidth`, `maxHeight`, `aspectRatio`        |
| Flex       | `flex`, `flexGrow`, `flexShrink`, `flexBasis`, `flexDirection`, `flexWrap`                |
| Alignment  | `justifyContent`, `alignItems`, `alignSelf`, `alignContent`                               |
| Spacing    | `margin*`, `padding*` (each side, `Horizontal`, `Vertical`), `gap`, `rowGap`, `columnGap` |
| Position   | `position` (`relative` or `absolute`), `top`, `left`, `right`, `bottom`                   |
| Visibility | `display` (`flex` or `none`), `overflow`, `zIndex`                                        |

`width`, `height` and `flexBasis` accept a percentage of the parent's content box. So do the four
insets: a percentage on `left` or `right` is of the containing block's width, on `top` or `bottom`
of its height. Margins, padding, min/max sizes and borders take pixels only.

**Absolute positioning** follows Yoga's rules. An axis is pinned by an explicit length, a
percentage, or a pair of opposing insets (`left` and `right` together). `aspectRatio` derives the
other axis from a pinned one, and does nothing when both or neither are pinned. An axis left
unresolved sizes to the node's own content, as a flow child would.

**Text** is measured by the engine's own text layout, in the font and size the node uses, and the
measurement is memoised within a pass so a deep tree does not re-measure the same string on the
way down. Fonts are bitmaps baked at fixed sizes, so a `fontSize` that was not baked measures at
the nearest size that was. See [Assets](./assets.md).

## What is different

- **No right-to-left layout.** There is no `direction` property and no `start`/`end` edges;
  `left` means left.
- **`ScrollView` scrolls whichever axis overflows.** There is no `horizontal` prop; lay the content
  out with `flexDirection: 'row'` and it scrolls sideways. `FlatList` is an alias for `ScrollView`,
  not a virtualised list: every row is a real node that stays mounted.
- **A fixed node pool.** The engine allocates `ERUI_MAX_NODES` nodes at compile time (512 by
  default) and never more. A long list has to fit; the [Memory](../guides/memory.md) guide covers
  choosing the number.
- **`display: 'none'` keeps the subtree.** A hidden node and everything under it drops out of
  layout, rendering and hit-testing, but its nodes, props and React state survive. Its computed
  rectangle collapses to zero, so it takes no space and `onLayout` reports an empty box. This is
  the cheap way to switch between pages: build each once, then flip `display`.
- **`onLayout` reports the box in the parent's coordinates**, after the commit that laid it
  out. Layout is only current once a commit has run, so code that reads a rectangle part-way
  through a frame sees the previous commit's answer.

## Layout that moves

`LayoutAnimation.configureNext(...)` before a state change makes the next commit tween every node
whose computed rectangle changed, from where it was to where it now belongs. The tween runs in the
engine, with no per-frame JavaScript, and the config is consumed by that one commit. Nodes
appearing for the first time snap into place rather than animating from nothing. The presets are
the React Native ones: `easeInEaseOut`, `linear` and `spring`.

## Designing for small panels

A phone layout assumes a few hundred logical points of width and a system font that scales.
A panel gives you its physical pixels and nothing else: 240 of them on the smallest verified
board, 800 on the largest. Two habits carry an app across that range.

- **Read the size.** The host injects a `screen` global with `width` and `height`, and both flows
  let module-level constants derive from it. Pick paddings, font sizes and even whole layouts
  from `screen.width`, as the starter and the thermostat demo do, and one file fits every board.
  In Flow B the size is baked at build time (`--screen 240x320`), so the compiler folds those
  constants and emits only the branch that board takes.
- **Let flexbox absorb the rest.** Percentages, `flex: 1` and `maxWidth` do the same job here
  as on a phone. A card that is `width: '100%'` with a `maxWidth` fills a small panel and centres
  on a large one without a second layout.

The [simulator](../getting-started/simulator.md) renders at any size you type, so the layout can
be checked at every target before a board is involved.
