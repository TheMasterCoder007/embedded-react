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
import {printVectorTables} from '../compiler/c/vectors.mts';
import {Out} from '../compiler/out.mts';

// Keyboard configs and baked <Svg> drawings lower to IR data; these check it and the const tables it prints.

/** The keyboard config a module declares. */
const keyboardOf = source =>
  lowerKeyboardConfig(parse(source, {sourceType: 'module'}).program);

/** Emits one JSX element with no state, returning what it lowered to and the output. */
function emit(jsx, refs = new Map()) {
  const expr = parse(`(${jsx})`, {
    sourceType: 'module',
    plugins: ['jsx'],
  }).program.body[0].expression;
  const env = {
    state: new Map(),
    locals: new Map(),
    consts: {},
    refs,
    animations: new Map(),
    math64: false,
    caps: {listCap: 8, listStrCap: 48, maxTextSpans: 4},
    found: new Set(),
  };
  const out = new Out(new Map(), null);
  const element = emitNode(expr, {}, out, env, {
    byName: new Map(),
    bySetter: new Map(),
  });
  return {element, out};
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
});

describe('vector IR: baked drawings', () => {
  it('bakes a static <Svg> into tables its box node is wired to', () => {
    const {element, out} = emit(
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
    expect(out.vectorData).toEqual(printVectorTables(wire.tables));
    expect(out.build.at(-1)).toBe(
      `    er_node_set_vector_ops(n0, s_svg0_ops, ${wire.tables.ops.length}, s_svg0_paints, 1, NULL, 0);`,
    );
  });

  it('leaves an empty drawing without tables', () => {
    const {element, out} = emit('<Svg width={10} height={10} />');
    expect(out.vectorData).toEqual([]);
    expect(
      out.build.some(line => line.includes('er_node_set_vector_ops')),
    ).toBe(false);
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
