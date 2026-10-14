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
import {lowerApp} from '../compiler/lower.mts';
import {writeApp} from '../compiler/assemble.mts';
import {resolveOptions} from '../compiler/options.mts';
import {
  printEffectFunction,
  printHandler,
  printMountEffect,
  printStaticDecl,
} from '../compiler/c/functions.mts';
import {printKeyboard} from '../compiler/c/keyboard.mts';
import {printElementTree} from '../compiler/c/nodes.mts';
import {nodesOf} from '../compiler/ir/nodes.mts';
import {printPanDecls} from '../compiler/c/pan-responder.mts';
import {printVectorBlocks} from '../compiler/c/vectors.mts';

// An app lowers to one IrApp, which the C backend writes as app.gen.c and app.gen.h.

const opts = resolveOptions({});

/** The app a source lowers to. */
const lower = source => lowerApp(source, opts, new Map(), []);

describe('the lowered app', () => {
  it('keeps the keyboard config as IR, which the writer prints with its setup call', () => {
    const app = lower(
      "import {View, setKeyboardConfig} from 'embedded-react';\n" +
        "setKeyboardConfig({panelColor: '#333333', rowHeight: 30});\n" +
        'export function App() { return <View />; }',
    );
    expect(app.keyboard).toMatchObject({
      layers: null,
      rowHeight: 30,
      panelColor: 0xff333333,
    });

    const {c} = writeApp(app, 'probe', opts);
    expect(c).toContain(printKeyboard(app.keyboard).data);
    expect(c).toContain(
      '    er_keyboard_set_config(&kbd_cfg); /* app-supplied on-screen keyboard layout/appearance */',
    );
  });

  it('has no keyboard config when the module sets none', () => {
    const app = lower(
      "import {View} from 'embedded-react';\nexport function App() { return <View />; }",
    );
    expect(app.keyboard).toBeNull();
    expect(app.root.id).toBe('n0');
    expect(writeApp(app, 'probe', opts).c).not.toContain('kbd_cfg');
  });

  it('holds drawings and responders as IR, which the writer declares at file scope', () => {
    const app = lower(
      "import React, {useRef, useState} from 'react';\n" +
        "import {View, Svg, Circle, PanResponder} from 'embedded-react';\n" +
        'export function App() {\n' +
        '  const [n, setN] = useState(0);\n' +
        '  const pan = useRef(PanResponder.create({\n' +
        '    onStartShouldSetPanResponder: () => n > 2,\n' +
        '    onPanResponderMove: (e, g) => setN(g.dx),\n' +
        '  })).current;\n' +
        '  return (\n' +
        '    <View {...pan.panHandlers}>\n' +
        '      <Svg width={10} height={10}><Circle cx={5} cy={5} r={2} fill="#0f0" /></Svg>\n' +
        '      <Svg width={10} height={10}><Circle cx={n} cy={5} r={2} fill="#00f" /></Svg>\n' +
        '    </View>\n' +
        '  );\n' +
        '}',
    );
    const {out} = app;
    expect(out.vectors.map(vector => vector.kind)).toEqual([
      'tables',
      'drawing',
    ]);
    expect(out.panResponders).toMatchObject([
      {name: 'pan', fnPrefix: 'er_pan_pan', stateID: 1},
    ]);

    const {c} = writeApp(app, 'probe', opts);
    const {data, builders} = printVectorBlocks(out.vectors);
    expect(c).toContain(data);
    expect(c).toContain(builders);
    expect(c).toContain(printPanDecls(out.panResponders[0]).join('\n'));
    expect(c).toContain(
      'static bool er_pan_pan_start_should_set(ERNode* node, const EREventData* data, void* user_data)',
    );
  });
});

describe('the lowered app: functions', () => {
  /** An app module around a component body and the JSX it returns. */
  const app = (body, jsx) =>
    "import React, {useState, useEffect} from 'react';\n" +
    "import {View, Text, TouchableOpacity, Animated, useAnimatedValue} from 'embedded-react';\n" +
    `export function App() {\n${body}\n  return ${jsx};\n}`;

  it('wraps a press handler in its fade, and gives the unhandled end a fade of its own', () => {
    const {out} = lower(
      app(
        '  const [n, setN] = useState(0);',
        '<TouchableOpacity onPress={() => setN(1)} onPressIn={() => setN(2)}><Text>{n}</Text></TouchableOpacity>',
      ),
    );
    expect(out.handlers.map(handler => handler.kind)).toEqual([
      'callback',
      'callback',
      'pressFade',
      'pressFade',
    ]);
    expect(out.handlers[2]).toMatchObject({
      name: 'er_handler_2',
      fade: {value: 's_av_press_n0', opacity: 0.2, durationMs: 0},
      then: 'er_handler_1',
    });
    expect(out.handlers[3]).toMatchObject({
      fade: {opacity: 1, durationMs: 250},
      then: null,
    });
    expect(printHandler(out.handlers[2]).body.at(-1)).toBe(
      '    er_handler_1(node, data, user_data);',
    );
  });

  it('lowers a dep-driven effect with a cleanup to its functions, statics and mount run', () => {
    const {out} = lower(
      app(
        '  const [n, setN] = useState(0);\n' +
          '  useEffect(() => { const id = setInterval(() => setN(k => k + 1), 100); return () => clearInterval(id); }, [n]);',
        '<Text>{n}</Text>',
      ),
    );
    const [effect] = out.depEffects;
    expect(effect).toMatchObject({id: 0, hasCleanup: true});
    expect(effect.deps).toHaveLength(1);
    expect(out.effectFns).toEqual([
      {kind: 'cleanup', effect},
      {kind: 'effect', effect},
    ]);
    expect(out.effectDecls).toEqual([
      {kind: 'armed', effect},
      {kind: 'local', name: 's_eff0_l_id', cType: 'int'},
      {kind: 'deps', effect},
    ]);
    expect(out.effectDecls.flatMap(decl => printStaticDecl(decl, 48))).toEqual([
      'static int s_eff0_armed;',
      'static int s_eff0_l_id;',
      'static int s_eff0_d0;',
    ]);
    expect(out.mountEffects).toEqual([{kind: 'depEffect', effect}]);
    expect(printMountEffect(out.mountEffects[0])).toEqual([
      '    er_effect_0();',
      '    s_eff0_d0 = s_state.n;',
    ]);
    expect(printEffectFunction(out.effectFns[1]).body.slice(0, 2)).toEqual([
      '    if (s_eff0_armed)',
      '    {',
    ]);
  });

  it('runs a mount effect inline, or calls it when it returns early', () => {
    const {out} = lower(
      app(
        '  const [n, setN] = useState(0);\n' +
          '  useEffect(() => { setN(1); }, []);\n' +
          '  useEffect(() => { if (n > 1) return; setN(2); }, []);',
        '<Text>{n}</Text>',
      ),
    );
    expect(out.mountEffects).toMatchObject([
      {kind: 'inline', callback: {updatesState: true}},
      {kind: 'call', name: 'er_effect_0', updatesState: true},
    ]);
    expect(out.effectFns).toMatchObject([{kind: 'mount', name: 'er_effect_0'}]);
  });

  it('drops app_update() calls when the app has nothing to re-apply', () => {
    const lowered = lower(
      app(
        '  const [n, setN] = useState(0);\n  useEffect(() => { setN(1); }, []);',
        '<View onTouchStart={() => setN(2)} />',
      ),
    );
    expect(lowered.hasUpdate).toBe(false);
    expect(lowered.out.handlers[0].callback.updatesState).toBe(false);
    expect(lowered.out.mountEffects[0].callback.updatesState).toBe(false);
    expect(writeApp(lowered, 'probe', opts).c).not.toContain('app_update');
  });

  it('drops the wait bookkeeping from a stop no start waits on', () => {
    const {out} = lower(
      app(
        '  const a = useAnimatedValue(0);',
        '<View onTouchStart={() => Animated.sequence([Animated.timing(a, {toValue: 1}), Animated.delay(50)]).stop()} />',
      ),
    );
    expect(out.handlers[0].callback.body).toMatchObject([
      {kind: 'stopAnimations', handles: ['s_av_a'], waitOwner: null},
    ]);
  });
});

describe('the lowered app: er_app_build and app_update', () => {
  /** An app module whose component declares `body` and returns `jsx`. */
  const app = (body, jsx) =>
    "import React, {useState, useRef} from 'react';\n" +
    "import {View, Text, Svg, Circle, useAnimatedValue, useHostValue} from 'embedded-react';\n" +
    `export function App() {\n${body}\n  return ${jsx};\n}`;

  it('builds the tree in er_app_build, and re-applies its state-driven nodes and drawings in app_update', () => {
    const lowered = lower(
      app(
        '  const [n, setN] = useState(0);',
        '<View><Text>{n}</Text><Text>still</Text><Svg width={10} height={10}><Circle cx={n} cy={5} r={2} fill="#0f0" /></Svg></View>',
      ),
    );
    expect(nodesOf(lowered.root).map(node => node.id)).toEqual([
      'n0',
      'n1',
      'n2',
      'n3',
    ]);
    expect(lowered.hasUpdate).toBe(true);

    const {c, updates} = writeApp(lowered, 'probe', opts);
    expect(c).toContain(printElementTree(lowered.root).join('\n'));
    expect(c).toContain('    er_tree_append_child(root, n0);');
    expect(updates).toBe(1);
    expect(c).toContain('static ERNode* s_n1;\nstatic ERNode* s_n3;');
    expect(c).toContain('    er_node_set_props(s_n1, &p);\n    build_svg0();');
  });

  it('has no app_update() when nothing state drives is on screen', () => {
    const lowered = lower(
      app('  const [n, setN] = useState(0);', '<Text>{"fixed"}</Text>'),
    );
    expect(lowered.hasUpdate).toBe(false);
    expect(writeApp(lowered, 'probe', opts).c).not.toContain('app_update');
  });

  it('keeps initial values as values, which the writer spells as C', () => {
    const lowered = lower(
      app(
        "  const [n, setN] = useState(3);\n  const [label] = useState('hi');\n  const [f] = useState(1.5);\n" +
          '  const r = useRef(2);\n  const a = useAnimatedValue(0.5);',
        '<Text onTouchStart={() => { r.current = 4; }} style={{opacity: a}}>{label + n + f}</Text>',
      ),
    );
    expect(lowered.state.map(record => record.initial)).toEqual([3, 'hi', 1.5]);
    expect(lowered.refs).toMatchObject([{cVar: 's_ref_r', initial: 2}]);
    expect(lowered.animations).toMatchObject([{cVar: 's_av_a', initial: 0.5}]);

    const {c} = writeApp(lowered, 'probe', opts);
    expect(c).toContain('{ .n = 3, .label = "hi", .f = 1.5f };');
    expect(c).toContain('static int s_ref_r = 2;');
    expect(c).toContain('    s_av_a = er_anim_value_create(0.5f);');
  });

  it('refuses state C cannot hold while lowering', () => {
    /** An app holding one list state with these items. */
    const listApp = items =>
      app(
        `  const [items, setItems] = useState(${items});`,
        '<View>{items.map(item => <Text>{item.v}</Text>)}</View>',
      );
    expect(() => lower(listApp('[{v: 1.5}, {v: 1 / 0}]'))).toThrow(
      /folded to Infinity/,
    );
    expect(() => lower(listApp('[{v: 1.5}, {v: 0 / 0}]'))).toThrow(
      /folded to NaN/,
    );
    expect(() => lower(listApp("[{v: 1}, {v: 'many'}]"))).toThrow(
      /folded to NaN/,
    );
    expect(() => lower(listApp('[{v: 1}, {v: -1 / 0}]'))).toThrow(
      /folded to -Infinity/,
    );
    expect(
      writeApp(lower(listApp('[{v: 1.5}, {}]')), 'probe', opts).c,
    ).toContain('{\n    { 1.5f },\n    { 0.0f }\n};');
    expect(() =>
      lower(
        app(
          '  const wall_clock = useHostValue(0);',
          '<Text>{wall_clock}</Text>',
        ),
      ),
    ).toThrow(/would collide with er_app_set_wall_clock/);
  });
});
