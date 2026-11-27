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

// The props of the node tree are listed four times: the engine's descriptor names them (ERDebugProp) and gives each
// one's offset, and gdb/erb.py and the simulator's debugger each say how one is read and shown. A prop added to one
// and not the others would read every prop after it from the wrong place.

import {readFileSync} from 'node:fs';
import {fileURLToPath} from 'node:url';
import {describe, expect, it} from 'vitest';
import {NODE_PROPS} from '../aot-sim/nodes.mts';

const read = (path: string): string =>
  readFileSync(fileURLToPath(new URL(path, import.meta.url)), 'utf8');

/** The engine's props, in the enum's order, without the prefix. */
const declared = [
  ...read('../../../engine/include/er_debug.h')
    .split('typedef enum')
    .find(part => part.includes('} ERDebugProp;'))!
    .matchAll(/ER_DEBUG_PROP_(\w+?)(?: = 0)?,/g),
]
  .map(match => match[1])
  .filter(name => name !== 'COUNT_');

describe('node tree props', () => {
  it('has an offset in the engine for each prop it names', () => {
    const offsets = [
      ...read('../../../engine/scene/compositor.c').matchAll(
        /\[ER_DEBUG_PROP_(\w+)\] = offsetof\(ERNode, /g,
      ),
    ].map(match => match[1]);
    expect(offsets).toEqual(declared);
  });

  it("reads the engine's props in the simulator's debugger, in their order", () => {
    expect(NODE_PROPS.map(([name]) => name)).toEqual(declared);
  });

  it('reads and shows each prop in gdb/erb.py as the simulator does', () => {
    const table = read('../gdb/erb.py')
      .split('NODE_PROPS = [\n')[1]
      .split('\n]\n')[0];
    const rows = [
      ...table.matchAll(/^ {4}\("(\w+)", "([\w.]+)", (.+), "(\w+)"\),$/gm),
    ].map(([, name, shown, kind, nodes]) => [
      name,
      shown,
      JSON.parse(
        kind.replace(/^\(/, '[').replace(/\)$/, ']').replaceAll('None', 'null'),
      ),
      nodes,
    ]);
    expect(rows).toEqual(NODE_PROPS);
  });
});
