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

| Property             | Notes                                                                                                                                             |
| -------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------- |
| `backgroundColor`    | Animatable. **Flow B:** the alpha of a `View` background is ignored (opaque fill); pre-mix a tint                                                 |
| `backgroundGradient` | A linear or radial gradient painted instead of `backgroundColor`; see [Gradients](#gradients)                                                     |
| `opacity`            | 0 to 1, animatable. Below 1 the subtree composites through offscreen scratch; keep such nodes small                                               |
| `pointerEvents`      | `'auto' \| 'none' \| 'box-none' \| 'box-only'`. A style entry here (React Native also allows it as a prop), because that is what the engine reads |

### Gradients

```jsx
<View style={{backgroundGradient: {type: 'linear', to: 'bottom right', stops: [{color: '#0f172a'}, {color: '#38bdf8'}]}}} />
<View style={{backgroundGradient: {type: 'radial', stops: [{color: '#ffffff'}, {color: '#ffffff00', offset: 0.7}]}}} />
```

`backgroundGradient` paints a `View` the way CSS `linear-gradient()` and `radial-gradient()` do. A
linear gradient takes `angle` in degrees (0 points to the top, 90 to the right) or `to` with a side
(`'top'`, `'right'`, `'bottom'`, `'left'`) or a corner (`'top right'` and so on); the default is
`to: 'bottom'`. A corner follows the box's aspect ratio, as in CSS, so the two other corners share
the middle colour. A radial gradient is `circle farthest-corner at center`.

`stops` holds up to 4 `{color, offset?}` entries; a missing `offset` spaces the stops evenly, and an
offset below an earlier one is raised to it. Stops are interpolated premultiplied, so a fade to
transparent keeps its colour, and the ramp is dithered so long, dark ramps show no 8-bit bands. The
gradient is clipped to `borderRadius`, and a partial repaint only computes the damaged pixels.

The gradient needs a build with `ERUI_GRADIENT`; without it the `View` paints its
`backgroundColor`, so set both for a fallback. A radial gradient also needs
`ERUI_GRADIENT_RADIAL`: a build with `ERUI_GRADIENT` but without it paints neither the gradient nor
the `backgroundColor`. **Flow B** takes a static
gradient only, with 2 to 4 stops; a state-driven one is rejected. Switch between two `View`s, or
between two `StyleSheet.create` entries, instead.

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
`rotateX`/`rotateY`/`perspective` need a build with `ERUI_3D_TRANSFORMS`. **Flow B** takes a
transform only as `Animated.Value`s on `scale`, `scaleX`, `scaleY`, `translateX`, `translateY` or
`rotate`/`rotateZ`; a static transform is rejected.

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
[Assets](../concepts/assets.md). **Flow B:** see [what it does not accept](#flow-b).

Two components add a property of their own: `Modal` reads `backdropColor` for its scrim, and
`TextInput` reads `cursorColor` for the caret.

## Colours

Flow A parses `#rgb`, `#rrggbb`, `#rrggbbaa`, `rgb()`/`rgba()` and the lowercase names
`transparent`, `black`, `white`, `red`, `green`, `blue`, `gray`/`grey`, `yellow`, `cyan`, `magenta`
and `orange`. **Flow B** accepts `#rgb`, `#rrggbb`, `#rrggbbaa` and the first six of those names
only, and a state-driven color must be a literal or a ternary of literals. Neither flow parses
`#rgba` in a style.

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
`aspectRatio`, `flexWrap`, `alignContent`, `overflow`, `pointerEvents`, `borderStyle`, the per-side
border widths and colors, the shadow keys and `elevation`, `transformOrigin`, `fontFamily`,
`fontStyle`, `textAlign` or `textDecorationLine`. `Text`'s `numberOfLines` and `ellipsizeMode` are
not read.

## Animated values in styles

An `Animated.Value` can sit directly in `backgroundColor`, `opacity`, `color`, any `transform` axis,
and a `Dial`'s `value`/`valueStart`, on any element; the `Animated.*` wrappers are pass-through. The engine binds the value to the property and drives it natively; no JavaScript runs per
frame. See [Animated](./animated.md).
