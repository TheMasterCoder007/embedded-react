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

import {describe, it, expect} from 'vitest';
import {readFileSync} from 'node:fs';
import {fileURLToPath} from 'node:url';
import {dirname, resolve} from 'node:path';
import {NAMED_COLORS} from '../named-colors.js';
import {parseColor} from '../svg-ops.js';
import {parseColorValue} from '../../../aot/style-map.mts';

// Flow A styles (the bridge), SVG paint and Flow B must resolve a name to the same color.

const HERE = dirname(fileURLToPath(import.meta.url));
const BRIDGE_C = resolve(HERE, '../../../../native_ui_bridge.c');

/** The bridge's k_named table, as {name: argb}. */
function bridgeNamedColors() {
  const c = readFileSync(BRIDGE_C, 'utf8');
  const table = c.match(/k_named\[\]\s*=\s*\{([\s\S]*?)\};/);
  if (!table) throw new Error('native_ui_bridge.c: could not find k_named');
  return Object.fromEntries(
    [...table[1].matchAll(/\{"([a-z]+)",\s*0x([0-9A-Fa-f]{8})U\}/g)].map(m => [
      m[1],
      parseInt(m[2], 16),
    ]),
  );
}

const NAMES = Object.keys(NAMED_COLORS);

describe('named colors', () => {
  it('the bridge lists the same names and values', () => {
    expect(bridgeNamedColors()).toEqual({...NAMED_COLORS});
  });

  it('uses CSS values', () => {
    expect(NAMED_COLORS.green).toBe(0xff008000);
    expect(NAMED_COLORS.orange).toBe(0xffffa500);
  });

  it.each(NAMES)('SVG paint and Flow B agree on %s', name => {
    expect(parseColor(name)).toBe(NAMED_COLORS[name]);
    expect(parseColorValue(name)).toBe(NAMED_COLORS[name]);
  });

  it('matches names in any case', () => {
    expect(parseColor('Green')).toBe(0xff008000);
    expect(parseColorValue('ORANGE')).toBe(0xffffa500);
  });

  it('does not find names on Object.prototype', () => {
    expect('constructor' in NAMED_COLORS).toBe(false);
    expect(() => parseColorValue('constructor')).toThrow(/unsupported color/);
    expect(parseColor('constructor')).toBe(0xff000000);
  });
});
