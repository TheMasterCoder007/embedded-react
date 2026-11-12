---
title: 'Hooks and app APIs'
sidebar_label: 'Hooks'
description: "The hooks the package adds to React's, plus AppRegistry, Platform, Dimensions and the screen global."
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
`erb_input_set_int()` whenever the value changes; the app re-renders as if `setState` had been called. In the
simulator, where there is no host to write it, it returns its initial value. For anything more than a number the
firmware sets, a [native module](../guides/native-modules.md) works in both flows and runs on a mock in the simulator.

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

**TypeScript.** Declares a module the firmware provides in C. The [native modules guide](../guides/native-modules.md)
walks through a firmware's device package, which apps import declared modules from, with the board examples' modules
on three boards. A module the app declares itself has its interface in the app's file or in a local `.ts` file the app
imports it from. A method takes and returns `Int` (a whole number), `Int64` (a whole number past
an `Int`'s range), `number` (a float), `boolean`, `string` and [records](#records) of them, and may return `void`. It
takes up to 16 values and returns up to 16, counting each field of a record.

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
float, a `boolean` is true or false, an `Int64` is cut to a whole number that wraps at 64 bits, and a `string` is the
value's string, up to any NUL. A module with no mock, or a mock missing one of the interface's methods,
stops the app in the simulator when it loads, naming what to add. A board build never includes the mock.

A mock that posts [events](#usenativeevent) is a function instead. It is given `emit`, which posts one of the
module's events as the firmware would, and returns the methods. As on a board, a post from inside one of the mock's
methods is refused (`emit` returns false), so post from a timer:

```ts
import type {NativeMock} from 'embedded-react';
import type {Sensors} from './sensors';

const sensors: NativeMock<Sensors> = emit => ({
  read: channel => 20 + channel / 10,
  measure: () => {
    setTimeout(() => emit('reading', 21.5), 500);
  },
});

export default sensors;
```

### Strings and Int64s

```c
static char label[32];

static bool wifi_label(const int32_t* arguments, int32_t* result, void* context)
{
    snprintf(label, sizeof(label), "%s (%d)", erb_native_string(arguments[0]), (int)arguments[1]);
    *result = erb_native_text(label);
    return true;
}

static bool clock_add(const int32_t* arguments, int32_t* result, void* context)
{
    erb_native_set_int64(result, erb_native_int64(&arguments[0]) + arguments[2]);
    return true;
}

static const ErbNative natives[] = {
    {"wifi.label", 1, "s:si", wifi_label},
    {"clock.add", 1, "l:li", clock_add},
};
```

A `string` is `s` in a signature and an `Int64` is `l`. An `Int64` takes two cells, low word first, which
`erb_native_int64()` and `erb_native_set_int64()` read and write. A string takes one cell, which holds it by
reference: `erb_native_string()` reads an argument in place, so copy it to keep it past the call. A native returns
one with `erb_native_text()`, whose text must stay as it is until the call returns. Flow B copies it into a buffer of
the call's own, which holds 255 bytes: a longer one is cut short, and reported as other strings are.

An event's string is made the same way, and its handlers read it in place, so a post doesn't copy it:

```c
int32_t values[3];
values[0] = erb_native_text(ssid);
erb_native_set_int64(&values[1], seen_at_ms);
erb_native_post(WIFI_FOUND, values);
```

In JS an `Int64` is a number, exact up to 2^53. Pass a string as a string or a const that holds one: Flow B builds
text only where it is kept, so build it in a const first rather than in the call.

### Records

```tsx
import type {Int, Int64} from 'embedded-react';

interface Point {
  x: Int;
  y: Int;
}

interface Gps {
  fix(): {place: Point; at: Int64; label: string};
  move(from: Point, step: Int): Point;
}

const Gps = nativeModule<Gps>('gps', 1);

// In a handler:
const fix = Gps.fix();
setLabel(fix.label);
setPlace(Gps.move(fix.place, 2));
```

A record is an interface or object type of fields, each of the types above or a record, all required. Declare it
in the interface's file, or inline. It passes as its fields one by one, in order, so the C side reads them as
consecutive cells. The signature names them in braces, `{name:code,…}`, so a renamed or reordered field is a
mismatch at load rather than swapped values:

```c
static char label[32];

/* {place:{x:i,y:i},at:l,label:s}: */
static bool gps_fix(const int32_t* arguments, int32_t* result, void* context)
{
    result[0] = fix_x;
    result[1] = fix_y;
    erb_native_set_int64(&result[2], fix_ms);
    snprintf(label, sizeof(label), "%d sats", (int)satellites);
    result[4] = erb_native_text(label);
    return true;
}

/* {x:i,y:i}:{x:i,y:i}i */
static bool gps_move(const int32_t* arguments, int32_t* result, void* context)
{
    result[0] = arguments[0] + arguments[2];
    result[1] = arguments[1] + arguments[2];
    return true;
}

static const ErbNative natives[] = {
    {"gps.fix", 1, "{place:{x:i,y:i},at:l,label:s}:", gps_fix},
    {"gps.move", 1, "{x:i,y:i}:{x:i,y:i}i", gps_move},
};
```

Pass a record as an object with every field: an object state, a const, or an object built in the call. A record a
method returns is kept in a `const`, which may destructure it, or given to an object state's setter or another
native method, as is a field of it: `setPlace(Gps.fix().place)`. Anywhere else it is refused. An event's value may be
a record too, which its handler may destructure. In Flow A the bridge passes a record as its fields and nativeModule()
builds the object again, so the app sees the same in both flows, and a mock's records are converted field by field.

### Optional modules

```tsx
import {Pressable, Text, nativeModule} from 'embedded-react';
import type {Int} from 'embedded-react';

interface Wifi {
  connect(channel: Int): boolean;
  events: {found(rssi: Int): void};
}

const Wifi = nativeModule.optional<Wifi>('wifi', 1);

export function App() {
  return Wifi.available ? <WifiPage /> : <Text>No Wi-Fi on this board</Text>;
}
```

`nativeModule.optional` declares a module only some boards have. A package that uses it loads on a board without
it, and `Wifi.available` says whether the firmware has every method and event of the interface. It is set when the
app loads and never changes, so render can read it to leave a page out.

Check `available` before calling a method: a call to a module that isn't there stops the app, with the error screen
naming the method. A `useNativeEvent` of it is fine, and its events never arrive.

Flow B lists every method and event of an optional module in the package, called or not, so `available` means the
same as in Flow A, which looks them all up. They count toward the package's 32 imports and 32 events. In the
simulators, a module is there when it has a mock.

## useNativeEvent

```tsx
import {useState} from 'react';
import {Text, nativeModule, useNativeEvent} from 'embedded-react';

interface Sensors {
  measure(): void;
  events: {
    reading(celsius: number): void;
  };
}

const Sensors = nativeModule<Sensors>('sensors', 1);

function Thermometer() {
  const [celsius, setCelsius] = useState(0.0);
  useNativeEvent(Sensors, 'reading', value => setCelsius(value));
  return <Text>{celsius}</Text>;
}
```

**TypeScript.** Runs a handler each time the firmware posts one of a native module's events. This is how
long-running work reports back: a method starts it, and the firmware posts an event when it is done. The interface's
`events` lists them, each a method that returns `void` and takes values as a method's parameters do. A handler runs
only while its component is mounted, and may take fewer parameters than the event has values. It may destructure a
[record](#records) value: `({lat, lon}) => …`. Call it at the top of a component, as every hook.

The firmware declares each event in the same `ErbNative` table as its functions, with no callback, and posts it by
its index from the frame loop. The app's handlers run at once, then the update, as `erb_input_set_int()` runs it.
Native code never calls anything else in the app. A package that subscribes to an event the firmware doesn't
declare doesn't load, naming it. An event nothing subscribes to is dropped.

```c
enum { SENSORS_READING = 1 };

static const ErbNative natives[] = {
    {"sensors.measure", 1, "v:", sensors_measure},
    [SENSORS_READING] = {"sensors.reading", 1, "v:f", NULL},
};

/* In the frame loop, once a measurement is ready. */
int32_t value;
memcpy(&value, &celsius, sizeof(value));
if (erb_native_post(SENSORS_READING, &value)) {
    measurement_ready = false;
}
```

A post the runtime can't take returns false: no app runs, or the app is running code (a native function posting
from inside its call). Post it again next frame. In Flow A, `er_runtime_native_post()` takes the same index and
values and calls the subscribed handlers as one batch.

**Flow B.** Each event the app subscribes to is a handler in the package, which runs every `useNativeEvent` of it
whose component is mounted: the conditions it is built under, and its list row being in use, read before any of
them runs. A package holds 32 subscriptions, and one in a list row counts once for each row.

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

## Dimensions and useWindowDimensions

```jsx
import {Dimensions, useWindowDimensions} from 'embedded-react';

const {width, height} = Dimensions.get('window');

export function App() {
  const window = useWindowDimensions();
  return <View style={{width: window.width / 2}} />;
}
```

The React Native way to read the same size, for code shared with a phone app. Both return
`{width, height, scale, fontScale}`: `width` and `height` are the `screen` global's, and `scale` and
`fontScale` are always 1, since a panel has no pixel-density or font setting. `'window'` and
`'screen'` are the same size.

The size is fixed while the app runs, so `useWindowDimensions()` never re-renders and there is no
`Dimensions.addEventListener`. **Flow B:** both are worked out at boot exactly as `screen` is, so a
breakpoint written with either one behaves the same.

## Timers and time

`setTimeout`, `setInterval`, `clearTimeout` and `clearInterval` are the web ones, driven by the
engine's clock and serviced once per frame. `Date.now()` and `performance.now()` read the same
clock; `Date.now()` counts from boot until the firmware sets the wall clock from an RTC or NTP.
There are no `Date` objects in the runtime's default profile. **Flow B:** timestamps are 64-bit
integers with the arithmetic rules in [the AOT subset](../guides/aot-subset.md#state-hooks-and-lists).
