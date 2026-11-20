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
import {lowerKeyboardConfig} from '../compiler/keyboard.mts';

import {lowerStmts} from '../compiler/handlers.mts';

import {Out} from '../compiler/out.mts';

// Keyboard configs and baked <Svg> drawings lower to IR data; these check it.

/** The keyboard config a module declares. */
const keyboardOf = source =>
  lowerKeyboardConfig(parse(source, {sourceType: 'module'}).program);

/** One int state slot, `n`, and one boolean, `on`. */
const scalar = (name, isBool) => ({
  kind: 'scalar',
  name,
  slotKind: 'int',
  isBoolean: isBool,
  slotName: name,
});
const stateMap = new Map([
  ['n', scalar('n', false)],
  ['on', scalar('on', true)],
]);

describe('keyboard IR', () => {
  it('rejects a layout with no layers, or a layer with no rows, at the config', () => {
    let error;
    try {
      keyboardOf('setKeyboardConfig({layers: []});');
    } catch (thrown) {
      error = thrown;
    }
    expect(error.message).toBe(
      'AOT: setKeyboardConfig `layers` needs at least one layer',
    );
    expect(error.aotLoc).toMatchObject({line: 1, column: 18});
    expect(() =>
      keyboardOf('setKeyboardConfig({layers: [[[{char: "a"}]], []]});'),
    ).toThrow('AOT: setKeyboardConfig `layers[1]` needs at least one row');
  });

  it('rejects a layer key switching to a layer that is not there', () => {
    expect(() =>
      keyboardOf(
        'setKeyboardConfig({layers: [[[{char: "a"}, {label: "up", layer: 1}]]]});',
      ),
    ).toThrow(
      'AOT: a setKeyboardConfig layer key switches to layer 1, but there are only 1',
    );
  });
});

describe('vector IR: updateVector', () => {
  /** Lowers a handler body that can update the node ref `art`, inside a touch handler's event `e`. */
  function lowerHandler(source) {
    const statements = parse(source, {sourceType: 'module'}).program.body;
    const environment = {
      state: stateMap,
      locals: new Map(),
      constants: {},
      refs: new Map([
        ['art', {kind: 'node', slotName: 'ref_art', used: false}],
      ]),
      event: 'e',
      math64: undefined,
      caps: {listCap: 8, listStrCap: 48, maxTextSpans: 4},
      stores: [],
    };
    const context = {
      stateChanged: false,
      output: new Out(new Map(), null),
    };
    return {
      stmts: lowerStmts(
        statements,
        environment,
        {byName: stateMap, bySetter: new Map()},
        context,
      ),
      context,
    };
  }

  it('rejects a shape array too short for its shape', () => {
    expect(() => lowerHandler('updateVector(art, [{arc: [1, 2]}]);')).toThrow(
      'AOT: updateVector "arc" needs 5 numbers (got 2)',
    );
    expect(() =>
      lowerHandler('updateVector(art, [{rect: [1, 2, 3]}]);'),
    ).toThrow('AOT: updateVector "rect" needs 4 numbers (got 3)');
  });

  it('rejects an empty slot, including in an optional rect radius', () => {
    expect(() =>
      lowerHandler('updateVector(art, [{arc: [1, , 3, 4, 5]}]);'),
    ).toThrow('AOT: updateVector "arc" has an empty slot at position 1');
    expect(() =>
      lowerHandler('updateVector(art, [{rect: [1, 2, 3, 4, , 6]}]);'),
    ).toThrow('AOT: updateVector "rect" has an empty slot at position 4');
  });

  it('dashes a shape after its header, with an offset state can drive', () => {
    const tapeOf = source =>
      lowerHandler(source).context.output.vectors[0].update.tape;
    const tape = tapeOf(
      "updateVector(art, [{line: [0, 0, 9, 0], stroke: '#fff', dash: [4, 2], dashOffset: n}]);",
    );
    expect(tape.slice(0, 5)).toEqual([
      {kind: 'op', operation: 'ER_VECTOR_OP_SHAPE'},
      {kind: 'number', value: 0},
      {kind: 'op', operation: 'ER_VECTOR_OP_DASH'},
      {kind: 'number', value: 4},
      {kind: 'number', value: 2},
    ]);
    expect(tape[5]).toEqual({
      kind: 'op',
      operation: 'ER_VECTOR_OP_DASH_OFFSET',
    });
    expect(tape[6].kind).toBe('float');
    expect(tape[7]).toEqual({kind: 'number', value: 0});
    expect(tape[8]).toEqual({kind: 'op', operation: 'ER_VECTOR_OP_MOVE'});

    // No offset writes none, and a dashOffset alone dashes nothing.
    const plain = tapeOf(
      "updateVector(art, [{line: [0, 0, 9, 0], dash: '4'}]);",
    );
    expect(plain[5]).toEqual({kind: 'op', operation: 'ER_VECTOR_OP_MOVE'});
    const solid = tapeOf(
      'updateVector(art, [{line: [0, 0, 9, 0], dashOffset: 3}]);',
    );
    expect(solid[2]).toEqual({kind: 'op', operation: 'ER_VECTOR_OP_MOVE'});
    expect(() =>
      lowerHandler('updateVector(art, [{line: [0, 0, 9, 0], dash: [n, 2]}]);'),
    ).toThrow('AOT: updateVector paint "dash" must be static');
  });

  it('rejects an empty slot in the dirty rect', () => {
    expect(() =>
      lowerHandler('updateVector(art, [{circle: [1, 2, 3]}], [0, , 40, 40]);'),
    ).toThrow('AOT: updateVector dirtyRect has an empty slot at position 1');
  });
});
