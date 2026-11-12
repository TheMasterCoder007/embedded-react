/*
 * Copyright 2026 Cory Lamming
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

import {mkdirSync, mkdtempSync, readFileSync, writeFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {dirname, join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {afterEach, describe, it, expect} from 'vitest';
import Reconciler from 'react-reconciler';
import {createElement} from 'react';
import {loadAppFile} from '../app-file-loader.mts';
import {
  addNativeSignatures,
  nativeMockFiles,
  readNativeInterfaces,
} from '../compiler/native-modules.mts';
import {
  nativeModule,
  useNativeEvent,
} from '../../src/embedded-react/nativeModule.js';

// Native modules in Flow A: the bundler gives nativeModule() each method's and event's signature, and nativeModule()
// looks each one up in the bridge's __erNative, or in the simulators runs the module's mock. Whether the bridge calls
// them, whether useNativeEvent hears only while mounted, and whether a mock draws what the firmware does, is
// test/erb-frames' job (native-calls.tsx, native-events.tsx) and the bridge's native test.

const FIXTURES = join(
  dirname(fileURLToPath(import.meta.url)),
  '../../test/erb-frames/fixtures',
);

const SENSORS =
  "import {nativeModule} from 'embedded-react';\n" +
  "import type {Int} from 'embedded-react';\n" +
  'interface Sensors {\n' +
  '  read(channel: Int): number;\n' +
  '  pressed(button: Int): boolean;\n' +
  '  setLed(on: boolean): void;\n' +
  '  scale: (value: number) => number;\n' +
  '  name(): number[];\n' +
  '}\n' +
  "export const Sensors = nativeModule<Sensors>('sensors', 2);\n" +
  'interface Clock { now(): Int; }\n' +
  "const Clock = nativeModule<Clock>('clock', 1);\n";

/** The signatures the bundler gives a module with these methods and no events. */
const methods = (signatures: Record<string, unknown>) => ({
  methods: signatures,
  events: {},
});

/** A host's __erNative, offering `natives` (events after `events:`) and recording each call. */
function fakeHost(natives: string[]) {
  const calls: unknown[][] = [];
  return {
    calls,
    find: (name: string, version: number, signature: string) =>
      natives.indexOf(`${name}@${version}:${signature}`),
    findEvent: (name: string, version: number, signature: string) =>
      natives.indexOf(`events:${name}@${version}:${signature}`),
    call: (...callArguments: unknown[]) => {
      calls.push(callArguments);
      return 42;
    },
  };
}

/**
 * Renders `element` into a root of its own, with its effects run; returns a function that renders again. The
 * renderer needs the bridge's NativeUI, which a tree of components that draw nothing never calls.
 */
async function mount(element: unknown) {
  (globalThis as {NativeUI?: unknown}).NativeUI ??= new Proxy(
    {},
    {get: () => () => 0},
  );
  const {hostConfig} = await import('../../src/host-config.js');
  const reconciler = Reconciler(hostConfig as never);
  const root = reconciler.createContainer(
    {},
    0,
    null,
    false,
    null,
    '',
    () => {},
    null,
  );
  const render = (next: unknown) => {
    reconciler.updateContainer(next as never, root, null, null);
    reconciler.flushPassiveEffects();
  };
  render(element);
  return render;
}

describe('native module signatures in Flow A', () => {
  it("gives each nativeModule() call its methods' signatures", () => {
    const signed = addNativeSignatures(SENSORS, {});
    expect(signed).toContain(
      "nativeModule<Sensors>('sensors', 2, " +
        '{"methods":{"read":"f:i","pressed":"b:i","setLed":"v:b","scale":"f:f",' +
        '"name":{"reason":"returns a type native modules don\'t pass yet"}},"events":{}});',
    );
    expect(signed).toContain(
      'nativeModule<Clock>(\'clock\', 1, {"methods":{"now":"i:"},"events":{}});',
    );
  });

  it('reads an interface from a local file', () => {
    const source = readFileSync(join(FIXTURES, 'native-calls.tsx'), 'utf8');
    expect(
      addNativeSignatures(source, readNativeInterfaces(source, FIXTURES)),
    ).toContain(
      "nativeModule<Probe>('probe', 1, " +
        '{"methods":{"add":"i:ii","half":"f:i","even":"b:i","flip":"b:b","next":"i:","store":"v:f","stored":"f:",' +
        '"start":"v:i","refused":"i:","greet":"s:s","length":"i:s","shift":"l:il","announce":"v:i"},' +
        '"events":{"done":"v:ifb","idle":"v:","named":"v:sl"}});',
    );
  });

  it('holds a module to the rules Flow B does', () => {
    expect(() =>
      addNativeSignatures(
        "const Sensors = nativeModule<Missing>('sensors', 2);\n",
        {},
      ),
    ).toThrow(/can't find the interface Missing/);
    expect(() =>
      addNativeSignatures(
        'interface Odd { __proto__(): Int; }\n' +
          "const Odd = nativeModule<Odd>('odd', 1);\n",
        {},
      ),
    ).toThrow(/Odd has a method named __proto__/);
  });
});

describe('native module mocks', () => {
  it('gives each nativeModule() call its mock, or null when it has none', () => {
    const signed = addNativeSignatures(
      SENSORS,
      {},
      new Map([
        ['sensors', './sensors.mock.ts'],
        ['clock', null],
      ]),
    );
    expect(signed).toContain('"scale":"f:f",');
    expect(signed).toMatch(
      /"name":\{"reason":"[^"]*"\}\},"events":\{\}\}, __erNativeMock_sensors\);/,
    );
    expect(signed).toContain('{"now":"i:"},"events":{}}, null);');
    // The import goes at the end, so the app's lines keep their numbers.
    expect(signed.split('\n').slice(0, 11).join('\n')).not.toContain(
      'import __erNativeMock',
    );
    expect(signed).toMatch(
      /\nimport __erNativeMock_sensors from "\.\/sensors\.mock\.ts";\n$/,
    );
  });

  it("names a mock's import so it doesn't clash with the file's own names", () => {
    const signed = addNativeSignatures(
      'interface Clock { now(): Int; }\n' +
        'const __erNativeMock_clock = 1;\n' +
        "const Clock = nativeModule<Clock>('clock', 1);\n" +
        'interface Other { now(): Int; }\n' +
        "const Other = nativeModule<Other>('clock_', 1);\n",
      {},
      new Map([
        ['clock', './clock.mock.ts'],
        ['clock_', './clock_.mock.ts'],
      ]),
    );
    expect(signed).toContain(
      '{"now":"i:"},"events":{}}, __erNativeMock_clock_);',
    );
    expect(signed).toContain(
      '{"now":"i:"},"events":{}}, __erNativeMock_clock__);',
    );
  });

  it('keeps a module named __proto__', async () => {
    const directory = mkdtempSync(join(tmpdir(), 'native-mock-'));
    writeFileSync(join(directory, '__proto__.mock.ts'), 'export default {};\n');
    const appPath = join(directory, 'App.tsx');
    writeFileSync(
      appPath,
      'interface Odd { now(): Int; }\n' +
        "const Odd = nativeModule<Odd>('__proto__', 1);\n",
    );
    const result = await loadAppFile(appPath, undefined, {mocks: true});
    expect(result?.contents).toContain(
      'import __erNativeMock___proto__ from "./__proto__.mock.ts";',
    );
  });

  it('looks for a mock beside the interface', () => {
    const source = readFileSync(join(FIXTURES, 'native-calls.tsx'), 'utf8');
    expect(nativeMockFiles(source, '/elsewhere')).toEqual(
      new Map([
        [
          'probe',
          ['.ts', '.tsx', '.js', '.jsx'].map(extension =>
            join('/elsewhere', `probe.mock${extension}`),
          ),
        ],
      ]),
    );
    const files = nativeMockFiles(source, FIXTURES);
    expect(files.get('probe')?.[0]).toBe(join(FIXTURES, 'probe.mock.ts'));
  });

  it("looks in the interface file's folder", () => {
    const directory = mkdtempSync(join(tmpdir(), 'native-mock-'));
    mkdirSync(join(directory, 'native'));
    writeFileSync(
      join(directory, 'native', 'sensors.ts'),
      'export interface Sensors { read(): number; }\n',
    );
    const files = nativeMockFiles(
      "import type {Sensors} from './native/sensors';\n" +
        "const Sensors = nativeModule<Sensors>('sensors', 1);\n",
      directory,
    );
    expect(files.get('sensors')?.[0]).toBe(
      join(directory, 'native', 'sensors.mock.ts'),
    );
  });
});

describe('the Flow A app file loader', () => {
  const directory = mkdtempSync(join(tmpdir(), 'native-loader-'));
  const write = (name: string, source: string) => {
    const path = join(directory, name);
    writeFileSync(path, source);
    return path;
  };

  it('signs native modules and keeps the loader for the file', async () => {
    const result = await loadAppFile(write('App.tsx', SENSORS));
    expect(result?.loader).toBe('tsx');
    expect(result?.contents).toContain('{"now":"i:"}');
  });

  it('leaves a file without native modules to esbuild', async () => {
    expect(
      await loadAppFile(write('Plain.tsx', 'export const answer = 42;\n')),
    ).toBeUndefined();
  });

  it("runs the bundler's transform after the signatures", async () => {
    const result = await loadAppFile(
      write('Plain.jsx', 'export const answer = 42;\n'),
      source => source + '// transformed\n',
    );
    expect(result).toEqual({
      contents: 'export const answer = 42;\n// transformed\n',
      loader: 'jsx',
    });
  });

  it('reports a bad declaration where it is', async () => {
    const result = await loadAppFile(
      write(
        'Bad.tsx',
        "\nconst Sensors = nativeModule<Missing>('sensors', 2);\n",
      ),
    );
    const [error] = result?.errors ?? [];
    expect(error.text).toBe("can't find the interface Missing");
    expect(error.location).toMatchObject({line: 2, column: 16});
    expect(error.notes?.[0].text).toMatch(/declare it in this file/);
  });

  it('imports the mock beside the interface in the simulators', async () => {
    write('clock.mock.ts', 'export default {now: () => 5};\n');
    const result = await loadAppFile(write('Mocked.tsx', SENSORS), undefined, {
      mocks: true,
    });
    expect(result?.contents).toContain(
      '{"now":"i:"},"events":{}}, __erNativeMock_clock);',
    );
    expect(result?.contents).toContain(
      'import __erNativeMock_clock from "./clock.mock.ts";',
    );
    expect(result?.contents).toMatch(
      /"name":\{"reason":"[^"]*"\}\},"events":\{\}\}, null\);/,
    );
    // A mock added later rebuilds the app.
    expect(result?.watchFiles).toContain(join(directory, 'sensors.mock.ts'));
  });

  it('leaves the mocks out of a board build', async () => {
    const result = await loadAppFile(write('Board.tsx', SENSORS));
    expect(result?.contents).not.toContain('__erNativeMock');
    expect(result?.watchFiles).toBeUndefined();
  });

  it('leaves a syntax error to esbuild', async () => {
    expect(
      await loadAppFile(write('Broken.tsx', 'nativeModule(\n')),
    ).toBeUndefined();
  });
});

describe('nativeModule() in Flow A', () => {
  const global = globalThis as {__erNative?: unknown};
  afterEach(() => {
    delete global.__erNative;
  });

  it('calls each method through the bridge by its index', () => {
    const host = fakeHost(['sensors.read@2:f:i', 'sensors.setLed@2:v:b']);
    global.__erNative = host;
    const Sensors = nativeModule(
      'sensors',
      2,
      methods({read: 'f:i', setLed: 'v:b'}),
    );
    expect(Sensors.read(3)).toBe(42);
    Sensors.setLed(true);
    expect(host.calls).toEqual([
      [0, 3],
      [1, true],
    ]);
  });

  it('stops the app at load when the firmware lacks a method', () => {
    global.__erNative = fakeHost(['sensors.read@1:f:i']);
    expect(() => nativeModule('sensors', 2, methods({read: 'f:i'}))).toThrow(
      'missing native module: sensors.read (version 2, signature "f:i")',
    );
  });

  it('throws when a method that cannot be called is', () => {
    global.__erNative = fakeHost([]);
    const Sensors = nativeModule(
      'sensors',
      2,
      methods({
        name: {reason: "returns a type native modules don't pass yet"},
      }),
    );
    expect(() => Sensors.name()).toThrow(
      "sensors.name returns a type native modules don't pass yet",
    );
  });

  it('runs the mock in the simulators, not the firmware', () => {
    const host = fakeHost(['sensors.read@2:f:i']);
    global.__erNative = host;
    const Sensors = nativeModule('sensors', 2, methods({read: 'f:i'}), {
      read: (channel: number) => channel * 2,
    });
    expect(Sensors.read(3)).toBe(6);
    expect(host.calls).toEqual([]);
  });

  it("converts a mock's values as the bridge does", () => {
    const seen: unknown[][] = [];
    const Sensors = nativeModule(
      'sensors',
      2,
      methods({read: 'f:i', count: 'i:f', pressed: 'b:b', setLed: 'v:b'}),
      {
        read: (...values: unknown[]) => (seen.push(values), 0.1),
        count: (...values: unknown[]) => (seen.push(values), 7.9),
        pressed: (...values: unknown[]) => (seen.push(values), 2),
        setLed: (...values: unknown[]) => (seen.push(values), 'ignored'),
      },
    );
    // An Int is whole, a number goes through a C float, and a boolean is true or false.
    expect(Sensors.read(3.7)).toBe(Math.fround(0.1));
    expect(Sensors.count(0.1)).toBe(7);
    expect(Sensors.pressed(0)).toBe(true);
    expect(Sensors.setLed('yes')).toBeUndefined();
    expect(Sensors.read(2 ** 32 + 5)).toBe(Math.fround(0.1));
    expect(seen).toEqual([[3], [Math.fround(0.1)], [false], [true], [5]]);
  });

  it("converts a mock's strings and Int64s as the bridge does", () => {
    const seen: unknown[][] = [];
    const Words = nativeModule(
      'words',
      1,
      methods({greet: 's:s', shift: 'l:l'}),
      {
        greet: (...values: unknown[]) => (seen.push(values), 42),
        shift: (...values: unknown[]) => (seen.push(values), 2 ** 64 + 4096),
      },
    );
    // Any value passes as its string, which ends at a NUL. An Int64 is truncated and wraps at 64 bits.
    expect(Words.greet(7)).toBe('42');
    expect(Words.greet('a\0b')).toBe('42');
    expect(Words.shift(-5e9 - 0.5)).toBe(4096);
    expect(Words.shift(Number.NaN)).toBe(4096);
    expect(Words.shift(2 ** 63)).toBe(4096);
    expect(seen).toEqual([['7'], ['a'], [-5e9], [0], [-(2 ** 63)]]);
  });

  it('wraps an Int64 as BigInt.asIntN would, without BigInt', () => {
    const Clock = nativeModule('clock', 1, methods({echo: 'l:l'}), {
      echo: (value: number) => value,
    });
    const values = [
      -0.5,
      7.9,
      -5e9 - 0.5,
      2 ** 53 + 2,
      2 ** 63 - 2048,
      2 ** 63,
      -(2 ** 63),
      -(2 ** 63) - 4096,
      2 ** 64 + 4096,
      2 ** 70 + 2 ** 20,
      -(2 ** 90) - 2 ** 40,
      1e30,
      2 ** 116,
      Infinity,
    ];
    const expected = values.map(value =>
      Number.isFinite(value)
        ? Number(BigInt.asIntN(64, BigInt(Math.trunc(value))))
        : 0,
    );
    const {BigInt: saved} = globalThis;
    try {
      delete (globalThis as {BigInt?: unknown}).BigInt;
      expect(values.map(value => Clock.echo(value))).toEqual(expected);
    } finally {
      globalThis.BigInt = saved;
    }
  });

  it("calls a mock's methods on the mock, so it can keep state", () => {
    const Counter = nativeModule('counter', 1, methods({next: 'i:'}), {
      count: 0,
      next() {
        return ++this.count;
      },
    });
    Counter.next();
    expect(Counter.next()).toBe(2);
  });

  it('checks the arguments a mock is called with, as the bridge does', () => {
    const Sensors = nativeModule('sensors', 2, methods({read: 'f:i'}), {
      read: () => 1,
    });
    expect(() => Sensors.read()).toThrow(
      'sensors.read() takes 1 argument, not 0',
    );
  });

  it('stops the app at load when the mock lacks a method', () => {
    expect(() =>
      nativeModule('sensors', 2, methods({read: 'f:i', setLed: 'v:b'}), {
        read: () => 1,
      }),
    ).toThrow('the mock of native module "sensors" has no setLed()');
  });

  it("doesn't take a method a plain object inherits as the mock's", () => {
    expect(() =>
      nativeModule('sensors', 2, methods({toString: 'i:'}), {}),
    ).toThrow('the mock of native module "sensors" has no toString()');
  });

  it('stops the app at load when a module has no mock', () => {
    expect(() =>
      nativeModule('sensors', 2, methods({read: 'f:i'}), null),
    ).toThrow(
      'native module "sensors" has no mock for the simulator; add sensors.mock.ts beside its interface',
    );
  });

  it('needs the signatures the bundler gives', () => {
    expect(() => nativeModule('sensors', 2)).toThrow(/has no signatures/);
  });
});

describe('native events in Flow A', () => {
  const global = globalThis as {
    __erNative?: unknown;
    __erNativeEvent?: (index: number, values: unknown[]) => void;
  };
  afterEach(() => {
    delete global.__erNative;
    delete global.__erNativeEvent;
  });
  const events = (signatures: Record<string, unknown>) => ({
    methods: {},
    events: signatures,
  });

  it('looks each event up in the firmware and takes its posts', () => {
    global.__erNative = fakeHost([
      'sensors.read@2:f:i',
      'events:sensors.reading@2:v:f',
    ]);
    nativeModule('sensors', 2, events({reading: 'v:f'}));
    // Nothing subscribes yet, so the post is dropped, as is one for an index the app doesn't know.
    expect(() => global.__erNativeEvent!(1, [21.5])).not.toThrow();
    expect(() => global.__erNativeEvent!(7, [])).not.toThrow();
  });

  it('stops the app at load when the firmware lacks an event', () => {
    global.__erNative = fakeHost(['sensors.reading@2:v:f']);
    expect(() => nativeModule('sensors', 2, events({reading: 'v:f'}))).toThrow(
      'missing native module: sensors.reading (version 2, signature "v:f")',
    );
  });

  it("checks useNativeEvent's module and event before it subscribes", () => {
    global.__erNative = fakeHost(['events:sensors.reading@2:v:f']);
    const Sensors = nativeModule(
      'sensors',
      2,
      events({
        reading: 'v:f',
        name: {reason: "takes a type native modules don't pass yet"},
      }),
    );
    const handler = () => {};
    expect(() => useNativeEvent({}, 'reading', handler)).toThrow(
      'useNativeEvent() takes a native module, as nativeModule() returns it',
    );
    expect(() => useNativeEvent(Sensors, 'done', handler)).toThrow(
      'native module "sensors" has no event done',
    );
    expect(() => useNativeEvent(Sensors, 'name', handler)).toThrow(
      "sensors.name takes a type native modules don't pass yet",
    );
  });

  /** A component that subscribes to sensors.reading, recording what it hears. */
  const listener = (Sensors: unknown, heard: unknown[][]) => (): null => {
    useNativeEvent(Sensors, 'reading', (...values: unknown[]) =>
      heard.push(values),
    );
    return null;
  };

  it('calls a subscriber with the values the bridge posts while it is mounted', async () => {
    global.__erNative = fakeHost(['events:sensors.reading@2:v:f']);
    const Sensors = nativeModule('sensors', 2, events({reading: 'v:f'}));
    const heard: unknown[][] = [];
    const render = await mount(createElement(listener(Sensors, heard)));
    global.__erNativeEvent!(0, [21.5]);
    render(null);
    global.__erNativeEvent!(0, [22.5]);
    expect(heard).toEqual([[21.5]]);
  });

  it('reaches the subscribers of an app evaluated again, as a hot reload does', async () => {
    global.__erNative = fakeHost(['events:sensors.reading@2:v:f']);
    const before = nativeModule('sensors', 2, events({reading: 'v:f'}));
    const heardBefore: unknown[][] = [];
    await mount(createElement(listener(before, heardBefore)));
    const after = nativeModule('sensors', 2, events({reading: 'v:f'}));
    const heardAfter: unknown[][] = [];
    await mount(createElement(listener(after, heardAfter)));
    global.__erNativeEvent!(0, [21.5]);
    expect(heardBefore).toEqual([[21.5]]);
    expect(heardAfter).toEqual([[21.5]]);
  });

  it("gives a mock that is a function an emit, which posts the module's events", () => {
    let emit: (event: string, ...values: unknown[]) => boolean = () => false;
    const Sensors = nativeModule(
      'sensors',
      2,
      {methods: {start: 'v:'}, events: {reading: 'v:fb'}},
      (given: typeof emit) => {
        emit = given;
        return {start: () => emit('reading', 1, true)};
      },
    );
    // Taken outside the mock's calls, and refused inside one, as the bridge refuses a post from a native call.
    expect(emit('reading', 21.5, false)).toBe(true);
    let posted: boolean | undefined;
    const Refusing = nativeModule(
      'refusing',
      1,
      {methods: {start: 'v:'}, events: {reading: 'v:fb'}},
      (given: typeof emit) => ({
        start: () => {
          posted = given('reading', 1, true);
        },
      }),
    );
    Refusing.start();
    expect(posted).toBe(false);
    expect(() => emit('done')).toThrow(
      'native module "sensors" has no event done',
    );
    expect(() => emit('reading', 1)).toThrow(
      'sensors.reading is posted with 2 values, not 1',
    );
    expect(Sensors.start).toBeTypeOf('function');
  });
});

describe('nativeModule.optional() in Flow A', () => {
  const global = globalThis as {
    __erNative?: unknown;
    __erNativeEvent?: (index: number, values: unknown[]) => void;
  };
  afterEach(() => {
    delete global.__erNative;
    delete global.__erNativeEvent;
  });
  const WIFI = {
    methods: {
      connect: 'b:i',
      name: {reason: "returns a type native modules don't pass yet"},
    },
    events: {found: 'v:i'},
  };

  it('is available when the firmware has every method and event', () => {
    const host = fakeHost(['wifi.connect@1:b:i', 'events:wifi.found@1:v:i']);
    global.__erNative = host;
    const Wifi = nativeModule.optional('wifi', 1, WIFI);
    expect(Wifi.available).toBe(true);
    expect(Wifi.connect(6)).toBe(42);
    expect(host.calls).toEqual([[0, 6]]);
  });

  it("loads without a module the firmware lacks, or has only part of, and isn't available", () => {
    for (const natives of [[], ['wifi.connect@1:b:i']]) {
      global.__erNative = fakeHost(natives);
      const Wifi = nativeModule.optional('wifi', 1, WIFI);
      expect(Wifi.available).toBe(false);
      expect(() => Wifi.connect(6)).toThrow(
        /missing native module: wifi\.connect \(the optional module "wifi" isn't available/,
      );
    }
  });

  it('keeps available as it was set at load', () => {
    global.__erNative = fakeHost([]);
    const Wifi = nativeModule.optional('wifi', 1, WIFI);
    expect(() => {
      (Wifi as {available: boolean}).available = true;
    }).toThrow(TypeError);
    expect(Wifi.available).toBe(false);
  });

  it("lets a component subscribe to a missing module's events, which never arrive", async () => {
    global.__erNative = fakeHost(['events:wifi.found@1:v:i']);
    const Wifi = nativeModule.optional('wifi', 1, WIFI);
    const heard: unknown[][] = [];
    const Listener = (): null => {
      useNativeEvent(Wifi, 'found', (...values: unknown[]) =>
        heard.push(values),
      );
      return null;
    };
    await mount(createElement(Listener));
    global.__erNativeEvent?.(0, [-40]);
    expect(heard).toEqual([]);
  });

  it('is available in the simulators when it has a mock, and not when it has none', () => {
    const mocked = nativeModule.optional('wifi', 1, WIFI, {
      connect: () => true,
    });
    expect(mocked.available).toBe(true);
    expect(mocked.connect(6)).toBe(true);
    const unmocked = nativeModule.optional('wifi', 1, WIFI, null);
    expect(unmocked.available).toBe(false);
    expect(() => unmocked.connect(6)).toThrow(/isn't available/);
  });

  it('leaves available off a module the firmware must have', () => {
    global.__erNative = fakeHost(['sensors.read@2:f:i']);
    const Sensors = nativeModule('sensors', 2, methods({read: 'f:i'}));
    expect('available' in Sensors).toBe(false);
  });
});
