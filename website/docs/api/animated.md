---
title: 'Animated'
description: 'Values, timing, spring and decay, composition, interpolate, Easing and LayoutAnimation, all driven by the engine.'
---

The `Animated` API is React Native's, with one difference that changes how you use it: **every
animation runs in the engine.** A value bound to a style property is updated in C each frame, with
no JavaScript involved, so animating is cheap even on a 240 MHz core, and `useNativeDriver` is
accepted and irrelevant.

```jsx
const pulse = useAnimatedValue(1);

useEffect(() => {
  const anim = Animated.loop(
    Animated.sequence([
      Animated.timing(pulse, {toValue: 1.1, duration: 900}),
      Animated.timing(pulse, {toValue: 1.0, duration: 900}),
    ]),
  );
  anim.start();
  return () => anim.stop();
}, []);

<Animated.Image
  source={logo}
  style={{width: 64, height: 64, transform: [{scale: pulse}]}}
/>;
```

## Values

`new Animated.Value(initial)` creates an engine-side float; `useAnimatedValue(initial)` creates one
tied to the component and destroys it on unmount, which is the form to prefer. A value has:

- `setValue(v)`: set it at once, pushing to everything bound to it.
- `interpolate({inputRange, outputRange, extrapolate?, extrapolateLeft?, extrapolateRight?})`: a
  derived value mapped through a piecewise-linear curve, up to 8 breakpoints. Both ranges are
  numbers; a value bound to a rotation axis is read in degrees, so `outputRange: [0, 360]` is a
  full turn. `extrapolate` is `'extend'` (default), `'clamp'` or `'identity'`, with per-end
  overrides.

## Where a value can go

`Animated.View`, `Animated.Text` and `Animated.Image` accept values in `backgroundColor`, `opacity`,
`color` and any `transform` axis (`scale` binds both axes). `Dial` accepts one in `value` and
`valueStart` without an `Animated.` prefix. `Animated.createAnimatedComponent` wraps another
component. A `TouchableOpacity` owns its own `opacity` for the press feedback, so animate opacity on
a `Pressable` instead.

## Animations

| Function                         | Config                                                                                | Notes                                                                                                                                                  |
| -------------------------------- | ------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `Animated.timing(value, config)` | `toValue`, `duration` (ms), `delay`, `easing`                                         | A curve over a fixed time. `duration: 0` finishes at once                                                                                              |
| `Animated.spring(value, config)` | `toValue`, `stiffness` (default 100), `damping` (10), `mass` (1), `velocity`, `delay` | A damped harmonic oscillator, in the engine's physical parameters. React Native's `tension`/`friction` and `bounciness`/`speed` spellings are not read |
| `Animated.decay(value, config)`  | `velocity` (value per ms), `deceleration` (per-ms friction, default 0.998)            | Coasts from a velocity; there is no target                                                                                                             |

Each returns an animation with `start(callback?)` and `stop()`. The callback receives
`{finished}`, `false` if the animation was stopped or interrupted, and is wired to the engine's own
completion so a chained animation starts exactly when the previous one ends.

## Composition

`Animated.sequence([...])`, `Animated.parallel([...], {stopTogether?})`,
`Animated.stagger(delayMs, [...])`, `Animated.delay(ms)` and
`Animated.loop(animation, {iterations?, resetBeforeIteration?})` compose animations as in React
Native. Composition is JavaScript over each child's `start`/`stop`, with completion coming from the
engine, so a sequence of timings costs one engine animation at a time.

**Flow B** compiles all of these, with limits listed in [the AOT subset](../guides/aot-subset.md#animation):
a `loop` can repeat a timing, spring, decay or sequence; a sequence entry must have a known duration;
`iterations` must be a constant.

## Easing

`Animated.timing` takes an `Easing` token: `Easing.linear`, `ease`, `easeIn`, `easeOut`,
`easeInOut`, `quadIn`, `quadOut`, `quadInOut`, `cubicIn`, `cubicOut`, `cubicInOut`, `bounceOut`,
`elasticOut`, and `Easing.bezier(x1, y1, x2, y2)` for a custom cubic curve. Each maps to a curve the
engine evaluates; a JavaScript easing function is not called per frame.

## LayoutAnimation

```jsx
LayoutAnimation.configureNext(LayoutAnimation.Presets.easeInEaseOut);
setExpanded(true);
```

Call `configureNext(config, onDone?)` before a state change and the next commit tweens every node
whose computed rectangle moved, from where it was to where it now belongs, in the engine. The config
is consumed by that one commit. `Presets.easeInEaseOut`, `Presets.linear` and `Presets.spring` are
the React Native ones (300 ms timings, or the default spring); the shorthands
`LayoutAnimation.easeInEaseOut()`, `linear()` and `spring()` configure and return; `create(duration,
type, property)`, `Types` and `Properties` build a custom config. Nodes appearing for the first time
snap into place rather than animating from nothing.

## What it costs

An animation the engine drives damages only what it changes: an opacity fade repaints the node, a
translate repaints the old and new positions, a `Dial` value repaints the swept sliver. Two cases
cost more and are worth knowing:

- A node with `opacity` below 1, or a transform, composites through scratch memory each frame. Keep
  such nodes small, and enable the engine's fade cache (`ERUI_FADE_CACHE_W/H`) on boards with the RAM
  for it: after one capture, each frame of a pure opacity animation is a single blend.
- `setState` on a timer is not an animation. Sixty `setState`s a second re-render, re-diff and
  re-commit sixty times; a `timing` on an `Animated.Value` does none of that.
