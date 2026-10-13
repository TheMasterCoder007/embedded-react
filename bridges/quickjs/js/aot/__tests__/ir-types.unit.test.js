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
import {
  BOOL,
  FLOAT,
  INT,
  INT64_CONSTANT,
  INT64_MATH,
  NODE,
  STRING,
  STRUCT,
  TIME,
  isIntSlot,
  plainOf,
  refType,
  slotOf,
  typeOfCExpr,
  typeOfSlot,
} from '../compiler/ir/types.mts';
import {compileSource} from '../compile.mts';
import {inferWideSlots} from '../compiler/expressions.mts';
import {lowerApp} from '../compiler/lower.mts';
import {resolveOptions} from '../compiler/options.mts';

// The IR types its values itself; a backend picks the slot each one is stored in.

describe('IR types', () => {
  it('stores a boolean in an int, and every 64-bit number in an int64', () => {
    expect(
      [BOOL, INT, FLOAT, STRING, TIME, INT64_CONSTANT, INT64_MATH].map(slotOf),
    ).toEqual(['int', 'int', 'float', 'string', 'i64', 'i64', 'i64']);
    expect(slotOf(STRUCT)).toBeUndefined();
    expect(isIntSlot(BOOL)).toBe(true);
    expect(isIntSlot(FLOAT)).toBe(false);
  });

  it('types a value lowered to C from its slot and what it records', () => {
    expect(typeOfCExpr({cType: 'int', isBool: true})).toBe(BOOL);
    expect(typeOfCExpr({cType: 'int', isBool: false})).toBe(INT);
    expect(typeOfCExpr({cType: 'i64'})).toBe(TIME);
    expect(typeOfCExpr({cType: 'i64', lit: true})).toBe(INT64_CONSTANT);
    expect(typeOfCExpr({cType: 'i64', wide: true})).toBe(INT64_MATH);
    expect(typeOfSlot(undefined)).toBe(STRUCT);
    expect(() => typeOfCExpr({cType: 'int', lit: true})).toThrow(
      /64-bit source/,
    );
    expect(() => typeOfSlot('float', true)).toThrow(
      /a boolean in a "float" slot/,
    );
  });

  it('types a ref by its slot, and a node ref as a handle', () => {
    expect(refType({kind: 'value', cType: 'float'})).toBe(FLOAT);
    expect(refType({kind: 'node', cType: 'ERNode*'})).toBe(NODE);
  });

  it('drops what JS adds to a slot', () => {
    expect(plainOf(BOOL)).toBe(INT);
    expect(plainOf(INT64_CONSTANT)).toBe(TIME);
    expect(plainOf(FLOAT)).toBe(FLOAT);
  });
});

describe('IR types: updaters', () => {
  it("types an updater's parameter as a read of the state, boolean included", () => {
    const {c} = compileSource(
      "import React, {useState} from 'react';\n" +
        "import {View} from 'embedded-react';\n" +
        'export function App() {\n' +
        '  const [on, setOn] = useState(false);\n' +
        '  return <View onTouchStart={() => setOn(on === 1)} onTouchEnd={() => setOn(prev => prev === 1)} />;\n' +
        '}',
    );
    expect(c.match(/s_state\.on = 0;/g)).toHaveLength(2);
    expect(c).not.toContain('s_state.on == 1');
  });
});

describe('IR types: slot widths', () => {
  const app =
    "import React, {useState, useRef} from 'react';\n" +
    "import {View, Text} from 'embedded-react';\n" +
    'export function App() {\n' +
    '  const [a, setA] = useState(0);\n' +
    '  const [b, setB] = useState(0);\n' +
    '  const [c, setC] = useState(0);\n' +
    '  const [n, setN] = useState(0);\n' +
    '  const r = useRef(0);\n' +
    '  return (\n' +
    '    <View onTouchStart={() => { setA(Date.now()); setB(a); const t = b + 1; setC(t); r.current = c; setN(n + 1); }}>\n' +
    '      <Text>{c}</Text>\n' +
    '    </View>\n' +
    '  );\n' +
    '}';

  it('records every value stored into an int slot', () => {
    const stores = [];
    lowerApp(app, resolveOptions({}), new Set(), stores);
    expect(stores.map(store => store.slot)).toEqual([
      'a',
      'b',
      'c',
      's_ref_r',
      'n',
    ]);
    expect(stores[0].value).toMatchObject({
      kind: 'fixed',
      expr: {kind: 'clock'},
    });
    expect(stores[2].value).toMatchObject({kind: 'local'});
  });

  it('follows a timestamp through every slot it reaches, through locals too', () => {
    const stores = [];
    lowerApp(app, resolveOptions({}), new Set(), stores);
    expect([...inferWideSlots(stores, new Set())].sort()).toEqual([
      'a',
      'b',
      'c',
      's_ref_r',
    ]);
  });

  it('compiles the widened slots as 64-bit', () => {
    const {c} = compileSource(app);
    for (const field of ['a', 'b', 'c']) {
      expect(c).toContain(`    int64_t ${field};`);
    }
    expect(c).toContain('    int n;');
    expect(c).toContain('static int64_t s_ref_r = 0;');
  });
});
