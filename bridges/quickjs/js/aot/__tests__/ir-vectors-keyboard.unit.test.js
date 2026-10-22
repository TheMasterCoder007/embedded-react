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
import {printKeyboard} from '../compiler/c/keyboard.mts';
import {emitNode} from '../compiler/nodes.mts';
import {lowerStmts} from '../compiler/handlers.mts';
import {printStmts} from '../compiler/c/statements.mts';
import {
  printStateDrivenVector,
  printTapeValue,
  printVectorBlocks,
  printVectorRebuild,
  printVectorTables,
} from '../compiler/c/vectors.mts';
import {printElementTree} from '../compiler/c/nodes.mts';
import {stateDrivenVectorOf} from '../compiler/ir/nodes.mts';
import {Out} from '../compiler/out.mts';

// Keyboard configs and baked <Svg> drawings lower to IR data; these check it and the const tables it prints.

/** The keyboard config a module declares. */
const keyboardOf = source =>
  lowerKeyboardConfig(parse(source, {sourceType: 'module'}).program);

/** One int state slot, `n`, and one boolean, `on`. */
const scalar = (name, isBool) => ({
  kind: 'scalar',
  name,
  cType: 'int',
  isBool,
  cField: name,
  cMember: `s_state.${name}`,
});
const stateMap = new Map([
  ['n', scalar('n', false)],
  ['on', scalar('on', true)],
]);

/** Emits one JSX element over the state above, returning what it lowered to and the output. */
function emit(jsx, refs = new Map()) {
  const expr = parse(`(${jsx})`, {
    sourceType: 'module',
    plugins: ['jsx'],
  }).program.body[0].expression;
  const env = {
    state: stateMap,
    locals: new Map(),
    consts: {},
    refs,
    animations: new Map(),
    math64: false,
    caps: {listCap: 8, listStrCap: 48, maxTextSpans: 4},
    stores: [],
  };
  const out = new Out(new Map(), null);
  const element = emitNode(expr, {}, out, env, {
    byName: stateMap,
    bySetter: new Map(),
  });
  return {element, out, build: printElementTree(element)};
}

describe('keyboard IR', () => {
  it('lowers each key kind with its defaults', () => {
    const keyboard = keyboardOf(`setKeyboardConfig({
      keyColor: '#333',
      gridCols: 10,
      layers: [[[{char: 'q'}, {char: ' ', span: 4}, {label: 'shift', layer: 1, highlight: true}, {backspace: true}, {done: true}]]],
    });`);
    expect(keyboard.layers[0][0]).toEqual([
      {
        kind: 'char',
        label: 'q',
        text: 'q',
        layer: 0,
        span: 1,
        highlightLayer: null,
      },
      {
        kind: 'char',
        label: null,
        text: ' ',
        layer: 0,
        span: 4,
        highlightLayer: null,
      },
      {
        kind: 'layer',
        label: 'shift',
        text: null,
        layer: 1,
        span: 1,
        highlightLayer: 0,
      },
      {
        kind: 'backspace',
        label: '<',
        text: null,
        layer: 0,
        span: 1,
        highlightLayer: null,
      },
      {
        kind: 'done',
        label: 'OK',
        text: null,
        layer: 0,
        span: 1,
        highlightLayer: null,
      },
    ]);
    expect(keyboard).toMatchObject({
      gridCols: 10,
      rowHeight: null,
      keyColor: 0xff333333,
      panelColor: null,
    });

    const {data, setup} = printKeyboard(keyboard);
    expect(data).toContain(
      '{ "shift", NULL, ER_KBD_KEY_LAYER, 1, 1, 0 }, { "<", NULL, ER_KBD_KEY_BACKSPACE, 0, 1, 255 }',
    );
    expect(data).toContain(
      'static const ERKeyboardConfig kbd_cfg = { kbd_layers, 1, 10, 0, 0, 0, 0, 0, 0xFF333333u, 0, 0 };',
    );
    expect(setup).toBe('    er_keyboard_set_config(&kbd_cfg);');
  });

  it('keeps the built-in layout when the config gives none, and is null without a config', () => {
    const keyboard = keyboardOf(`setKeyboardConfig({labelColor: 'white'});`);
    expect(keyboard.layers).toBeNull();
    expect(printKeyboard(keyboard).data).toBe(
      'static const ERKeyboardConfig kbd_cfg = { NULL, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFFFFFFFFu };',
    );
    expect(keyboardOf('const x = 1;')).toBeNull();
  });

  it('rejects a layout with no layers, or a layer with no rows, at the config', () => {
    let err;
    try {
      keyboardOf('setKeyboardConfig({layers: []});');
    } catch (e) {
      err = e;
    }
    expect(err.message).toBe(
      'AOT: setKeyboardConfig `layers` needs at least one layer',
    );
    expect(err.aotLoc).toMatchObject({line: 1, column: 18});
    expect(() =>
      keyboardOf('setKeyboardConfig({layers: [[[{char: "a"}]], []]});'),
    ).toThrow('AOT: setKeyboardConfig `layers[1]` needs at least one row');
  });
});

describe('vector IR: baked drawings', () => {
  it('bakes a static <Svg> into tables its box node is wired to', () => {
    const {element, out, build} = emit(
      '<Svg width={40} height={20} style={{margin: 4}}><Circle cx={10} cy={10} r={5} fill="#f00" /></Svg>',
    );
    const node = element.node;
    expect(node).toMatchObject({
      type: 'ER_NODE_VECTOR',
      constantProps: [
        {field: 'width', expr: '(int16_t)40'},
        {field: 'height', expr: '(int16_t)20'},
        {field: 'margin', expr: '4'},
      ],
    });
    const wire = node.wiring.find(wiring => wiring.kind === 'vectorTables');
    expect(wire.tables).toMatchObject({id: 0, gradients: []});
    expect(wire.tables.paints).toEqual([
      expect.objectContaining({fill: 0xffff0000, fillGradient: 0}),
    ]);
    expect(out.vectors).toEqual([{kind: 'tables', tables: wire.tables}]);
    expect(printVectorBlocks(out.vectors)).toEqual({
      data: printVectorTables(wire.tables).join('\n\n'),
      builders: '',
    });
    expect(build.at(-1)).toBe(
      `    er_node_set_vector_ops(n0, s_svg0_ops, ${wire.tables.ops.length}, s_svg0_paints, 1, NULL, 0);`,
    );
  });

  it('leaves an empty drawing without tables', () => {
    const {element, out, build} = emit('<Svg width={10} height={10} />');
    expect(printVectorBlocks(out.vectors)).toEqual({data: '', builders: ''});
    expect(build.some(line => line.includes('er_node_set_vector_ops'))).toBe(
      false,
    );
    expect(element.node.wiring).toEqual([
      expect.objectContaining({kind: 'vectorTables'}),
    ]);
  });

  it('prints a gradient table when the drawing has gradients', () => {
    const declarations = printVectorTables({
      id: 3,
      ops: [0, 1, 2],
      paints: [
        {
          fill: 0,
          stroke: 0,
          strokeWidth: 1,
          miter: 4,
          cap: 0,
          join: 0,
          fillRule: 0,
          fillGradient: 1,
          strokeGradient: 0,
        },
      ],
      gradients: [
        {
          type: 1,
          stops: [
            {offset: 0, color: 0xff0000ff},
            {offset: 1, color: 0xffff0000},
          ],
          ax: 0,
          ay: 0,
          bx: 10,
          by: 0,
        },
      ],
    });
    expect(declarations).toHaveLength(3);
    expect(declarations[2]).toContain(
      '{ .type = 1, .stop_count = 2, .stops = { { 4278190335u, 0.0f }, { 4294901760u, 1.0f } }',
    );
  });
});

describe('vector IR: state-driven drawings', () => {
  /** The state-driven drawing an <Svg> lowered to. */
  const drawingOf = element =>
    element.node.wiring.find(wire => wire.kind === 'stateDrivenVector').vector;

  it('lowers geometry to tape slots, binding each state-driven corner radius to a local', () => {
    const {element, out} = emit(
      '<Svg width={60} height={40}><Rect x={n} y={4} width={50} height={30} rx={n / 2} /></Svg>',
    );
    const vector = drawingOf(element);
    expect(vector.tape.slice(0, 2)).toEqual([
      {kind: 'op', op: 'ER_VOP_SHAPE'},
      {kind: 'number', value: 0},
    ]);
    // Without ry, the vertical radius takes rx's value, so both are clamped locals.
    expect(vector.locals).toEqual([
      {name: 'rx_s0', value: expect.objectContaining({kind: 'clampRadius'})},
      {name: 'ry_s0', value: expect.objectContaining({kind: 'clampRadius'})},
    ]);

    // The corner's x: the inner right edge plus the radius's KAPPA-length handle.
    expect(printTapeValue(vector.tape[9])).toBe(
      '(((float)(s_state.n) + 50.0f - rx_s0) + (rx_s0 * 0.5522847498307936f))',
    );
    expect(out.vectors).toEqual([{kind: 'drawing', vector}]);
    expect(printVectorBlocks(out.vectors).builders).toContain(
      '    const float rx_s0 = fminf(fmaxf(',
    );
    expect(stateDrivenVectorOf(element.node)).toBe(vector);
    expect(printVectorRebuild('n0', vector)).toEqual([
      '    build_svg0();',
      `    er_node_set_vector_ops(s_n0, s_svg0_ops, ${vector.tape.length}, s_svg0_paints, 1, NULL, 0);`,
    ]);
  });

  it('folds a corner radius that is known at build time', () => {
    const {element} = emit(
      '<Svg width={60} height={40}><Rect x={n} y={0} width={10} height={10} rx={30} /></Svg>',
    );
    const vector = drawingOf(element);
    expect(vector.locals).toEqual([]);
    expect(vector.tape.map(printTapeValue)).toContain(
      '((float)(s_state.n) + 5.0f)',
    );
  });

  it('keeps paints const until state drives one, then rebuilds them', () => {
    const {element: still} = emit(
      '<Svg width={10} height={10}><Circle cx={n} cy={5} r={2} fill="#f00" /></Svg>',
    );
    const stillVector = drawingOf(still);
    expect(stillVector.paints[0].fill).toEqual({
      kind: 'static',
      argb: 0xffff0000,
    });
    expect(printStateDrivenVector(stillVector).data[1]).toMatch(
      /^static const ERVectorPaint s_svg0_paints\[\] = \{/,
    );

    const {element: lit} = emit(
      `<Svg width={10} height={10}><Circle cx={5} cy={5} r={2} fill={on ? '#f00' : '#00f'} /></Svg>`,
    );
    const litVector = drawingOf(lit);
    expect(litVector.paints[0].fill).toMatchObject({
      kind: 'state',
      value: {kind: 'select'},
    });
    const printed = printStateDrivenVector(litVector);
    expect(printed.data[1]).toBe('static ERVectorPaint s_svg0_paints[1];');
    expect(printed.builder).toContain(
      '    s_svg0_paints[0].fill = ((s_state.on) ? 0xFFFF0000u : 0xFF0000FFu);',
    );
  });

  it('gives each gradient a table place, used only while its condition holds', () => {
    const {element} = emit(
      `<Svg width={10} height={10}><Circle cx={n} cy={5} r={2}
        fillGrad={{type: 1, bx: n, stops: [{color: '#f00', offset: 0}, {color: '#00f', offset: 1}]}}
        strokeGrad={on ? null : {type: 2, r: 4, stops: [{color: '#fff', offset: 0}, {color: '#000', offset: 1}]}} /></Svg>`,
    );
    const vector = drawingOf(element);
    expect(vector.gradients.map(gradient => gradient.type)).toEqual([1, 2]);
    expect(vector.gradients[0].bx.kind).toBe('float');
    expect(vector.paints[0].fillGradient).toEqual({index: 1, when: null});
    expect(vector.paints[0].strokeGradient).toMatchObject({
      index: 2,
      when: {negate: true, test: {kind: 'state'}},
    });

    const {data, builder} = printStateDrivenVector(vector);
    expect(data).toContain('static ERVectorGradient s_svg0_grads[2];');
    expect(builder).toContain(
      '    s_svg0_paints[0].stroke_grad = ((!(s_state.on)) ? 2 : 0);',
    );
    expect(builder).toContain('    s_svg0_grads[0].bx = (float)(s_state.n);');
  });
});

describe('vector IR: updateVector', () => {
  /** Lowers a handler body that can update the node ref `art`, inside a touch handler's event `e`. */
  function lowerHandler(source) {
    const statements = parse(source, {sourceType: 'module'}).program.body;
    const env = {
      state: stateMap,
      locals: new Map(),
      consts: {},
      refs: new Map([['art', {kind: 'node', cVar: 's_ref_art', used: false}]]),
      event: 'e',
      math64: false,
      caps: {listCap: 8, listStrCap: 48, maxTextSpans: 4},
      stores: [],
    };
    const ctx = {
      stateChanged: false,
      animIdx: 0,
      out: new Out(new Map(), null),
    };
    return {
      stmts: lowerStmts(
        statements,
        env,
        {byName: stateMap, bySetter: new Map()},
        ctx,
      ),
      ctx,
    };
  }

  it('lowers the shapes to tape slots on the same geometry as the JSX path', () => {
    const {stmts, ctx} = lowerHandler(
      "updateVector(art, [{rect: [e.x, 0, 30, 20, n, 4], fill: '#f00'}], [0, 0, 40, n]);",
    );
    const [update] = stmts;
    expect(update).toMatchObject({
      kind: 'updateVector',
      id: 0,
      node: 's_ref_art',
      locals: [{name: 'rx_uv0_0', value: {kind: 'clampRadius'}}],
      paints: [expect.objectContaining({fill: 0xffff0000})],
    });
    expect(update.tape.slice(0, 4)).toEqual([
      {kind: 'op', op: 'ER_VOP_SHAPE'},
      {kind: 'number', value: 0},
      {kind: 'op', op: 'ER_VOP_MOVE'},
      expect.objectContaining({kind: 'chain'}),
    ]);
    expect(update.dirty.map(edge => edge.kind)).toEqual([
      'number',
      'number',
      'number',
      'state',
    ]);

    // The tables are file-scope; the statement prints at its own indent and hands the tape to the node.
    expect(ctx.out.vectors).toEqual([{kind: 'updateVector', update}]);
    expect(printVectorBlocks(ctx.out.vectors).data).toMatch(
      new RegExp(`^static float s_uv0_ops\\[${update.tape.length}\\];`),
    );
    const lines = printStmts(stmts, '        ');
    expect(lines[0]).toMatch(/^ {8}const float rx_uv0_0 = fminf\(/);
    expect(lines.at(-1)).toBe(
      '        er_node_set_vector_dirty_rect(s_ref_art, 0, 0, 40, s_state.n);',
    );
  });

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

  it('rejects an empty slot in the dirty rect', () => {
    expect(() =>
      lowerHandler('updateVector(art, [{circle: [1, 2, 3]}], [0, , 40, 40]);'),
    ).toThrow('AOT: updateVector dirtyRect has an empty slot at position 1');
  });
});
