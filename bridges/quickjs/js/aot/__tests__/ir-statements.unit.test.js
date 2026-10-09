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
import {lowerStmts} from '../compiler/handlers.mts';
import {printStmts} from '../compiler/c/statements.mts';
import {Out} from '../compiler/out.mts';

// Handler statements lower to the IR first; these check the statements, and the C their printer makes of them.

/** A scalar state slot, as collectState records one. */
const scalar = (name, cType, isBool = false) => ({
  kind: 'scalar',
  name,
  cType,
  isBool,
  cField: name,
  cMember: `s_state.${name}`,
});

/** A list state slot of up to 8 items. */
const list = (name, fields) => ({
  kind: 'list',
  name,
  struct: {fields},
  cap: 8,
  arrayName: `s_${name}`,
  countMember: `s_${name}_count`,
});

/** A value ref. */
const valueRef = (cType, cVar) => ({
  kind: 'value',
  cVar,
  cType,
  initCode: '0',
  used: false,
});

/** A state table over these slots, each with its `setX` setter. */
const stateOf = (...slots) => ({
  byName: new Map(slots.map(slot => [slot.name, slot])),
  bySetter: new Map(
    slots.map(slot => [
      'set' + slot.name[0].toUpperCase() + slot.name.slice(1),
      slot,
    ]),
  ),
});

/** Lowers a handler body over this state (and refs), returning its statements and the context it filled. */
function lowerBody(source, state, refs = new Map()) {
  const statements = parse(source, {sourceType: 'module'}).program.body;
  const env = {
    state: state.byName,
    locals: new Map(),
    consts: {},
    refs,
    math64: false,
    caps: {listCap: 8, listStrCap: 48, maxTextSpans: 4},
    found: new Set(),
  };
  const ctx = {stateChanged: false, animIdx: 0, out: new Out(new Map(), null)};
  return {stmts: lowerStmts(statements, env, state, ctx), ctx};
}

describe('handler statements lower to the IR', () => {
  it('lowers a numeric setter to a state write, and notes that state changed', () => {
    const count = scalar('count', 'int');
    const {stmts, ctx} = lowerBody('setCount(count + 1);', stateOf(count));
    expect(stmts).toMatchObject([
      {kind: 'setState', state: count, value: {kind: 'helper', helper: 'add'}},
    ]);
    expect(ctx.stateChanged).toBe(true);
    expect(printStmts(stmts, '    ')).toEqual([
      '    s_state.count = app_add(s_state.count, 1);',
    ]);
  });

  it('lowers a string setter to text', () => {
    const {stmts} = lowerBody(
      "setLabel('n=' + count);",
      stateOf(scalar('label', 'string'), scalar('count', 'int')),
    );
    expect(stmts).toMatchObject([{kind: 'setStateText'}]);
    expect(printStmts(stmts, '    ')).toEqual([
      '    snprintf(s_state.label, sizeof(s_state.label), "n=%d", s_state.count);',
    ]);
  });

  it('writes through a temporary when the new text reads the state itself', () => {
    const {stmts} = lowerBody(
      "setLabel(label + '!');",
      stateOf(scalar('label', 'string')),
    );
    expect(printStmts(stmts, '    ')[0]).toContain(
      'char next[sizeof(s_state.label)];',
    );
  });

  it('declares a local that the statements after it read', () => {
    const {stmts} = lowerBody(
      'const doubled = count * 2; setCount(doubled);',
      stateOf(scalar('count', 'int')),
    );
    expect(stmts).toMatchObject([
      {kind: 'declareLocal', name: 'l_doubled', isHoisted: false},
      {kind: 'setState', value: {kind: 'local'}},
    ]);
    expect(printStmts(stmts, '    ')).toEqual([
      '    int l_doubled = app_mul(s_state.count, 2);',
      '    s_state.count = l_doubled;',
    ]);
  });

  it('lowers if / else to its test and both branches', () => {
    const {stmts} = lowerBody(
      'if (count > 3) { setCount(0); } else { setCount(count + 1); }',
      stateOf(scalar('count', 'int')),
    );
    expect(stmts).toMatchObject([
      {
        kind: 'if',
        test: {kind: 'compare', op: '>'},
        consequent: [{kind: 'setState'}],
        alternate: [{kind: 'setState'}],
      },
    ]);
    expect(printStmts(stmts, '    ')).toEqual([
      '    if ((s_state.count > 3))',
      '    {',
      '        s_state.count = 0;',
      '    }',
      '    else',
      '    {',
      '        s_state.count = app_add(s_state.count, 1);',
      '    }',
    ]);
  });

  it('lowers list appends, slices and clears', () => {
    const fields = [
      {key: 'name', kind: 'string'},
      {key: 'score', kind: 'int'},
    ];
    const {stmts} = lowerBody(
      'setItems([...items, {name: label, score: count}]);' +
        'setItems(items.slice(0, -1));' +
        'setItems([]);',
      stateOf(
        list('items', fields),
        scalar('label', 'string'),
        scalar('count', 'int'),
      ),
    );
    expect(stmts.map(stmt => stmt.kind)).toEqual([
      'appendItem',
      'dropLastItem',
      'clearList',
    ]);
    expect(stmts[0].values).toMatchObject([
      {field: {key: 'name'}, text: {parts: [{kind: 'value'}]}},
      {field: {key: 'score'}, value: {kind: 'state'}},
    ]);
  });

  it("prints list operations at the body's top-level indent, even inside a block", () => {
    const {stmts} = lowerBody(
      'if (on) { setItems([]); }',
      stateOf(list('items', []), scalar('on', 'int', true)),
    );
    expect(printStmts(stmts, '    ')).toEqual([
      '    if (s_state.on)',
      '    {',
      '    s_items_count = 0;',
      '    }',
    ]);
  });

  it('starts a timer whose callback becomes a function of its own', () => {
    const {stmts, ctx} = lowerBody(
      'setInterval(() => setCount(count + 1), 1000);',
      stateOf(scalar('count', 'int')),
    );
    expect(stmts).toMatchObject([
      {
        kind: 'startTimer',
        timer: {fn: 'er_timer_fn_0', isRepeating: true},
        idLocal: null,
      },
    ]);
    expect(ctx.out.timerFns[0].name).toBe('er_timer_fn_0');
    expect(printStmts(stmts, '    ')).toEqual([
      '    er_timer_add((int)(1000), true, er_timer_fn_0);',
    ]);
  });

  it('writes a whole-number ref through the saturating helper', () => {
    const refs = new Map([['total', valueRef('int', 's_ref_total')]]);
    const {stmts} = lowerBody('total.current += 2;', stateOf(), refs);
    expect(stmts).toMatchObject([
      {kind: 'writeRef', op: '=', value: {kind: 'helper', helper: 'add'}},
    ]);
    expect(printStmts(stmts, '    ')).toEqual([
      '    s_ref_total = app_add(s_ref_total, 2);',
    ]);
  });

  it('refuses a statement it cannot lower', () => {
    expect(() => lowerBody('for (;;) {}', stateOf())).toThrow(
      /unsupported statement "ForStatement"/,
    );
  });
});
