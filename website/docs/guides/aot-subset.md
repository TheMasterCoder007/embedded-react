---
title: 'The AOT subset'
description: 'What the ahead-of-time compiler accepts, what it refuses and how it tells you. For apps headed to a board with no external RAM.'
---

The Flow B compiler turns JSX into a package. It can only do that for code it can fully resolve at build
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
target, run `npx embedded-react build --aot` from the first day, so each rule turns up
while the code that breaks it is still one line.
:::

## Components and props

- The entry is `export function App() {...}` and a component body must return a JSX element. A
  component that branches at the top level with `if` may only test a compile-time constant, such as
  the `screen` global; runtime layout branching is expressed inside JSX with `&&` or a ternary.
  The left of a state-driven `&&` must be a condition, not a number: React draws a `0` as text, so
  write `count > 0 && <El/>`.
- A state-driven `&&` or ternary builds the branch it picks and destroys it when another is picked,
  so the package needs nodes for the largest set shown at once. To keep a small branch that flips
  often built, write `<El visible={cond}/>` instead. In a list row, every branch is built with the
  row and toggled. The state of a component in a branch is kept while the branch is gone.
  `updateVector` can't draw on a node in a branch.
- Components live in the same file as `App`: local `.jsx` imports are not resolved.
- A component that receives state-driven or function props must destructure them:
  `function Card({title})`. `function Card(props)` works only when every prop is a constant. Rest
  props (`...rest`) are not supported. A spread onto a component (`{...x}`) must be a
  compile-time-constant object.
- Every element takes a fixed set of props, style keys, and children; anything else is an `AOT:`
  error at the prop. That covers a prop the element does not have, a style key the engine does not
  draw on it (`backgroundColor` on a `<Text>`, `borderWidth` on an `<Image>`; see
  [Styles](../api/styles.md#flow-b)), text outside a `<Text>`, and children on an `<Image>`,
  `<Switch>`, `<TextInput>` or `<ActivityIndicator>`. `testID` and the accessibility props are
  accepted and ignored everywhere but `<FlatList>`.
- `<Modal>` needs a `visible` prop, and its `animationType` can only be `'none'`; `<Switch>` with
  `onValueChange` needs a `value` prop, and cannot be `disabled`; `disabled` on a `<Pressable>` or
  `<TouchableOpacity>` and `activeOpacity` must be constants. `<Image source>` takes an imported
  asset, a string asset name, `{uri: 'name'}` or string state; a state-driven source bakes every
  image the app imports. `<Text numberOfLines>` and `ellipsizeMode` must be constants, and
  `ellipsizeMode` is `'tail'` or `'clip'`. `visible` or an event on an `<Svg>` is not supported:
  wrap it in a `<View>` or `<Pressable>`.
- `<TextInput>`, `<Switch>`, `<ActivityIndicator>`, `Animated.View`/`Text`/`Image` and a
  module-level `setKeyboardConfig({...})` also compile.
- `<FlatList>` honours `data`, `renderItem`, `keyExtractor` and `style` only, and
  `renderItem` must destructure `({item, index})`. For anything more, use `<ScrollView>` with `.map`.

## State, hooks, and lists

- `useState`, `setState` (including the updater form), `useEffect` with an array-literal dependency
  list and an optional cleanup as its last statement, `useRef`, `useCallback`, `useMemo`, and
  `useAnimatedValue` with a constant initial value (every `Animated` call drives one).
- `useHostValue(initial)` is Flow B's way to feed host data (a sensor, a clock) into the app: the
  compiler lists it as a package input, and the host sets it with `erb_input_set_int`. The
  [RP2040 guide](./boards/rp2040.md) shows it driving a step counter.
- `.map` over list state builds a pool of rows, sized by `ER_AOT_LIST_CAP` (16 by default, lower it
  on a tight board). A list state holds objects of strings, numbers and booleans, nested objects
  included, or plain numbers, strings, or booleans. Its fields are those of its initial items, and an
  appended item can leave fields out but not add new ones.
- A field that some item leaves out, or sets to `null`, reads as `undefined`: text shows nothing, it
  is falsy, and `??` and `=== undefined` work. Anywhere else a missing number reads as 0, so write
  `item.width ?? 40` where the difference matters.
- `items[i].field` and `values[i]` read a row by a runtime whole-number index, in render and in
  handlers. Past the end, `items[i].field` stops the app with the error screen, as JS throws there;
  `items[i]?.field` and `values[i]` read as `undefined`. A constant array read by a runtime index
  is kept in RAM, like a list state, and holds at most 32767 elements.
- A list state, a row's item, or `items[i]` can be passed to a component.
- `.map` over an array built in render (inline, a `const`, a `useMemo`, or a prop) unrolls one row
  per element, and its elements can read state. Such an array can't be read by a runtime index.
  A list computed at runtime from anything else does not compile.
- `Date.now()` and `performance.now()` work and are 64-bit whole milliseconds. Keep a timestamp in
  state, a ref, or a local; compare it; show it; divide it by a constant with `%` or
  `Math.floor/ceil/round/trunc(a / b)`. A plain `/`, a divisor from state, or mixing it with a float
  or a boolean is a compile error. There are no `Date` objects.
- Whole-number arithmetic is defined at the edges: `+`, `-`, `*` and negation saturate rather than
  overflow, `x % 0` is 0, `x / 0` saturates, and a float that is NaN or out of range becomes 0 or
  the nearest integer. Constant maths is folded at build time.

## Handlers

An event handler body may contain `const` locals holding a number, a boolean, or a string, including one built
with `+`, a template literal or the string helpers above; `if` and
`else`; state setter calls; `ref.current` writes, including `+=` and `++`; `setInterval`,
`setTimeout` and their clears; `updateVector` calls; and `.start()` or `.stop()` on any `Animated`
animation or composition. Locals cannot be reassigned or destructured, and loops, `switch` and `try`
are not lowered. A helper function called as a statement is inlined, but one used inside an
expression is refused, so express a calculation as a chain of `const`s and ternaries.
`setInterval`/`setTimeout` callbacks must be inline functions. `updateText` is Flow A only.

## Styles and colours

- Colors are `#rgb`, `#rrggbb`, `#rrggbbaa` or a named color. `rgba()` and `hsl()` are not parsed.
  A state-driven color must be a color literal or a ternary of literals.
- A `View`'s `backgroundColor` alpha is ignored (the fill is opaque); borders and vector paints do
  blend. For a "10% tint" fill, pre-mix a solid color.
- Static or state-driven is decided per style key, so constants and state can share one inline
  object. A state-driven value works on colors, `opacity`, sizes, margins, padding, `gap`,
  `flexGrow`/`flexShrink`, border radii, `borderWidth`, `zIndex`, `fontSize`, `lineHeight`,
  `letterSpacing` and the enums `flexDirection`, `alignItems`, `alignSelf`, `justifyContent`,
  `position` and `display`. `flex`, `flexBasis`, `left`/`top`/`right`/`bottom` and `fontWeight` must
  be constants.
- A state-driven enum style (`display`, `flexDirection` and the like) must be a string literal or a
  ternary of literals. `display: page === 'home' ? 'flex' : 'none'` compiles; `visible={bool}` takes
  any boolean.
- `fontFamily`, `textAlign` and `overflow` are not in the subset. Text uses the built-in font; center it with a
  wrapping `View` and `alignItems: 'center'`.

## Text

A `<Text>` compiles to one formatted string. Interpolation (`count is {count}`) and one level of nested
`<Text>` spans work; a span inside a span, a dynamic `{...}` segment inside a multi-span text, and a
state-driven style on a span do not. A ternary in text cannot mix a string branch with a numeric
one, or a boolean branch with a non-boolean one; split those into two `{cond && <Text/>}` nodes.
Comparing a string with a number is refused rather than coerced. A nested `<Text>` holds at most 4
spans by default; `ER_AOT_MAX_TEXT_SPANS` raises it, and must match the engine's `ER_TEXT_MAX_SPANS`.

Template literals, `String(x)`, `x.toFixed(n)`, `s.padStart(n, fill)`, `s.slice(start, end)` and `s.toUpperCase()`
work in text and wherever else a string is built: a string state's setter, a list's string field, and a handler's
`const`. `s.includes(search)` gives a boolean anywhere one is allowed.

- `toFixed` takes 0 to 20 decimals and rounds as JS does. A fraction is a 32-bit float, though, so a long one can
  differ from Flow A in its last digits.
- `padStart` pads one value with one ASCII character. Pad each value on its own:
  `` `${String(h).padStart(2, '0')}:${String(m).padStart(2, '0')}` ``. `slice` cuts one value the same way.
- The numbers `toFixed` and `padStart` take must be constants; `slice`'s start and end can be any whole numbers.
  The limits here hold even when the whole call is a constant, so turning a constant into state never stops an app
  compiling.
- `toUpperCase` changes ASCII and Latin-1 letters (`é` to `É`, `ß` to `SS`) and leaves other characters as they are.
- `includes` searches for a string, with no start position, in a string that's kept somewhere (a state, a prop, a
  local). Put one built in place into a `const` first.
- A ternary in text still can't build a string in a branch, so `{ready ? t.toFixed(1) : '--'}` is refused.

Text can only show glyphs the built-in font bakes: printable ASCII plus a fixed symbol set. `×`,
`•`, `−`, `–` and `—` are there; `·` and `✕` are not, and render as `?`. The build warns about any
character it cannot find. See [Assets](../concepts/assets.md).

## Animation

- `Animated.timing`, `spring`, `decay`, `sequence`, `parallel`, `stagger`, `delay` and `loop` all
  compile, with `.start()` and `anim.stop()`. `loop` can repeat a timing, spring, decay or sequence
  (not a parallel or stagger), and `iterations` must be a build-time constant. A `sequence` takes
  `timing`, `spring`, `decay` and `delay` entries, but not a nested `parallel`, `stagger` or `loop`;
  a sequence inside a `parallel` or `stagger` needs a known duration, so no spring or decay there. A
  `.start(callback)` on a parallel or stagger of more than one animation is not yet supported; besides
  delays, one animation's callback runs when whichever ends last has finished.
- `interpolate` takes an object literal with `inputRange` and `outputRange` of equal length, up to
  8 breakpoints.
- The engine drives every animation natively once started, in both flows.

## Vector shapes and dials

- A static `<Svg source={imported.svg}>` supports the full baked feature set: `viewBox`, groups,
  gradients. A state-driven `<Svg>` (shapes whose props come from state) uses absolute pixels, with
  no `viewBox` and no `<G>`, and its children must be literal shape elements.
- An `<Svg>` cannot sit directly under a state-driven `&&` or ternary, or in a row of a state list:
  put the condition on a wrapping `<View>`.
- A state-driven `<Path d>` is not supported. Use `<Arc>`, `<Circle>`, `<Rect>` and `<Line>` for
  geometry that moves; they take `Math.sin`/`Math.cos` in their props.
- By default one `<Svg>` node holds at most 16 shapes, and the engine 8 vector nodes at once
  (`ERUI_VECTOR_PAINTS_MAX`, `ERUI_MAX_VECTOR_NODES`; see [Memory](./memory.md)). Combine many tick
  marks into one `<Path>` with several subpaths rather than many `<Line>`s.
- For a dial, gauge or progress ring, use `<Dial>`: one engine node, animated natively, with
  built-in drag-to-set. Its `indicatorGradient` must be a static object with 2 to 4 stops, and its
  `onChange` an inline function or a `useCallback`.

## Gestures

`PanResponder.create({...})` compiles when kept in a `useRef` and given an object literal of
callbacks, spread onto a host element. Should-set predicates must be a single boolean expression.
`onPanResponderStart`/`End` are refused: they report extra fingers, and Flow B lowers one gesture
per responder. The gesture itself runs
in the engine's C responder negotiation, so the compiled version is not a transpilation.

## Assets and screen size

Images and SVGs must be imports (`import logo from './logo.png'`).

Values derived from `screen.width` and `screen.height` (with `+ - * /`, comparisons, ternaries and
`Math.min`, `max`, `floor`, `ceil`, `round`, `trunc` and `abs`) are worked out at boot from the real
panel. A branch on `screen` whose two sides differ only in values (styles, props, `{text}`) is one
tree whose values are picked at boot.

A branch that changes the tree adds a layout. A package declares no sizes: the compiler finds the
layouts from the app's breakpoints, proves every panel size up to 32767×32767 has one, and stores each
with a check. The firmware passes its resolution and the loader picks the first layout whose check
passes, so one package runs on every panel. Anything identical between layouts is stored once, and
only the chosen layout uses RAM.

```jsx
const WIDE = screen.width >= 480;

export function App() {
  return WIDE ? <TwoColumns /> : <OneColumn />;
}
```

A top-level `if (COMPACT) return ...` or a returned ternary keeps a whole layout in one place.

- **A state's initial value** derived from `screen` is worked out at boot, as long as it is a number
  or a boolean. A string adds a layout for each value.
- **`width={screen.width}` on an `<Svg viewBox>`** is worked out at boot, and the drawing scales with
  no layout per size. Shapes calculated from `screen` fail the build, naming the expression: draw in
  the viewBox's units instead.
- **Every layout has to compile.** One that doesn't fails the build, and the error names a size that
  picks it. A package holds at most 16 layouts; past that the build fails, naming the expression that
  changes the most.
- **`--screen 240x320,800x480` only drops layouts.** It keeps the ones those sizes pick, which saves
  the flash of the rest. A panel that picks a dropped layout shows the error screen.

Limits worth knowing:

- Fonts are baked at fixed sizes. Breakpoint sizes work, but `fontSize: screen.width / 10` snaps to the
  nearest baked size, with a warning.
- Every board gets every layout, including the images and fonts only one layout uses, unless
  `--screen` drops them.
- The build proves each layout compiles, not that it looks right. A very small screen gets the
  smallest layout and may overflow, as on the web.

## Fixed limits

A compiled app keeps its state in buffers sized at build time. The compiler checks each limit where it
can know the answer, and the app reports the rest at run time:

| Limit                                                                   | Default               | Raise it with                                      | When it is past                                                                                                                                                                                        |
| ----------------------------------------------------------------------- | --------------------- | -------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| Rows in a list state                                                    | 16                    | `ER_AOT_LIST_CAP`                                  | An initial list with more rows fails the build. An append to a full list is skipped and reported.                                                                                                      |
| Bytes in a string built from itself, like `setLog(log + '.')`           | 47 (a 48-byte buffer) | `ER_AOT_LIST_STR_CAP`                              | A constant string that is too long, as an initial value or a write, fails the build. A string built at run time is cut short and reported.                                                             |
| Bytes in a string written at run time                                   | 255                   | Fixed in `er_scene.h` (`ER_TEXT_MAX`)              | A constant string that is too long fails the build. A string built at run time is cut short and reported.                                                                                              |
| Bytes in a node's text, a `<Text>` span, a placeholder or an image name | 255, 63, 63, 63       | Fixed in `er_scene.h` (`ER_TEXT_MAX` and the like) | A constant string that is too long fails the build. A string built at run time is cut short and reported.                                                                                              |
| Engine nodes                                                            | 512                   | `ERUI_MAX_NODES`                                   | The app's node count is fixed when it compiles, and the runtime checks it against `ERUI_MAX_NODES` when it loads the package, refusing one a pool can't hold. A node the pool cannot give is reported. |
| Live timers                                                             | 8                     | `ER_AOT_MAX_TIMERS`                                | A `setTimeout` or `setInterval` past it never fires, and is reported.                                                                                                                                  |
| Animation completion waits                                              | 8                     | Fixed in the compiler                              | The run ends at once, as not finished, and is reported.                                                                                                                                                |

Every other string state, list field, and local is sized to the longest value that can reach it: its initial
value, the constants it is set to, and the most a number, a boolean, or another string can print. A string copied
from a `<TextInput>` holds 255 bytes. To keep one smaller, or to bound a string built from itself, cut it with a
fixed-length `slice`. `setName(text.slice(0, 20))` holds 60 bytes, since a character can take 3, and
`setLog(log.slice(-39) + '.')` holds 40, since everything in it is ASCII.

`ER_AOT_LIST_CAP`, `ER_AOT_LIST_STR_CAP` and `ER_AOT_MAX_TIMERS` are read by the compiler (`npm run aot`, or
`embedded-react build --aot`). They size what the package holds, so raising one needs no firmware rebuild. A report is
one line through the runtime's log hook, once for each place in the app that can hit it, and names the limit to raise:

```text
ERB: list full; the row was dropped (raise ER_AOT_LIST_CAP) (App.jsx:41)
```

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
- Express handler logic as `const` chains. If a calculation wants a helper, call the helper as a
  statement, or compute the value in render and pass it down.
- Reach for `<Dial>` and `<Arc>` before a `<Path>` that would need to change.
- Use `visible` and `display: 'none'` to switch pages; they cost no node churn in either flow.

The demos in the repository build both ways and are the best worked examples; the thermostat's
compact layout in particular is a tour of these rules.
