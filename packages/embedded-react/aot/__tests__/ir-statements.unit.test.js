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

import {Out} from '../compiler/out.mts';

// Handler statements lower to the IR first; these check the statements.

/** A scalar state slot, as collectState records one. */
const scalar = (name, slotKind, isBool = false) => ({
  kind: 'scalar',
  name,
  slotKind,
  isBoolean: isBool,
  slotName: name,
});

/** A list state slot of up to 8 items. */
const list = (name, fields) => ({
  kind: 'list',
  name,
  struct: {fields},
  capacity: 8,
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
  const environment = {
    state: state.byName,
    locals: new Map(),
    constants: {},
    refs,
    math64: undefined,
    caps: {listCap: 8, listStrCap: 48, maxTextSpans: 4},
    stores: [],
  };
  const context = {stateChanged: false, output: new Out(new Map(), null)};
  return {
    statements: lowerStmts(statements, environment, state, context),
    context,
  };
}

describe('handler statements lower to the IR', () => {
  it('lowers list appends, slices and clears', () => {
    const fields = [
      {key: 'name', kind: 'string'},
      {key: 'score', kind: 'int'},
    ];
    const {statements} = lowerBody(
      'setItems([...items, {name: label, score: count}]);' +
        'setItems(items.slice(0, -1));' +
        'setItems([]);',
      stateOf(
        list('items', fields),
        scalar('label', 'string'),
        scalar('count', 'int'),
      ),
    );
    expect(statements.map(statement => statement.kind)).toEqual([
      'appendItem',
      'dropLastItem',
      'clearList',
    ]);
    expect(statements[0].values).toMatchObject([
      {field: {key: 'name'}, text: {parts: [{kind: 'value'}]}},
      {field: {key: 'score'}, value: {kind: 'state'}},
    ]);
  });

  it('refuses an empty slot in a list append', () => {
    expect(() =>
      lowerBody(
        "setItems([...items, , {name: 'b'}]);",
        stateOf(list('items', [{key: 'name', kind: 'string'}])),
      ),
    ).toThrow('AOT: a list literal has an empty slot');
  });

  it('refuses a statement it cannot lower', () => {
    expect(() => lowerBody('try {} catch {}', stateOf())).toThrow(
      /unsupported statement "TryStatement"/,
    );
  });
});
