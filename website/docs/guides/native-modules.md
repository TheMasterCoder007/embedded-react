---
title: 'Native modules'
description: "Offer a firmware's native modules as a device package: the TypeScript declarations, the generated C header, the C functions and events behind it, optional modules, mocks for the simulator, and the limits."
---

A native module is how an app reaches what the UI can't do on its own: a sensor, the radio, storage, heavy maths.
The firmware implements it in C and publishes what it offers as a device package: an npm package that declares each
module in TypeScript. An app imports the modules it uses, and the same app runs them in both flows: Flow A calls the C
through the QuickJS bridge, and a Flow B package lists what it uses as imports the runtime matches at load.

The board examples are the worked example on this page. One device package serves the
[ESP32-S3](./boards/esp32-s3.md), the [CYD](./boards/esp32-cyd.md) and the [RP2040](./boards/rp2040.md), and the
watch face demo imports its three modules:

| Module        | What it is                                                            | S3  | CYD | RP2040 |
| ------------- | --------------------------------------------------------------------- | --- | --- | ------ |
| `chip`        | The chip's name and free heap, and an event each second for the clock | Yes | Yes | Yes    |
| `thermometer` | The chip's temperature sensor, optional                               | Yes | No  | Yes    |
| `motion`      | The IMU's step count and the level's tilt, optional                   | No  | No  | Yes    |

The package is in [`examples/device`](https://github.com/TheMasterCoder007/embedded-react/tree/master/examples/device),
the app in [`demos/watch-face`](https://github.com/TheMasterCoder007/embedded-react/tree/master/demos/watch-face),
and each board's C in its example's `native_modules.c`. The [hooks reference](../api/hooks.md#nativemodule) lists
every type and signature letter.

## 1. Declare the modules in a device package

A device package is a folder with a `package.json` whose `exports` (or `main`) points at its `.ts` source. The types
are what give each method its signature, so a package that ships only JavaScript or `.d.ts` files is refused.

```json title="package.json"
{
  "name": "example-device",
  "version": "1.0.0",
  "type": "module",
  "exports": "./index.ts",
  "peerDependencies": {
    "embedded-react": "*"
  }
}
```

Its file declares each module at the top level, with the name and version the firmware registers it under, and
exports it. Methods take and return `Int` (a whole number, an `int32_t` in C), `Int64`, `number` (a float),
`boolean`, `string` and records of them. Events go under `events`:

```ts title="examples/device/index.ts"
import {nativeModule} from 'embedded-react';
import type {Int, Int64} from 'embedded-react';

export interface Chip {
  name(): string;
  freeMemory(): Int;
  events: {
    second(uptimeMs: Int64): void;
  };
}

export const Chip = nativeModule<Chip>('chip', 1);
```

The interface is in the same file, or in a local one the file imports it from. An imported interface needs a name of
its own there, since TypeScript won't export a const beside an imported type of the same name:
`import type {Chip as ChipInterface} from './chip'`.

## 2. Import them in an app

An app depends on the package like any other, from a registry, a workspace, or a `file:` path, and imports the
modules it uses:

```json title="package.json"
"dependencies": {
  "embedded-react": "^0.20.0",
  "example-device": "file:./device"
}
```

```tsx title="App.tsx"
import {useNativeEvent} from 'embedded-react';
import {Chip} from 'example-device';

export function App() {
  const [seconds, setSeconds] = useState(0);
  const [chipName, setChipName] = useState('');

  useEffect(() => {
    setChipName(Chip.name());
  }, []);
  useNativeEvent(Chip, 'second', uptimeMs =>
    setSeconds(Math.floor(uptimeMs / 1000)),
  );
  // …
}
```

Only the methods it calls and the events it subscribes to end up in its package, so one firmware serves many apps,
each using its own part of what the firmware offers.

Call a method in an effect, a handler, or a timer, and keep what it returns in state. Render can't call one: Flow A
would call it on every render, and Flow B only when what it reads changes.

The package is found the Node way, from the app's folder. The app imports a module from the file that declares it: a
file that passes it on with `export * from` or `export {Chip} from` isn't followed.

## 3. Generate the C header

The firmware's C has to match what apps compile against: each name, version and signature, and where each value is
in a call's cells. `native-header` writes that down from the package, one header per module:

```bash
npx embedded-react native-header example-device --out main/generated
```

It takes a package's name or folder, or the `.ts` file that declares the modules. Each header, `chip_module.h` for
`chip`, has:

- An entry per method and event, to put in the table: `{CHIP_NAME_ENTRY, chip_name}`, or `{CHIP_SECOND_ENTRY, NULL}`
  for an event.
- Where each argument starts in a call's or post's cells, `CHIP_SECOND_ARG_UPTIME_MS`, and how many cells they take,
  `CHIP_SECOND_ARG_CELLS`.
- A struct per record, its cell count, and `static inline` helpers that pack and unpack it.

A record is named after its type, and one written in place after where it is: `MotionOffset`, `GpsFixPlace`. The
headers are header-only, so there is nothing to link. Generate them again when the package changes, in the
firmware's build or with a test that fails when a committed one is stale, as the board examples do.

## 4. Write the C side

Each method is an `ErbNative` entry: its name (`module.method`), version and signature, which the header gives, and
a function. An event is an entry with no function.

```c title="native_modules.c"
#include "chip_module.h"

/* Each entry's place in the table, which an event is posted by. */
enum
{
    CHIP_NAME,
    CHIP_FREE_MEMORY,
    CHIP_SECOND,
    NATIVE_COUNT
};

static bool chip_name(const int32_t* arguments, int32_t* result, void* context)
{
    *result = erb_native_text("ESP32");
    return true;
}

static bool chip_free_memory(const int32_t* arguments, int32_t* result, void* context)
{
    *result = (int32_t)esp_get_free_heap_size();
    return true;
}

const ErbNative native_modules[NATIVE_COUNT] = {
    [CHIP_NAME] = {CHIP_NAME_ENTRY, chip_name},
    [CHIP_FREE_MEMORY] = {CHIP_FREE_MEMORY_ENTRY, chip_free_memory},
    [CHIP_SECOND] = {CHIP_SECOND_ENTRY, NULL},
};
```

A signature is the result, a colon, then the parameters, one letter each: `i` for `Int`, `f` for `number`, `b` for
`boolean`, `l` for `Int64`, `s` for `string`, `v` for no result. A record is its fields in braces, `{x:i,y:i}`.

Values pass as 32-bit cells: an `Int` or `boolean` in one, a float's bits in one (`erb_native_float()` reads it, `erb_native_set_float()` writes it), an `Int64`
in two, low word first. A string's cell holds it by reference: `erb_native_string()` reads an argument, and
`erb_native_text()` makes a result, whose text must stay as it is until the call returns. A function returns false to
stop the app with the error screen naming the module.

A record goes through its struct, and an argument is read at its offset:

```c
#include "gps_module.h"

/* nearest(from: Fix, radius: Int): Fix */
static bool gps_nearest(const int32_t* arguments, int32_t* result, void* context)
{
    const GpsFix from = gps_fix_unpack(&arguments[GPS_NEAREST_ARG_FROM]);
    const int32_t radius = arguments[GPS_NEAREST_ARG_RADIUS];
    const GpsFix nearest = find_nearest(&from, radius);
    gps_fix_pack(result, &nearest);
    return true;
}
```

Give the table to whichever flow the board runs. Both take the same table, so one file serves both, as on the S3:

```c
/* Flow B */
const ErbConfig config = {
    /* … */
    .natives = native_modules,
    .native_count = NATIVE_COUNT,
};

/* Flow A */
const ErRuntimeConfig runtime_config = {
    /* … */
    .natives = native_modules,
    .native_count = NATIVE_COUNT,
};
```

When an app loads, what it uses is matched against the table by name, version, and signature. A missing or different
one stops it, and the error screen names it.

## 5. Post events for slow work

A call runs on the frame loop, so it must be quick: read a register, copy a value, start something. Work that takes
longer starts in a call and reports back with an event. The thermometer does this: `measure()` only sets a flag, and
the frame loop reads the sensor and posts the reading.

```c
static bool thermometer_measure(const int32_t* arguments, int32_t* result, void* context)
{
    s_measure_asked = true;
    return true;
}

/* In the frame loop. */
if (s_measure_asked)
{
    int32_t reading[THERMOMETER_READING_ARG_CELLS];
    erb_native_set_float(&reading[THERMOMETER_READING_ARG_CELSIUS], read_celsius());
    s_measure_asked = !erb_native_post(THERMOMETER_READING, reading);
}
```

Post from the frame loop's task, with `erb_native_post()` in Flow B or `er_runtime_native_post()` in Flow A, by the
event's index in the table. The app's handlers run at once, then its update. A post returns false when the runtime
can't take it now: no app is loaded, or a native function is posting from inside its own call. Keep the value and post
it again next frame, as above. Work on another task (a radio's callback, an interrupt) hands its result to the frame
loop through a flag or a queue, and the frame loop posts it.

Native code never calls into the app any other way. An event nothing subscribes to is dropped.

## 6. Make a module optional

A module only some boards have is declared with `nativeModule.optional`. An app that uses it still loads on a board
without it, and `available` says whether the firmware has all of it:

```ts title="examples/device/index.ts"
export const Thermometer = nativeModule.optional<Thermometer>('thermometer', 1);
```

```tsx title="App.tsx"
useEffect(() => {
  if (Thermometer.available) Thermometer.measure();
}, []);
useNativeEvent(Thermometer, 'reading', celsius =>
  setTemperature(`${Math.round(celsius)}`),
);
```

`available` is set at load and never changes, so render may read it to leave a page out. Read it where you use it
rather than keeping it in a const: Flow B can't keep it in one. A call to a module that isn't there stops the app, and
its events never arrive, so the CYD's watch face shows `--` for the temperature.

Everything in an optional module's interface counts toward an app's imports and events, used or not, since
`available` answers for all of it. A firmware that has every module declares them required, and its apps carry only
what they use.

## 7. Mock them for the simulator

The simulators have no firmware, so each module runs a mock there: `<module name>.mock.ts` beside the interface,
whose default export implements it. The package ships its mocks, so every app that imports a module runs in the
simulator. A mock that posts events is a function given `emit`:

```ts title="examples/device/chip.mock.ts"
import type {NativeMock} from 'embedded-react';
import type {Chip} from './index';

const chip: NativeMock<Chip> = emit => {
  let seconds = 0;
  setInterval(() => emit('second', ++seconds * 1000), 1000);
  return {
    name: () => 'Simulator',
    freeMemory: () => 180 * 1024,
  };
};

export default chip;
```

Values go through the declared types as they would on a board, so an `Int` is cut to a whole number and a `number`
to a float's precision. A required module with no mock stops the app in the simulator, naming the file to add. An
optional one with no mock is unavailable, which is how to try the app as a board without it would run. A board build
never includes the mocks.

## 8. A firmware that is never rebuilt

A firmware in the field keeps serving apps written after it shipped. Three things keep that working:

- **The package's version follows the firmware's.** Adding a module, a method or an event is safe: apps built
  against the older package still load. Changing a signature, a record's fields included, stops the apps that use it
  on the old firmware, so add a new method or bump the module's version instead, and keep the old entry in the table
  for as long as apps use it.
- **Optional modules cost their whole interface.** As [above](#6-make-a-module-optional), everything in one counts
  toward an app's imports and events. Keep a module that only some boards have small, and declare the rest required.
- **No value is kept between posts.** An app that loads after an event was posted waits for the next one. A value
  an app needs at any time gets a method beside its event: `Chip.freeMemory()` answers at once, and `second` keeps
  it fresh.

## Modules declared in the app

An app can still declare a module itself, with the interface in its own file or a local `.ts` file, and the mock
beside the interface:

```tsx title="App.tsx"
import {nativeModule} from 'embedded-react';
import type {Sensors} from './modules/sensors';

const Sensors = nativeModule<Sensors>('sensors', 1);
```

That suits an app written for one firmware. `native-header` reads the app's file the same way.

## Limits

- **A call must be quick.** It runs on the frame loop while the app's code waits for it, and so does the frame. Start
  slow work in a call and post an event when it is done.
- **Values, not callbacks.** A method takes and returns values; it can't take a function. Long-running work reports
  back by an event.
- **Sizes.** A method or event takes up to 16 values and returns up to 16, a record's fields counted one by one. A
  package has up to 32 imports and 32 event subscriptions, and Flow B copies a returned string into a 255-byte buffer.
- **Strings by reference.** A string argument is read in place, so copy it to keep it past the call. Flow B builds
  text only where it is kept, so build a string in a const before passing it.
- **No lists.** A method can't return a list. Post an event for each row instead.
- **TypeScript.** The interface is what gives each method its signature, so an app with native modules is written in
  TypeScript.

Flow B's [`useHostValue`](../api/hooks.md#usehostvalue) is still there for a single number the firmware sets, but a
module works in both flows, checks its types at load, and runs in the simulator.
