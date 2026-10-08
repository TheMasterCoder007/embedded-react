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

## Border effects

A `View` can draw a ring along the inside of its rounded edge, over its content, for a focus
highlight. The ring follows the corner radius and is anti-aliased on both edges.

```jsx
function FocusRing({children}) {
  const phase = useRef(new Animated.Value(0)).current;
  useEffect(() => {
    Animated.loop(
      Animated.timing(phase, {
        toValue: 1,
        duration: 2000,
        easing: Easing.linear,
        useNativeDriver: true,
      }),
    ).start();
  }, [phase]);
  return (
    <Animated.View
      style={{
        borderRadius: 16,
        borderSweepColor: '#7dd3fc',
        borderSweepWidth: 3,
        borderSweepPhase: phase,
      }}>
      {children}
    </Animated.View>
  );
}
```

| Property            | Notes                                                                                                            |
| ------------------- | ---------------------------------------------------------------------------------------------------------------- |
| `borderSweepColor`  | Colour at the head of a light travelling around the edge; it fades to transparent along its tail                 |
| `borderSweepWidth`  | Ring thickness in px, inside the box; `0` (default) draws nothing                                                |
| `borderSweepLength` | Tail length as a fraction of the perimeter, default `0.3`                                                        |
| `borderSweepPhase`  | Head position, `0` to `1` clockwise from the left end of the top edge, wrapping. Animatable on the native driver |

`borderGradient` shows a gradient through a ring of the same kind, for the rotating or moving
gradient border. It is `{type: 'conic', width, angle?, stops}`, a CSS `conic-gradient()` (0 degrees
up, clockwise), or `{type: 'radial', width, size?, stops}`, a CSS `radial-gradient()` (ellipse,
farthest corner) on a background `size` times the box (default 1). Either is seen only inside a ring
`width` px thick, and takes up to 6 stops of `{color, offset?}`.

`borderGradientAngle` animates it on the native driver. For a conic gradient it is the start angle in
degrees, overriding `angle`, so a linear loop of 0 to 360 spins it. For a radial one it plays a CSS
keyframe loop: one turn takes `background-position` from `0% 0%` to `100% 100%` at 180 degrees and
back.

A change of `borderSweepPhase` or `borderGradientAngle` alone repaints the bands along the four edges
the ring can reach, not the whole box. **Flow A only** for now: the AOT rejects these keys.

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
