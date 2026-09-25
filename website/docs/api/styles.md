---
title: 'Styles'
description: 'The style properties the engine reads, colour formats, StyleSheet, and what an Animated.Value can drive.'
---

Styles are React Native styles: plain objects, `StyleSheet.create`, arrays that flatten. The engine
reads a defined set of properties and ignores the rest, and the package's types declare exactly that
set, so a property that typechecks is one that does something.

```jsx
const styles = StyleSheet.create({
  card: {flex: 1, padding: 16, borderRadius: 12, backgroundColor: '#0d2035'},
  title: {color: '#e6edf5', fontSize: 20, fontWeight: '700'},
});

<View
  style={[styles.card, active && {borderColor: '#38bdf8', borderWidth: 1}]}
/>;
```

## Layout

The flexbox properties: `width`, `height`, `minWidth`, `minHeight`, `maxWidth`, `maxHeight`,
`aspectRatio`; `flex`, `flexGrow`, `flexShrink`, `flexBasis`, `flexDirection` (including the
`-reverse` values), `flexWrap` (including `wrap-reverse`); `justifyContent`, `alignItems`,
`alignSelf`, `alignContent`; `margin` and `padding` with their `Top`/`Right`/`Bottom`/`Left`/
`Horizontal`/`Vertical` variants; `gap`, `rowGap`, `columnGap`; `position` (`relative` or `absolute`)
with `top`, `left`, `right`, `bottom`; `display` (`flex` or `none`), `overflow` (`visible`, `hidden`,
`scroll`) and `zIndex`.

**Percentages.** `width`, `height`, `flexBasis` and the four insets accept a `'50%'` string. A
percentage on `width`/`height` measures the parent's content box; on `left`/`right` the containing
block's width; on `top`/`bottom` its height. Margins, padding, min/max sizes and borders take pixels
only.

The [Layout](../concepts/layout.md) page explains the solver and where it differs from a phone.

## Paint

| Property          | Notes                                                                                                                                             |
| ----------------- | ------------------------------------------------------------------------------------------------------------------------------------------------- |
| `backgroundColor` | Animatable. **Flow B:** the alpha of a `View` background is ignored (opaque fill); pre-mix a tint                                                 |
| `opacity`         | 0 to 1, animatable. Below 1 the subtree composites through offscreen scratch; keep such nodes small                                               |
| `pointerEvents`   | `'auto' \| 'none' \| 'box-none' \| 'box-only'`. A style entry here (React Native also allows it as a prop), because that is what the engine reads |

## Borders

`borderRadius` and the four per-corner radii; `borderWidth` and the four per-side widths;
`borderColor` and the four per-side colours; `borderStyle` (`solid`, `dashed`, `dotted`). Radius
edges are anti-aliased (`ERUI_BORDER_AA`).

## Transform

```jsx
style={{transform: [{rotate: '45deg'}, {scale: pulse}], transformOrigin: [0.5, 1]}}
```

`transform` is an array of `{scale}`, `{scaleX}`, `{scaleY}`, `{translateX}`, `{translateY}`,
`{rotate}`, `{rotateX}`, `{rotateY}`, `{rotateZ}` or `{perspective}`. Rotations are CSS angle strings
(`'45deg'`, `'0.5rad'`). Every axis except `perspective` can be an `Animated.Value`; `scale` binds
both axes at once. `transformOrigin` is a fractional `[x, y]` pivot in 0 to 1, default the centre.
`rotateX`/`rotateY`/`perspective` need a build with `ERUI_3D_TRANSFORMS`.

A transformed subtree renders through the transform scratch buffer, whose size caps the largest node
that can rotate or scale; see [Memory](../guides/memory.md).

## Shadow

`shadowColor`, `shadowOffset` (`{width, height}`), `shadowOpacity`, `shadowRadius` and `elevation`,
rendered only in a build with `ERUI_SHADOWS` and a `shadowOpacity` above 0. Shadows are a two-pass
blur through scratch memory; the small-board examples build without them.

## Text

On `Text` (and `TextInput`, `ActivityIndicator`): `color` (animatable), `fontSize`, `fontFamily`
(a baked font's family name), `fontWeight` (600 and up, or `'bold'`, selects the bold face; the engine
carries no other weights), `fontStyle` (`'italic'` is a synthetic slant), `textAlign`,
`textDecorationLine` (`underline`, `line-through`), `lineHeight`, `letterSpacing`.

Only baked font sizes exist on the device; another size snaps to the nearest baked one. See
[Assets](../concepts/assets.md). **Flow B:** `fontFamily` and `textAlign` are not in the subset.

Two components add a property of their own: `Modal` reads `backdropColor` for its scrim, and
`TextInput` reads `cursorColor` for the caret.

## Colours

Flow A parses `#rgb`, `#rgba`, `#rrggbb`, `#rrggbbaa`, `rgb()`/`rgba()` and named colours. **Flow B**
accepts hex and named colours only, and a state-driven colour must be a literal or a ternary of
literals.

## StyleSheet

`StyleSheet.create(styles)` returns its argument; it exists for parity and for the Flow B compiler,
which treats a `StyleSheet.create` reference as the static layer of a style. `StyleSheet.flatten`
merges an array into one object.

**Flow B:** a style object holding any state-driven value may contain only keys the compiler can
update at runtime (colours, opacity, sizes, margins, padding, the flex alignment enums, `position`,
`display`). Keep the rest in `StyleSheet.create` and overlay the dynamic part:
`style={[styles.btn, {backgroundColor: on ? A : B}]}`, with the static layer a `StyleSheet.create`
reference rather than an inline object.

## Animated values in styles

An `Animated.Value` can sit directly in `backgroundColor`, `opacity`, `color`, any `transform` axis,
and a `Dial`'s `value`/`valueStart`, on an `Animated.View`, `Animated.Text`, `Animated.Image` or
`Dial`. The engine binds the value to the property and drives it natively; no JavaScript runs per
frame. See [Animated](./animated.md).
