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
  INT64_NUMBER,
  INT64_MATH,
  NODE,
  STRING,
  STRUCT,
  TIME,
  isIntSlot,
  int64Join,
  stateType,
  refType,
  slotOf,
  typeOfSlotValue,
  typeOfSlot,
} from '../compiler/ir/types.mts';

import {inferWideSlots} from '../compiler/expressions.mts';
import {lowerApp} from '../compiler/lower.mts';
import {resolveOptions} from '../compiler/options.mts';

// The IR types its values itself; a backend picks the slot each one is stored in.

describe('IR types', () => {
  it('stores a boolean in an int, and every 64-bit number in an int64', () => {
    expect(
      [BOOL, INT, FLOAT, STRING, TIME, INT64_NUMBER, INT64_MATH].map(slotOf),
    ).toEqual(['int', 'int', 'float', 'string', 'i64', 'i64', 'i64']);
    expect(slotOf(STRUCT)).toBeUndefined();
    expect(isIntSlot(BOOL)).toBe(true);
    expect(isIntSlot(FLOAT)).toBe(false);
  });

  it('types a value from its slot and what it records', () => {
    expect(typeOfSlotValue({slotKind: 'int', isBoolean: true})).toBe(BOOL);
    expect(typeOfSlotValue({slotKind: 'int', isBoolean: false})).toBe(INT);
    expect(typeOfSlotValue({slotKind: 'i64'})).toBe(TIME);
    expect(typeOfSlotValue({slotKind: 'i64', isLiteral: true})).toBe(
      INT64_NUMBER,
    );
    expect(typeOfSlotValue({slotKind: 'i64', wide: true})).toBe(INT64_MATH);
    expect(typeOfSlot(undefined)).toBe(STRUCT);
    expect(() => typeOfSlotValue({slotKind: 'int', isLiteral: true})).toThrow(
      /64-bit source/,
    );
    expect(() => typeOfSlot('float', true)).toThrow(
      /a boolean in a "float" slot/,
    );
  });

  it('types a ref by its slot, and a node ref as a handle', () => {
    expect(refType({kind: 'value', slotKind: 'float'})).toBe(FLOAT);
    expect(refType({kind: 'node', slotKind: 'node'})).toBe(NODE);
  });

  it('joins 64-bit values into a time only when one of them is', () => {
    expect(int64Join(TIME, INT)).toBe(TIME);
    expect(int64Join(INT64_NUMBER, INT)).toBe(INT64_NUMBER);
    expect(int64Join(INT64_NUMBER, INT64_MATH)).toBe(INT64_MATH);
    expect(int64Join(INT64_NUMBER, TIME)).toBe(TIME);
  });

  it('reads a widened slot as a time only when it holds one', () => {
    const state = widened => ({
      kind: 'scalar',
      slotKind: 'i64',
      isBoolean: false,
      widened,
    });
    expect(stateType(state('time'))).toBe(TIME);
    expect(stateType(state('number'))).toBe(INT64_NUMBER);
    expect(refType({kind: 'value', slotKind: 'i64', widened: 'number'})).toBe(
      INT64_NUMBER,
    );
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
    lowerApp(app, resolveOptions({}), new Map(), stores);
    expect(stores.map(store => store.slot)).toEqual([
      'a',
      'b',
      'c',
      'ref_r',
      'n',
    ]);
    expect(stores[0].value).toMatchObject({
      kind: 'fixed',
      expression: {kind: 'clock'},
    });
    expect(stores[2].value).toMatchObject({kind: 'local'});
  });

  it('follows a timestamp through every slot it reaches, through locals too', () => {
    const stores = [];
    lowerApp(app, resolveOptions({}), new Map(), stores);
    expect(inferWideSlots(stores, new Map())).toEqual(
      new Map([
        ['a', 'time'],
        ['b', 'time'],
        ['c', 'time'],
        ['ref_r', 'time'],
      ]),
    );
  });
});
