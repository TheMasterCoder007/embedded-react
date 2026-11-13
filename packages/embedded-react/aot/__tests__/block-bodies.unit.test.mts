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
import {lowerBody} from '../compiler/handlers.mts';
import {Out} from '../compiler/out.mts';
import type {IrStmt} from '../compiler/ir/statements.mts';
import type {Env, Scope} from '../compiler/types.mts';

// Block bodies in useMemo and in state updaters.

const SCREEN = {screen: {width: 240, height: 320}};
const CAPS = {listCap: 8, listStrCap: 48, maxTextSpans: 4, maxTimers: 4};

/** An app module whose component body is `body`, rendering `jsx`. */
const appOf = (body: string, jsx: string) =>
  "import {useMemo, useState} from 'react';\n" +
  "import {Pressable, Text, View} from 'embedded-react';\n" +
  `export function App() {\n${body}\n  return ${jsx};\n}`;

const compile = (source: string) => compilePackage(source, 'app', SCREEN);

/** The state every handler here runs against. */
const STATE =
  "const [n, setN] = useState(0);\nconst [label, setLabel] = useState('a');";

/** Lowers a handler body as a body of its own. */
function lowerHandler(handler: string): IrStmt[] {
  const parsed = (source: string) =>
    parse(source, {sourceType: 'module', allowReturnOutsideFunction: true})
      .program.body;
  const body = {
    type: 'BlockStatement',
    body: parsed(STATE),
  } as t.BlockStatement;
  const scope: Scope = {};
  const state = collectState(body, scope, CAPS);
  const environment = {
    state: state.byName,
    locals: new Map(),
    constants: scope,
    refs: new Map(),
    math64: undefined,
    caps: CAPS,
    stores: [],
    wide: new Map(),
    helpers: collectHelpers(body, parse('', {sourceType: 'module'}).program),
  } as unknown as Env;
  return lowerBody(parsed(handler), environment, state, {
    stateChanged: false,
    output: new Out(new Map(), null as never),
  });
}

/** What each statement does, nested statements in brackets. */
const shape = (statements: IrStmt[]): unknown[] =>
  statements.map(statement => {
    switch (statement.kind) {
      case 'if':
        return {
          if: shape(statement.consequent),
          ...(statement.alternate ? {else: shape(statement.alternate)} : {}),
        };
      case 'block':
        return {block: shape(statement.body)};
      case 'loop':
        return {loop: shape(statement.body)};
      case 'setState':
      case 'setStateText':
        return `set ${statement.state.name}`;
      default:
        return statement.kind;
    }
  });

describe('useMemo with a block body', () => {
  /** The program an app compiles to whose memo `m` is `memo`. */
  const programOf = (memo: string) =>
    Buffer.from(
      compile(
        appOf(
          `  const [n, setN] = useState(0);\n  const m = useMemo(${memo}, [n]);`,
          '<Pressable onPress={() => setN(n + 1)}><Text>{m}</Text></Pressable>',
        ),
      ).program,
    ).toString('hex');

  it('works out each branch as a ternary', () => {
    expect(
      programOf(
        "() => { const top = 2; if (n > top) return 'hi'; if (n < 0) return 'neg'; return 'lo'; }",
      ),
    ).toBe(programOf("() => (n > 2 ? 'hi' : n < 0 ? 'neg' : 'lo')"));
  });

  it('compiles a switch, a memo before it, and a tag it narrows on', () => {
    const body =
      '  const [n, setN] = useState(0);\n' +
      "  const [job, setJob] = useState({kind: 'idle'});\n" +
      "  const grade = useMemo(() => { switch (n % 3) { case 0: return 'zero'; case 1: return 'one'; } " +
      "return 'two'; }, [n]);\n" +
      "  const loud = useMemo(() => { const big = grade === 'two'; if (big) return 1; return 0; }, [grade]);\n" +
      "  const status = useMemo(() => { if (job.kind === 'idle') return 'idle'; " +
      "if (job.pct >= 50) return 'late'; return 'early'; }, [job]);";
    expect(() =>
      compile(
        appOf(
          body,
          "<Pressable onPress={() => { setN(n + 1); setJob({kind: 'busy', pct: 0}); }}>" +
            '<Text>{grade}{loud}{status}</Text></Pressable>',
        ),
      ),
    ).not.toThrow();
  });

  it('compiles in a child component', () => {
    const source =
      appOf(
        '  const [n, setN] = useState(0);',
        '<Pressable onPress={() => setN(n + 1)}><Show v={n} /></Pressable>',
      ) +
      "\nfunction Show({v}) {\n  const t = useMemo(() => { if (v > 1) return 'many'; return 'few'; }, [v]);\n" +
      '  return <Text>{t}</Text>;\n}';
    expect(() => compile(source)).not.toThrow();
  });

  it('refuses a side effect, and one that can end without a value', () => {
    const memo = (body: string) =>
      compile(
        appOf(
          `  const [n, setN] = useState(0);\n  const t = useMemo(() => { ${body} }, [n]);`,
          '<Text>{t}</Text>',
        ),
      );
    expect(() => memo('setN(1); return n;')).toThrow(
      /the useMemo for "t" works out a value, so it can only declare locals, branch, loop and return/,
    );
    expect(() => memo('if (n > 1) return 1;')).toThrow(
      /the useMemo for "t" works out a value, but can end without returning one/,
    );
  });
});

describe('updaters with a block body', () => {
  it('sets the state from each return, and leaves the updater', () => {
    expect(
      shape(
        lowerHandler(
          'setN(v => { const next = v + 1; if (next > 9) return 0; return next; }); setLabel("x");',
        ),
      ),
    ).toEqual([
      {block: ['declareLocal', {if: ['set n', 'return']}, 'set n']},
      'set label',
    ]);
  });

  it('needs no block when it returns only at the end', () => {
    expect(
      shape(lowerHandler('setN(v => { const next = v * 2; return next; });')),
    ).toEqual(['declareLocal', 'set n']);
  });

  it('returns from inside its loop', () => {
    expect(
      shape(
        lowerHandler(
          'setN(v => { let total = v; for (let i = 1; i <= 3; i++) { total += i; if (total > 9) return 9; } ' +
            'return total; });',
        ),
      ),
    ).toEqual([
      {
        block: [
          'declareLocal',
          'declareLocal',
          {loop: ['assignLocal', {if: ['set n', 'return']}]},
          'set n',
        ],
      },
    ]);
  });

  it('compiles for an object, a tagged object it narrows, and a list', () => {
    const body =
      '  const [p, setP] = useState({x: 0, y: 0});\n' +
      "  const [job, setJob] = useState({kind: 'idle'});\n" +
      '  const [items, setItems] = useState([1, 2]);';
    const handler =
      'setP(prev => { if (prev.x >= 2) return {x: 0, y: prev.y + 1}; return {...prev, x: prev.x + 1}; });\n' +
      "setJob(prev => { if (prev.kind === 'idle') return {kind: 'busy', pct: 0}; " +
      "if (prev.pct >= 75) return {kind: 'idle'}; return {kind: 'busy', pct: prev.pct + 25}; });\n" +
      'setItems(prev => { let sum = 0; for (const v of prev) { sum += v; if (sum > 12) return []; } ' +
      'return [...prev, sum]; });';
    expect(() =>
      compile(
        appOf(
          body,
          `<Pressable onPress={() => {${handler}}}><Text>{p.x}{job.kind}{items.length}</Text></Pressable>`,
        ),
      ),
    ).not.toThrow();
  });

  it('lets its parameter hide a state of the same name', () => {
    /** The program an app compiles to whose handler is `handler`. */
    const programOf = (handler: string) =>
      Buffer.from(
        compile(
          appOf(
            '  const [n, setN] = useState(7);\n' +
              '  const [p, setP] = useState({x: 0, y: 0});\n' +
              '  const [items, setItems] = useState([1]);',
            `<Pressable onPress={() => {${handler}}}><Text>{n}{p.x}{items.length}</Text></Pressable>`,
          ),
        ).program,
      ).toString('hex');
    expect(
      programOf('setP(n => { const x = n.x + 1; return {...n, x}; });'),
    ).toBe(programOf('setP(q => { const x = q.x + 1; return {...q, x}; });'));
    expect(programOf('setP(n => ({...n, x: n.x + 1}));')).toBe(
      programOf('setP(q => ({...q, x: q.x + 1}));'),
    );
    expect(programOf('setN(p => { return p + 1; });')).toBe(
      programOf('setN(v => { return v + 1; });'),
    );
    expect(programOf('setItems(n => [...n, 2]);')).toBe(
      programOf('setItems(w => [...w, 2]);'),
    );
  });

  it('writes its own locals where they are in scope', () => {
    expect(() =>
      lowerHandler(
        'setN(v => { let total = v; for (let i = 0; i < 2; i++) { let step = i; step++; total += step; } ' +
          'if (v > 1) { total = 0; } return total; });',
      ),
    ).not.toThrow();
    expect(() =>
      lowerHandler(
        'let k = 0; setN(v => { if (v > 0) { const k = 1; } k = v; return v; }); setN(k);',
      ),
    ).toThrow(/setN's updater can only write its own locals/);
    expect(() =>
      lowerHandler(
        'let k = 0; setN(v => { switch (v) { case 1: let k = 1; break; default: k = v; } return v; }); setN(k);',
      ),
    ).toThrow(/setN's updater can only write its own locals/);
    expect(() =>
      lowerHandler(
        'setN(v => { switch (v) { case 1: let t = 5; t += 1; return t; default: return 0; } });',
      ),
    ).not.toThrow();
  });

  it('refuses side effects, writes to outer locals, and a way out without a value', () => {
    expect(() =>
      lowerHandler('setN(v => { setLabel("x"); return v + 1; });'),
    ).toThrow(/setN's updater can't call setLabel/);
    expect(() =>
      lowerHandler('let k = 0; setN(v => { k = v; return v + 1; });'),
    ).toThrow(/setN's updater can only write its own locals/);
    expect(() =>
      lowerHandler(
        'setN(v => { const id = setTimeout(() => {}, 5); return v; });',
      ),
    ).toThrow(/setN's updater can't call setTimeout/);
    expect(() =>
      lowerHandler('setN(v => { if (v > 1) return; return v; });'),
    ).toThrow(/each `return` in setN's updater must give the new value/);
    expect(() => lowerHandler('setN(v => { if (v > 1) return 0; });')).toThrow(
      /setN's updater can end without returning the new value/,
    );
  });
});
