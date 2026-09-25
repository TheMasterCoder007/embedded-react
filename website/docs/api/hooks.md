---
title: 'Hooks and app APIs'
sidebar_label: 'Hooks'
description: "The three hooks the package adds to React's, plus AppRegistry, Platform and the screen global."
---

React's own hooks come from `react` and work as they do anywhere: `useState`, `useEffect`, `useRef`,
`useCallback`, `useMemo`, `useReducer`, `useContext`. The runtime services effects and timers once
per frame from the host pump, so `useEffect` and `setTimeout` behave as you expect. The package adds
three hooks of its own.

## useAnimatedValue

```jsx
const pulse = useAnimatedValue(1);
```

Creates an `Animated.Value` tied to the component's lifetime: it is destroyed on unmount. Use it
instead of `useRef(new Animated.Value(1)).current` so the engine-side value is released.
See [Animated](./animated.md).

## usePersistentState

```jsx
const [count, setCount] = usePersistentState(0);
```

`useState` whose value survives a hot reload in the simulators and the on-device dev loop. You rarely
call it: in those loops the bundler rewrites plain `useState` to this, keyed by component name and
hook order, so state survives transparently. Reach for it directly when you want a key that is
explicitly yours. The value must be JSON-serialisable. On a device, and in any release build, it is
exactly `useState`. [Hot reload](../guides/hot-reload.md) explains the keying.

## useHostValue

```jsx
const steps = useHostValue(0);
```

**Flow B.** Declares a number the host feeds into the app. The compiler lowers it to a state field
plus a generated C setter named after the variable, `er_app_set_steps(int)` here, which the firmware
calls whenever the value changes; the app re-renders as if `setState` had been called. This is how
sensor readings, clocks and any other host data reach a compiled app with no JavaScript on the
device. In the simulator, where there is no host to write it, it returns its initial value.
The [RP2040 guide](../guides/boards/rp2040.md) shows it driving a pedometer.

## AppRegistry

```jsx
import {AppRegistry} from 'embedded-react';
import {App} from './App.jsx';

AppRegistry.registerComponent('my-app', () => App);
```

Registers the root component and mounts it into a container the size of the screen. In React Native
the native side calls `runApplication` when the activity starts; here running the bundle _is_
starting the app, so registration mounts immediately. The root is created once and reused across
hot reloads, which is what lets React reconcile the old tree into the new one in place.

## Platform

`Platform.OS` is `'embedded'`. `Platform.select({embedded: a, default: b})` picks by that key, as
in React Native, so shared code can branch without a build flag.

## The `screen` global

```jsx
const SW = screen.width;
const SH = screen.height;
const COMPACT = SW < 300;
```

The host injects `screen` with the panel's `width` and `height` in pixels before the app runs. In
Flow A it is the real panel (or the simulator's current size); in Flow B the size passed to
`--screen` is baked in, and module-level constants derived from it fold at compile time, so a
responsive app compiles to exactly one layout per board. [Layout](../concepts/layout.md#designing-for-small-panels)
shows the pattern.

## Timers and time

`setTimeout`, `setInterval`, `clearTimeout` and `clearInterval` are the web ones, driven by the
engine's clock and serviced once per frame. `Date.now()` and `performance.now()` read the same
clock; `Date.now()` counts from boot until the firmware sets the wall clock from an RTC or NTP.
There are no `Date` objects in the runtime's default profile. **Flow B:** timestamps are 64-bit
integers with the arithmetic rules in [the AOT subset](../guides/aot-subset.md#state-hooks-and-lists).
