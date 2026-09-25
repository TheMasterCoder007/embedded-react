---
title: 'Components'
description: 'Every host component and its props, from View to Dial and the Svg family.'
---

The components are the React Native ones, with the same names and, where the engine implements a
prop, the same meaning. This page lists what each takes. Props shared by every component come first.

## Shared props

Every host component accepts these, on top of its own.

| Prop                                                         | Type                  | Notes                                                                                                                                                                                        |
| ------------------------------------------------------------ | --------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `style`                                                      | style object or array | See [Styles](./styles.md). Arrays and falsy entries are flattened, as in React Native                                                                                                        |
| `visible`                                                    | `boolean`             | `false` prunes the subtree from layout, rendering and hit-testing without unmounting it; the nodes and their React state stay. A `display` in `style` wins over it                           |
| `ref`                                                        | `Ref<NodeHandle>`     | Receives the engine node handle, for [`updateVector`/`updateText`](#imperative-updates)                                                                                                      |
| `onLayout`                                                   | `(e) => void`         | `e.layout` is `{x, y, width, height}` in the parent's coordinates, after the commit that laid it out                                                                                         |
| `onTouchStart`, `onTouchMove`, `onTouchEnd`, `onTouchCancel` | `(e) => void`         | Raw touches. They bubble from the node that was hit up through its ancestors, whatever the responder system decides. `onTouchCancel` fires when a sequence is abandoned rather than finished |

A touch event carries `{type, x, y, dx, dy, vx, vy}`: the point in screen pixels, the distance
travelled since touch-down, and the velocity in px/ms over the most recent move. That makes a flick
`onTouchEnd={e => e.vx > 0.4 && next()}`, with no gesture recogniser needed.

**Responder props.** Every component also accepts the gesture responder handlers
(`onStartShouldSetResponder`, `onMoveShouldSetResponder`, their `Capture` variants,
`onResponderGrant`/`Move`/`Release`/`Terminate`/`Reject` and `onResponderTerminationRequest`). A
should-set predicate returning `true` claims the gesture through real negotiation in the engine:
the capture phase asks root to leaf, then bubble asks leaf to root, and a granted responder owns the
touch stream, which stops a `ScrollView` ancestor auto-scrolling under it. Most apps use
[`PanResponder`](#panresponder) rather than wiring these directly.

## View

The building block: a box with flexbox layout, background, borders, shadow and transform.

```jsx
<View
  style={{flex: 1, padding: 16, backgroundColor: '#0d2035', borderRadius: 12}}>
  {children}
</View>
```

`View` has no props beyond the shared ones. Everything it draws comes from `style`.

## Text

```jsx
<Text style={{color: '#e6edf5', fontSize: 20}} numberOfLines={1}>
  Hello, {name}
</Text>
```

| Prop            | Type                                     | Notes                                                                                          |
| --------------- | ---------------------------------------- | ---------------------------------------------------------------------------------------------- |
| `numberOfLines` | `number`                                 | Truncate past this many lines; `0` (default) does not truncate                                 |
| `ellipsizeMode` | `'head' \| 'middle' \| 'tail' \| 'clip'` | How truncated text is cut. `tail` and `clip` are implemented; `head` and `middle` are deferred |

Children may be a string, an interpolation (`Hi {name}`), or nested `<Text>` runs with their own
style, which become inline spans on one engine node (up to 4 spans). Text draws with the built-in
font unless a baked `fontFamily` is set; see [Assets](../concepts/assets.md) for sizes and glyphs.

**Flow B:** one level of nested spans; a span cannot carry a state-driven style; a `<Text>` compiles
to a single `snprintf`, so a ternary in text cannot mix a string branch with a numeric one.

## Image

```jsx
import logo from './assets/logo.png';
<Image source={logo} style={{width: 64, height: 64}} resizeMode="contain" />;
```

| Prop         | Type                                                        | Notes                                                                                       |
| ------------ | ----------------------------------------------------------- | ------------------------------------------------------------------------------------------- |
| `source`     | asset name or `{uri}`                                       | The name an asset import resolves to. React Native's numeric `require()` id is not accepted |
| `resizeMode` | `'cover' \| 'contain' \| 'stretch' \| 'repeat' \| 'center'` | Default `'cover'`                                                                           |
| `tintColor`  | colour                                                      | Recolours the image, keeping its alpha; for monochrome icons                                |
| `imageName`  | `string`                                                    | The engine asset name, when set directly instead of through `source`                        |

An image must be baked into the app; see [Assets](../concepts/assets.md). An opaque image, or one
baked as RGB565, is drawn through the engine's copy fast path with no per-pixel blending.

## Pressable

```jsx
<Pressable onPress={() => setCount(c => c + 1)} style={styles.button}>
  <Text>count is {count}</Text>
</Pressable>
```

| Prop                                                | Type          | Notes                                                                                                                                   |
| --------------------------------------------------- | ------------- | --------------------------------------------------------------------------------------------------------------------------------------- |
| `onPress`, `onPressIn`, `onPressOut`, `onLongPress` | `(e) => void` | The press family. A press looks for the nearest ancestor carrying one of these, so wrapping a subtree in a single `Pressable` is enough |
| `delayLongPress`                                    | `number`      | Milliseconds before `onLongPress` fires                                                                                                 |
| `disabled`                                          | `boolean`     |                                                                                                                                         |

## TouchableOpacity

A `Pressable` that dims while held. Every `Pressable` prop works unchanged.

| Prop            | Type     | Notes                                   |
| --------------- | -------- | --------------------------------------- |
| `activeOpacity` | `number` | Opacity while held, 0 to 1. Default 0.2 |

The fade runs on the native driver, so it costs no re-render. It owns the node's `opacity`: an
animated opacity in `style` is ignored here (and rejected by the AOT); animate opacity yourself with
a `Pressable` instead. A dimmed node composites its whole subtree offscreen, so dim the box that
reads as the button rather than the surrounding screen.

## ScrollView

```jsx
<ScrollView style={{flex: 1}} onScroll={e => setY(e.scrollY)}>
  {rows.map(r => (
    <Row key={r.id} {...r} />
  ))}
</ScrollView>
```

| Prop       | Type          | Notes                              |
| ---------- | ------------- | ---------------------------------- |
| `onScroll` | `(e) => void` | `e.scrollX`, `e.scrollY` in pixels |

It scrolls whichever axis overflows: lay the content out with `flexDirection: 'row'` for a
horizontal scroller. There is no `horizontal` prop. Scrolling has momentum, and a granted gesture
responder inside it (a `PanResponder`, a `Dial adjustable`) takes the touch away from the scroller.

## FlatList

A thin alias for `ScrollView`, **not** a virtualised list: every row mounts as a real engine node and
stays mounted.

| Prop           | Type                                | Notes |
| -------------- | ----------------------------------- | ----- |
| `data`         | `readonly T[]`                      |       |
| `renderItem`   | `({item, index}) => ReactNode`      |       |
| `keyExtractor` | `(item, index) => string \| number` |       |
| `style`        | style                               |       |

Those four are the only props either flow honours; the AOT rejects any other at compile time. For
headers, footers, separators or `onEndReached`, use a `ScrollView` with `.map` directly.

## SectionList

Also a `ScrollView` alias: no virtualisation, no sticky headers. Headers, rows and footers are flat
siblings, as in React Native.

| Prop                                         | Type                                                   | Notes                                                                                                                           |
| -------------------------------------------- | ------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------- |
| `sections`                                   | `Array<{data, key?, renderItem?, keyExtractor?, ...}>` | A section carries its rows plus any fields of your own; per-section `renderItem` and `keyExtractor` override the list-wide ones |
| `renderItem`                                 | `({item, index, section}) => ReactNode`                | `index` is within the section                                                                                                   |
| `renderSectionHeader`, `renderSectionFooter` | `({section}) => ReactNode`                             |                                                                                                                                 |
| `keyExtractor`                               | `(item, index) => string \| number`                    |                                                                                                                                 |
| `style`                                      | style                                                  |                                                                                                                                 |

**Flow A only** for now; the AOT has no `SectionList` lowering yet.

## Button

React Native's pre-styled button: a `Pressable` around one centred `Text`. It takes no `style`, as
upstream; build the `Pressable` and `Text` yourself when you need one.

| Prop       | Type          | Notes                                   |
| ---------- | ------------- | --------------------------------------- |
| `title`    | `string`      |                                         |
| `onPress`  | `(e) => void` |                                         |
| `color`    | colour        | Fill colour, replacing the default blue |
| `disabled` | `boolean`     | Greys it out and detaches `onPress`     |

Accessibility, TV-focus and `testID` props are accepted and ignored. **Flow A only** for now.

## ImageBackground

A `View` with an `Image` stretched behind its children, for the case `Image` cannot cover because
it takes no children.

| Prop                 | Type              | Notes                                                                                     |
| -------------------- | ----------------- | ----------------------------------------------------------------------------------------- |
| `style`              | style             | The container: the box the picture fills (its content box, so padding insets the picture) |
| `imageStyle`         | style             | Merged over the fill: `borderRadius`, `opacity`, `tintColor`                              |
| `imageRef`           | `Ref<NodeHandle>` | The `Image` node; the outer `ref` is the `View`                                           |
| _every `Image` prop_ |                   | `source`, `resizeMode`, `tintColor` go to the image                                       |

**Flow A only** for now.

## TextInput

```jsx
<TextInput
  style={styles.field}
  value={ssid}
  placeholder="Network name"
  onChangeText={setSsid}
  onSubmitEditing={connect}
/>
```

| Prop                                           | Type             | Notes                     |
| ---------------------------------------------- | ---------------- | ------------------------- |
| `value`, `placeholder`, `placeholderTextColor` |                  |                           |
| `editable`                                     | `boolean`        |                           |
| `secureTextEntry`                              | `boolean`        | Draws a dot per character |
| `onChangeText`                                 | `(text) => void` |                           |
| `onSubmitEditing`, `onFocus`, `onBlur`         | `(e) => void`    |                           |

Focus brings up the engine's on-screen keyboard, in builds that include it (`ERUI_ONSCREEN_KEYBOARD`).
Its layout and colours are configurable with `setKeyboardConfig`, below. The style may set
`cursorColor`.

## Switch

| Prop            | Type              | Notes |
| --------------- | ----------------- | ----- |
| `value`         | `boolean`         |       |
| `onValueChange` | `(value) => void` |       |
| `trackColor`    | `{false?, true?}` |       |
| `thumbColor`    | colour            |       |

**Flow B:** a `Switch` with `onValueChange` needs a `value` prop.

## ActivityIndicator

| Prop          | Type      | Notes                                  |
| ------------- | --------- | -------------------------------------- |
| `animating`   | `boolean` | Default `true`                         |
| `style.color` | colour    | The spinner's tint, as in React Native |

The spinner animates in the engine.

## Modal

```jsx
<Modal visible={open} backdropColor="#00000099" style={styles.sheet}>
  {content}
</Modal>
```

| Prop            | Type      | Notes                                                                                |
| --------------- | --------- | ------------------------------------------------------------------------------------ |
| `visible`       | `boolean` | Shows and hides the modal (this is the modal's own switch, not the shared `visible`) |
| `backdropColor` | colour    | The scrim painted behind it; also settable in `style`                                |

Give a modal an explicit full-screen style
(`{position: 'absolute', left: 0, top: 0, width: screen.width, height: screen.height, alignItems: 'center', justifyContent: 'center'}`):
the engine lays a modal out as an ordinary flex child, so without one the sheet is flex-sized rather
than centred on screen. The AOT honours the same style. **Flow B:** `visible` is required.

## Dial

The engine's native arc widget: dials, gauges and progress rings as one node, drawn analytically,
animated natively, with built-in drag-to-set.

```jsx
<Dial
  style={{width: 200, height: 200}}
  min={50}
  max={90}
  value={target}
  thickness={14}
  cap="round"
  trackColor="#1f3550"
  indicatorColor="#38bdf8"
  knob="circle"
  knobSize={22}
  adjustable
  onChange={setTarget}
/>
```

Angles are degrees clockwise from 3 o'clock; the default is a 270° sweep starting at 135°.

| Prop                                                                       | Type                                                     | Notes                                                                                                                                                       |
| -------------------------------------------------------------------------- | -------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `value`                                                                    | `number \| AnimatedValue`                                | The current value in `[min, max]`; animatable on the native driver                                                                                          |
| `min`, `max`                                                               | `number`                                                 |                                                                                                                                                             |
| `startAngle`, `sweepAngle`                                                 | `number`                                                 | Degrees                                                                                                                                                     |
| `step`                                                                     | `number`                                                 | Drag quantisation, default 1                                                                                                                                |
| `thickness`                                                                | `number`                                                 | Track and indicator thickness in px; default a tenth of the smaller side                                                                                    |
| `bandThickness`, `bandColor`                                               |                                                          | An optional wider backing band behind the track                                                                                                             |
| `trackColor`, `indicatorColor`                                             | colour                                                   |                                                                                                                                                             |
| `indicatorGradient`                                                        | `{type: 'conic' \| 'radial', stops: [{color, offset?}]}` | Replaces `indicatorColor`. Conic sweeps the stops along the angle, radial across the thickness                                                              |
| `cap`                                                                      | `'butt' \| 'round'`                                      |                                                                                                                                                             |
| `segments`, `gapAngle`                                                     | `number`                                                 | Split the arc into N segments separated by `gapAngle` degrees (default 2)                                                                                   |
| `knob`                                                                     | `'none' \| 'circle' \| 'image' \| 'child'`               | `'child'` positions the first child on the value point, for custom or multiple knobs                                                                        |
| `knobSize`, `knobColor`, `knobBorderColor`, `knobBorderWidth`, `knobImage` |                                                          |                                                                                                                                                             |
| `adjustable`                                                               | `boolean`                                                | The knob follows the finger in the engine; `onChange` gets the quantised value. It claims the gesture ahead of any `ScrollView`                             |
| `range`                                                                    | `boolean`                                                | Dual-setpoint mode: the indicator spans `[valueStart, value]` with a knob at each end. A drag latches the nearer end, and the ends clamp against each other |
| `valueStart`                                                               | `number \| AnimatedValue`                                | Range mode: the low end                                                                                                                                     |
| `minSpan`                                                                  | `number`                                                 | Range mode: the minimum separation. Above 0, a drag that would close the gap pushes the other end along                                                     |
| `onChange`                                                                 | `(value, valueStart) => void`                            |                                                                                                                                                             |

A value change repaints only the swept sliver and the knob, not the whole dial. Hit-testing is
ring-only, so the hole and the unswept gap fall through to what is behind.

**Flow B:** `indicatorGradient` must be a static object (2 to 4 stops); `onChange` an inline function
or a `useCallback`; `knobImage` a static asset.

## Svg

`<Svg>` is one engine node, a vector shape tape. The shape elements inside it are descriptive
children flattened into that tape, like text inside `<Text>`: they take no `style`, no events and no
`ref`, and a component that returns them is never mounted (inline the shapes, or call it as a plain
function). Arrays and fragments of shapes are unwrapped, so a mapped list works.

```jsx
import gauge from './gauge.svg';

<Svg source={gauge} width={120} height={120} />          // a baked file

<Svg width={200} height={200}>                           // shapes described in JSX
  <Circle cx={100} cy={100} r={80} fill="none" stroke="#1f3550" strokeWidth={12} />
  <Arc cx={100} cy={100} r={80} startAngle={-135} endAngle={angle}
       fill="none" stroke="#38bdf8" strokeWidth={12} strokeLinecap="round" />
</Svg>
```

| Prop              | Type              | Notes                                                                                                                                             |
| ----------------- | ----------------- | ------------------------------------------------------------------------------------------------------------------------------------------------- |
| `source`          | imported `.svg`   | Baked at build time to a vector tape (or a raster, if the file uses text, masks or filters). Scaled to the render box; shape children are ignored |
| `width`, `height` | number or string  | The render box, react-native-svg style; a `style` width/height wins                                                                               |
| `viewBox`         | `'minX minY w h'` | Coordinates are baked into the tape against `width`/`height`                                                                                      |
| paint props       |                   | See below; set on `<Svg>` they apply to every shape                                                                                               |

**Paint props**, on `<Svg>`, `<G>` and every shape: `fill`, `stroke`, `strokeWidth`, `strokeLinecap`
(`butt`/`round`/`square`), `strokeLinejoin` (`miter`/`round`/`bevel`), `strokeMiterlimit`,
`fillRule` (`nonzero`/`evenodd`), and `fillGrad`/`strokeGrad`, a gradient
`{type: 1 linear | 2 radial | 3 conic, stops: [{color, offset}], ax, ay, bx, by, r}` with up to 8
stops. Colours accept `#rgb`, `#rgba`, `#rrggbb`, `#rrggbbaa`, `rgb()`/`rgba()` and a few names.
**An unfilled shape defaults to black**: set `fill="none"` on anything meant to be stroke-only.

| Shape       | Props                                                                                                                                                                                                  |
| ----------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `<Path>`    | `d` (M/L/H/V/C/S/Q/T/A/Z, absolute and relative). Parsing `d` costs more than the primitives; prefer them for anything rebuilt per frame                                                               |
| `<Circle>`  | `cx`, `cy`, `r`                                                                                                                                                                                        |
| `<Ellipse>` | `cx`, `cy`, `rx`, `ry`                                                                                                                                                                                 |
| `<Rect>`    | `x`, `y`, `width`, `height`, `rx`, `ry` (a negative radius is `auto`, as in SVG; each is clamped to half its side)                                                                                     |
| `<Line>`    | `x1`, `y1`, `x2`, `y2`                                                                                                                                                                                 |
| `<Arc>`     | `cx`, `cy`, `r`, `startAngle`, `endAngle`: degrees clockwise from 12 o'clock. Not a standard SVG element; it emits a native arc op with no bezier conversion, cheap enough to rebuild every drag frame |
| `<G>`       | Groups shapes under one paint and an optional `x`/`y`/`translateX`/`translateY`/`scale`, composed with enclosing groups                                                                                |

Limits: 16 shapes per `<Svg>` and 8 vector nodes at once, by default (`ERUI_VECTOR_PAINTS_MAX`,
`ERUI_MAX_VECTOR_NODES`). Combine many tick marks into one `<Path>` with several subpaths.

**Flow B:** a state-driven `<Svg>` uses absolute pixels (no `viewBox`, no `<G>`) and literal shape
children; a state-driven `<Path d>` is not supported, and `visible` on an `<Svg>` is rejected (wrap
it in a `View`).

## PanResponder

Recognises drags, swipes and flings through the engine's responder system. Create it once per
component and keep it in a ref; re-creating it per render loses the drag.

```jsx
const pan = useRef(
  PanResponder.create({
    onMoveShouldSetPanResponder: (e, g) => Math.abs(g.dx) > 8,
    onPanResponderMove: (e, g) => setOffset(g.dx),
    onPanResponderRelease: (e, g) => settle(g.vx),
  }),
).current;

<View {...pan.panHandlers} style={styles.track} />;
```

The config is React Native's: the four should-set predicates, `onPanResponderGrant`/`Move`/
`Release`/`Terminate`/`Reject`, `onPanResponderTerminationRequest`, and `onPanResponderStart`/`End`
for extra fingers. Only the Android-specific `onShouldBlockNativeResponder` is absent. Every
callback gets `(event, gestureState)`, where `gestureState` is one mutable object reused for the
responder's lifetime with `stateID`, `moveX`/`moveY`, `x0`/`y0`, `dx`/`dy`, `vx`/`vy` and
`numberActiveTouches`; read what you need during the callback rather than keeping it.

**Flow B:** compiles the same config onto the engine's C negotiation, so the gesture needs no
JavaScript on the device. `onPanResponderStart`/`End` are the exception (Flow A only, rejected by
name).

## Imperative updates

For continuous gestures where a React render per pointer move is too slow: take the node handle from
a `ref`, push updates directly, and commit the result back to React state when the gesture ends.

```jsx
const needle = useRef(null);
// on each move:
updateVector(
  needle.current,
  [
    {
      line: [cx, cy, cx + r * Math.sin(a), cy - r * Math.cos(a)],
      stroke: '#38bdf8',
      strokeWidth: 3,
    },
  ],
  [cx - r, cy - r, 2 * r, 2 * r],
);
```

- **`updateVector(handle, shapes, dirtyRect?)`** sets an `<Svg>` node's geometry from primitive
  descriptors, skipping React and the `d` parser. Each shape has exactly one geometry key (`arc`,
  `circle`, `line`, `rect`, `path`) plus paint in the short spellings (`fill`, `stroke`, `strokeWidth`,
  `cap`, `join`, `miter`, `fillRule`, `fillGrad`, `strokeGrad`). The optional `dirtyRect`
  `[x, y, w, h]`, in node-local pixels, limits the repaint to that region.
- **`updateText(handle, text)`** sets a `<Text>` node's content without touching its style. A later
  React render reverts it cleanly.
- **`setKeyboardConfig(config?)`** replaces the on-screen keyboard's layout and colours
  (`panelColor`, `keyColor`, `keyActiveColor`, `labelColor`, `fontSize`, `rowHeight`, `keyGap`,
  `keyRadius`, `gridCols`, and `layers` as rows of keys with `char`, `label`, `layer`, `backspace`,
  `done`, `span`, `highlight`). Pass nothing to restore the built-in QWERTY. A no-op in builds without
  the keyboard.

Measured on the ESP32-S3, an imperative `updateVector` cut the JavaScript cost of a dial drag by
about 98% compared with a render per move. See [Performance](../guides/performance.md).
