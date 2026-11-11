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

import {mkdtempSync, readFileSync, writeFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {dirname, join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {afterEach, describe, it, expect} from 'vitest';
import {loadAppFile} from '../app-file-loader.mts';
import {
  addNativeSignatures,
  readNativeInterfaces,
} from '../compiler/native-modules.mts';
import {nativeModule} from '../../src/embedded-react/nativeModule.js';

// Native modules in Flow A: the bundler gives nativeModule() each method's signature, and nativeModule() looks each
// one up in the bridge's __erNative. Whether the bridge calls them is test/erb-frames' job (native-calls.tsx in Flow
// A) and the bridge's native test.

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

  it('needs the signatures the bundler gives', () => {
    expect(() => nativeModule('sensors', 2)).toThrow(/has no signatures/);
  });
});
