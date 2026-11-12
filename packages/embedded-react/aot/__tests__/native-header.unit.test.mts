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
import {describe, it, expect} from 'vitest';
import {nativeHeaders} from '../compiler/native-header.mts';
import {readModuleFile} from '../compiler/native-files.mts';

// The C header a module's interface becomes. Whether its offsets match the compiler is test/erb-frames' job: the
// host packs with tools/erb-frames/probe_module.h.

const HERE = dirname(fileURLToPath(import.meta.url));
const FIXTURES = join(HERE, '../../test/erb-frames/fixtures');
const HOST = join(HERE, '../../../../tools/erb-frames');
const DEVICE = join(HERE, '../../../../examples/device');

const IMPORTS =
  "import {nativeModule} from 'embedded-react';\n" +
  "import type {Int, Int64} from 'embedded-react';\n";

const headerOf = (source: string): string => {
  const headers = nativeHeaders(IMPORTS + source);
  expect(headers).toHaveLength(1);
  return headers[0].text;
};

const directoryOf = (files: Record<string, string>): string => {
  const directory = mkdtempSync(join(tmpdir(), 'native-header-'));
  for (const [path, source] of Object.entries(files)) {
    mkdirSync(dirname(join(directory, path)), {recursive: true});
    writeFileSync(join(directory, path), source);
  }
  return directory;
};

describe('native-header', () => {
  it("the erb-frames host's header is what its module generates", () => {
    const {source, files} = readModuleFile('native-probe-module.ts', FIXTURES);
    const [header, ...others] = nativeHeaders(source, files);
    expect(others).toEqual([]);
    expect(header.file).toBe('probe_module.h');
    // Stale? npx embedded-react native-header test/erb-frames/fixtures/native-probe-module.ts --out ../../tools/erb-frames
    expect(header.text).toBe(
      readFileSync(join(HOST, header.file), 'utf8').replace(/\r\n/g, '\n'),
    );
  });

  it("the board examples' headers are what their device package generates", () => {
    const {source, files} = readModuleFile('.', DEVICE);
    const headers = nativeHeaders(source, files);
    expect(headers.map(header => header.file)).toEqual([
      'chip_module.h',
      'thermometer_module.h',
      'motion_module.h',
    ]);
    // Stale? npx embedded-react native-header ../../examples/device --out ../../examples/device/include
    for (const header of headers) {
      expect(header.text).toBe(
        readFileSync(join(DEVICE, 'include', header.file), 'utf8').replace(
          /\r\n/g,
          '\n',
        ),
      );
    }
  });

  it('names a module, its methods and its arguments by their words', () => {
    const header = headerOf(
      'export interface Gps {\n' +
        '  lastFix(sinceMs: Int64, maxAge: Int): boolean;\n' +
        '  events: {fixLost(): void};\n' +
        '}\n' +
        "export const Gps = nativeModule.optional<Gps>('gpsRadio', 3);\n",
    );
    expect(header).toContain('#ifndef GPS_RADIO_MODULE_H');
    expect(header).toContain(
      '#define GPS_RADIO_LAST_FIX_ENTRY "gpsRadio.lastFix", 3, "b:li"',
    );
    expect(header).toContain('#define GPS_RADIO_LAST_FIX_ARG_SINCE_MS 0');
    expect(header).toContain('#define GPS_RADIO_LAST_FIX_ARG_MAX_AGE 2');
    expect(header).toContain('#define GPS_RADIO_LAST_FIX_ARG_CELLS 3');
    expect(header).toContain(
      '#define GPS_RADIO_FIX_LOST_ENTRY "gpsRadio.fixLost", 3, "v:"',
    );
    expect(header).not.toContain('/* Records */');
    expect(
      nativeHeaders(
        IMPORTS +
          'interface A { go(): void }\nconst a = nativeModule<A>("gpsRadio", 1);',
      )[0].file,
    ).toBe('gps_radio_module.h');
  });

  it('names a record with no name after where it is', () => {
    const header = headerOf(
      'interface Can {\n' +
        '  send(frame: {id: Int; data: {low: Int; high: Int}}): {ok: boolean};\n' +
        '}\n' +
        "export const Can = nativeModule<Can>('can', 1);\n",
    );
    expect(header).toContain('} CanSendFrameData;');
    expect(header).toContain('    CanSendFrameData data;');
    expect(header).toContain('} CanSendFrame;');
    expect(header).toContain('#define CAN_SEND_FRAME_CELLS 3');
    expect(header).toContain('} CanSendResult;');
    expect(header).toContain('/* send(frame: CanSendFrame): CanSendResult */');
    expect(
      headerOf(
        'interface Can { sendFrame(rawData: {id: Int}): void }\n' +
          "export const Can = nativeModule<Can>('can', 1);\n",
      ),
    ).toContain('#define CAN_SEND_FRAME_RAW_DATA_CELLS 1');
    expect(header.indexOf('} CanSendFrameData;')).toBeLessThan(
      header.indexOf('} CanSendFrame;'),
    );
  });

  it('writes a record used twice once', () => {
    const header = headerOf(
      'interface Point { x: Int; y: Int }\n' +
        'interface Touch {\n' +
        '  last(): Point;\n' +
        '  events: {moved(from: Point, to: Point): void};\n' +
        '}\n' +
        "export const Touch = nativeModule<Touch>('touch', 1);\n",
    );
    expect(header.match(/} TouchPoint;/g)).toHaveLength(1);
    expect(header).toContain('#define TOUCH_MOVED_ARG_TO 2');
  });

  it('writes a header for each module a package declares', () => {
    const {source, files} = readModuleFile('probe-device', FIXTURES);
    expect(nativeHeaders(source, files).map(header => header.file)).toEqual([
      'probe_module.h',
      'radio_module.h',
    ]);
  });

  it('reads a package by its folder, and refuses one with no .ts source', () => {
    const directory = directoryOf({
      'device/package.json': '{"name": "device", "exports": "./src/index.ts"}',
      'device/src/index.ts':
        IMPORTS +
        "import type {Can as CanInterface} from './can';\n" +
        "export const Can = nativeModule<CanInterface>('can', 1);\n",
      'device/src/can.ts': 'export interface Can { reset(): void }\n',
      'built/package.json': '{"name": "built", "main": "./index.js"}',
      'built/index.js': 'export const Can = {};\n',
    });
    const {path, source, files} = readModuleFile('device', directory);
    expect(path).toBe(join(directory, 'device/src/index.ts'));
    expect(nativeHeaders(source, files)[0].text).toContain(
      '#define CAN_RESET_ENTRY "can.reset", 1, "v:"',
    );
    expect(() => readModuleFile('built', directory)).toThrow(
      /JavaScript or a \.d\.ts/,
    );
    expect(() => readModuleFile('missing', directory)).toThrow(
      /No file or package at missing/,
    );
  });

  it('refuses what the firmware could not offer', () => {
    const refused = (source: string): string => {
      try {
        nativeHeaders(IMPORTS + source);
      } catch (error) {
        return (error as Error).message;
      }
      return 'no error';
    };
    expect(refused('export const count = 1;\n')).toBe(
      'The file declares no native modules',
    );
    expect(
      refused(
        'interface Can { send(data: number[]): void }\n' +
          "export const Can = nativeModule<Can>('can', 1);\n",
      ),
    ).toBe("Can.send takes a type native modules don't pass");
    expect(
      refused(
        'interface Fix { class: Int }\n' +
          'interface Gps { fix(): Fix }\n' +
          "export const Gps = nativeModule<Gps>('gps', 1);\n",
      ),
    ).toBe(
      'Gps: the record Fix has a field named class, which C keeps for itself',
    );
    for (const field of ['constexpr', 'wchar_t', '_Atomic', 'low__high']) {
      expect(
        refused(
          `interface Fix { ${field}: Int }\n` +
            'interface Gps { fix(): Fix }\n' +
            "export const Gps = nativeModule<Gps>('gps', 1);\n",
        ),
      ).toBe(
        `Gps: the record Fix has a field named ${field}, which C keeps for itself`,
      );
    }
    expect(
      refused(
        'interface Wire { read(): Int }\n' +
          "export const Wire = nativeModule<Wire>('_1wire', 1);\n",
      ),
    ).toBe(
      'Wire: the module is named _1wire, so its names in the header would start with a digit',
    );
    expect(
      refused(
        'interface Wire { read(): Int }\n' +
          "export const Wire = nativeModule<Wire>('_', 1);\n",
      ),
    ).toBe(
      'Wire: the module is named _, which leaves nothing to name it by in the header',
    );
    expect(
      refused(
        'interface Wire { _(): Int }\n' +
          "export const Wire = nativeModule<Wire>('wire', 1);\n",
      ),
    ).toBe(
      'Wire: a method or event is named _, which leaves nothing to name it by in the header',
    );
    expect(
      refused(
        'interface Gps { lastFix(): Int; last_fix(): Int }\n' +
          "export const Gps = nativeModule<Gps>('gps', 1);\n",
      ),
    ).toBe(
      'Gps: lastFix() and last_fix() would both be GPS_LAST_FIX_ENTRY in the header',
    );
    expect(
      refused(
        'interface A { go(): void }\n' +
          'interface B { go(): void }\n' +
          "export const A = nativeModule<A>('fooBar', 1);\n" +
          "export const B = nativeModule<B>('foo_bar', 1);\n",
      ),
    ).toBe(
      'The modules "fooBar" and "foo_bar" would both be written to foo_bar_module.h',
    );
  });
});
