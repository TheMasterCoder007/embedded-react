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
import type {IrStmt} from '../compiler/ir/statements.mts';

// useReducer, inlined at each dispatch.

const SCREEN = {screen: {width: 240, height: 320}};

/** A counter reducer that throws for any other type. */
const COUNTER =
  'function counter(state, action) {\n' +
  '  switch (action.type) {\n' +
  "    case 'inc': return state + 1;\n" +
  "    case 'add': return state + action.by;\n" +
  "    case 'reset': return 0;\n" +
  "    default: throw new Error('unknown');\n" +
  '  }\n}\n';

/** An app module with `before` at module level, the hooks in `hooks`, and a button per handler. */
const appOf = (before: string, hooks: string, handlers: string[]) =>
  "import {useEffect, useReducer, useState} from 'react';\n" +
  "import {Pressable, Text, View} from 'embedded-react';\n" +
  `${before}\nexport function App() {\n${hooks}\n  return <View>` +
  handlers
    .map(
      handler =>
        `<Pressable onPress={() => {${handler}}}><Text>x</Text></Pressable>`,
    )
    .join('') +
  '<Text>{n}</Text></View>;\n}';

const COUNTER_HOOKS =
  '  const [n, dispatch] = useReducer(counter, 0);\n' +
  '  const [step, setStep] = useState(3);\n' +
  "  const [word, setWord] = useState('inc');";

const compile = (source: string) => compilePackage(source, 'app', SCREEN);

/** Each handler's statements. */
const handlersOf = (source: string): IrStmt[][] =>
  lowerApp(source, resolveOptions(SCREEN), new Map(), []).output.handlers.map(
    handler => handler.callback.body,
  );

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

describe('useReducer', () => {
  it('runs only the case a constant type picks', () => {
    const handlers = handlersOf(
      appOf(COUNTER, COUNTER_HOOKS, [
        "dispatch({type: 'inc'});",
        "dispatch({type: 'add', by: step});",
        "dispatch({type: 'reset'});",
      ]),
    );
    expect(handlers.map(shape)).toEqual([['set n'], ['set n'], ['set n']]);
  });

  it('dispatches a type picked at run time as a branch per action', () => {
    const handlers = handlersOf(
      appOf(COUNTER, COUNTER_HOOKS, [
        "dispatch({type: step > 3 ? 'inc' : 'reset'});",
        "dispatch(step > 3 ? {type: 'add', by: step} : {type: 'inc'});",
      ]),
    );
    expect(handlers.map(shape)).toEqual([
      [{if: ['set n'], else: ['set n']}],
      [{if: ['set n'], else: ['set n']}],
    ]);
  });

  it('keeps each case a runtime type can take', () => {
    const reducer =
      'function counter(state, action) {\n' +
      "  switch (action.type) { case 'inc': return state + 1; case 'dec': return state - 1; }\n" +
      '  return state;\n}\n';
    const [handler] = handlersOf(
      appOf(reducer, COUNTER_HOOKS, ['dispatch({type: word});']),
    );
    expect(shape(handler)).toEqual([
      {
        block: [
          {if: ['set n', 'return'], else: [{if: ['set n', 'return']}]},
          'set n',
        ],
      },
    ]);
  });

  it('works out a runtime field where it is dispatched', () => {
    const reducer =
      'function counter(state, action) { const step = 100; return state + action.by + step; }\n';
    const [handler] = handlersOf(
      appOf(reducer, COUNTER_HOOKS, ["dispatch({type: 'add', by: step});"]),
    );
    expect(shape(handler)).toEqual(['declareLocal', 'set n']);
    // `by` reads the state `step`, not the reducer's own `step`, which is a local holding 100.
    const write = JSON.stringify(handler[1]);
    expect(write).toContain('"slotName":"step"');
    expect(JSON.stringify(handler[0])).toContain('"value":100');
  });

  it("uses the component's binding of the reducer's name over the module's", () => {
    const module = 'function counter(state, action) { return state + 100; }\n';
    const [[write]] = handlersOf(
      appOf(
        module,
        '  const counter = (state, action) => state + 7;\n' + COUNTER_HOOKS,
        ["dispatch({type: 'inc'});"],
      ),
    );
    expect(JSON.stringify(write)).toContain('"value":7');
    expect(() =>
      compile(
        appOf(module, '  const counter = 0;\n' + COUNTER_HOOKS, [
          "dispatch({type: 'inc'});",
        ]),
      ),
    ).toThrow(/the reducer for "n" must be a function in this file/);
  });

  it('drops the branches a constant type rules out inside a loop', () => {
    const reducer =
      'function counter(state, action) {\n' +
      '  let total = state;\n' +
      "  for (let i = 0; i < 2; i++) { if (action.type === 'inc') total += 1; else throw new Error('x'); }\n" +
      '  return total;\n}\n';
    expect(() =>
      compile(appOf(reducer, COUNTER_HOOKS, ["dispatch({type: 'inc'});"])),
    ).not.toThrow();
  });

  it('folds no test through a name a block declares', () => {
    const reducer =
      'function counter(state, action) {\n' +
      "  if (state > 5) { const action = {type: 'dec'}; if (action.type === 'inc') return state + 1; " +
      'return state - 1; }\n' +
      '  return state;\n}\n';
    const [handler] = handlersOf(
      appOf(reducer, COUNTER_HOOKS, ["dispatch({type: 'inc'});"]),
    );
    const branch = JSON.stringify(handler);
    expect(branch).toContain('"helper":"add"');
    expect(branch).toContain('"helper":"sub"');
  });

  it('compiles for objects, tagged objects, lists and string actions, in children, helpers, effects and loops', () => {
    const before =
      COUNTER +
      'function walk(state, action) {\n' +
      "  if (action.type === 'step') { const x = state.x + action.dx; if (x > 4) return {x: 0, y: state.y + 1}; " +
      'return {...state, x}; }\n' +
      '  return {...state, y: state.y + 1};\n}\n' +
      'function job(state, action) {\n' +
      "  switch (action.type) { case 'start': return {kind: 'busy', pct: 0}; case 'tick':\n" +
      "    if (state.kind !== 'busy') return state; if (state.pct >= 75) return {kind: 'idle'}; " +
      "return {kind: 'busy', pct: state.pct + 25}; }\n}\n" +
      'function items(state, action) {\n' +
      "  switch (action.type) { case 'add': return [...state, action.value]; case 'clear': return []; }\n}\n" +
      'function Counter() {\n' +
      '  const [c, dispatchC] = useReducer(counter, 0);\n' +
      "  return <Pressable onPress={() => dispatchC({type: 'inc'})}><Text>{c}</Text></Pressable>;\n}\n";
    const hooks =
      COUNTER_HOOKS +
      '\n  const [p, dispatchWalk] = useReducer(walk, {x: 0, y: 0});\n' +
      "  const [task, dispatchJob] = useReducer(job, {kind: 'idle'});\n" +
      '  const [list, dispatchList] = useReducer(items, [1, 2]);\n' +
      "  const [mode, dispatchMode] = useReducer((current, action) => (action === 'next' ? 'b' : 'a'), 'a');\n" +
      "  const twice = () => { dispatch({type: 'inc'}); dispatch({type: 'inc'}); };\n" +
      "  useEffect(() => { const id = setInterval(() => dispatch({type: 'inc'}), 1000); " +
      'return () => clearInterval(id); }, []);';
    const handlers = [
      "dispatchWalk({type: 'step', dx: 2}); dispatchWalk({type: 'down'});",
      "dispatchJob({type: 'start'}); dispatchJob({type: 'tick'});",
      "dispatchList({type: 'add', value: list.length}); dispatchList({type: 'clear'});",
      "dispatchMode('next');",
      'twice();',
      "for (let i = 0; i < 3; i++) dispatch({type: 'inc'});",
    ];
    const source = appOf(before, hooks, handlers).replace(
      '<Text>{n}</Text>',
      '<Text>{n}{p.x}{task.kind}{list.length}{mode}</Text><Counter />',
    );
    expect(() => compile(source)).not.toThrow();
  });

  it('refuses a new shape a reducer returns as a spread', () => {
    const reducer =
      "function job(state, action) { if (action.type === 'start') return {...state, kind: 'busy', pct: 0}; " +
      'return state; }\n';
    expect(() =>
      compile(
        appOf(
          reducer,
          "  const [n, dispatch] = useReducer(job, {kind: 'idle'});",
          ["dispatch({type: 'start'});"],
        ).replace('<Text>{n}</Text>', '<Text>{n.kind}</Text>'),
      ),
    ).toThrow(
      /a write that spreads "n" keeps its shape, so it can't add "pct"[\s\S]*return \{kind: 'busy', pct: …\}/,
    );
  });

  it('refuses a throw a dispatch can reach', () => {
    expect(() =>
      compile(appOf(COUNTER, COUNTER_HOOKS, ['dispatch({type: word});'])),
    ).toThrow(/reducer "counter" can reach a `throw`/);
    expect(() =>
      compile(appOf(COUNTER, COUNTER_HOOKS, ["dispatch({type: 'dec'});"])),
    ).toThrow(/reducer "counter" can reach a `throw`/);
  });

  it('refuses a reducer it cannot find or inline, and a dispatch with no action', () => {
    const hooks = (call: string) =>
      `  const [n, dispatch] = ${call};\n  const [step] = useState(1);`;
    expect(() =>
      compile(appOf('', hooks('useReducer(nowhere, 0)'), [])),
    ).toThrow(/the reducer for "n" must be a function in this file/);
    expect(() =>
      compile(appOf(COUNTER, hooks('useReducer(counter, 0, x => x)'), [])),
    ).toThrow(/useReducer for "n" must take a reducer and an initial value/);
    expect(() =>
      compile(
        appOf(
          'function pair(state, {type}) { return state; }',
          hooks('useReducer(pair, 0)'),
          [],
        ),
      ),
    ).toThrow(/reducer "pair" must take plain \(state, action\) params/);
    expect(() =>
      compile(appOf(COUNTER, COUNTER_HOOKS, ['dispatch();'])),
    ).toThrow(/dispatch must be given an action/);
    expect(() =>
      compile(
        appOf(
          'function noisy(state, action) { setTimeout(() => {}, 5); return state + 1; }',
          hooks('useReducer(noisy, 0)'),
          ["dispatch({type: 'inc'});"],
        ),
      ),
    ).toThrow(/reducer "noisy" can't call setTimeout/);
  });
});
