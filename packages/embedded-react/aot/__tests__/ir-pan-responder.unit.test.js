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
import {
  collectPanResponders,
  lowerPanResponder,
} from '../compiler/pan-responder.mts';

import {Out} from '../compiler/out.mts';

// A PanResponder lowers to the IR first: gesture fields, event handlers with their roles, and should-set queries.

/** One int state slot, `n`, with its setter. */
const nState = {
  kind: 'scalar',
  name: 'n',
  slotKind: 'int',
  isBoolean: false,
  slotName: 'n',
};
const state = {
  byName: new Map([['n', nState]]),
  bySetter: new Map([['setN', nState]]),
};

const environment = {
  state: state.byName,
  locals: new Map(),
  constants: {},
  refs: new Map(),
  math64: undefined,
  caps: {listCap: 8, listStrCap: 48, maxTextSpans: 4},
  stores: [],
};

/** The responder `pan` a component body declares with this config, given gesture id 1. */
function responder(config) {
  const source = `function App() { const pan = useRef(PanResponder.create(${config})).current; }`;
  const functionNode = parse(source, {sourceType: 'module'}).program.body[0];
  const pan = collectPanResponders(functionNode.body).get('pan');
  pan.stateID = 1;
  return pan;
}

/** Lowers the responder `pan` with this config. */
function lowerConfig(config) {
  const output = new Out(new Map(), null);
  return {
    lowered: lowerPanResponder(responder(config), environment, state, output),
    output,
  };
}

describe('PanResponder IR: gesture fields', () => {
  it('rejects a field a gestureState does not carry', () => {
    expect(() =>
      lowerConfig('{onStartShouldSetPanResponder: (e, g) => g.speed > 1}'),
    ).toThrow(/unknown gestureState field "speed"/);
  });
});

describe('PanResponder IR: events and queries', () => {
  it('always keeps grant, release and terminate, and adds move and reject only when given', () => {
    const {lowered} = lowerConfig('{onPanResponderRelease: () => setN(1)}');
    expect(
      lowered.events.map(event => [event.role, event.callback !== null]),
    ).toEqual([
      ['grant', false],
      ['release', true],
      ['terminate', false],
    ]);

    const {lowered: full} = lowerConfig(
      '{onPanResponderMove: () => setN(2), onPanResponderReject: () => setN(3)}',
    );
    expect(full.events.map(event => event.role)).toEqual([
      'grant',
      'release',
      'terminate',
      'move',
      'reject',
    ]);
  });
});
