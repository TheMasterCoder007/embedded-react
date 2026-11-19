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
| `backgroundImage` | One gradient, as a CSS string or an array of one gradient object. See [Gradients](#gradients)                                                     |
| `opacity`         | 0 to 1, animatable. Below 1 the subtree composites through offscreen scratch; keep such nodes small                                               |
| `pointerEvents`   | `'auto' \| 'none' \| 'box-none' \| 'box-only'`. A style entry here (React Native also allows it as a prop), because that is what the engine reads |

### Gradients

`backgroundImage` paints one gradient behind a `View`, `ScrollView`, `Pressable`, `TouchableOpacity` or
`Modal`, clipped to its `borderRadius`. It takes React Native's two forms: a CSS string, or an array of
one gradient object.

```jsx
const styles = StyleSheet.create({
  hero: {
    backgroundColor: '#0f172a', // what firmware without ERUI_GRADIENT paints
    backgroundImage: 'linear-gradient(to bottom right, #0f172a, #38bdf8 70%)',
  },
  glow: {
    backgroundImage: [
      {
        type: 'radial-gradient',
        shape: 'circle',
        colorStops: [
          {color: '#fde047'},
          {color: '#451a03', positions: ['100%']},
        ],
      },
    ],
  },
});
```

- **Linear.** `linear-gradient(<direction>, <stops>)`. The direction is an angle (`45deg`, also `rad`,
  `grad` and `turn`; 0 points to the top and 90 to the right) or `to` a side or corner, and defaults to
  `to bottom`. A corner follows the box as in CSS: the two other corners share the middle color whatever
  the aspect ratio.
- **Radial.** `radial-gradient(circle, <stops>)`: a circle from the center to the farthest corner. An
  ellipse, another size or another position is not drawn. Needs `ERUI_GRADIENT_RADIAL`, which the ESP32
  examples build without; there it paints the `backgroundColor`.
- **Stops.** Two to four, each a color with an optional percentage from 0% to 100%. A stop without one
  is spaced evenly, a position below an earlier one is raised to it, and a color with two positions
  counts as two stops. Colors interpolate premultiplied, so a fade to transparent keeps its hue.

A value outside this (several gradients, a `url()`, a fifth stop, a length in pixels) is dropped with a
console warning in Flow A, and the node paints its `backgroundColor`. Gradients need a build with
`ERUI_GRADIENT`; without it the node also paints its `backgroundColor`, so set both.

**Flow B:** `backgroundImage` must be static, and a value the engine does not draw is a compile error.

## Borders

`borderRadius` and the four per-corner radii; `borderWidth` and the four per-side widths;
`borderColor` and the four per-side colours; `borderStyle` (`solid`, `dashed`, `dotted`). Radius
edges are anti-aliased (`ERUI_BORDER_AA`). On an `Image` the radii clip the bitmap to the same
rounded edge; an `Image` draws no border.

## Transform

```jsx
style={{transform: [{rotate: '45deg'}, {scale: pulse}], transformOrigin: [0.5, 1]}}
```

`transform` is an array of `{scale}`, `{scaleX}`, `{scaleY}`, `{translateX}`, `{translateY}`,
`{rotate}`, `{rotateX}`, `{rotateY}`, `{rotateZ}` or `{perspective}`. Rotations are CSS angle strings
(`'45deg'`, `'0.5rad'`). Every axis except `perspective` can be an `Animated.Value`; `scale` binds
both axes at once. `transformOrigin` is a fractional `[x, y]` pivot in 0 to 1, default the centre.
`rotateX`/`rotateY`/`perspective` need a build with `ERUI_3D_TRANSFORMS`.

**Flow B** takes each entry as a constant, a state-driven value or an `Animated.Value`, mixed freely
in one array. A state-driven angle is written `` `${turn}deg` `` or as a number of degrees, and a
ternary of angle strings works too. The array itself is written in place, so state goes inside an
entry. An `Animated.Value` drives `scale`, `scaleX`, `scaleY`, `translateX`, `translateY` and
`rotate`/`rotateZ`. `transformOrigin` is a constant.

An `<Svg>` transforms directly: its shapes are drawn through the transform, at any size, and strokes
and gradients turn and scale with them. An `<Image>` transforms directly too: its asset is sampled
through the transform, at any size, with its `resizeMode` and `tintColor` applied. Every other
transformed subtree renders through the transform scratch buffer, whose size caps the largest node
that can rotate or scale; see [Memory](../guides/memory.md). The buffer is also used by an `<Svg>` or
`<Image>` under a 3D transform, and by an `<Image>` with `resizeMode: 'repeat'` or rounded corners.

## Shadow

`shadowColor`, `shadowOffset` (`{width, height}`), `shadowOpacity`, `shadowRadius` and `elevation`,
rendered only in a build with `ERUI_SHADOWS`. A node casts a shadow when its `shadowOpacity` is above
0; otherwise an `elevation` above 0 casts a black one sized from it. Shadows are a two-pass
blur through scratch memory; the small-board examples build without them. They work on `View`,
`ScrollView`, `Pressable`, `TouchableOpacity` and `Modal`. **Flow B:** `shadowOffset` must be static;
the other four can follow state.

## Text

On `Text` (and `TextInput`, `ActivityIndicator`): `color` (animatable), `fontSize`, `fontFamily`
(a baked font's family name), `fontWeight` (600 and up, or `'bold'`, selects the bold face; the engine
carries no other weights), `fontStyle` (`'italic'` is a synthetic slant), `textAlign`,
`textDecorationLine` (`underline`, `line-through`), `lineHeight`, `letterSpacing`.

Only baked font sizes exist on the device; another size snaps to the nearest baked one. See
[Assets](../concepts/assets.md). **Flow B:** see [what it does not accept](#flow-b).

Two components add a property of their own: `Modal` reads `backdropColor` for its scrim, and
`TextInput` reads `cursorColor` for the caret.

## Colours

Flow A parses `#rgb`, `#rrggbb`, `#rrggbbaa`, `rgb()`/`rgba()` and the names `transparent`,
`black`, `white`, `red`, `green`, `blue`, `gray`/`grey`, `yellow`, `cyan`, `magenta` and `orange`, in
any case and with their CSS values (`green` is `#008000`). **Flow B** accepts `#rgb`, `#rrggbb`,
`#rrggbbaa` and the same names, and a state-driven color must be a literal or a ternary of literals.
Neither flow parses `#rgba` in a style.

## StyleSheet

`StyleSheet.create(styles)` returns its argument; it exists for parity and for the Flow B compiler,
which treats a `StyleSheet.create` reference as the static layer of a style. `StyleSheet.flatten`
merges an array into one object. `StyleSheet.hairlineWidth` is `1` and `StyleSheet.absoluteFill` is
`{position: 'absolute', top: 0, left: 0, right: 0, bottom: 0}`; Flow B cannot reference either, so
write the values out.

**Flow B:** a style object holding any state-driven value may contain only keys the compiler can
update at runtime (colors, opacity, sizes, margins, padding, the flex alignment enums, `position`,
`display`). Keep the rest in `StyleSheet.create` and overlay the dynamic part:
`style={[styles.btn, {backgroundColor: on ? A : B}]}`, with the static layer a `StyleSheet.create`
reference rather than an inline object.

## Flow B

The AOT compiler rejects a style key it cannot lower. Beyond the limits above, Flow B does not accept
`aspectRatio`, `alignContent`, `overflow`, `pointerEvents`, `borderStyle`, the per-side
border widths and colors, `fontStyle`, `textAlign` or
`textDecorationLine`. `fontFamily` must be an imported font, or a ternary of them.

It also rejects a key the engine does not draw on that element, which Flow A ignores without a word.
The layout keys, `transform` and `transformOrigin` work everywhere. `backgroundColor`, the borders and `opacity` work on
`View`, `ScrollView`, `Pressable`, `TouchableOpacity` and `Modal`, and on `TextInput` without the
per-corner radii. `backgroundImage` works on the first five. `borderRadius`, the per-corner radii and `opacity` work on `Image`. `color`,
`fontSize`, `fontWeight`, `lineHeight`, `letterSpacing` and `opacity` work on `Text`;
`TextInput` takes `color` and `fontSize`, and `ActivityIndicator` takes `color` as its tint. A nested
`<Text>` span takes `color`, `fontSize`, `fontWeight` and `letterSpacing` only.

## Animated values in styles

An `Animated.Value` can sit directly in any `transform` axis on any element, in `opacity` on a `View`,
`ScrollView`, `Pressable`, `Modal`, `Text`, `Image` or `TextInput`, and in a `Dial`'s
`value`/`valueStart`. `backgroundColor` on a `View`, `ScrollView`, `Pressable` or `Modal` and `color` on
a `Text` take an interpolation onto colors:
`v.interpolate({inputRange: [0, 1], outputRange: ['#000', '#fff']})`. The engine animates nothing else,
so Flow B rejects it. The `Animated.*`
wrappers are pass-through. The engine binds the value to the property and drives it natively; no JavaScript runs per
frame. See [Animated](./animated.md).
