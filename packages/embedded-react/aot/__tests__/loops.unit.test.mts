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
import {parse} from '@babel/parser';
import type * as t from '@babel/types';
import {compilePackage} from '../compile.mts';
import {collectHelpers, collectState} from '../compiler/collect.mts';
import {inferWideSlots} from '../compiler/expressions.mts';
import {lowerBody} from '../compiler/handlers.mts';
import {Out} from '../compiler/out.mts';
import type {IrStmt} from '../compiler/ir/statements.mts';
import type {Env, Scope} from '../compiler/types.mts';

// `for` and `for…of` over bounded data, `break`, `continue` and `let` locals.

const SCREEN = {screen: {width: 240, height: 320}};
const CAPS = {listCap: 8, listStrCap: 48, maxTextSpans: 4, maxTimers: 4};

/** The state every handler here runs against. */
const STATE =
  'const [n, setN] = useState(0);\n' +
  "const [label, setLabel] = useState('a,b');\n" +
  'const [items, setItems] = useState([{id: 1, v: 3}, {id: 2, v: 4}]);';

/** The module constants every handler here can read. */
const CONSTANTS = "const NAMES = ['a', 'b', 'c'];";

/** An app module whose button runs `handler`, after the helpers in `before`. */
const appOf = (handler: string, before = '') =>
  "import {useState} from 'react';\n" +
  "import {Pressable, Text, View} from 'embedded-react';\n" +
  `${CONSTANTS}\n${before}\nexport function App() {\n${STATE}\n` +
  `  return <View><Pressable onPress={() => {${handler}}}><Text>{label}</Text></Pressable>` +
  '<Text>{n}</Text>{items.map(item => <Text key={item.id}>{item.v}</Text>)}</View>;\n}';

const compile = (source: string) => compilePackage(source, 'app', SCREEN);

/** Lowers a handler body as a body of its own, and the environment it was lowered in. */
function lowered(handler: string): {statements: IrStmt[]; environment: Env} {
  const parsed = (source: string) =>
    parse(source, {sourceType: 'module', allowReturnOutsideFunction: true})
      .program.body;
  const body = {
    type: 'BlockStatement',
    body: parsed(STATE),
  } as t.BlockStatement;
  const scope: Scope = {NAMES: ['a', 'b', 'c']};
  const state = collectState(body, scope, CAPS);
  const environment = {
    state: state.byName,
    locals: new Map(),
    constants: scope,
    refs: new Map(),
    math64: undefined,
    caps: CAPS,
    wide: new Map(),
    stores: [],
    constantLists: new Map(),
    derivedLists: new Map(),
    splitLists: new Map(),
    helpers: collectHelpers(body, parse('', {sourceType: 'module'}).program),
  } as unknown as Env;
  const statements = lowerBody(parsed(handler), environment, state, {
    stateChanged: false,
    output: new Out(new Map(), null as never),
  });
  return {statements, environment};
}

const lowerHandler = (handler: string): IrStmt[] => lowered(handler).statements;

/** What each statement does, nested statements in brackets. */
const shape = (statements: IrStmt[]): unknown[] =>
  statements.map(statement => {
    switch (statement.kind) {
      case 'if':
        return {
          if: shape(statement.consequent),
          ...(statement.alternate ? {else: shape(statement.alternate)} : {}),
        };
      case 'loop':
        return {
          loop: shape(statement.body),
          ...(statement.step.length ? {step: shape(statement.step)} : {}),
        };
      case 'block':
        return {block: shape(statement.body)};
      case 'setState':
      case 'setStateText':
        return `set ${statement.state.name}`;
      default:
        return statement.kind;
    }
  });

/** The first loop in statements, at any depth. */
function firstLoop(statements: IrStmt[]): IrStmt & {kind: 'loop'} {
  for (const statement of statements) {
    if (statement.kind === 'loop') return statement;
    const inner =
      statement.kind === 'if'
        ? [...statement.consequent, ...(statement.alternate ?? [])]
        : statement.kind === 'block'
          ? statement.body
          : [];
    const found = inner.length ? firstLoop(inner) : null;
    if (found) return found;
  }
  return null as never;
}

describe('for…of', () => {
  it("visits a list state's rows, as many as its cap, stopping at its count", () => {
    const statements = lowerHandler(
      'for (const item of items) setN(v => v + item.v);',
    );
    expect(shape(statements)).toEqual([{loop: ['set n']}]);
    const loop = firstLoop(statements);
    expect(loop.bound).toBe(CAPS.listCap);
    expect(loop.test).toMatchObject({
      kind: 'compare',
      operator: '<',
      left: {kind: 'local', local: {name: loop.visit!.name}},
      right: {kind: 'listLength'},
    });
  });

  it('visits every element of a constant array, with no test', () => {
    const loop = firstLoop(
      lowerHandler('for (const name of NAMES) setLabel(name);'),
    );
    expect(loop.bound).toBe(3);
    expect(loop.test).toBeNull();
  });

  it('takes an item apart into a const per field', () => {
    const loop = firstLoop(
      lowerHandler(
        'for (const {id, v: value} of items) setN(m => m + id * value);',
      ),
    );
    expect(shape(loop.body)).toEqual(['declareLocal', 'declareLocal', 'set n']);
  });

  it('reads the index of entries() as the visit', () => {
    const loop = firstLoop(
      lowerHandler(
        'for (const [i, item] of items.entries()) setN(m => m + i * item.v);',
      ),
    );
    const [write] = loop.body;
    expect(JSON.stringify(write)).toContain(loop.visit!.name);
  });

  it("visits a handler's split", () => {
    expect(
      shape(
        lowerHandler(
          "const parts = label.split(','); for (const part of parts) setLabel(part);",
        ),
      ),
    ).toEqual(['splitItems', {loop: ['set label']}]);
  });

  it('declares its item with const or let, and refuses var', () => {
    expect(() =>
      lowerHandler('for (var item of items) setN(v => v + item.v);'),
    ).toThrow(/must declare its item with const or let/);
    expect(() =>
      lowerHandler('for (let item of items) item = items[0];'),
    ).toThrow(/"item" is a list's item, which can't be set/);
    // A let's fields can be set, as JS's can.
    const loop = firstLoop(
      lowerHandler('for (let {v} of items) { v += 1; setN(m => m + v); }'),
    );
    expect(shape(loop.body)).toEqual(['declareLocal', 'assignLocal', 'set n']);
  });

  it('names a field that hides an outer one apart from it', () => {
    const outer = firstLoop(
      lowerHandler(
        'for (const {v} of items) { for (const {v} of items) setN(m => m + v); setN(m => m + v); }',
      ),
    );
    const inner = firstLoop(outer.body);
    const [outerField] = outer.body;
    const [innerField] = inner.body;
    expect(outerField.kind === 'declareLocal' && outerField.name).not.toBe(
      innerField.kind === 'declareLocal' && innerField.name,
    );
  });

  it('refuses a local that hides a list state', () => {
    expect(() =>
      lowerHandler('const items = 1; for (const item of items) setN(item);'),
    ).toThrow(/a for…of must go over a list/);
  });

  it('refuses anything but a list', () => {
    expect(() => lowerHandler('for (const ch of label) setN(1);')).toThrow(
      /a for…of must go over a list/,
    );
  });
});

describe('for', () => {
  it('counts between constants exactly, with no test', () => {
    const statements = lowerHandler(
      'for (let i = 0; i < 10; i += 3) setN(v => v + i);',
    );
    expect(shape(statements)).toEqual([
      'declareLocal',
      {loop: ['set n'], step: ['assignLocal']},
    ]);
    const loop = firstLoop(statements);
    expect(loop.bound).toBe(4);
    expect(loop.test).toBeNull();
  });

  it("runs up to a list's cap when bounded by its length, tested each visit", () => {
    const loop = firstLoop(
      lowerHandler(
        'for (let i = items.length - 1; i >= 0; i--) setN(v => v + items[i].v);',
      ),
    );
    expect(loop.bound).toBe(CAPS.listCap);
    expect(loop.test).toMatchObject({kind: 'compare', operator: '>='});
  });

  it("bounds an inner loop by the outer counter's values", () => {
    const outer = firstLoop(
      lowerHandler(
        'for (let a = 0; a < 3; a++) for (let b = 0; b <= a; b++) setN(v => v + 1);',
      ),
    );
    const inner = firstLoop(outer.body);
    expect(outer.bound).toBe(3);
    expect(inner.bound).toBe(3);
    expect(inner.test).not.toBeNull();
  });

  it('refuses a loop that never ends or runs too often', () => {
    expect(() => lowerHandler('for (let i = 0; i < 3; i--) setN(1);')).toThrow(
      /never ends/,
    );
    expect(() =>
      lowerHandler('for (let i = 0; i < 70000; i++) setN(1);'),
    ).toThrow(/at most 65535 times/);
  });

  it('refuses a bound read from state, and a counter not declared with let', () => {
    expect(() => lowerHandler('for (let i = 0; i < n; i++) setN(1);')).toThrow(
      /start and end must be constants or lists' lengths/,
    );
    expect(() =>
      lowerHandler('let i = 0; for (i = 0; i < 3; i++) setN(1);'),
    ).toThrow(/must declare its counter with let/);
  });

  it('refuses a var counter, and one that leaves the int range', () => {
    expect(() => lowerHandler('for (var i = 0; i < 3; i++) setN(1);')).toThrow(
      /must declare its counter with let/,
    );
    expect(() =>
      lowerHandler(
        'for (let i = 2147483640; i < 2147483648; i++) setN(v => v + 1);',
      ),
    ).toThrow(/must stay a 32-bit whole number/);
    expect(() =>
      lowerHandler(
        'for (let i = 2147483640; i <= 2147483646; i += 2) setN(v => v + 1);',
      ),
    ).toThrow(/must stay a 32-bit whole number/);
  });

  it("refuses a start or end that reads the loop's own counter", () => {
    expect(() =>
      lowerHandler(
        'for (let i = 0; i < 3; i++) for (let i = 0; i < i; i++) setN(1);',
      ),
    ).toThrow(/can't read its counter i/);
  });

  it('refuses a body that sets the counter', () => {
    expect(() => lowerHandler('for (let i = 0; i < 3; i++) i += 1;')).toThrow(
      /a loop's counter, which only the loop sets/,
    );
  });
});

describe('break and continue', () => {
  it('lower inside a loop, including from a switch case', () => {
    const loop = firstLoop(
      lowerHandler(
        'for (const item of items) { if (item.v === 3) continue; ' +
          'switch (item.id) { case 2: break; default: continue; } break; }',
      ),
    );
    expect(shape(loop.body)).toEqual([
      {if: ['continue']},
      {if: [], else: ['continue']},
      'break',
    ]);
  });

  it('refuse a label, and while loops', () => {
    expect(() =>
      lowerHandler('outer: for (const item of items) { break outer; }'),
    ).toThrow(/a labeled statement is not supported/);
    expect(() => lowerHandler('while (n < 3) setN(v => v + 1);')).toThrow(
      /a while loop is not supported/,
    );
  });
});

describe('let', () => {
  it('lowers =, compound assignments and ++ to assignments', () => {
    expect(
      shape(lowerHandler('let x = 1; x = 2; x += 3; x++; setN(x);')),
    ).toEqual([
      'declareLocal',
      'assignLocal',
      'assignLocal',
      'assignLocal',
      'set n',
    ]);
  });

  it('records a fraction stored in a whole let, which widens it to a float', () => {
    const {environment} = lowered(
      'let total = 0; for (const item of items) total += item.v / 2; setN(total);',
    );
    const widths = inferWideSlots(environment.stores, new Map());
    expect([...widths.values()]).toEqual(['float']);
  });

  it('builds a string in its own buffer', () => {
    const statements = lowerHandler(
      "let text = ''; for (const name of NAMES) text += name; setLabel(text);",
    );
    const loop = firstLoop(statements);
    const [write] = loop.body;
    expect(write).toMatchObject({kind: 'assignLocal', text: {}});
  });

  it('refuses a const, and a value of another kind', () => {
    expect(() => lowerHandler('const x = 1; x = 2;')).toThrow(
      /only a `let` can/,
    );
    expect(() => lowerHandler('let ok = false; ok = 1;')).toThrow(
      /holds a boolean/,
    );
    expect(() => lowerHandler("let s = ''; s = 3;")).toThrow(/holds a string/);
  });
});

describe('setters in a loop', () => {
  it('refuse changing the list the loop visits or is bounded by', () => {
    expect(() =>
      lowerHandler('for (const item of items) setItems([]);'),
    ).toThrow(/setItems can't change items inside a loop over it/);
    expect(() =>
      lowerHandler(
        'for (let i = 0; i < items.length; i++) setItems(p => p.slice(0, -1));',
      ),
    ).toThrow(/inside a loop over it/);
  });

  it("refuse a value read from the setter's own state, but take an updater", () => {
    expect(() =>
      lowerHandler('for (const item of items) setN(n + item.v);'),
    ).toThrow(/setN\(…\) inside a loop must take an updater to read n/);
    expect(() =>
      lowerHandler('for (const item of items) setN(v => v + item.v);'),
    ).not.toThrow();
  });
});

describe('packages', () => {
  it('compile loops nested around list updates and sorts', () => {
    expect(() =>
      compile(
        appOf(
          'for (let a = 0; a < 2; a++) { for (const item of items) { if (item.v > 3) continue; ' +
            'setN(v => v + item.v); } setItems(p => p.toSorted((x, y) => x.v - y.v)); }',
        ),
      ),
    ).not.toThrow();
  });

  it('widen a whole let given a fraction', () => {
    expect(() =>
      compile(
        appOf(
          'let mean = 0; for (const item of items) mean += item.v / items.length; ' +
            'setLabel(mean.toFixed(2));',
        ),
      ),
    ).not.toThrow();
  });

  it('refuse more loops in one function than the verifier takes', () => {
    const loops = Array.from(
      {length: 33},
      () => 'for (const item of items) setN(v => v + item.v);',
    ).join(' ');
    expect(() => compile(appOf(loops))).toThrow(/more than 32 loops/);
  });
});
