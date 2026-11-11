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
import {loadAppFile} from '../app-file-loader.mts';
import {
  addNativeSignatures,
  nativeMockFiles,
  readNativeInterfaces,
} from '../compiler/native-modules.mts';
import {nativeModule} from '../../src/embedded-react/nativeModule.js';

// Native modules in Flow A: the bundler gives nativeModule() each method's signature, and nativeModule() looks each
// one up in the bridge's __erNative, or in the simulators runs the module's mock. Whether the bridge calls them, and
// whether a mock draws what the firmware does, is test/erb-frames' job (native-calls.tsx) and the bridge's native test.

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
  '  name(): string;\n' +
  '}\n' +
  "export const Sensors = nativeModule<Sensors>('sensors', 2);\n" +
  'interface Clock { now(): Int; }\n' +
  "const Clock = nativeModule<Clock>('clock', 1);\n";

/** A host's __erNative, offering `natives` and recording each call. */
function fakeHost(natives: string[]) {
  const calls: unknown[][] = [];
  return {
    calls,
    find: (name: string, version: number, signature: string) =>
      natives.indexOf(`${name}@${version}:${signature}`),
    call: (...callArguments: unknown[]) => {
      calls.push(callArguments);
      return 42;
    },
  };
}

describe('native module signatures in Flow A', () => {
  it("gives each nativeModule() call its methods' signatures", () => {
    const signed = addNativeSignatures(SENSORS, {});
    expect(signed).toContain(
      "nativeModule<Sensors>('sensors', 2, " +
        '{"read":"f:i","pressed":"b:i","setLed":"v:b","scale":"f:f",' +
        '"name":{"reason":"returns a string, which native modules don\'t pass yet"}});',
    );
    expect(signed).toContain(
      'nativeModule<Clock>(\'clock\', 1, {"now":"i:"});',
    );
  });

  it('reads an interface from a local file', () => {
    const source = readFileSync(join(FIXTURES, 'native-calls.tsx'), 'utf8');
    expect(
      addNativeSignatures(source, readNativeInterfaces(source, FIXTURES)),
    ).toContain(
      "nativeModule<Probe>('probe', 1, " +
        '{"add":"i:ii","half":"f:i","even":"b:i","flip":"b:b","next":"i:","store":"v:f","stored":"f:"});',
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
      /"name":\{"reason":"[^"]*"\}\}, __erNativeMock_sensors\);/,
    );
    expect(signed).toContain('{"now":"i:"}, null);');
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
    expect(signed).toContain('{"now":"i:"}, __erNativeMock_clock_);');
    expect(signed).toContain('{"now":"i:"}, __erNativeMock_clock__);');
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
    expect(result?.contents).toContain('{"now":"i:"}, __erNativeMock_clock);');
    expect(result?.contents).toContain(
      'import __erNativeMock_clock from "./clock.mock.ts";',
    );
    expect(result?.contents).toMatch(/"name":\{"reason":"[^"]*"\}\}, null\);/);
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
    const Sensors = nativeModule('sensors', 2, {read: 'f:i', setLed: 'v:b'});
    expect(Sensors.read(3)).toBe(42);
    Sensors.setLed(true);
    expect(host.calls).toEqual([
      [0, 3],
      [1, true],
    ]);
  });

  it('stops the app at load when the firmware lacks a method', () => {
    global.__erNative = fakeHost(['sensors.read@1:f:i']);
    expect(() => nativeModule('sensors', 2, {read: 'f:i'})).toThrow(
      'missing native module: sensors.read (version 2, signature "f:i")',
    );
  });

  it('throws when a method that cannot be called is', () => {
    global.__erNative = fakeHost([]);
    const Sensors = nativeModule('sensors', 2, {
      name: {reason: "returns a string, which native modules don't pass yet"},
    });
    expect(() => Sensors.name()).toThrow(
      "sensors.name returns a string, which native modules don't pass yet",
    );
  });

  it('runs the mock in the simulators, not the firmware', () => {
    const host = fakeHost(['sensors.read@2:f:i']);
    global.__erNative = host;
    const Sensors = nativeModule(
      'sensors',
      2,
      {read: 'f:i'},
      {read: (channel: number) => channel * 2},
    );
    expect(Sensors.read(3)).toBe(6);
    expect(host.calls).toEqual([]);
  });

  it("converts a mock's values as the bridge does", () => {
    const seen: unknown[][] = [];
    const Sensors = nativeModule(
      'sensors',
      2,
      {read: 'f:i', count: 'i:f', pressed: 'b:b', setLed: 'v:b'},
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

  it("calls a mock's methods on the mock, so it can keep state", () => {
    const Counter = nativeModule(
      'counter',
      1,
      {next: 'i:'},
      {
        count: 0,
        next() {
          return ++this.count;
        },
      },
    );
    Counter.next();
    expect(Counter.next()).toBe(2);
  });

  it('checks the arguments a mock is called with, as the bridge does', () => {
    const Sensors = nativeModule('sensors', 2, {read: 'f:i'}, {read: () => 1});
    expect(() => Sensors.read()).toThrow(
      'sensors.read() takes 1 argument, not 0',
    );
  });

  it('stops the app at load when the mock lacks a method', () => {
    expect(() =>
      nativeModule('sensors', 2, {read: 'f:i', setLed: 'v:b'}, {read: () => 1}),
    ).toThrow('the mock of native module "sensors" has no setLed()');
  });

  it("doesn't take a method a plain object inherits as the mock's", () => {
    expect(() => nativeModule('sensors', 2, {toString: 'i:'}, {})).toThrow(
      'the mock of native module "sensors" has no toString()',
    );
  });

  it('stops the app at load when a module has no mock', () => {
    expect(() => nativeModule('sensors', 2, {read: 'f:i'}, null)).toThrow(
      'native module "sensors" has no mock for the simulator; add sensors.mock.ts beside its interface',
    );
  });

  it('needs the signatures the bundler gives', () => {
    expect(() => nativeModule('sensors', 2)).toThrow(/has no signatures/);
  });
});
