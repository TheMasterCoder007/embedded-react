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
import {compilePackage} from '../compile.mts';
import {lowerApp} from '../compiler/lower.mts';
import {resolveOptions} from '../compiler/options.mts';
import {nodesOf} from '../compiler/ir/nodes.mts';
import {nodeReads} from '../compiler/ir/reads.mts';
import type {IrStmt} from '../compiler/ir/statements.mts';
import type {ScalarState} from '../compiler/types.mts';

// Loops in useMemo and in helpers used as values.

const SCREEN = {screen: {width: 240, height: 320}};

/** The state every app here starts from. */
const STATE =
  '  const [n, setN] = useState(0);\n' +
  '  const [items, setItems] = useState([{id: 1, v: 3}, {id: 2, v: 4}]);\n';

/** An app module with `before` at module level, then `body` and the JSX the App returns. */
const appOf = (before: string, body: string, jsx: string) =>
  "import {useMemo, useState} from 'react';\n" +
  "import {Pressable, Text, View} from 'embedded-react';\n" +
  `${before}\nexport function App() {\n${STATE}${body}\n  return ${jsx};\n}`;

const compile = (source: string) => compilePackage(source, 'app', SCREEN);
const lower = (source: string) =>
  lowerApp(source, resolveOptions(SCREEN), new Map(), []);

/** The values an app works out with a loop in render. */
const workedOf = (source: string): ScalarState[] =>
  lower(source).state.filter(
    (state): state is ScalarState => state.kind === 'scalar' && !!state.worked,
  );

/** The slots each node reads, for the nodes that read any. */
const readsOf = (source: string) =>
  nodesOf(lower(source).root)
    .map(node => [...(nodeReads(node) ?? [])].map(state => state.slotName))
    .filter(reads => reads.length);

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

/** The statements the first handler of an app runs. */
function handlerOf(source: string): IrStmt[] {
  const [handler] = lower(source).output.handlers;
  if (handler?.kind !== 'callback') throw new Error('no handler');
  return handler.callback.body;
}

/** A memo that adds up the items. */
const TOTAL =
  '  const total = useMemo(() => {\n' +
  '    let sum = 0;\n' +
  '    for (const item of items) sum += item.v;\n' +
  '    return sum;\n' +
  '  }, [items]);';

/** Helpers that loop. */
const HELPERS =
  'function biggest(list) { let best = 0; for (const item of list) if (item.v > best) best = item.v; return best; }\n' +
  'function firstOver(list, limit) { for (const item of list) { if (item.v > limit) return item.id; } return 0; }\n' +
  "function dots(count) { let line = ''; for (let i = 0; i < 4; i++) { if (i >= count) break; line += '.'; } return line; }";

describe('a useMemo that loops', () => {
  it('is a state of its own, which a node reads as what its loop reads', () => {
    const source = appOf('', TOTAL, '<Text>{total}</Text>');
    const [total] = workedOf(source);
    expect(total).toMatchObject({name: 'total', slotKind: 'int'});
    expect(shape(total.worked!.rebuild)).toEqual([
      'declareLocal',
      'declareLocal',
      {loop: ['assignLocal']},
      'assignLocal',
      'set total',
    ]);
    expect(readsOf(source)).toEqual([['items']]);
  });

  it('holds what its returns give: a float when any is, a boolean or a string', () => {
    const kinds = (body: string) =>
      workedOf(
        appOf(
          '',
          `  const t = useMemo(() => { ${body} }, [items]);`,
          '<Text>{String(t)}</Text>',
        ),
      ).map(({slotKind, isBoolean}) => [slotKind, isBoolean]);
    expect(
      kinds(
        'if (items.length === 0) return 0; let sum = 0; for (const item of items) sum += item.v; ' +
          'return sum / items.length;',
      ),
    ).toEqual([['float', false]]);
    expect(
      kinds(
        'for (const item of items) if (item.v > 8) return false; return true;',
      ),
    ).toEqual([['int', true]]);
    expect(
      kinds(
        "let line = ''; for (const item of items) line += String(item.id); return line;",
      ),
    ).toEqual([['string', false]]);
  });

  it('holds a 64-bit number, a time unless each is a plain number', () => {
    const source = appOf(
      '',
      '  const at = useMemo(() => {\n' +
        '    let t = Date.now();\n' +
        '    for (const item of items) t += item.v;\n' +
        '    return t;\n' +
        '  }, [items]);',
      '<Text>{at}</Text>',
    );
    expect(
      workedOf(source).map(({slotKind, widened}) => [slotKind, widened]),
    ).toEqual([['i64', 'time']]);
    expect(() => compile(source)).not.toThrow();
    expect(() =>
      compile(
        appOf(
          '',
          '  const t = useMemo(() => { for (const item of items) if (item.v > 3) return Date.now(); ' +
            'return 0.5; }, [items]);',
          '<Text>{t}</Text>',
        ),
      ),
    ).toThrow(/can't return a 64-bit whole number on one path and a fraction/);
  });

  it('is worked out after a memo or a filtered list it reads', () => {
    const source = appOf(
      '',
      `${TOTAL}\n` +
        '  const above = useMemo(() => {\n' +
        '    let count = 0;\n' +
        '    for (const item of items) if (item.v * 2 > total) count++;\n' +
        '    return count;\n' +
        '  }, [items, total]);\n' +
        '  const big = items.filter(item => item.v > n);\n' +
        '  const bigSum = useMemo(() => {\n' +
        '    let sum = 0;\n' +
        '    for (const item of big) sum += item.v;\n' +
        '    return sum;\n' +
        '  }, [big]);',
      '<View><Text>{above}</Text><Text>{bigSum}</Text></View>',
    );
    const app = lower(source);
    const order = (name: string) => {
      const state = app.state.find(each => each.name === name)!;
      return state.kind === 'list'
        ? state.derived!.order!
        : state.worked!.order;
    };
    expect(order('total')).toBeLessThan(order('above'));
    expect(order('items.filter')).toBeLessThan(order('bigSum'));
    // A memo that reads another reads what that one reads.
    expect(readsOf(source)).toEqual([['items'], ['items', 'n']]);
  });

  it('in a component, is a state for each instance', () => {
    const source =
      "import {useMemo, useState} from 'react';\n" +
      "import {Pressable, Text, View} from 'embedded-react';\n" +
      'function Tally({step}) {\n' +
      '  const [n, setN] = useState(0);\n' +
      '  const tally = useMemo(() => { let s = 0; for (let i = 0; i < 4; i++) if (i < n) s += step; return s; }, [n]);\n' +
      '  return <Pressable onPress={() => setN(n + 1)}><Text>{tally}</Text></Pressable>;\n' +
      '}\n' +
      'export function App() { return <View><Tally step={2} /><Tally step={5} /></View>; }';
    expect(workedOf(source).map(({slotName}) => slotName)).toEqual([
      'c0_w0_tally',
      'c1_w1_tally',
    ]);
    expect(readsOf(source)).toEqual([['c0_n'], ['c1_n']]);
  });

  it('compiles with break, continue and a for that counts to a constant', () => {
    const source = appOf(
      '',
      '  const line = useMemo(() => {\n' +
        "    let text = '';\n" +
        '    for (const item of items) {\n' +
        '      if (item.v % 2 === 1) continue;\n' +
        "      if (text !== '') break;\n" +
        '      for (let i = 0; i < 3; i++) text += String(item.id);\n' +
        '    }\n' +
        '    return text;\n' +
        '  }, [items]);',
      '<Text>{line}</Text>',
    );
    expect(() => compile(source)).not.toThrow();
  });

  it('refuses side effects, mixed kinds and a path without a value', () => {
    const memo = (body: string) =>
      compile(
        appOf(
          '',
          `  const t = useMemo(() => { ${body} }, [items]);`,
          '<Text>{t}</Text>',
        ),
      );
    expect(() =>
      memo('let s = 0; for (const item of items) setN(item.v); return s;'),
    ).toThrow(/the useMemo for "t" can't call setN/);
    expect(() =>
      memo("for (const item of items) if (item.v > 3) return 'big'; return 0;"),
    ).toThrow(
      /the useMemo for "t" loops, so each `return` must give the same kind of value/,
    );
    expect(() =>
      memo('for (const item of items) if (item.v > 3) return item.v;'),
    ).toThrow(/the useMemo for "t" can end without returning a value/);
    expect(() => memo('let s = 0; while (s < 3) s++; return s;')).toThrow(
      /a while loop is not supported/,
    );
  });
});

describe('a helper used as a value that loops', () => {
  it('in render, gets a state for each call', () => {
    const source = appOf(
      HELPERS,
      '',
      '<View><Text>{firstOver(items, n)}</Text><Text>{firstOver(items, 3)}</Text>' +
        '<Text>{dots(n)}</Text></View>',
    );
    expect(
      workedOf(source).map(({name, slotKind}) => [name, slotKind]),
    ).toEqual([
      ['firstOver', 'int'],
      ['firstOver', 'int'],
      ['dots', 'string'],
    ]);
    expect(readsOf(source)).toEqual([['items', 'n'], ['items'], ['n']]);
    expect(() => compile(source)).not.toThrow();
  });

  it('in a handler, is worked out just before the statement that reads it', () => {
    const source = appOf(
      HELPERS,
      '',
      '<Pressable onPress={() => { setN(0); setN(biggest(items) * 10 + firstOver(items, 3)); }}>' +
        '<Text>{n}</Text></Pressable>',
    );
    expect(shape(handlerOf(source))).toEqual([
      'set n',
      'declareLocal',
      'declareLocal',
      {loop: [{if: ['assignLocal']}]},
      'assignLocal',
      'declareLocal',
      {block: [{loop: [{if: ['assignLocal', 'return']}]}, 'assignLocal']},
      'set n',
    ]);
    expect(() => compile(source)).not.toThrow();
  });

  it('in a declaration, is worked out just before the initializer that reads it', () => {
    const source = appOf(
      HELPERS,
      '',
      '<Pressable onPress={() => { const a = biggest(items), b = firstOver(items, a); setN(b); }}>' +
        '<Text>{n}</Text></Pressable>',
    );
    const declared = handlerOf(source).flatMap(statement =>
      statement.kind === 'declareLocal' ? [statement.name] : [],
    );
    expect(declared.map(name => name.replace(/[#@]\d+$/, m => m[0]))).toEqual([
      'value@biggest#',
      'local_best@',
      'local_a',
      'value@firstOver#',
      'local_b',
    ]);
    expect(() => compile(source)).not.toThrow();
  });

  it("in a handler's loop, is worked out on each visit", () => {
    const counted = appOf(
      HELPERS,
      '',
      '<Pressable onPress={() => { for (const item of items) setN(v => v + firstOver(items, item.v)); }}>' +
        '<Text>{n}</Text></Pressable>',
    );
    expect(shape(handlerOf(counted))).toEqual([
      {
        loop: [
          'declareLocal',
          {block: [{loop: [{if: ['assignLocal', 'return']}]}, 'assignLocal']},
          'set n',
        ],
      },
    ]);
  });

  it('is given a filtered list or a constant array as the list', () => {
    const source = appOf(
      `${HELPERS}\nconst LIMITS = [{id: 1, v: 7}, {id: 2, v: 9}];`,
      '  const big = items.filter(item => item.v > n);',
      '<View><Pressable onPress={() => setN(biggest(big) + biggest(LIMITS))}><Text>{n}</Text></Pressable>' +
        '<Text>{biggest(big)}</Text><Text>{biggest(LIMITS)}</Text></View>',
    );
    const app = lower(source);
    const big = app.state.find(state => state.name === 'items.filter')!;
    const [overBig] = workedOf(source);
    expect(big.kind === 'list' && big.derived!.order).toBeLessThan(
      overBig.worked!.order,
    );
    // The constant array's loop reads no state.
    expect(readsOf(source)).toEqual([['n'], ['items', 'n']]);
    expect(() => compile(source)).not.toThrow();
  });

  it('nests, each helper reading its own locals', () => {
    const source = appOf(
      'function sum(list) { const total = 100; let s = total; for (const item of list) s += item.v; return s; }\n' +
        'function outer(list) { const total = 1; let k = 0; for (const item of list) k += 1; ' +
        'const inner = sum(list); return total + inner + k; }',
      '',
      '<Pressable onPress={() => setN(outer(items))}><Text>{n}</Text></Pressable>',
    );
    const declared = handlerOf(source).flatMap(function names(
      statement: IrStmt,
    ): string[] {
      if (statement.kind === 'declareLocal') return [statement.name];
      if (statement.kind === 'block' || statement.kind === 'loop') {
        return statement.body.flatMap(names);
      }
      return [];
    });
    expect(declared.map(name => name.replace(/[#@]\d+$/, m => m[0]))).toEqual([
      'value@outer#',
      'local_total',
      'local_k@',
      'value@sum#',
      'local_total@',
      'local_s@',
      'local_inner',
    ]);
    expect(() => compile(source)).not.toThrow();
  });

  it('refuses one in a list row, and one that calls itself', () => {
    expect(() =>
      compile(
        appOf(
          HELPERS,
          '',
          '<View>{items.map(item => <Text key={item.id}>{dots(item.v)}</Text>)}</View>',
        ),
      ),
    ).toThrow(/helper "dots" loops, so it can't be used inside a list row/);
    expect(() =>
      compile(
        appOf(
          'function down(k) { let s = 0; for (let i = 0; i < 2; i++) s += down(k); return s; }',
          '',
          '<Text>{down(n)}</Text>',
        ),
      ),
    ).toThrow(/helper "down" is recursive/);
  });
});
