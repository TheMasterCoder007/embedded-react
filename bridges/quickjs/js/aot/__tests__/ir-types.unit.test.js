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
