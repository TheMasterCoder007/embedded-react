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
const [count, setCount] = usePersistentState('count', 0);
```

`useState` whose value survives a hot reload in the simulators and the on-device dev loop. You rarely
call it: in those loops the bundler rewrites plain `useState` to this, keyed by component name and
hook order, so the state survives transparently. Reach for it directly when you want a key that is
explicitly yours. The value must be JSON-serializable. On a device, and in any release build, it is
exactly `useState`. [Hot reload](../guides/hot-reload.md) explains the keying.

## useHostValue

```jsx
const steps = useHostValue(0);
```

**Flow B.** Declares a number the host feeds into the app. The compiler lowers it to a state slot, and
the package names it as an input. The firmware finds it with `erb_input_find("steps")` and calls
`erb_input_set_int()` whenever the value changes; the app re-renders as if `setState` had been called. This is how
sensor readings, clocks and any other host data reach a compiled app with no JavaScript on the
device. In the simulator, where there is no host to write it, it returns its initial value.
The [RP2040 guide](../guides/boards/rp2040.md) shows it driving a pedometer.

## nativeModule

```tsx
import {nativeModule} from 'embedded-react';
import type {Int} from 'embedded-react';

interface Sensors {
  read(channel: Int): number;
  setLed(on: boolean): void;
}

const Sensors = nativeModule<Sensors>('sensors', 1);
```

**TypeScript.** Declares a module the firmware provides in C. The interface is in the app's file or in a local
`.ts` file the app imports it from. A method takes and returns `Int` (a whole number), `number` (a float) and
`boolean`, and may return `void`. It takes up to 16 parameters.

Call a method in a handler, an effect or a timer, and keep what it returns in state. A call in render is refused,
since Flow A would make it on every render and Flow B only when what it reads changes.

Each method called becomes an import named `module.method`, with the module's version and a signature such as
`f:i` (returns a float, takes an int). The firmware registers an `ErbNative` for each one in `ErbConfig::natives`,
and a package whose imports aren't all there doesn't load: the error screen names the missing one.

```c
static bool sensors_read(const int32_t* arguments, int32_t* result, void* context)
{
    const float value = read_adc(arguments[0]);
    memcpy(result, &value, sizeof(value));
    return true;
}

static const ErbNative natives[] = {{"sensors.read", 1, "f:i", sensors_read}};
```

In Flow A the bundler reads the same interface and gives `nativeModule()` each method's signature. Give the same
table to `ErRuntimeConfig::natives`, so both flows call the same C functions and get the same values (a `number`
goes through a C float in both). When the app loads, each method in the interface is checked against the table,
and a missing one stops the app with the error screen naming it.

The simulators have no firmware, so a module runs a JS mock there instead: `sensors.mock.ts` beside the
interface, named after the module, whose default export implements it.

```ts
import type {Sensors} from './sensors';

let led = false;

const sensors: Sensors = {
  read: channel => 20 + channel / 10,
  setLed: on => {
    led = on;
  },
};

export default sensors;
```

The mock's values are converted as on a board: an `Int` is cut to a whole number, a `number` goes through a C
float, and a `boolean` is true or false. A module with no mock, or a mock missing one of the interface's methods,
stops the app in the simulator when it loads, naming what to add. A board build never includes the mock.

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
Flow A it is the real panel (or the simulator's current size). In Flow B, values derived from it are
worked out at boot from the real panel, and a branch that changes the tree adds a layout the panel
picks at boot, so one build runs on every panel; see
[the AOT subset](../guides/aot-subset.md#assets-and-screen-size).
[Layout](../concepts/layout.md#designing-for-small-panels) shows the pattern.

## Timers and time

`setTimeout`, `setInterval`, `clearTimeout` and `clearInterval` are the web ones, driven by the
engine's clock and serviced once per frame. `Date.now()` and `performance.now()` read the same
clock; `Date.now()` counts from boot until the firmware sets the wall clock from an RTC or NTP.
There are no `Date` objects in the runtime's default profile. **Flow B:** timestamps are 64-bit
integers with the arithmetic rules in [the AOT subset](../guides/aot-subset.md#state-hooks-and-lists).
