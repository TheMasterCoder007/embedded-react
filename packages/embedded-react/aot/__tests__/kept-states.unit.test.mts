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

import {describe, expect, it} from 'vitest';
import {compilePackage} from '../compile.mts';
import {Table} from '../compiler/package/format.mts';
import {OP} from '../compiler/package/bytecode.mts';

// A body reads the state its render had: one that sets a state and then reads it keeps a copy to read. Whether the
// copy shows the right frame is test/erb-frames' job (counter.tsx).

/** An app with `handler` as a button's onPress body, and `body` before the App's return. */
const appOf = (handler: string, body = '') =>
  "import {useEffect, useReducer, useState} from 'react';\n" +
  "import {Pressable, Text} from 'embedded-react';\n" +
  'export function App() {\n' +
  '  const [count, setCount] = useState(0);\n' +
  '  const [other, setOther] = useState(0);\n' +
  "  const [label, setLabel] = useState('none');\n" +
  "  const [mode, setMode] = useState('auto');\n" +
  '  const [point, setPoint] = useState({x: 0, y: 0});\n' +
  body +
  `  return <Pressable onPress={() => { ${handler} }}>` +
  '<Text>{count}{other}{label}{mode}{point.x}{point.y}</Text></Pressable>;\n' +
  '}\n';

const OP_NAMES = Object.fromEntries(
  Object.entries(OP).map(([name, {code}]) => [code, name]),
);

/**
 * Each function's slot reads and writes, in order: `get 3`, `set 3`.
 *
 * @param source  The app.
 *
 * @returns The functions that write a slot.
 */
function slotOps(source: string): string[][] {
  const bytes = compilePackage(source, 'app', {
    filename: 'App.tsx',
    screen: {width: 240, height: 320},
  }).program;
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.length);
  const u16 = (offset: number) => view.getUint16(offset, true);
  const u32 = (offset: number) => view.getUint32(offset, true);
  const directory = 24 + u16(18) * 16 + u16(20) * 8;
  const tables = new Map<number, {offset: number; length: number}>();
  for (let index = 0; index < u16(22); index++) {
    const entry = directory + index * 12;
    tables.set(u16(entry), {offset: u32(entry + 4), length: u32(entry + 8)});
  }
  const functions = tables.get(Table.functions)!;
  const code = tables.get(Table.code)!;
  const found: string[][] = [];
  for (let index = 0; index < functions.length / 16; index++) {
    const record = functions.offset + index * 16;
    const start = u32(record);
    const ops: string[] = [];
    for (let at = start; at < start + u32(record + 4);) {
      const word = u32(code.offset + at * 4);
      const name = OP_NAMES[word & 0xff];
      const slot = word >>> 16;
      if (/^GET(32|64|S)$/.test(name)) ops.push(`get ${slot}`);
      if (/^(SET(32|64|S)|FMTSET)$/.test(name)) ops.push(`set ${slot}`);
      at += OP[name as keyof typeof OP].words;
    }
    if (ops.some(op => op.startsWith('set'))) found.push(ops);
  }
  return found;
}

/** The handler's reads and writes: the first function that writes a slot, without the update request it ends on. */
const handlerOps = (handler: string, body = '') =>
  slotOps(appOf(handler, body))[0].slice(0, -2);

/** The copies a body keeps, as `state>copy`: a slot set once, from another, and read after. */
function copiesOf(ops: string[]): string[] {
  const copies: string[] = [];
  for (let at = 0; at + 1 < ops.length; at++) {
    const [from, into] = [ops[at].slice(4), ops[at + 1].slice(4)];
    if (
      ops[at].startsWith('get') &&
      ops[at + 1].startsWith('set') &&
      from !== into &&
      ops.filter(op => op === `set ${into}`).length === 1 &&
      ops.indexOf(`get ${into}`, at + 2) >= 0
    ) {
      copies.push(`${from}>${into}`);
    }
  }
  return copies;
}

describe('a state read after its setter', () => {
  it('reads a copy kept at the start of the handler', () => {
    // The copy, then count + 1 from it into the state, then the copy into the other state.
    expect(handlerOps('setCount(count + 1); setOther(count);')).toEqual([
      'get 0',
      'set 6',
      'get 6',
      'set 0',
      'get 6',
      'set 1',
    ]);
  });

  it('keeps no copy when nothing reads the state after', () => {
    expect(handlerOps('setOther(count); setCount(count + 1);')).toEqual([
      'get 0',
      'set 1',
      'get 0',
      'set 0',
    ]);
  });

  it('adds once for two setters given the same read', () => {
    expect(handlerOps('setCount(count + 1); setCount(count + 1);')).toEqual([
      'get 0',
      'set 6',
      'get 6',
      'set 0',
      'get 6',
      'set 0',
    ]);
  });

  it('gives an updater the value the state holds now', () => {
    // `previous` reads the state, and `count` the copy.
    expect(
      handlerOps(
        'setCount(count + 1); setCount(previous => previous + count);',
      ),
    ).toEqual(['get 0', 'set 6', 'get 6', 'set 0', 'get 0', 'get 6', 'set 0']);
    expect(
      copiesOf(
        handlerOps('setCount(count + 1); setCount(previous => previous + 1);'),
      ),
    ).toEqual([]);
    expect(
      copiesOf(
        handlerOps(
          'setCount(count + 1); setCount(previous => { if (previous > 2) return 0; return previous + 1; });',
        ),
      ),
    ).toEqual([]);
  });

  it('copies a string and an interned string', () => {
    expect(
      copiesOf(
        handlerOps("setLabel(label + '!'); setCount(label === 'a' ? 1 : 0);"),
      ),
    ).toHaveLength(1);
    expect(
      copiesOf(
        handlerOps("setMode('heat'); setCount(mode === 'auto' ? 1 : 0);"),
      ),
    ).toHaveLength(1);
  });

  it("copies an object state's field, which its updater reads as it is now", () => {
    expect(
      copiesOf(
        handlerOps('setPoint({...point, x: point.x + 1}); setCount(point.x);'),
      ),
    ).toHaveLength(1);
    expect(
      copiesOf(
        handlerOps(
          'setPoint({...point, x: 1}); setPoint(previous => ({...previous, x: previous.x + 1}));',
        ),
      ),
    ).toEqual([]);
  });

  it('copies in a helper, an effect and a timer', () => {
    expect(
      copiesOf(
        handlerOps(
          'bump(1);',
          '  const bump = (step: number) => { setCount(count + step); setOther(count + step); };\n',
        ),
      ),
    ).toHaveLength(1);
    const bodies = slotOps(
      appOf(
        '',
        '  useEffect(() => { setCount(count + 1); setOther(count); ' +
          'setTimeout(() => { setCount(count + 2); setOther(count); }, 10); }, [label]);\n',
      ),
    ).filter(ops => copiesOf(ops).length === 1);
    // The effect's body, which the update runs too, and the timer's.
    expect(bodies.length).toBeGreaterThanOrEqual(2);
  });

  it('copies a state a loop sets and reads on its next visit', () => {
    expect(
      copiesOf(
        handlerOps(
          'for (let i = 0; i < 3; i++) { setOther(count + i); setCount(i); }',
        ),
      ),
    ).toHaveLength(1);
  });

  it("gives a reducer's state the value it holds now", () => {
    const source = appOf('dispatch(1); dispatch(2);').replace(
      '  const [count, setCount] = useState(0);\n',
      '  const [count, dispatch] = useReducer((total: number, step: number) => total + step, 0);\n',
    );
    expect(copiesOf(slotOps(source)[0])).toEqual([]);
  });
});
