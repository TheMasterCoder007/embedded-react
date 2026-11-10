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
import {collectState} from '../compiler/collect.mts';
import {lowerStmts} from '../compiler/handlers.mts';
import {Out} from '../compiler/out.mts';
import type {IrStmt} from '../compiler/ir/statements.mts';
import type {Env, RecordState, Scope} from '../compiler/types.mts';

// Object state of several shapes: a hidden tag, a slot for every field, and tests on the tag that narrow reads.

const SCREEN = {screen: {width: 240, height: 320}};
const CAPS = {listCap: 8, listStrCap: 48, maxTextSpans: 4, maxTimers: 4};

/** An app module around a component body, the JSX it returns, and what precedes App. */
const appOf = (body: string, jsx: string, before = ''): string =>
  "import {useState} from 'react';\n" +
  "import {Pressable, Text, View} from 'embedded-react';\n" +
  `${before}\nexport function App() {\n${body}\n  return ${jsx};\n}`;

const compile = (source: string) => compilePackage(source, 'app', SCREEN);

/** The job state every test starts from: idle, busy with a percentage, or done with a time and a note. */
const JOB =
  "const [job, setJob] = useState({kind: 'idle'});\n" +
  'const [n, setN] = useState(0);\n';

/** Writes that give the job's other shapes in full. */
const SHAPES =
  "setJob({kind: 'busy', pct: 0}); setJob({kind: 'done', took: 0.5, note: ''});";

/** An app whose button runs `handler`, over the job, rendering `jsx`. */
const jobApp = (handler: string, jsx = '<Text>{n}</Text>') =>
  appOf(
    JOB,
    `<View><Pressable onPress={() => {${SHAPES} ${handler}}}><Text>{n}</Text></Pressable>${jsx}</View>`,
  );

/** The parsed statements of a snippet. */
const parsed = (source: string): t.Statement[] =>
  parse(source, {sourceType: 'module'}).program.body;

/** Collects the state of a component body, scanning `writes` for the objects its setters are given. */
function collect(stateBody: string, writes = '') {
  const scope: Scope = {};
  const state = collectState(
    {
      type: 'BlockStatement',
      body: [...parsed(stateBody), ...parsed(writes)],
    } as t.BlockStatement,
    scope,
    CAPS,
    'c0_',
  );
  return {scope, state};
}

/** Lowers a handler body against the job, returning its statements. */
function lowerHandler(handler: string): IrStmt[] {
  const {scope, state} = collect(JOB, SHAPES);
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

/** What each lowered statement writes or declares, an `if`'s branches nested. */
const targets = (statements: IrStmt[]): unknown[] =>
  statements.map(statement =>
    statement.kind === 'if'
      ? {
          if: targets(statement.consequent),
          else: statement.alternate && targets(statement.alternate),
        }
      : statement.kind === 'declareLocal'
        ? `declare ${statement.name}`
        : 'state' in statement
          ? `${statement.kind} ${statement.state.name}`
          : statement.kind,
  );

describe('tagged object state', () => {
  it('keeps a hidden tag and a slot for every field of every shape', () => {
    const {state} = collect(JOB, SHAPES);
    const record = state.bySetter.get('setJob') as RecordState;
    expect(record.tag?.key).toBe('kind');
    expect(record.tag?.values).toEqual(['idle', 'busy', 'done']);
    expect(record.tag?.shapes.map(shape => [...shape])).toEqual([
      [],
      ['pct'],
      ['took', 'note'],
    ]);
    expect(
      [record.tag!.state, ...record.fields.values()].map(field => [
        field.slotName,
        field.slotKind,
        field.initial,
      ]),
    ).toEqual([
      ['c0_job.kind', 'int', 0],
      ['c0_job.pct', 'int', 0],
      ['c0_job.took', 'float', 0],
      ['c0_job.note', 'string', ''],
    ]);
    expect([...state.byName.keys()]).toEqual([
      'job.kind',
      'job.pct',
      'job.took',
      'job.note',
      'n',
    ]);
  });

  it('starts in the shape of its initial value, which may have fields', () => {
    const {state} = collect(
      'const [r, setR] = useState({ok: true, value: 3, at: {x: 1}});',
      "setR({ok: false, error: 'none'});",
    );
    const record = state.bySetter.get('setR') as RecordState;
    expect(record.tag?.key).toBe('ok');
    expect(
      [...record.fields].map(([key, field]) => [key, field.initial]),
    ).toEqual([
      ['value', 3],
      ['at.x', 1],
      ['error', ''],
    ]);
  });

  it('takes a field kind from any constant written for it', () => {
    const {state} = collect(
      "const [s, setS] = useState({kind: 'a'});",
      "setS({kind: 'b', x: n}); setS({kind: 'b', x: 2.0}); setS(q => n ? {kind: 'c', on: true} : q);",
    );
    const record = state.bySetter.get('setS') as RecordState;
    expect(
      [...record.fields].map(([key, field]) => [
        key,
        field.slotKind,
        field.isBoolean,
      ]),
    ).toEqual([
      ['x', 'float', false],
      ['on', 'int', true],
    ]);
  });

  it('writes the tag and the fields of the shape it is given', () => {
    expect(targets(lowerHandler("setJob({kind: 'busy', pct: n});"))).toEqual([
      'setState job.kind',
      'setState job.pct',
    ]);
    expect(targets(lowerHandler("setJob({kind: 'idle'});"))).toEqual([
      'setState job.kind',
    ]);
  });

  it('skips the tag where a test already fixes the shape', () => {
    expect(
      targets(
        lowerHandler(
          "if (job.kind === 'busy') { setJob({...job, pct: job.pct + 1}); }",
        ),
      ),
    ).toEqual([{if: ['setState job.pct'], else: null}]);
    // A spread of the state keeps its shape, whatever it is.
    expect(targets(lowerHandler('setJob({...job});'))).toEqual([]);
    expect(targets(lowerHandler('setJob(j => j);'))).toEqual([]);
  });

  it('sets the shape a ternary picks', () => {
    expect(
      targets(
        lowerHandler(
          "setJob(j => j.kind === 'busy' && j.pct >= 90 ? {kind: 'done', took: 1, note: 'ok'} : j);",
        ),
      ),
    ).toEqual([
      {
        if: ['setState job.kind', 'setState job.took', 'setStateText job.note'],
        else: null,
      },
    ]);
    expect(
      targets(
        lowerHandler(
          "setJob(job.kind === 'busy' ? {...job, pct: job.pct + 1} : {kind: 'busy', pct: 0});",
        ),
      ),
    ).toEqual([
      {
        if: ['setState job.pct'],
        else: ['setState job.kind', 'setState job.pct'],
      },
    ]);
  });

  it('works out first a field that reads the tag it writes', () => {
    expect(
      targets(
        lowerHandler(
          "setJob({kind: 'busy', pct: job.kind === 'busy' ? job.pct : 0});",
        ),
      ),
    ).toEqual([
      'declare local_c0_job.pct',
      'setState job.kind',
      'setState job.pct',
    ]);
  });

  it('reads a field where a test on the tag shows the shape has it', () => {
    for (const jsx of [
      "<Text>{job.kind === 'busy' && job.pct > 50 ? 'late' : ''}</Text>",
      "<Text>{job.kind !== 'busy' ? 0 : job.pct}</Text>",
      "<Text>{job.kind === 'idle' || job.kind === 'done' ? 'still' : job.pct > 1 ? 'on' : 'off'}</Text>",
      "{job.kind === 'busy' && <Text>{job.pct}%</Text>}",
      "{job.kind === 'done' ? <Text>{job.took}s {job.note}</Text> : <Text>{job.kind}</Text>}",
      "{!(job.kind !== 'busy') && <View style={{width: job.pct}} />}",
    ]) {
      expect(() => compile(jobApp('setN(1);', jsx)), jsx).not.toThrow();
    }
    for (const handler of [
      "if (job.kind === 'busy') { setN(job.pct); } else if (job.kind === 'done') { setN(job.took); }",
      "if (job.kind !== 'busy') { setN(0); } else { setN(job.pct); }",
      "setN('busy' === job.kind ? job.pct : 0);",
    ]) {
      expect(() => compile(jobApp(handler)), handler).not.toThrow();
    }
  });

  it('joins the shapes either side of `||` or `&&` leaves', () => {
    const app = (test: string) =>
      appOf(
        "const [s, setS] = useState({kind: 'a', x: 1}); const [n, setN] = useState(0);",
        "<View><Pressable onPress={() => { setS({kind: 'b', x: 2}); setS({kind: 'c'}); " +
          `if (${test}) { setN(s.x); } }} /><Text>{${test} ? s.x : 0}</Text></View>`,
      );
    for (const test of [
      "s.kind === 'a' || s.kind === 'b'",
      "!(s.kind !== 'a' && s.kind !== 'b')",
      "s.kind !== 'c' || (s.kind === 'b' && n > 1)",
    ]) {
      expect(() => compile(app(test)), test).not.toThrow();
    }
    expect(() => compile(app("s.kind === 'a' || s.kind === 'c'"))).toThrow(
      `"s.x" is only there when s.kind is 'a' or 'b'`,
    );
  });

  it('narrows on a boolean tag read as a test', () => {
    expect(() =>
      compile(
        appOf(
          "const [r, setR] = useState({ok: false, error: 'none'});",
          "<View><Pressable onPress={() => setR({ok: true, value: 2})} /><Text>{r.ok ? 'got' : r.error}</Text>{!r.ok ? null : <Text>{r.value}</Text>}</View>",
        ),
      ),
    ).not.toThrow();
  });

  it('gives each child instance its own tagged state', () => {
    expect(() =>
      compile(
        appOf(
          '',
          '<View><Job /><Job /></View>',
          "function Job() { const [j, setJ] = useState({kind: 'idle'}); " +
            "return <Pressable onPress={() => setJ(j.kind === 'idle' ? {kind: 'busy', pct: 0} : {kind: 'idle'})}>" +
            "{j.kind === 'busy' ? <Text>{j.pct}</Text> : <Text>{j.kind}</Text>}</Pressable>; }",
        ),
      ),
    ).not.toThrow();
  });

  it('passes a tagged state to a child, which narrows it too', () => {
    expect(() =>
      compile(
        appOf(
          JOB,
          `<View><Pressable onPress={() => {${SHAPES}}} /><Bar job={job} /></View>`,
          "function Bar({job}) { return <View>{job.kind === 'busy' ? <Text>{job.pct}</Text> : <Text>{job.kind}</Text>}</View>; }",
        ),
      ),
    ).not.toThrow();
  });

  it('refuses a read of a field the shape may not have', () => {
    expect(() => compile(jobApp('setN(1);', '<Text>{job.pct}</Text>'))).toThrow(
      `"job.pct" is only there when job.kind is 'busy'`,
    );
    expect(() => compile(jobApp('setN(job.took);'))).toThrow(
      `"job.took" is only there when job.kind is 'done'`,
    );
    expect(() =>
      compile(jobApp("if (job.kind !== 'idle') { setN(job.pct); }")),
    ).toThrow(`"job.pct" is only there when job.kind is 'busy'`);
  });

  it('refuses a test against a tag no shape has', () => {
    expect(() => compile(jobApp("setN(job.kind === 'gone' ? 1 : 0);"))).toThrow(
      "job.kind is never 'gone'",
    );
  });

  it('refuses a write that leaves out a field, adds one, or names no shape', () => {
    expect(() =>
      compile(
        jobApp("if (job.kind === 'idle') { setJob({...job, kind: 'busy'}); }"),
      ),
    ).toThrow('setJob leaves out "job.pct"');
    expect(() =>
      compile(
        jobApp(
          "if (job.kind === 'busy') { setJob({...job, kind: 'done', took: 1, note: ''}); }",
        ),
      ),
    ).toThrow(`the 'done' shape of "job" has no field "pct"`);
    expect(() =>
      compile(
        jobApp("if (job.kind === 'idle') { setJob({...job, kind: 'gone'}); }"),
      ),
    ).toThrow(`state "job" has no shape with kind 'gone'`);
    expect(() =>
      compile(jobApp("setJob({...job, kind: n > 1 ? 'busy' : 'idle'});")),
    ).toThrow('setJob needs to know which shape "job" takes');
  });

  it('refuses shapes it cannot tell apart or type', () => {
    const refuses = (initial: string, writes: string, message: string) =>
      expect(() =>
        compile(
          appOf(
            `const [s, setS] = useState(${initial}); const [n, setN] = useState(0);`,
            `<Pressable onPress={() => {${writes}}} />`,
          ),
        ),
      ).toThrow(message);
    refuses(
      "{kind: 'a'}",
      "setS({kind: 'b', x: n});",
      '"s.x" is never written with a constant',
    );
    refuses(
      "{kind: 'a'}",
      "setS({kind: 'b', x: 1}); setS({kind: 'c', x: 'one'});",
      '"s.x" holds a number in one place and a string in another',
    );
    refuses(
      "{kind: 'a', pos: 1}",
      "setS({kind: 'b', pos: {x: 1}});",
      '"s.pos" is an object in one shape and a value in another',
    );
    refuses(
      "{kind: 'a', x: 1}",
      "setS({kind: 'a', y: 1});",
      'state "s" is written with objects of different fields',
    );
    refuses(
      "{kind: 'a'}",
      "setS({kind: 'b', x: null});",
      '"s.x" is written with null',
    );
  });
});
