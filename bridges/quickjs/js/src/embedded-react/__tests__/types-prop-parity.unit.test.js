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

// Guards the shipped types against the runtime at the PROP level (types-parity covers the value exports).
// `npm run typecheck` only proves index.d.ts compiles — it cannot notice a prop the engine gained and
// nobody typed, and it cannot notice a prop that was only ever a type. Both are silent failures for the
// app author: the first makes a supported prop an error, the second makes an inert prop typecheck.
//
// The runtime's three authoritative lists are read straight from source:
//   k_prop_names          (native_ui_bridge.c) — every style/flat prop the bridge marshals
//   event_type_from_name  (native_ui_bridge.c) — every handler name that maps to an engine event
//   PASSTHROUGH           (src/props.js)       — the props buildProps forwards outside of `style`
//
// The check is name-level: a prop must appear as a declared key SOMEWHERE in index.d.ts. Which interface
// it belongs on is a judgement call (style vs component, View vs Image) that this test does not make.

const HERE = dirname(fileURLToPath(import.meta.url));
const SRC = resolve(HERE, '..');
const BRIDGE_C = resolve(HERE, '../../../../native_ui_bridge.c');

/** Strip // line and block comments so a name mentioned only in prose never counts as declared. */
const stripComments = src =>
  src.replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/[^\n]*/g, '');

// Props the renderer owns and an app never writes, so they are deliberately absent from the public types.
// `text` is filled in from a <Text>'s children by buildProps.
const INTERNAL_PROPS = new Set(['text']);

/** The JS prop names of the bridge's `k_prop_names` table (designated initializers keep it exhaustive). */
const bridgePropNames = c =>
  [...c.matchAll(/\[PROP_[A-Z0-9_]+\]\s*=\s*"([A-Za-z]+)"/g)].map(m => m[1]);

/** The handler names of the bridge's `event_type_from_name` map. */
const bridgeEventNames = c =>
  [...c.matchAll(/\{"(on[A-Za-z]+)",\s*ER_EVENT_/g)].map(m => m[1]);

/** The entries of props.js's PASSTHROUGH array. */
function passthroughNames(js) {
  const body = js.match(/export const PASSTHROUGH = \[([\s\S]*?)\];/);
  if (!body) throw new Error('props.js: could not find the PASSTHROUGH array');
  return [...body[1].matchAll(/'([A-Za-z]+)'/g)].map(m => m[1]);
}

/** Names declared as an object key (`name?: T` / `name: T`) anywhere in the declarations. */
const declaredKeys = dts =>
  new Set(
    [...stripComments(dts).matchAll(/^\s*([A-Za-z][A-Za-z0-9]*)\??:/gm)].map(
      m => m[1],
    ),
  );

/** Handler-shaped keys (`onFoo?: …`) declared in the declarations. */
const declaredHandlers = dts => [
  ...new Set(
    [...stripComments(dts).matchAll(/^\s*(on[A-Z][A-Za-z0-9]*)\??:/gm)].map(
      m => m[1],
    ),
  ),
];

describe('embedded-react public types match the runtime prop surface', () => {
  const c = readFileSync(BRIDGE_C, 'utf8');
  const dts = readFileSync(resolve(SRC, 'index.d.ts'), 'utf8');
  const js = readFileSync(resolve(SRC, '../props.js'), 'utf8');

  const props = bridgePropNames(c);
  const events = bridgeEventNames(c);
  const passthrough = passthroughNames(js);
  const declared = declaredKeys(dts);

  it('parses a non-trivial set of names from each runtime table (sanity)', () => {
    expect(props.length).toBeGreaterThan(80);
    expect(events.length).toBeGreaterThan(10);
    expect(passthrough.length).toBeGreaterThan(20);
    expect(declared.size).toBeGreaterThan(80);
  });

  it('declares every prop the bridge marshals', () => {
    const missing = props
      .filter(p => !INTERNAL_PROPS.has(p) && !declared.has(p))
      .sort();
    expect(
      missing,
      `native_ui_bridge.c marshals these but index.d.ts does not declare them: ${missing.join(', ')}`,
    ).toEqual([]);
  });

  it('declares every prop buildProps forwards outside of style', () => {
    const missing = passthrough.filter(p => !declared.has(p)).sort();
    expect(
      missing,
      `props.js PASSTHROUGH forwards these but index.d.ts does not declare them: ${missing.join(', ')}`,
    ).toEqual([]);
  });

  it('declares every event handler the bridge recognises', () => {
    const missing = events.filter(e => !declared.has(e)).sort();
    expect(
      missing,
      `native_ui_bridge.c routes these handlers but index.d.ts does not declare them: ${missing.join(', ')}`,
    ).toEqual([]);
  });

  it('declares no handler the bridge would ignore', () => {
    const phantom = declaredHandlers(dts)
      .filter(n => !events.includes(n))
      .sort();
    expect(
      phantom,
      `index.d.ts declares these handlers but native_ui_bridge.c never routes them (a typed no-op): ${phantom.join(', ')}`,
    ).toEqual([]);
  });
});
