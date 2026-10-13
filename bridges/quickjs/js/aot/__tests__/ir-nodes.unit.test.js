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
import {emitNode} from '../compiler/nodes.mts';
import {printNode, printPropValue} from '../compiler/c/nodes.mts';
import {Out} from '../compiler/out.mts';

// Host elements lower to IR nodes whose state-driven props are typed values; these check both and their C.

/** A scalar state slot. */
const scalar = (name, cType, isBool = false) => ({
  kind: 'scalar',
  name,
  cType,
  isBool,
  cField: name,
  cMember: `s_state.${name}`,
});

const slots = [scalar('n', 'int'), scalar('on', 'int', true)];
const state = {
  byName: new Map(slots.map(slot => [slot.name, slot])),
  bySetter: new Map([['setN', slots[0]]]),
};

/** Emits one JSX element over the state above, returning what it added to the output. */
function emit(jsx, refs = new Map()) {
  const expr = parse(`(${jsx})`, {
    sourceType: 'module',
    plugins: ['jsx'],
  }).program.body[0].expression;
  const env = {
    state: state.byName,
    locals: new Map(),
    consts: {},
    refs,
    animations: new Map(),
    math64: false,
    caps: {listCap: 8, listStrCap: 48, maxTextSpans: 4},
    found: new Set(),
  };
  const out = new Out(new Map(), null);
  const nodeId = emitNode(expr, {}, out, env, state);
  return {nodeId, out};
}

/** A state-driven prop of the update entry for `nodeId`, by field. */
const stateProp = (out, nodeId, field) =>
  out.updates
    .find(update => update.nodeId === nodeId)
    .dynAssigns.find(assign => assign.field === field);

describe('node IR: state-driven style values', () => {
  it('lowers each kind of style value to a typed value', () => {
    const {nodeId, out} = emit(
      `<View style={{backgroundColor: on ? '#ff0000' : '#00ff00', width: n * 2, opacity: n / 10, flexDirection: on ? 'row' : 'column'}} />`,
    );
    const color = stateProp(out, nodeId, 'background_color').value;
    expect(color).toMatchObject({
      kind: 'select',
      test: {kind: 'state'},
      whenTrue: {kind: 'constant', code: '0xFFFF0000u'},
      whenFalse: {kind: 'constant', code: '0xFF00FF00u'},
    });
    expect(printPropValue(color)).toBe(
      '((s_state.on) ? 0xFFFF0000u : 0xFF00FF00u)',
    );
    expect(stateProp(out, nodeId, 'width').value.kind).toBe('dimension');
    expect(stateProp(out, nodeId, 'opacity').value.kind).toBe('opacity');
    expect(printPropValue(stateProp(out, nodeId, 'flex_direction').value)).toBe(
      '((s_state.on) ? ER_FLEX_ROW : ER_FLEX_COL)',
    );
  });

  it('lowers a state-driven `visible` to a visible value', () => {
    const {nodeId, out} = emit('<View visible={n > 2} />');
    const display = stateProp(out, nodeId, 'display').value;
    expect(display).toMatchObject({kind: 'visible', value: {kind: 'compare'}});
    expect(printPropValue(display)).toBe(
      '(((s_state.n > 2)) ? ER_DISPLAY_FLEX : ER_DISPLAY_NONE)',
    );
  });

  it('sets a node with no state-driven content once, at build time', () => {
    const {out} = emit('<View style={{width: 10}} />');
    expect(out.updates).toEqual([]);
    expect(out.build).toEqual([
      '    n0 = er_node_create(ER_NODE_VIEW);',
      '    er_props_default(&p);',
      '    p.width = 10;',
      '    er_node_set_props(n0, &p);',
    ]);
  });
});

describe('node IR: conditional children', () => {
  it('shows a state-driven ternary branch by its test, and the else branch by its negation', () => {
    const {out} = emit(
      '<View>{on ? <View style={{width: 1}} /> : <View style={{width: 2}} />}</View>',
    );
    const [whenTrue, whenFalse] = out.updates.map(
      update =>
        update.dynAssigns.find(assign => assign.field === 'display').value,
    );
    expect(whenTrue).toMatchObject({
      kind: 'shown',
      show: {kind: 'condition', negate: false, test: {kind: 'state'}},
    });
    expect(whenFalse.show.negate).toBe(true);
    expect(whenFalse.show.test).toBe(whenTrue.show.test);
    expect(printPropValue(whenFalse)).toBe(
      '((!(s_state.on)) ? ER_DISPLAY_FLEX : ER_DISPLAY_NONE)',
    );
  });
});

describe('node IR: printing', () => {
  it('prints a state-driven node as a handle and an app_update entry, then its wiring in order', () => {
    const printed = printNode({
      id: 'n3',
      type: 'ER_NODE_PRESSABLE',
      constantProps: [{field: 'width', expr: '40'}],
      stateProps: [{field: 'opacity', value: {kind: 'constant', code: '255'}}],
      placeholder: null,
      text: null,
      image: null,
      spans: null,
      wiring: [
        {
          kind: 'bind',
          bind: {cVar: 's_av_a', prop: 'ER_PROP_OPACITY'},
          index: 0,
        },
        {kind: 'ref', slot: 's_ref_box'},
        {kind: 'event', event: 'ER_EVENT_PRESS', handler: 'er_handler_0'},
      ],
    });
    expect(printed.build).toEqual([
      '    n3 = er_node_create(ER_NODE_PRESSABLE);',
      '    s_n3 = n3;',
      '    er_anim_value_bind(s_av_a, n3, ER_PROP_OPACITY);',
      '    s_ref_box = n3;',
      '    er_event_set(n3, ER_EVENT_PRESS, er_handler_0, NULL);',
    ]);
    expect(printed.update).toMatchObject({
      nodeId: 'n3',
      styleAssigns: [{field: 'width', expr: '40'}],
      imageName: null,
    });
  });
});

describe('node IR: typed elements', () => {
  it('lowers element values to typed flags, bytes and floats', () => {
    const {nodeId: switchId, out: switchOut} = emit(
      '<Switch value={on} onValueChange={v => setN(1)} />',
    );
    const switchValue = stateProp(switchOut, switchId, 'switch_value').value;
    expect(switchValue).toMatchObject({
      kind: 'flag',
      truthiness: 'condition',
      negate: false,
      whenTrue: 1,
    });
    expect(printPropValue(switchValue)).toBe('(uint8_t)((s_state.on) ? 1 : 0)');

    const {nodeId, out} = emit(
      `<Dial min={n} cap={on ? 'round' : 'butt'} indicatorGradient={on ? null : {stops: [{color: '#00f'}, {color: '#f00'}]}} />`,
    );
    expect(stateProp(out, nodeId, 'arc_min').value.kind).toBe('float');
    expect(stateProp(out, nodeId, 'arc_cap').value).toMatchObject({
      kind: 'byte',
      value: {kind: 'select'},
    });
    const stops = stateProp(out, nodeId, 'gradient_stop_count').value;
    expect(stops).toMatchObject({
      kind: 'flag',
      truthiness: 'value',
      negate: true,
      whenTrue: 2,
    });
    expect(printPropValue(stops)).toBe('(uint8_t)((!(s_state.on)) ? 2 : 0)');
  });

  it("re-sets a Dial's knob image on every update", () => {
    const {nodeId, out} = emit('<Dial knobImage="knob" min={n} />');
    expect(out.updates.find(u => u.nodeId === nodeId).imageName).toBe('"knob"');
  });

  it("prints a TextInput's placeholder before its text, and wires its event before its ref", () => {
    const refs = new Map([
      ['box', {kind: 'node', cVar: 's_ref_box', used: false}],
    ]);
    const {out} = emit(
      '<TextInput ref={box} value="hi" placeholder="name" onChangeText={t => setN(2)} />',
      refs,
    );
    expect(out.build).toEqual([
      '    n0 = er_node_create(ER_NODE_TEXT_INPUT);',
      '    er_props_default(&p);',
      '    snprintf(p.placeholder, sizeof(p.placeholder), "%s", "name");',
      '    snprintf(p.text, sizeof(p.text), "%s", "hi");',
      '    er_node_set_props(n0, &p);',
      '    er_event_set(n0, ER_EVENT_CHANGE_TEXT, er_handler_0, NULL);',
      '    s_ref_box = n0;',
    ]);
    expect(refs.get('box').used).toBe(true);
  });
});
