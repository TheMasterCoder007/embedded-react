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

// `switch`, early `return` and helpers that return a value.

const SCREEN = {screen: {width: 240, height: 320}};
const CAPS = {listCap: 8, listStrCap: 48, maxTextSpans: 4, maxTimers: 4};

/** The state every handler here runs against. */
const STATE =
  "const [n, setN] = useState(0);\nconst [label, setLabel] = useState('a');";

/** An app module whose button runs `handler`, after the helpers in `before`. */
const appOf = (handler: string, before = '', jsx = '<Text>{n}</Text>') =>
  "import {useState} from 'react';\n" +
  "import {Pressable, Text, View} from 'embedded-react';\n" +
  `${before}\nexport function App() {\n${STATE}\n` +
  `  return <View><Pressable onPress={() => {${handler}}}><Text>{label}</Text></Pressable>${jsx}</View>;\n}`;

const compile = (source: string) => compilePackage(source, 'app', SCREEN);

/** Lowers a handler body as a body of its own, with the helpers in `before`. */
function lowerHandler(handler: string, before = ''): IrStmt[] {
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
    math64: false,
    caps: CAPS,
    stores: [],
    helpers: collectHelpers(
      body,
      parse(before, {sourceType: 'module'}).program,
    ),
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
      case 'setState':
      case 'setStateText':
        return `set ${statement.state.name}`;
      default:
        return statement.kind;
    }
  });

describe('switch', () => {
  it('runs as an if/else chain, with empty cases sharing a test and the default last', () => {
    const statements = lowerHandler(
      'switch (n) { case 1: case 2: setN(0); break; ' +
        "case 3: setLabel('x'); default: setN(9); }",
    );
    expect(shape(statements)).toEqual([
      {if: ['set n'], else: [{if: ['set label', 'set n'], else: ['set n']}]},
    ]);
    const [chain] = statements;
    expect(chain.kind === 'if' && chain.test.kind).toBe('logical');
  });

  it('keeps an empty case that matches before the default', () => {
    expect(
      shape(lowerHandler('switch (n) { default: setN(1); break; case 2: }')),
    ).toEqual([{if: [], else: ['set n']}]);
  });

  it('runs the default in place, falling through to the case after it', () => {
    expect(
      shape(
        lowerHandler(
          "switch (n) { case 1: setN(1); break; default: setLabel('d'); case 2: setN(2); }",
        ),
      ),
    ).toEqual([
      {if: ['set n'], else: [{if: ['set n'], else: ['set label', 'set n']}]},
    ]);
  });

  it('works out a discriminant that is not a plain read once', () => {
    const statements = lowerHandler(
      'switch (n % 3) { case 0: setN(1); break; case 1: setN(2); }',
    );
    expect(shape(statements)).toEqual([
      'declareLocal',
      {if: ['set n'], else: [{if: ['set n']}]},
    ]);
    const [declaration, chain] = statements;
    expect(chain.kind === 'if' && chain.test).toMatchObject({
      left: {
        kind: 'local',
        local: {
          name: declaration.kind === 'declareLocal' ? declaration.name : '',
        },
      },
    });
  });

  it("names that local so it hides none of the app's", () => {
    // The switch starts at offset 21, as the old `$switch21` name did.
    const [, , chain] = lowerHandler(
      'const $switch21 = 5; switch (n % 3) { case 0: setN($switch21); }',
    );
    const [write] = chain.kind === 'if' ? chain.consequent : [];
    expect(write.kind === 'setState' && write.value).toMatchObject({
      kind: 'local',
      local: {name: 'local_$switch21', source: {kind: 'number', value: 5}},
    });
  });

  it('refuses a break that does not end its case', () => {
    expect(() =>
      lowerHandler('switch (n) { case 1: if (label) break; setN(1); break; }'),
    ).toThrow(/a `break` must be the last statement of its case/);
  });

  it('narrows a tagged object in each case', () => {
    const source =
      "import {useState} from 'react';\n" +
      "import {Pressable, Text} from 'embedded-react';\n" +
      'export function App() {\n' +
      "  const [job, setJob] = useState({kind: 'idle'});\n" +
      '  return <Pressable onPress={() => {\n' +
      '    switch (job.kind) {\n' +
      "      case 'busy': setJob(job.pct >= 2 ? {kind: 'idle'} : {...job, pct: job.pct + 1}); break;\n" +
      "      default: setJob({kind: 'busy', pct: 0});\n" +
      '    }\n' +
      '  }}><Text>{job.kind}</Text></Pressable>;\n}';
    expect(() => compile(source)).not.toThrow();
  });
});

describe('early return', () => {
  it('makes a handler a block its return leaves', () => {
    expect(shape(lowerHandler('if (n > 5) return; setN(n + 1);'))).toEqual([
      {block: [{if: ['return']}, 'set n']},
    ]);
  });

  it('needs no block for a tail return, and still runs a call it returns', () => {
    expect(shape(lowerHandler('setLabel("x"); return setN(1);'))).toEqual([
      'set label',
      'set n',
    ]);
  });

  it("leaves only a helper's block, and the handler goes on", () => {
    expect(
      shape(
        lowerHandler(
          'bump(); setLabel("b");',
          'function bump() { if (n > 5) return; setN(n + 1); }',
        ),
      ),
    ).toEqual([{block: [{if: ['return']}, 'set n']}, 'set label']);
  });

  it('narrows the statements after a branch that ends in a switch that returns', () => {
    const source =
      "import {useState} from 'react';\n" +
      "import {Pressable, Text} from 'embedded-react';\n" +
      'export function App() {\n' +
      "  const [job, setJob] = useState({kind: 'off'});\n" +
      '  const [n, setN] = useState(0);\n' +
      '  return <Pressable onPress={() => {\n' +
      "    if (job.kind === 'off') {\n" +
      '      switch (n) { case 0: return setN(1); default: return; }\n' +
      '    }\n' +
      "    setJob(job.speed > 2 ? {kind: 'run', speed: 0} : {...job, speed: job.speed + 1});\n" +
      '  }}><Text>{job.kind}</Text></Pressable>;\n}';
    expect(() => compile(source)).not.toThrow();
  });

  it('narrows the statements after a branch that returns', () => {
    const source =
      "import {useState} from 'react';\n" +
      "import {Pressable, Text} from 'embedded-react';\n" +
      'export function App() {\n' +
      "  const [job, setJob] = useState({kind: 'idle'});\n" +
      '  return <Pressable onPress={() => {\n' +
      "    if (job.kind !== 'busy') return setJob({kind: 'busy', pct: 0});\n" +
      '    setJob({...job, pct: job.pct + 1});\n' +
      '  }}><Text>{job.kind}</Text></Pressable>;\n}';
    expect(() => compile(source)).not.toThrow();
  });
});

describe('helpers that return a value', () => {
  it('folds a call with constant arguments', () => {
    const [statement] = lowerHandler(
      'setN(double(3));',
      'function double(v) { return v * 2; }',
    );
    expect(statement.kind === 'setState' && statement.value).toMatchObject({
      kind: 'number',
      value: 6,
    });
  });

  it('works out each branch as a ternary', () => {
    const [statement] = lowerHandler(
      'setN(clamp(n + 1));',
      'function clamp(v) { if (v > 9) return 9; if (v < 0) return 0; return v; }',
    );
    expect(statement.kind === 'setState' && statement.value).toMatchObject({
      kind: 'conditional',
      consequent: {kind: 'number', value: 9},
      alternate: {
        kind: 'conditional',
        consequent: {kind: 'number', value: 0},
      },
    });
  });

  it('builds text a helper returns where it is called', () => {
    const [statement] = lowerHandler(
      'setLabel(percent(n));',
      'const percent = v => `${v}%`;',
    );
    expect(statement.kind).toBe('setStateText');
    expect(
      statement.kind === 'setStateText' &&
        statement.text.parts.map(part => part.kind),
    ).toEqual(['literal', 'value', 'literal']);
  });

  it('compiles in render, from a switch, and with an object passed', () => {
    const source =
      "import {useState} from 'react';\n" +
      "import {Text, View} from 'embedded-react';\n" +
      "function name(v) { switch (v) { case 0: return 'none'; case 1: return 'one'; default: return 'many'; } }\n" +
      'function area(box) { const w = box.w; return w * box.h; }\n' +
      'export function App() {\n' +
      '  const [n] = useState(0);\n' +
      '  const [box] = useState({w: 2, h: 3});\n' +
      '  return <View style={{width: area(box)}}><Text>{name(n)}</Text></View>;\n}';
    expect(() => compile(source)).not.toThrow();
  });

  it('refuses one that calls itself', () => {
    expect(() =>
      compile(
        appOf(
          'setN(down(n));',
          'function down(v) { return v > 0 ? down(v - 1) : 0; }',
        ),
      ),
    ).toThrow(/helper "down" is recursive/);
  });

  it('refuses one with side effects, or that can end without a value', () => {
    expect(() =>
      compile(
        appOf(
          'setN(next(n));',
          'function next(v) { setLabel("x"); return v; }',
        ),
      ),
    ).toThrow(/can only declare locals, branch, loop and return/);
    expect(() =>
      compile(appOf('setN(pick(n));', 'function pick(v) { if (v) return 1; }')),
    ).toThrow(/can end without returning one/);
  });

  it('refuses a call that leaves out a param', () => {
    expect(() =>
      compile(appOf('setN(add(n));', 'const add = (a, b) => a + b;')),
    ).toThrow(/must be passed each of its params/);
  });
});
