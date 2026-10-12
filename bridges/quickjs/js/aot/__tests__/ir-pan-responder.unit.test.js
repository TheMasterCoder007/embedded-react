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
  emitPanResponder,
  lowerPanResponder,
} from '../compiler/pan-responder.mts';
import {printExpr} from '../compiler/c/expressions.mts';
import {printPanEvent, printPanQuery} from '../compiler/c/pan-responder.mts';
import {printPanWiring} from '../compiler/c/nodes.mts';
import {Out} from '../compiler/out.mts';

// A PanResponder lowers to the IR first: gesture fields, event handlers with their roles, and should-set queries.

/** One int state slot, `n`, with its setter. */
const nState = {
  kind: 'scalar',
  name: 'n',
  cType: 'int',
  isBool: false,
  cField: 'n',
  cMember: 's_state.n',
};
const state = {
  byName: new Map([['n', nState]]),
  bySetter: new Map([['setN', nState]]),
};

const env = {
  state: state.byName,
  locals: new Map(),
  consts: {},
  refs: new Map(),
  math64: false,
  caps: {listCap: 8, listStrCap: 48, maxTextSpans: 4},
  found: new Set(),
};

/** The responder `pan` a component body declares with this config, given gesture id 1. */
function responder(config) {
  const source = `function App() { const pan = useRef(PanResponder.create(${config})).current; }`;
  const fn = parse(source, {sourceType: 'module'}).program.body[0];
  const pan = collectPanResponders(fn.body).get('pan');
  pan.stateID = 1;
  return pan;
}

/** Lowers the responder `pan` with this config. */
function lowerConfig(config) {
  const out = new Out(new Map(), null);
  return {
    lowered: lowerPanResponder(responder(config), env, state, out),
    out,
  };
}

describe('PanResponder IR: gesture fields', () => {
  it('lowers each gestureState field to a gesture node the printer reads from the right place', () => {
    const {lowered} = lowerConfig(
      '{onStartShouldSetPanResponder: (e, g) => g.dx + g.dy + g.moveX + g.moveY + g.x0 + g.y0 + g.numberActiveTouches + g.stateID > g.vx + g.vy}',
    );
    const test = lowered.queries[0].test;
    const fields = [];
    const visit = node => {
      if (node && typeof node === 'object') {
        if (node.kind === 'gesture') {
          fields.push([node.field, node.cType, printExpr(node)]);
        }
        Object.values(node).forEach(visit);
      }
    };
    visit(test);
    expect(fields).toEqual([
      ['dx', 'int', '(data->dx - s_pan_pan_base_dx)'],
      ['dy', 'int', '(data->dy - s_pan_pan_base_dy)'],
      ['moveX', 'int', 'data->x'],
      ['moveY', 'int', 'data->y'],
      ['x0', 'int', 's_pan_pan_x0'],
      ['y0', 'int', 's_pan_pan_y0'],
      ['numberActiveTouches', 'int', 'er_touch_active_count()'],
      ['stateID', 'int', '1'],
      ['vx', 'float', 'data->vx'],
      ['vy', 'float', 'data->vy'],
    ]);
  });

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

  it('lowers a callback body to statements that end by re-applying state', () => {
    const {lowered} = lowerConfig('{onPanResponderMove: (e, g) => setN(g.dx)}');
    const move = lowered.events.find(event => event.role === 'move');
    expect(move.callback.updatesState).toBe(true);
    expect(move.callback.body).toMatchObject([
      {kind: 'setState', state: nState, value: {kind: 'gesture', field: 'dx'}},
    ]);
    expect(printPanEvent(lowered, move)).toEqual({
      name: 'er_pan_pan_move',
      body: [
        '    if (!s_pan_pan_granted)',
        '    {',
        '        return;',
        '    }',
        '    s_state.n = (data->dx - s_pan_pan_base_dx);',
        '    app_update();',
      ],
    });
  });

  it('wraps each role in its gesture bookkeeping', () => {
    const {lowered} = lowerConfig('{onPanResponderReject: () => setN(3)}');
    const [grant, release, , reject] = lowered.events.map(event =>
      printPanEvent(lowered, event),
    );
    expect(grant.body).toContain('    s_pan_pan_base_dx = data->dx;');
    expect(release.body.slice(-3)).toEqual([
      '    s_pan_pan_granted = 0;',
      '    s_pan_pan_base_dx = 0;',
      '    s_pan_pan_base_dy = 0;',
    ]);

    // A refused node never owned the gesture, so its handler has no guard.
    expect(reject.body).toEqual(['    s_state.n = 3;', '    app_update();']);
  });

  it('lowers a should-set predicate to one condition', () => {
    const {lowered} = lowerConfig(
      '{onMoveShouldSetPanResponderCapture: (e, g) => g.dy > 8}',
    );
    expect(lowered.queries).toMatchObject([
      {query: 'ER_QUERY_MOVE_SHOULD_SET_CAPTURE', test: {kind: 'compare'}},
    ]);
    expect(printPanQuery(lowered, lowered.queries[0])).toEqual({
      name: 'er_pan_pan_move_should_set_capture',
      expr: '(((data->dy - s_pan_pan_base_dy) > 8)) != 0',
    });
  });
});

describe('PanResponder IR: emitting', () => {
  it('emits the functions once however many nodes spread the responder, and wires a node to them', () => {
    const pan = responder(
      '{onStartShouldSetPanResponder: () => true, onPanResponderMove: () => setN(1)}',
    );
    const out = new Out(new Map(), null);
    emitPanResponder(pan, out, env, state);
    emitPanResponder(pan, out, env, state);
    expect(out.handlers.map(handler => handler.name)).toEqual([
      'er_pan_pan_grant',
      'er_pan_pan_release',
      'er_pan_pan_terminate',
      'er_pan_pan_move',
    ]);
    expect(out.queries.map(query => query.name)).toEqual([
      'er_pan_pan_start_should_set',
    ]);
    expect(out.panDecls).toContain('static int s_pan_pan_granted = 0;');
    expect(printPanWiring(pan, 'n2')).toEqual([
      '    er_event_set(n2, ER_EVENT_RESPONDER_GRANT, er_pan_pan_grant, NULL);',
      '    er_event_set(n2, ER_EVENT_RESPONDER_RELEASE, er_pan_pan_release, NULL);',
      '    er_event_set(n2, ER_EVENT_RESPONDER_TERMINATE, er_pan_pan_terminate, NULL);',
      '    er_event_set(n2, ER_EVENT_RESPONDER_MOVE, er_pan_pan_move, NULL);',
      '    er_responder_query_set(n2, ER_QUERY_START_SHOULD_SET, er_pan_pan_start_should_set, NULL);',
    ]);
  });
});
