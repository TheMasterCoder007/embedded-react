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
import {beginBoot, endBoot, screenBinding} from '../compiler/boot.mts';
import {collectState} from '../compiler/collect.mts';
import {lowerStmts} from '../compiler/handlers.mts';
import {Out} from '../compiler/out.mts';
import type {IrStmt} from '../compiler/ir/statements.mts';
import type {Env, RecordState, Scope} from '../compiler/types.mts';

// Object state and objects built in render or in a handler: a record of fields, a scalar slot each.

const SCREEN = {screen: {width: 240, height: 320}};
const CAPS = {listCap: 8, listStrCap: 48, maxTextSpans: 4, maxTimers: 4};

/** An app module around a component body, the JSX it returns, and what precedes App. */
const appOf = (body: string, jsx: string, before = ''): string =>
  "import {useEffect, useMemo, useState} from 'react';\n" +
  "import {Pressable, Text, View} from 'embedded-react';\n" +
  `${before}\nexport function App() {\n${body}\n  return ${jsx};\n}`;

const compile = (source: string) => compilePackage(source, 'app', SCREEN);

/** An app whose button runs `handler`, over a box state and a counter. */
const boxApp = (handler: string, body = '', jsx = '<Text>{box.pos.x}</Text>') =>
  appOf(
    "const [box, setBox] = useState({pos: {x: 1, y: 2}, label: 'a', on: false});\n" +
      'const [n, setN] = useState(0);\n' +
      body,
    `<View><Pressable onPress={() => {${handler}}}><Text>{n}</Text></Pressable>${jsx}</View>`,
  );

/** Lowers a handler body against a component's state, returning its statements. */
function lowerHandler(stateBody: string, handler: string): IrStmt[] {
  const parsed = (source: string) =>
    parse(source, {sourceType: 'module'}).program.body;
  const scope: Scope = {};
  const state = collectState(
    {type: 'BlockStatement', body: parsed(stateBody)} as t.BlockStatement,
    scope,
    CAPS,
  );
  const environment = {
    state: state.byName,
    locals: new Map(),
    constants: scope,
    refs: new Map(),
    math64: false,
    caps: CAPS,
    stores: [],
  } as unknown as Env;
  return lowerStmts(parsed(handler), environment, state, {
    stateChanged: false,
    output: new Out(new Map(), null as never),
  });
}

/** The fields a setter is given back as they were. */
const keptFields = (statements: IrStmt[]): string[] =>
  statements.flatMap(statement =>
    statement.kind === 'keepField' ? [statement.state.name] : [],
  );

/** What each lowered statement writes or declares, the fields given back as they were left out. */
const targets = (statements: IrStmt[]): string[] =>
  statements
    .filter(statement => statement.kind !== 'keepField')
    .map(statement =>
      statement.kind === 'declareLocal'
        ? `declare ${statement.name}`
        : 'state' in statement
          ? `${statement.kind} ${statement.state.name}`
          : statement.kind,
    );

describe('object state', () => {
  it('keeps a scalar state per field, nested objects flattened', () => {
    const scope: Scope = {};
    const state = collectState(
      {
        type: 'BlockStatement',
        body: parse(
          "const [box, setBox] = useState({pos: {x: 1, y: 2.0}, label: 'a', on: false});",
          {sourceType: 'module'},
        ).program.body,
      } as t.BlockStatement,
      scope,
      CAPS,
      'c0_',
    );
    const record = state.bySetter.get('setBox') as RecordState;
    expect(record.kind).toBe('record');
    expect(
      [...record.fields].map(([key, field]) => [
        key,
        field.slotName,
        field.slotKind,
        field.isBoolean,
      ]),
    ).toEqual([
      ['pos.x', 'c0_box.pos.x', 'int', false],
      ['pos.y', 'c0_box.pos.y', 'float', false],
      ['label', 'c0_box.label', 'string', false],
      ['on', 'c0_box.on', 'int', true],
    ]);
    expect([...state.byName.keys()]).toEqual([
      'box.pos.x',
      'box.pos.y',
      'box.label',
      'box.on',
    ]);
    expect(Object.keys(scope.box as object)).toEqual(['pos', 'label', 'on']);
  });

  it('takes a field worked out from the panel as a number', () => {
    beginBoot('');
    try {
      const scope: Scope = {
        screen: screenBinding({width: 240, height: 320}),
      };
      const state = collectState(
        {
          type: 'BlockStatement',
          body: parse(
            'const [p, setP] = useState({w: screen.width / 2, n: 1});',
            {sourceType: 'module'},
          ).program.body,
        } as t.BlockStatement,
        scope,
        CAPS,
      );
      const record = state.bySetter.get('setP') as RecordState;
      expect(
        [...record.fields].map(([key, field]) => [
          key,
          field.slotKind,
          field.initial,
        ]),
      ).toEqual([
        ['w', 'int', 120],
        ['n', 'int', 1],
      ]);
    } finally {
      endBoot();
    }
  });

  it('writes the fields a setter changes, and keeps the ones it spreads back', () => {
    const state = "const [p, setP] = useState({x: 1, y: 2, s: 'a'});";
    const spread = lowerHandler(state, 'setP({...p, x: p.x + 1});');
    expect(targets(spread)).toEqual(['setState p.x']);
    expect(keptFields(spread)).toEqual(['p.y', 'p.s']);
    expect(
      targets(lowerHandler(state, "setP(q => ({...q, s: q.s + '!'}));")),
    ).toEqual(['setStateText p.s']);
    expect(
      targets(lowerHandler(state, "setP({x: 3, y: p.y, s: 'b'});")),
    ).toEqual(['setState p.x', 'setStateText p.s']);
  });

  it('works out first a value that reads a field written before it', () => {
    const state = "const [p, setP] = useState({x: 1, y: 2, s: 'a', t: 'b'});";
    expect(
      targets(lowerHandler(state, 'setP({...p, x: p.y, y: p.x});')),
    ).toEqual(['declare local_p.y', 'setState p.x', 'setState p.y']);
    expect(
      targets(lowerHandler(state, 'setP({...p, s: p.t, t: p.s});')),
    ).toEqual(['declare local_p.t', 'setStateText p.s', 'setStateText p.t']);
    // A value that reads only fields written after it needs nothing first.
    expect(
      targets(lowerHandler(state, 'setP({...p, x: p.y + 1, y: 5});')),
    ).toEqual(['setState p.x', 'setState p.y']);
  });

  it('compiles reads, writes and objects passed around', () => {
    for (const handler of [
      'setBox({...box, pos: {...box.pos, x: box.pos.x + 1}});',
      'setBox(previous => ({...previous, on: !previous.on}));',
      "setBox(n > 1 ? {...box, label: 'b'} : {...box, label: 'c'});",
      'const next = {...box, on: true}; setBox(next);',
      'const {pos: {x}, label, ...rest} = box; setN(x); setBox({...rest, label, pos: {x: 0, y: 0}});',
      'const {a} = {a: n + 1}; setN(a);',
      'const theme = {gap: 4}; setN(theme.gap);',
    ]) {
      expect(() => compile(boxApp(handler)), handler).not.toThrow();
    }
    for (const [body, jsx] of [
      ['const {pos, label} = box;', '<Text>{label} {pos.x}</Text>'],
      ['const moved = {...box.pos, y: 9};', '<Text>{moved.x + moved.y}</Text>'],
      [
        'const sum = useMemo(() => ({...box.pos, total: box.pos.x + box.pos.y}), [box.pos.x, box.pos.y]);',
        '<Text>{sum.total}</Text>',
      ],
      ['', '<View style={{width: box.pos.x, height: box.on ? 4 : 8}} />'],
    ]) {
      expect(() => compile(boxApp('setN(1);', body, jsx)), body).not.toThrow();
    }
  });

  it('passes an object state to a child, and gives each child instance its own', () => {
    expect(() =>
      compile(
        appOf(
          'const [p, setP] = useState({x: 1, y: 2});',
          '<View><Dot pos={p} onMove={() => setP({...p, y: p.y + 1})} /><Counter /><Counter /></View>',
          'function Dot({pos, onMove}) { return <Pressable onPress={onMove}><Text>{pos.x},{pos.y}</Text></Pressable>; }\n' +
            'function Counter() { const [c, setC] = useState({n: 0}); ' +
            'return <Pressable onPress={() => setC({n: c.n + 1})}><Text>{c.n}</Text></Pressable>; }',
        ),
      ),
    ).not.toThrow();
  });

  it('refuses a write that leaves out a field or adds one', () => {
    expect(() =>
      compile(boxApp("const next = {label: 'b', on: true}; setBox(next);")),
    ).toThrow('setBox leaves out "box.pos.x"');
    // Written in full with other fields, it would be a second shape, but no field names the shape.
    expect(() =>
      compile(boxApp('setBox({pos: {x: 1, y: 2}, on: false});')),
    ).toThrow('state "box" is written with objects of different fields');
    expect(() => compile(boxApp('setBox({...box, size: 3});'))).toThrow(
      'state "box" has no field "size"',
    );
    expect(() => compile(boxApp('setBox(n);'))).toThrow(
      'setBox needs an object with the fields of "box"',
    );
    expect(() =>
      compile(boxApp('setBox(n > 1 ? {...box} : {...box.pos});')),
    ).toThrow('both sides of a ternary must be objects with the same fields');
  });

  it('refuses a field it cannot hold', () => {
    const refuses = (initial: string, message: string) =>
      expect(() =>
        compile(appOf(`const [p, setP] = useState(${initial});`, '<View />')),
      ).toThrow(message);
    refuses(
      '{x: 1, items: [1, 2]}',
      'state "p.items" is an array inside an object state',
    );
    refuses(
      '{x: 1, picked: null}',
      'the initial value of state "p.picked" is null',
    );
    refuses('{x: 1, pos: {}}', 'state "p.pos" is an empty object');
    refuses('{}', 'state "p" is an empty object');
    // Fields are keyed by their dotted path, which these would share.
    refuses("{'pos.x': 1, pos: {x: 2}}", 'state "p" has a field named "pos.x"');
    refuses("{pos: {'': 2}}", 'state "p.pos" has a field named ""');
    expect(() => compile(boxApp("setBox({...box, 'pos.x': 3});"))).toThrow(
      'an object has a field named "pos.x"',
    );
  });

  it('destructures a field named by a string or a number', () => {
    expect(() =>
      compile(
        boxApp(
          "const {'label': name} = box; setN(name === 'a' ? 1 : 0);",
          "const {'label': text, pos: {'x': left}} = box;",
          '<Text>{text} {left}</Text>',
        ),
      ),
    ).not.toThrow();
    expect(() =>
      compile(
        boxApp('setN(1);', 'const {1: one} = {1: n};', '<Text>{one}</Text>'),
      ),
    ).not.toThrow();
  });

  it('refuses a whole object where a value goes', () => {
    expect(() => compile(boxApp('setN(1);', '', '<Text>{box}</Text>'))).toThrow(
      '"box" is an object',
    );
    expect(() =>
      compile(
        boxApp('setN(1);', 'useEffect(() => { setN(box.pos.x); }, [box]);'),
      ),
    ).toThrow('a useEffect dependency on a whole object');
    expect(() => compile(boxApp('let next = {...box}; setBox(next);'))).toThrow(
      'an object in a handler must be a `const`',
    );
    expect(() => compile(boxApp('const {size} = box; setN(size);'))).toThrow(
      'the object has no field "size" to destructure',
    );
  });
});
