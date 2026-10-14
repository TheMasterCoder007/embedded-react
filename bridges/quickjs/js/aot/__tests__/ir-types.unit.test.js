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
      [BOOL, INT, FLOAT, STRING, TIME, INT64_NUMBER, INT64_MATH].map(slotOf),
    ).toEqual(['int', 'int', 'float', 'string', 'i64', 'i64', 'i64']);
    expect(slotOf(STRUCT)).toBeUndefined();
    expect(isIntSlot(BOOL)).toBe(true);
    expect(isIntSlot(FLOAT)).toBe(false);
  });

  it('types a value lowered to C from its slot and what it records', () => {
    expect(typeOfCExpr({cType: 'int', isBool: true})).toBe(BOOL);
    expect(typeOfCExpr({cType: 'int', isBool: false})).toBe(INT);
    expect(typeOfCExpr({cType: 'i64'})).toBe(TIME);
    expect(typeOfCExpr({cType: 'i64', lit: true})).toBe(INT64_NUMBER);
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

  it('joins 64-bit values into a time only when one of them is', () => {
    expect(int64Join(TIME, INT)).toBe(TIME);
    expect(int64Join(INT64_NUMBER, INT)).toBe(INT64_NUMBER);
    expect(int64Join(INT64_NUMBER, INT64_MATH)).toBe(INT64_MATH);
    expect(int64Join(INT64_NUMBER, TIME)).toBe(TIME);
  });

  it('reads a widened slot as a time only when it holds one', () => {
    const state = widened => ({
      kind: 'scalar',
      cType: 'i64',
      isBool: false,
      widened,
    });
    expect(stateType(state('time'))).toBe(TIME);
    expect(stateType(state('number'))).toBe(INT64_NUMBER);
    expect(refType({kind: 'value', cType: 'i64', widened: 'number'})).toBe(
      INT64_NUMBER,
    );
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
    lowerApp(app, resolveOptions({}), new Map(), stores);
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
    lowerApp(app, resolveOptions({}), new Map(), stores);
    expect(inferWideSlots(stores, new Map())).toEqual(
      new Map([
        ['a', 'time'],
        ['b', 'time'],
        ['c', 'time'],
        ['s_ref_r', 'time'],
      ]),
    );
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

describe('IR types: 64-bit numbers that are not times', () => {
  /** The C of an app with an int `n`, a float `f`, a boolean `on`, an int `big` and an int ref `r`. */
  const compile = handler =>
    compileSource(
      "import React, {useState, useRef} from 'react';\n" +
        "import {View, Text} from 'embedded-react';\n" +
        'export function App() {\n' +
        '  const [n, setN] = useState(0);\n' +
        '  const [f, setF] = useState(1.5);\n' +
        '  const [on, setOn] = useState(false);\n' +
        '  const [big, setBig] = useState(0);\n' +
        '  const r = useRef(0);\n' +
        `  return <View onTouchStart={() => { ${handler} }}><Text>{n + big + r.current}</Text></View>;\n` +
        '}',
    ).c;

  it('keeps `+` on a 64-bit number a number, which a float slot takes', () => {
    expect(compile('setF(+2147483648);')).toContain(
      's_state.f = (+(2147483648));',
    );
  });

  it('picks a number, not a time, from a ternary or `||` of numbers', () => {
    expect(compile('setN((on ? 2147483648 + 1 : 0) % n);')).toContain(
      's_state.n = app_mod64((s_state.on ? 2147483649 : 0), s_state.n);',
    );
    expect(compile('setN((n || 2147483648) % n);')).toContain('app_mod64(');
  });

  it('reads back a slot widened by numbers as a number, and one that holds a time as a time', () => {
    const c = compile('setBig(3000000000); setF(big * 0.5);');
    expect(c).toContain('    int64_t big;');
    expect(c).toContain('s_state.f = (((float)s_state.big) * 0.5f);');
    expect(() =>
      compile('setBig(3000000000); setBig(Date.now()); setF(big * 0.5);'),
    ).toThrow(/a 64-bit time value cannot be mixed with a float/);
  });

  it('divides a 64-bit ref that holds no time by any whole number', () => {
    const c = compile(
      'r.current = 3000000000; r.current %= n; r.current /= n; r.current++;',
    );
    expect(c).toContain('s_ref_r = app_mod64(s_ref_r, s_state.n);');
    expect(c).toContain('s_ref_r = app_div64(s_ref_r, s_state.n);');
    expect(() => compile('r.current = Date.now(); r.current %= n;')).toThrow(
      /can only be divided by a constant/,
    );
  });

  it('starts a state or ref with a whole number past the int range 64-bit', () => {
    const c = compileSource(
      "import React, {useState, useRef} from 'react';\n" +
        "import {Text} from 'embedded-react';\n" +
        'export function App() {\n' +
        '  const [big, setBig] = useState(3000000000);\n' +
        '  const r = useRef(-3000000000);\n' +
        '  return <Text onTouchStart={() => { r.current = r.current + 1; }}>{big + r.current}</Text>;\n' +
        '}',
    ).c;
    expect(c).toContain('    int64_t big;');
    expect(c).toContain('.big = 3000000000');
    expect(c).toContain('static int64_t s_ref_r = -3000000000;');
  });

  it('keeps a local holding a 64-bit number a number', () => {
    const c = compile(
      'const total = n + 3000000000; setF(total); setN(total % n);',
    );
    expect(c).toContain('int64_t l_total = app_add64(s_state.n, 3000000000);');
    expect(c).toContain('s_state.f = l_total;');
    expect(c).toContain('s_state.n = app_mod64(l_total, s_state.n);');
    expect(() => compile('const t = Date.now(); setF(t);')).toThrow(
      /a 64-bit time value cannot be stored in state "f"/,
    );
  });

  it('works a float Math function on a 64-bit number, and keeps whole-number Math exact', () => {
    const c = compile(
      'setBig(3000000000); setF(Math.sqrt(big) + Math.pow(big, 2)); setBig(Math.abs(big));',
    );
    expect(c).toContain('sqrtf((float)(((float)s_state.big)))');
    expect(c).toContain('powf((float)(((float)s_state.big)), (float)(2))');
    expect(c).toContain('s_state.big = app_abs64(s_state.big);');
    expect(() => compile('setF(Math.sqrt(Date.now()));')).toThrow(
      /Math\.sqrt\(\.\.\.\) on a 64-bit time value is not supported/,
    );
  });

  it("types an updater's parameter as the number its state holds", () => {
    expect(compile('setBig(3000000000); setBig(prev => prev % n);')).toContain(
      's_state.big = app_mod64(s_state.big, s_state.n);',
    );
  });
});
