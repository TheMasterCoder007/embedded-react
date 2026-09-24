---
title: 'The AOT subset'
description: 'What the ahead-of-time compiler accepts, what it refuses and how it tells you. For apps headed to a board with no external RAM.'
---

The Flow B compiler turns JSX into C. It can only do that for code it can fully resolve at build
time, so it accepts a subset of what runs under Flow A and refuses the rest, at build time, with the
file and line:

```text
AOT: a state-driven <Path d=…> is not yet supported (use Arc/Circle/Rect/Line for dynamic shapes)
  at App.jsx:41:12
```

The rules come in two kinds. Some are permanent, because a compiled app cannot do what only an
interpreter could: a component tree shaped by data that arrives at runtime, or a `<Path d>` string
built on the fly. Many more are simply **not yet implemented**, and the compiler says so in its
message (`not yet supported`). Those are the ones that shrink with each release; see
[Where the subset is going](#where-the-subset-is-going). This page lists the rules by area as they
stand today, then the habits that keep an app inside them.

:::tip[Test early]
The browser simulator runs Flow A and accepts everything. If a board without external RAM is the
target, run `npx embedded-react build --aot --screen WxH` from the first day, so each rule turns up
while the code that breaks it is still one line.
:::

## Components and props

- The entry is `export function App() {...}` and a component body must return a JSX element. A
  component that branches at the top level with `if` may only test a compile-time constant, such as
  the `screen` global; runtime layout branching is expressed inside JSX with `&&` or a ternary.
- Props reach a child component through a destructured parameter: `function Card({title})`, not
  `function Card(props)`. Rest props (`...rest`) are not supported. A spread onto a component
  (`{...x}`) must be a compile-time-constant object.
- `<Modal>` needs a `visible` prop; `<Switch>` with `onValueChange` needs a `value` prop;
  `<TouchableOpacity>`'s `activeOpacity` and `disabled` must be constants; `<Image source>` must
  resolve to an imported asset. `visible` on an `<Svg>` is not supported: wrap it in a `<View>`.
- `<FlatList>` honours `data`, `renderItem`, `keyExtractor` and `style` only, and
  `renderItem` must destructure `({item, index})`. For anything more, use `<ScrollView>` with `.map`.

## State, hooks and lists

- `useState`, `setState` (including the updater form), `useEffect` with an array-literal dependency
  list and an optional cleanup as its last statement, `useRef`, `useCallback`, `useMemo`.
- `useHostValue(initial)` is Flow B's way to feed host data (a sensor, a clock) into the app: the
  compiler emits a C setter for it. The [RP2040 guide](./boards/rp2040.md) shows it driving a step
  counter.
- `.map` over a list compiles when the list is a compile-time-constant array, or state that the app
  appends object literals to; the row pool is sized by `ER_AOT_LIST_CAP` (16 by default, lower it
  on a tight board). A list computed at runtime from anything else does not compile.
- `Date.now()` and `performance.now()` work and are 64-bit whole milliseconds. Keep a timestamp in
  state, a ref or a local; compare it; show it; divide it by a constant with `%` or
  `Math.floor/ceil/round/trunc(a / b)`. A plain `/`, a divisor from state, or mixing it with a float
  or a boolean is a compile error. There are no `Date` objects.
- Whole-number arithmetic is defined at the edges: `+`, `-`, `*` and negation saturate rather than
  overflow, `x % 0` is 0, `x / 0` saturates, and a float that is NaN or out of range becomes 0 or
  the nearest integer. Constant maths is folded at build time.

## Handlers

An event handler body may contain `const` locals holding a number, a boolean or a string; state
setter calls; ref writes; `updateVector`/`updateText` calls; and `Animated.timing/spring(...).start()`.
Locals cannot be reassigned or destructured, and a helper function defined elsewhere in the module
does not resolve inside a handler, so express a calculation as a chain of `const`s and ternaries,
inline. `setInterval`/`setTimeout` callbacks must be inline functions.

## Styles and colours

- Colours are `#rgb`, `#rrggbb`, `#rrggbbaa` or a named colour. `rgba()` and `hsl()` are not parsed.
  A state-driven colour must be a colour literal or a ternary of literals.
- A `View`'s `backgroundColor` alpha is ignored (the fill is opaque); borders and vector paints do
  blend. For a "10% tint" fill, pre-mix a solid colour.
- A style object that holds any state-driven value may contain only keys the compiler can update at
  runtime: colours, opacity, sizes, margins, padding, `flexDirection`, `alignItems`, `alignSelf`,
  `justifyContent`, `position`, `display`. Keep the rest (`flex`, `overflow`, …) in a
  `StyleSheet.create` layer and overlay the dynamic part: `style={[styles.btn, {backgroundColor: on ? A : B}]}`.
  The static layer must be a `StyleSheet.create` reference, not an inline object.
- A state-driven enum style (`display`, `flexDirection` and the like) must be a string literal or a
  ternary of literals. `display: page === 'home' ? 'flex' : 'none'` compiles; `visible={bool}` takes
  any boolean.
- `fontFamily` and `textAlign` are not in the subset. Text uses the built-in font; centre it with a
  wrapping `View` and `alignItems: 'center'`.

## Text

A `<Text>` compiles to one `snprintf`. Interpolation (`count is {count}`) and one level of nested
`<Text>` spans work; a span inside a span, a dynamic `{...}` segment inside a multi-span text, and a
state-driven style on a span do not. A ternary in text cannot mix a string branch with a numeric
one, or a boolean branch with a non-boolean one; split those into two `{cond && <Text/>}` nodes.
Comparing a string with a number is refused rather than coerced.

Text can only show glyphs the built-in font bakes: printable ASCII plus a fixed symbol set. `×`,
`•`, `−`, `–` and `—` are there; `·` and `✕` are not, and render as `?`. The build warns about any
character it cannot find. See [Assets](../concepts/assets.md).

## Animation

- `Animated.timing`, `spring`, `decay`, `sequence`, `parallel`, `stagger`, `delay` and `loop` all
  compile, with `.start()` and `anim.stop()`. `loop` can repeat a timing, spring, decay or sequence
  (not a parallel or stagger), and `iterations` must be a build-time constant. A `sequence` entry
  must have a known duration, so a spring, decay or loop cannot sit inside one. A `.start(callback)`
  on a parallel or stagger is not yet supported.
- `interpolate` takes an object literal with `inputRange` and `outputRange` of equal length, up to
  8 breakpoints.
- The engine drives every animation natively once started, in both flows.

## Vector shapes and dials

- A static `<Svg source={imported.svg}>` supports the full baked feature set: `viewBox`, groups,
  gradients. A state-driven `<Svg>` (shapes whose props come from state) uses absolute pixels, with
  no `viewBox` and no `<G>`, and its children must be literal shape elements.
- A state-driven `<Path d>` is not supported. Use `<Arc>`, `<Circle>`, `<Rect>` and `<Line>` for
  geometry that moves; they take `Math.sin`/`Math.cos` in their props.
- One `<Svg>` node holds at most 16 shapes, and the engine holds 8 vector nodes at once. Combine
  many tick marks into one `<Path>` with several subpaths rather than many `<Line>`s.
- For a dial, gauge or progress ring, use `<Dial>`: one engine node, animated natively, with
  built-in drag-to-set. Its `indicatorGradient` must be a static object with 2 to 4 stops, and its
  `onChange` an inline function or a `useCallback`.

## Gestures

`PanResponder.create({...})` compiles when kept in a `useRef` and given an object literal of
callbacks, spread onto a host element. Should-set predicates must be a single boolean expression.
`onPanResponderStart`/`End` are the two callbacks the compiler leaves out. The gesture itself runs
in the engine's C responder negotiation, so the compiled version is not a transpilation.

## Assets and screen size

Images and SVGs must be imports (`import logo from './logo.png'`). The panel size is baked in with
`--screen WxH`, which seeds the `screen` global; every module-level constant derived from it folds
with `+ - * /` and ternaries only, so write `a < b ? a : b` rather than `Math.min(a, b)` at module
level. (`Math.*` is fine inside state-driven expressions.) Only the branch a board takes is
compiled, so an import used solely in an untaken branch is fine, and a top-level `if (COMPACT)
return ...` keeps a board's whole layout in one place.

## Where the subset is going

The subset is a snapshot, not a boundary. Flow B started with the parts of the API a compiler can
resolve most directly, and every release since has widened it; `Animated.loop` around a sequence,
counted loops, `anim.stop()`, `useHostValue`, `PanResponder`, `<Dial>` and 64-bit time all arrived
after the first version. The direction is for anything that runs in the simulator to compile, and
for the remaining gap to be the permanent kind.

Queued next, from the [roadmap](/roadmap):

- **`Button`, `ImageBackground` and `SectionList`.** The three JavaScript-only wrappers that the
  compiler currently rejects by name, with the tree to write by hand. The first two are fixed
  rewrites of the kind `FlatList` already gets; a section is a header plus a variable-length list,
  which the list unroller has no shape for yet.
- **Live `toValue`.** An animation whose target depends on current state animates toward the value
  captured when the animation was set up, not the live one. Tracked for the animation hardening
  pass.
- **Animation composition on hardware.** Hardening `sequence`, `parallel`, `loop` and
  `interpolate`, and imperative refs, on the no-PSRAM boards.

If a rule on this page is in your way, say so in an
[issue](https://github.com/TheMasterCoder007/embedded-react/issues): the order the gaps close in
follows what real apps hit.

## Keeping an app in the subset

- Build for Flow B from the start, and let the compiler's messages shape the code.
- Separate static from dynamic: `StyleSheet.create` for what never changes, a small inline overlay
  for what does.
- Express handler logic as `const` chains. If a calculation wants a helper, compute it in render and
  pass the result down.
- Reach for `<Dial>` and `<Arc>` before a `<Path>` that would need to change.
- Use `visible` and `display: 'none'` to switch pages; they cost no node churn in either flow.

The demos in the repository build both ways and are the best worked examples; the thermostat's
compact layout in particular is a tour of these rules.
