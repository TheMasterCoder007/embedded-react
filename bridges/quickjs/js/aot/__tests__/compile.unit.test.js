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

import {describe, it, expect, vi} from 'vitest';
import {compileSource} from '../compile.mjs';
import {flattenSvg} from '../../src/embedded-react/svg-ops.js';

// The Flow B AOT compiler turns an App.jsx source string into C. These tests assert on the generated C
// (compileSource is pure — no file I/O), so each fixture is a complete minimal app. `gen` returns the
// .c text; `PRE` is the usual import preamble (the compiler pattern-matches names, it does not resolve
// imports, so the import line is cosmetic but kept for realism).
const PRE = `import { useState } from 'react';
import { View, Text, Pressable, StyleSheet, Animated, useAnimatedValue } from 'embedded-react';
`;
const gen = src => compileSource(src, 'test').c;

describe('AOT baseline (regression)', () => {
  it('compiles a static View/Text tree', () => {
    const c = gen(`${PRE}
      export function App() {
        return (<View><Text>Hello</Text></View>);
      }`);
    expect(c).toContain('er_node_create(ER_NODE_VIEW)');
    expect(c).toContain('er_node_create(ER_NODE_TEXT)');
    expect(c).toContain('"Hello"');
    expect(c).toContain('void er_app_build(int screen_w, int screen_h)');
  });

  it('lowers useState + an onPress setter to C state and a handler', () => {
    const c = gen(`${PRE}
      export function App() {
        const [n, setN] = useState(0);
        return (<Pressable onPress={() => setN(n + 1)}><Text>{n}</Text></Pressable>);
      }`);
    expect(c).toContain('ErAppState');
    expect(c).toContain('s_state.n = app_add(s_state.n, 1);');
    expect(c).toContain('er_event_set(');
    expect(c).toContain('app_update();');
  });

  it('folds a setState updater (prev => expr)', () => {
    const c = gen(`${PRE}
      export function App() {
        const [on, setOn] = useState(false);
        return (<Pressable onPress={() => setOn((p) => !p)}><Text>x</Text></Pressable>);
      }`);
    expect(c).toContain('s_state.on = (!(s_state.on));');
  });

  it('makes interpolated text dynamic with a printf format', () => {
    const c = gen(`${PRE}
      export function App() {
        const [n, setN] = useState(3);
        return (<Text>Count {n}</Text>);
      }`);
    expect(c).toContain('snprintf(p.text');
    expect(c).toMatch(/%d/);
    expect(c).toContain('s_state.n');
  });

  it('toggles a state-driven conditional via display none/flex', () => {
    const c = gen(`${PRE}
      export function App() {
        const [show, setShow] = useState(false);
        return (<View>{show && <Text>peekaboo</Text>}</View>);
      }`);
    expect(c).toContain('ER_DISPLAY_FLEX');
    expect(c).toContain('ER_DISPLAY_NONE');
  });

  it('lowers a static style display to the ERProps field', () => {
    const c = gen(`${PRE}
      export function App() {
        return (<View style={{display: 'none'}}><Text>hidden page</Text></View>);
      }`);
    expect(c).toContain('p.display = ER_DISPLAY_NONE;');
  });

  it('lowers a state-driven style display (the page-cache switch) into app_update', () => {
    const c = gen(`${PRE}
      export function App() {
        const [onHome, setOnHome] = useState(true);
        return (<View style={{display: onHome ? 'flex' : 'none'}}><Text>home</Text></View>);
      }`);
    expect(c).toContain('ER_DISPLAY_FLEX');
    expect(c).toContain('ER_DISPLAY_NONE');
    expect(c).toContain('s_state.onHome');
    // The toggle must land in the update path, not only in the one-shot build.
    expect(c.slice(c.indexOf('app_update'))).toMatch(/\.display\s*=/);
  });

  it('lowers visible={false} to a static display (the prop spelling, Flow A<->B parity)', () => {
    const c = gen(`${PRE}
      export function App() {
        return (<View visible={false}><Text>hidden</Text></View>);
      }`);
    expect(c).toContain('p.display = ER_DISPLAY_NONE;');
  });

  it('lowers a bare `visible` to display:flex', () => {
    const c = gen(`${PRE}
      export function App() {
        return (<View visible><Text>shown</Text></View>);
      }`);
    expect(c).toContain('p.display = ER_DISPLAY_FLEX;');
  });

  it('an explicit style display wins over visible (matches Flow A precedence)', () => {
    const c = gen(`${PRE}
      export function App() {
        return (<View visible={true} style={{display: 'none'}}><Text>x</Text></View>);
      }`);
    expect(c).toContain('p.display = ER_DISPLAY_NONE;');
    expect(c).not.toContain('p.display = ER_DISPLAY_FLEX;');
  });

  it('a state-driven style display wins over visible too', () => {
    const c = gen(`${PRE}
      export function App() {
        const [onHome, setOnHome] = useState(true);
        return (<View visible={false} style={{display: onHome ? 'flex' : 'none'}}><Text>x</Text></View>);
      }`);
    // The style's toggle is the only display write — visible must not have added a second, static one.
    expect(c).toContain(
      'p.display = ((s_state.onHome) ? ER_DISPLAY_FLEX : ER_DISPLAY_NONE);',
    );
    expect(c).not.toContain('p.display = ER_DISPLAY_NONE;');
  });

  it('rejects `visible` on an <Svg> instead of silently dropping it', () => {
    expect(() =>
      gen(`${PRE}
      export function App() {
        return (<View><Svg visible={false} width={10} height={10}><Path d="M0 0 L1 1" fill="none" stroke="#fff" /></Svg></View>);
      }`),
    ).toThrow(/`visible` on an <Svg> is not supported/);
  });

  it('lowers a state-driven visible into app_update', () => {
    const c = gen(`${PRE}
      export function App() {
        const [onHome, setOnHome] = useState(true);
        return (<View visible={onHome}><Text>home</Text></View>);
      }`);
    expect(c.slice(c.indexOf('app_update'))).toMatch(/\.display\s*=/);
    expect(c).toContain('s_state.onHome');
  });

  it('compiles multiple sequential setters in one handler with a single app_update', () => {
    const c = gen(`${PRE}
      export function App() {
        const [a, setA] = useState(0);
        const [b, setB] = useState(0);
        return (<Pressable onPress={() => { setA(a + 1); setB(b - 1); }}><Text>{a}</Text></Pressable>);
      }`);
    expect(c).toContain('s_state.a = app_add(s_state.a, 1);');
    expect(c).toContain('s_state.b = app_sub(s_state.b, 1);');
    // exactly one app_update per handler body
    const handler = c.slice(c.indexOf('er_handler_0'));
    expect(
      handler.slice(0, handler.indexOf('}')).match(/app_update\(\);/g),
    ).toHaveLength(1);
  });

  it('compiles a handler local const used by a later setter', () => {
    const c = gen(`${PRE}
      export function App() {
        const [n, setN] = useState(0);
        return (<Pressable onPress={() => { const step = 5; setN(n + step); }}><Text>x</Text></Pressable>);
      }`);
    expect(c).toContain('int l_step = 5;');
    expect(c).toContain('s_state.n = app_add(s_state.n, l_step);');
  });

  it('compiles a branching (if/else) handler', () => {
    const c = gen(`${PRE}
      export function App() {
        const [n, setN] = useState(0);
        return (<Pressable onPress={() => { if (n > 10) { setN(0); } else { setN(n + 1); } }}><Text>x</Text></Pressable>);
      }`);
    expect(c).toContain('if ((s_state.n > 10))');
    expect(c).toContain('s_state.n = 0;');
    expect(c).toContain('s_state.n = app_add(s_state.n, 1);');
    expect(c).toContain('else');
  });

  it('lowers a value useRef to a static slot, with .current read/write (no re-render)', () => {
    const c = gen(`${PRE}
      import { useRef } from 'react';
      export function App() {
        const [n, setN] = useState(0);
        const taps = useRef(0);
        return (<Pressable onPress={() => { taps.current = taps.current + 1; setN(taps.current); }}><Text>x</Text></Pressable>);
      }`);
    expect(c).toContain('static int s_ref_taps = 0;');
    expect(c).toContain('s_ref_taps = app_add(s_ref_taps, 1);');
    expect(c).toContain('s_state.n = s_ref_taps;');
  });

  it('a ref-only handler does not emit app_update (refs do not re-render)', () => {
    const c = gen(`${PRE}
      import { useRef } from 'react';
      export function App() {
        const acc = useRef(0);
        return (<Pressable onPress={() => { acc.current++; }}><Text>x</Text></Pressable>);
      }`);
    expect(c).toContain('s_ref_acc = app_add(s_ref_acc, 1);');
    const handler = c.slice(c.indexOf('er_handler_0('));
    expect(
      handler.slice(0, handler.indexOf('\n}')).includes('app_update();'),
    ).toBe(false);
  });

  // A setter queues app_update() before anything knows whether a node reads state; app_update itself is
  // emitted only when one does. Every kind of body that queues it is here, and nothing on screen reads n.
  it('calls no app_update when nothing on screen reads state', () => {
    const c = gen(`${PRE}
      import { useEffect } from 'react';
      import { Switch } from 'embedded-react';
      export function App() {
        const [n, setN] = useState(0);
        const [items, setItems] = useState([{w: 1}]);
        const a = useAnimatedValue(0);
        useEffect(() => { setN(1); }, []);
        useEffect(() => { if (n > 5) return; setN(2); }, []);
        useEffect(() => { setInterval(() => setN((v) => v + 1), 1000); }, []);
        return (
          <View>
            <Pressable onPress={() => { setN(n + 1); setItems([...items, {w: 2}]); Animated.timing(a, {toValue: 1, duration: 300}).start(() => setN(0)); }}><Text>x</Text></Pressable>
            <Switch value={true} onValueChange={() => setN(n - 1)} />
          </View>
        );
      }`);
    expect(c).not.toContain('app_update');
    // The setters themselves stay.
    expect(c).toContain('s_state.n = app_add(s_state.n, 1);');
    expect(c).toContain('s_state.n = app_sub(s_state.n, 1);');
    expect(c).toContain('s_state.n = 0;');
    expect(c).toContain('s_items_count++;');
  });

  it('lowers a string useState to a char buffer + snprintf setter + %s text', () => {
    const c = gen(`${PRE}
      export function App() {
        const [label, setLabel] = useState('Idle');
        return (<Pressable onPress={() => setLabel('Active')}><Text>{label}</Text></Pressable>);
      }`);
    expect(c).toMatch(/char label\[\d+\];/);
    expect(c).toContain('.label = "Idle"');
    expect(c).toContain(
      'snprintf(s_state.label, sizeof(s_state.label), "%s", "Active");',
    );
    // the Text node renders the string with %s
    expect(c).toContain(
      'snprintf(p.text, sizeof(p.text), "%s", s_state.label);',
    );
  });

  it('wires a useCallback identifier to a single shared handler', () => {
    const c = gen(`${PRE}
      import { useCallback } from 'react';
      export function App() {
        const [n, setN] = useState(0);
        const onTap = useCallback(() => setN(n + 1), [n]);
        return (<View><Pressable onPress={onTap}><Text>a</Text></Pressable><Pressable onPress={onTap}><Text>b</Text></Pressable></View>);
      }`);
    expect(c).toContain('static void er_cb_onTap(');
    // emitted once, referenced twice
    expect(c.match(/static void er_cb_onTap\(/g)).toHaveLength(1);
    expect(
      c.match(/er_event_set\([^,]+, ER_EVENT_PRESS, er_cb_onTap, NULL\);/g),
    ).toHaveLength(2);
  });

  it('inlines a state-dependent useMemo at its use site', () => {
    const c = gen(`${PRE}
      import { useMemo } from 'react';
      export function App() {
        const [n, setN] = useState(2);
        const doubled = useMemo(() => n * 2, [n]);
        return (<Text>Value {doubled}</Text>);
      }`);
    expect(c).toContain('app_mul(s_state.n, 2)');
  });

  it('constant-folds a useMemo with no dynamic deps', () => {
    const c = gen(`${PRE}
      import { useMemo } from 'react';
      export function App() {
        const base = useMemo(() => 3 * 4, []);
        return (<Text>{base}</Text>);
      }`);
    expect(c).toContain('"12"');
  });

  it('bakes a static <Svg> subtree into an op-tape + paint table on a vector node', () => {
    const c = gen(`${PRE}
      import { Svg, Circle, Arc } from 'embedded-react';
      export function App() {
        return (
          <Svg width={100} height={100} viewBox="0 0 100 100">
            <Circle cx={50} cy={50} r={40} fill="#16202f" stroke="#f4a261" strokeWidth={3} />
            <Arc cx={50} cy={50} r={40} startAngle={0} endAngle={180} stroke="#e76f51" strokeWidth={8} />
          </Svg>
        );
      }`);
    expect(c).toContain('er_node_create(ER_NODE_VECTOR)');
    expect(c).toContain('static const float s_svg0_ops[]');
    expect(c).toContain('static const ERVectorPaint s_svg0_paints[]');
    expect(c).toMatch(
      /er_node_set_vector_ops\(n\d+, s_svg0_ops, \d+, s_svg0_paints, 2, NULL, 0\);/,
    );
    expect(c).toContain('.stroke_w = 3.0f'); // circle stroke width baked
    expect(c).toContain('p.width = (int16_t)100;'); // node box from Svg width
  });

  it('bakes <Rect rx/ry> into cubic corners (static <Svg>), and leaves a plain <Rect> square', () => {
    const svg = rx =>
      gen(`${PRE}
      import { Svg, Rect } from 'embedded-react';
      export function App() {
        return (<Svg width={100} height={60}><Rect x={0} y={0} width={100} height={60} ${rx} fill="#fff" /></Svg>);
      }`);
    // A static <Svg> bakes to raw float literals (opcodes included), so assert on the tape length and
    // the corner geometry: 4 edges + 4 cubics = 46 entries vs the square rect's 15.
    const round = svg('rx={10}');
    expect(round).toMatch(/s_svg0_ops, 46,/);
    expect(round).toContain('95.52284749830794f'); // top-right corner control point (x + w - rx + rx*k)
    expect(svg('')).toMatch(/s_svg0_ops, 15,/);
  });

  it('emits the same rounded-rect tape as the runtime does (Flow A/B parity)', () => {
    // A dynamic sibling forces the state-driven path while the <Rect> itself stays static, so every
    // generated entry is literal arithmetic — evaluate it and diff against svg-ops. This is what catches
    // a transposed control point, which the opcode-shape assertions above would happily let through.
    const c = gen(`${PRE}
      import { Svg, Rect, Line } from 'embedded-react';
      export function App() {
        const [t, setT] = useState(0);
        return (
          <Pressable onPress={() => setT(t + 1)}>
            <Svg width={100} height={60}>
              <Rect x={7} y={3} width={80} height={40} rx={11} ry={5} fill="#fff" />
              <Line x1={0} y1={0} x2={t} y2={10} stroke="#fff" />
            </Svg>
          </Pressable>
        );
      }`);
    const VOP = {
      SHAPE: 0,
      MOVE: 1,
      LINE: 2,
      QUAD: 3,
      CUBIC: 4,
      ARC: 5,
      CLOSE: 6,
    };
    const {ops} = flattenSvg({
      children: [
        {
          type: 'Rect',
          props: {x: 7, y: 3, width: 80, height: 40, rx: 11, ry: 5},
        },
      ],
    });
    const got = [...c.matchAll(/s_svg0_ops\[(\d+)\] = (.+);/g)]
      .map(m => [Number(m[1]), m[2]])
      .sort((a, b) => a[0] - b[0])
      .slice(0, ops.length) // the dynamic <Line> that forced this path follows the rect
      .map(([, e]) =>
        // C float literals -> JS numbers, ER_VOP_* -> opcode values; the rest is plain arithmetic.
        Function(
          `return (${e
            .replace(/ER_VOP_(\w+)/g, (_, n) => String(VOP[n]))
            .replace(/(\d)f\b/g, '$1')})`,
        )(),
      );
    expect(got.length).toBe(ops.length);
    got.forEach((v, i) => expect(v).toBeCloseTo(ops[i], 5));
  });

  it('folds a static corner radius in a state-driven <Rect> and clamps a dynamic side at runtime', () => {
    const c = gen(`${PRE}
      import { Svg, Rect } from 'embedded-react';
      export function App() {
        const [w, setW] = useState(10);
        return (
          <Pressable onPress={() => setW(w + 1)}>
            <Svg width={100} height={40}><Rect x={0} y={0} width={w} height={20} rx={6} fill="#fff" /></Svg>
          </Pressable>
        );
      }`);
    expect(c).toContain('static void build_svg0(void)');
    expect((c.match(/ER_VOP_CUBIC/g) || []).length).toBe(4);
    // ry is bounded by a static height → folded to a literal and left inline; rx is bounded by state,
    // so it clamps in C — once, hoisted into a local, not re-evaluated at each of its ten use sites.
    expect(c).toContain(
      'const float rx_s0 = fminf(fmaxf(6.0f, 0.0f), ((float)(s_state.w)) * 0.5f);',
    );
    expect((c.match(/fminf/g) || []).length).toBe(1);
    expect(c).not.toContain('ry_s0'); // the folded radius needs no local
    expect(c).toContain('#include <math.h>');
  });

  it('rounds an imperative updateVector rect from its optional [.., rx, ry] elements', () => {
    const c = gen(`${PRE}
      import { useRef } from 'react';
      import { Svg, Rect, updateVector } from 'embedded-react';
      export function App() {
        const bar = useRef(null);
        return (
          <Pressable onPress={() => updateVector(bar, [{ rect: [0, 0, 80, 12, 6, 6], fill: '#f4a261' }])}>
            <Svg ref={bar} width={100} height={20}><Rect x={0} y={0} width={80} height={12} fill="#222" /></Svg>
          </Pressable>
        );
      }`);
    expect((c.match(/ER_VOP_CUBIC/g) || []).length).toBe(4); // only the imperative rect is rounded
    // Every side and radius here is a literal, so both clamps fold at compile time: no runtime fminf
    // and no hoisted local at all. cLit sees through the `(float)(N)` cast this path emits.
    expect(c).not.toContain('fminf');
    expect(c).not.toContain('const float rx_uv');
  });

  it('resolves rx/ry edge cases in a state-driven <Rect> exactly as the runtime does', () => {
    // The presence-only check used to send <Rect rx={-3} ry={8}> down the square-cornered path here
    // while the runtime (and the browser) round it at 8; and a literal rx={0} whose side was
    // state-driven could not fold, so it built 44 ops of degenerate cubics for a square rect.
    const svg = attrs =>
      gen(`${PRE}
      import { Svg, Rect, Line } from 'embedded-react';
      export function App() {
        const [t, setT] = useState(0);
        return (
          <Pressable onPress={() => setT(t + 1)}>
            <Svg width={200} height={60}>
              <Rect x={0} y={0} width={100} height={60} ${attrs} fill="#fff" />
              <Line x1={0} y1={0} x2={t} y2={9} stroke="#fff" />
            </Svg>
          </Pressable>
        );
      }`);
    const rounded = a => (svg(a).match(/ER_VOP_CUBIC/g) || []).length > 0;
    expect(rounded('rx={-3} ry={8}')).toBe(true); // negative is `auto` -> falls back to ry
    expect(rounded('rx={8} ry={-3}')).toBe(true); // ... and the other way round
    expect(rounded('rx={-3}')).toBe(false); // nothing to fall back to -> square
    expect(rounded('rx={0} ry={8}')).toBe(false); // an explicit 0 is valid and squares it
    expect(rounded('rx={8} ry={0}')).toBe(false);
    expect(rounded('rx={8}')).toBe(true);
  });

  it('folds a zero radius to a square rect even when the side it clamps against is state-driven', () => {
    const c = gen(`${PRE}
      import { Svg, Rect } from 'embedded-react';
      export function App() {
        const [w, setW] = useState(10);
        return (
          <Pressable onPress={() => setW(w + 1)}>
            {/* BOTH sides state-driven: neither clamp can fold from its side, so squaring this rect
                depends entirely on the radius itself being recognised as non-positive. */}
            <Svg width={200} height={40}><Rect x={0} y={0} width={w} height={w / 2} rx={0} fill="#fff" /></Svg>
          </Pressable>
        );
      }`);
    expect(c).toMatch(/s_svg0_ops\[15\]/); // the 13-op square tape + its SHAPE header, not 44 + 2
    expect(c).not.toContain('ER_VOP_CUBIC');
    expect(c).not.toContain('fminf'); // no runtime clamp for a radius that is statically zero
  });

  it('keeps hoisted radius locals unique across shapes and across calls in one handler block', () => {
    const c = gen(`${PRE}
      import { useRef } from 'react';
      import { Svg, Rect, updateVector } from 'embedded-react';
      export function App() {
        const [w, setW] = useState(10);
        const a = useRef(null);
        const b = useRef(null);
        return (
          <Pressable onPress={() => {
            updateVector(a, [{ rect: [0, 0, w, w / 2, 6, 6], fill: '#fff' }, { rect: [0, 30, w, w / 3, 4], fill: '#f00' }]);
            updateVector(b, [{ rect: [0, 0, w, w / 4, 6, 6], fill: '#0f0' }]);
          }}>
            <Svg ref={a} width={100} height={60} />
            <Svg ref={b} width={100} height={20} />
          </Pressable>
        );
      }`);
    // Both sides are state-driven so every radius hoists — two shapes in one call plus a second call,
    // all landing in the same C block. A name keyed only on the shape index would redeclare.
    const names = [...c.matchAll(/const float ((?:rx|ry)_\w+) =/g)].map(
      m => m[1],
    );
    expect(names.length).toBe(6);
    expect(new Set(names).size).toBe(6);
  });

  it('compiles a state-driven <Svg> (arc sweep) to a build_svg fn recomputed on update', () => {
    const c = gen(`${PRE}
      import { Svg, Circle, Arc } from 'embedded-react';
      export function App() {
        const [temp, setTemp] = useState(50);
        return (
          <Pressable onPress={() => setTemp(temp + 1)}>
            <Svg width={200} height={200}>
              <Circle cx={100} cy={100} r={80} fill="#16202f" />
              <Arc cx={100} cy={100} r={80} startAngle={-130} endAngle={temp * 2} stroke="#f4a261" strokeWidth={12} strokeLinecap="round" />
            </Svg>
          </Pressable>
        );
      }`);
    expect(c).toContain('#include <math.h>');
    expect(c).toMatch(/static float s_svg0_ops\[\d+\];/); // mutable, not const
    expect(c).toContain('static void build_svg0(void)');
    expect(c).toContain('ER_VOP_ARC');
    expect((c.match(/ER_VOP_MOVE/g) || []).length).toBe(1);
    expect(c).toContain('(float)M_PI / 180.0f'); // degrees → radians for the arc angles
    expect(c).toContain('app_mul(s_state.temp, 2)'); // dynamic endAngle expression
    expect(c).toContain('build_svg0();');
    expect(c).toMatch(
      /er_node_set_vector_ops\(s_n\d+, s_svg0_ops, \d+, s_svg0_paints, 2, NULL, 0\);/,
    ); // re-upload in app_update
  });

  it('lowers a state-driven <Svg> paint (stroke color/width) to a mutable paint table rebuilt from state', () => {
    const c = gen(`${PRE}
      import { Svg, Arc } from 'embedded-react';
      export function App() {
        const [mode, setMode] = useState('heat');
        return (
          <Pressable onPress={() => setMode('cool')}>
            <Svg width={100} height={100}>
              <Arc cx={50} cy={50} r={40} startAngle={-135} endAngle={135}
                   stroke={mode === 'cool' ? '#4cc9f0' : '#f4a261'} strokeWidth={mode === 'off' ? 2 : 8} />
            </Svg>
          </Pressable>
        );
      }`);
    expect(c).toMatch(/static ERVectorPaint s_svg0_paints\[1\];/); // MUTABLE table (not const)
    expect(c).not.toMatch(/static const ERVectorPaint s_svg0_paints/); // ...specifically not const
    // dynamic stroke color → an ARGB ternary, assigned in build_svg0
    expect(c).toContain(
      's_svg0_paints[0].stroke = (((strcmp(s_state.mode, "cool") == 0)) ? 0xFF4CC9F0u : 0xFFF4A261u);',
    );
    // dynamic stroke width → a numeric ternary cast to float
    expect(c).toContain(
      's_svg0_paints[0].stroke_w = (float)(((strcmp(s_state.mode, "off") == 0) ? 2 : 8));',
    );
  });

  it('keeps a state-driven <Svg> with STATIC paint on a const paint table (no per-update paint work)', () => {
    const c = gen(`${PRE}
      import { Svg, Arc } from 'embedded-react';
      export function App() {
        const [t, setT] = useState(0);
        return (
          <Pressable onPress={() => setT(t + 1)}>
            <Svg width={100} height={100}>
              <Arc cx={50} cy={50} r={40} startAngle={-135} endAngle={t * 2} stroke="#f4a261" strokeWidth={8} />
            </Svg>
          </Pressable>
        );
      }`);
    expect(c).toMatch(/static const ERVectorPaint s_svg0_paints\[\] = \{/); // const fast path retained
    expect(c).not.toContain('s_svg0_paints[0].stroke ='); // paint NOT reassigned per update
  });

  it('emits a baked <Svg source> with its gradient table (Flow B gradients via opts.svgArtifacts)', () => {
    const artifact = {
      ops: [0, 0, 1, 0, 0, 2, 10, 0, 2, 10, 10, 6], // SHAPE 0, MOVE 0,0, LINE 10,0, LINE 10,10, CLOSE
      paints: [0, 0, 4, 4, 0, 0, 0, 0, 1], // one 9-wide paint: solid fill 0, stroke_grad = 1 (a conic-stroked path)
      gradients: [
        {
          type: 3,
          stops: [
            {color: 0xff39bdf8, offset: 0},
            {color: 0xfff04741, offset: 0.75},
          ],
          ax: 5,
          ay: 5,
          bx: 0,
          by: 0,
          r: -2.356,
        },
      ],
      width: 20,
      height: 20,
    };
    const c = compileSource(
      `import { View, Svg } from 'embedded-react';\nimport dial from './climate.svg';\nexport function App() { return <View><Svg source={dial} width={20} height={20} /></View>; }`,
      'test',
      {svgArtifacts: {climate: artifact}},
    ).c;
    expect(c).toContain('static const ERVectorGradient s_svg0_grads[] = {'); // gradient table emitted
    expect(c).toContain('.type = 3, .stop_count = 2'); // conic, 2 stops
    expect(c).toContain('.stroke_grad = 1'); // the stroked path references the gradient (1-based)
    expect(c).toMatch(
      /er_node_set_vector_ops\(n\d+, s_svg0_ops, 12, s_svg0_paints, 1, s_svg0_grads, 1\);/,
    );
  });

  it('emits a raster-fallback <Svg source> as an Image node + registers its PNG (Flow B)', () => {
    // bakeSvgArtifacts produces this for an SVG that used unsupported features (e.g. <text>): rasterized to
    // a PNG. emitSvgSource must render it as an Image node, NOT a vector op-tape, and bake the PNG.
    const artifact = {
      kind: 'raster',
      name: 'badge',
      width: 20,
      height: 20,
      png: 'C:/tmp/badge-abcd1234.png',
    };
    const res = compileSource(
      `import { View, Svg } from 'embedded-react';\nimport badge from './badge.svg';\nexport function App() { return <View><Svg source={badge} width={20} height={20} /></View>; }`,
      'test',
      {svgArtifacts: {badge: artifact}},
    );
    expect(res.c).toContain('er_node_create(ER_NODE_IMAGE)'); // raster → image node
    expect(res.c).toContain(
      'snprintf(p.image_name, sizeof(p.image_name), "%s", "badge")',
    );
    expect(res.c).not.toContain('s_svg0_ops'); // no vector op-tape emitted for the rastered svg
    expect(
      res.images.some(
        im => im.name === 'badge' && /badge-abcd1234\.png$/.test(im.importPath),
      ),
    ).toBe(true);
  });

  it('scales a <Svg source> at compile time but leaves a conic gradient start ANGLE unscaled', () => {
    const artifact = {
      ops: [0, 0, 1, 0, 0, 2, 10, 10, 6],
      paints: [0, 0, 2, 4, 0, 0, 0, 0, 1],
      gradients: [
        {
          type: 3,
          stops: [
            {color: 0xff000000, offset: 0},
            {color: 0xffffffff, offset: 1},
          ],
          ax: 5,
          ay: 5,
          bx: 0,
          by: 0,
          r: 1.5,
        },
      ],
      width: 10,
      height: 10,
    };
    // width 20 on a 10px-intrinsic artifact → sx=2: centre (5,5) -> (10,10); conic r (an angle) stays 1.5.
    const c = compileSource(
      `import { View, Svg } from 'embedded-react';\nimport d from './d.svg';\nexport function App() { return <View><Svg source={d} width={20} height={20} /></View>; }`,
      'test',
      {svgArtifacts: {d: artifact}},
    ).c;
    expect(c).toMatch(/\.ax = 10(\.0+)?f, \.ay = 10(\.0+)?f/); // centre scaled x2
    expect(c).toContain('.r = 1.5f'); // angle NOT scaled
  });

  it('throws a clear error when <Svg source> is not an imported .svg', () => {
    expect(() =>
      compileSource(
        `import { View, Svg } from 'embedded-react';\nexport function App() { return <View><Svg source={{}} width={10} height={10} /></View>; }`,
        'test',
        {},
      ),
    ).toThrow(/<Svg source> must reference an imported \.svg/);
  });

  it('captures a node ref and lowers updateVector(ref, shapes, dirtyRect) imperatively', () => {
    const c = gen(`${PRE}
      import { useRef } from 'react';
      import { Svg, Circle } from 'embedded-react';
      export function App() {
        const dial = useRef();
        return (
          <Pressable onPress={(e) => updateVector(dial, [{ arc: [100, 100, 80, -130, e.x], stroke: '#f4a261', strokeWidth: 14, cap: 'round' }], [0, 0, 200, 200])}>
            <Svg ref={dial} width={200} height={200}>
              <Circle cx={100} cy={100} r={80} fill="#16202f" />
            </Svg>
          </Pressable>
        );
      }`);
    expect(c).toContain('static ERNode* s_ref_dial = NULL;'); // node ref slot
    expect(c).toMatch(/s_ref_dial = n\d+;/); // captured at build
    expect(c).toContain('#include <math.h>'); // arc trig
    expect(c).toMatch(/static float s_uv0_ops\[\d+\];/); // imperative op buffer
    expect(c).toContain('s_uv0_ops[0] = ER_VOP_SHAPE;');
    expect(c).toContain('data->x'); // event coord in arc endAngle
    expect(c).toMatch(
      /er_node_set_vector_ops\(s_ref_dial, s_uv0_ops, \d+, s_uv0_paints, 1, NULL, 0\);/,
    );
    expect(c).toContain(
      'er_node_set_vector_dirty_rect(s_ref_dial, 0, 0, 200, 200);',
    );
  });

  it('declares nothing for a ref the generated C never touches', () => {
    const c = gen(`${PRE}
      import { useRef } from 'react';
      export function App() {
        const [n, setN] = useState(0);
        const stepRef = useRef(null);   // holds a JS value only — never bound, never read in C
        const usedRef = useRef(0);
        return (
          <Pressable onPress={() => { usedRef.current = n; }}>
            <Text>{usedRef.current}</Text>
          </Pressable>
        );
      }`);
    expect(c).not.toContain('s_ref_stepRef');
    expect(c).toContain('static int s_ref_usedRef = 0;');
  });

  it('recognizes a memo()-wrapped component', () => {
    const c = gen(`${PRE}
      import { memo } from 'react';
      const Badge = memo(function Badge({ label }) { return (<Text>{label}</Text>); });
      export function App() { return (<View><Badge label="hi" /></View>); }`);
    expect(c).toContain('"hi"'); // inlined, prop substituted
    expect(c).toContain('er_node_create(ER_NODE_TEXT)');
  });

  it('emits props.children (destructured) passed to a component', () => {
    const c = gen(`${PRE}
      function Card({ children }) { return (<View style={{ padding: 8 }}>{children}</View>); }
      export function App() { return (<Card><Text>inside</Text></Card>); }`);
    expect(c).toContain('"inside"'); // the child Text is emitted
    expect(c).toContain('er_node_create(ER_NODE_VIEW)');
  });

  it('emits props.children via the whole-props parameter', () => {
    const c = gen(`${PRE}
      function Card(props) { return (<View>{props.children}</View>); }
      export function App() { return (<Card><Text>P</Text></Card>); }`);
    expect(c).toContain('"P"');
  });

  it("merges a static spread {...obj} into a component's props", () => {
    const c = gen(`${PRE}
      const cfg = { label: 'spread!' };
      function Badge({ label }) { return (<Text>{label}</Text>); }
      export function App() { return (<View><Badge {...cfg} /></View>); }`);
    expect(c).toContain('"spread!"');
  });

  it('lowers Math.*, string-state equality (strcmp), and static member folds', () => {
    const c = gen(`${PRE}
      const ITEMS = [{ key: 'a' }, { key: 'b' }];
      export function App() {
        const [sel, setSel] = useState('a');
        const [v, setV] = useState(10);
        return (
          <View>
            <Text>{Math.round(v * 1.5)}</Text>
            {ITEMS.map((it) => (
              <Pressable key={it.key} style={{ backgroundColor: sel === it.key ? '#ffffff' : '#000000' }} onPress={() => setSel(it.key)}>
                <Text>{it.key}</Text>
              </Pressable>
            ))}
          </View>
        );
      }`);
    expect(c).toContain('#include <math.h>');
    expect(c).toContain('app_f2i(app_roundf('); // Math.round → a whole int
    expect(c).toContain('strcmp(s_state.sel, "a")'); // string equality + it.key folded to "a"
    expect(c).toContain('strcmp(s_state.sel, "b")'); // second unrolled .map iteration
  });

  it('lowers Math.sin/cos/PI to libm for dynamic Svg coordinates', () => {
    const c = gen(`${PRE}
      import { Svg, Circle } from 'embedded-react';
      export function App() {
        const [a, setA] = useState(0);
        return (<Svg width={100} height={100}><Circle cx={50 + 40 * Math.sin((a * Math.PI) / 180)} cy={50} r={5} fill="#fff" /></Svg>);
      }`);
    expect(c).toContain('sinf(');
    expect(c).toContain('M_PI');
  });

  it('binds an animated transform and starts a spring from a handler', () => {
    const c = gen(`${PRE}
      export function App() {
        const s = useAnimatedValue(1);
        return (
          <Pressable
            style={{ transform: [{ scale: s }] }}
            onPressIn={() => Animated.spring(s, { toValue: 0.85 }).start()}
          ><Text>p</Text></Pressable>
        );
      }`);
    expect(c).toContain('er_anim_value_create(1.0f)');
    expect(c).toContain('er_anim_value_bind(s_av_s,');
    expect(c).toContain('ER_PROP_SCALE_X');
    expect(c).toContain('ER_ANIM_SPRING');
    expect(c).toContain('er_anim_value_animate(s_av_s,');
  });
});

describe('AOT animation completeness', () => {
  const A = `import { View, Animated, Easing, useAnimatedValue } from 'embedded-react';`;
  // Wraps a handler body in an App with two animated values a and b, on an onPress View.
  const app = (body, style = '') =>
    gen(`${A}
      export function App() {
        const a = useAnimatedValue(0);
        const b = useAnimatedValue(0);
        return (<View onPress={() => { ${body} }} ${style ? `style={${style}}` : ''}><Text>x</Text></View>);
      }`);

  it('chains Animated.sequence steps via on_complete', () => {
    const c = app(`Animated.sequence([
      Animated.timing(a, { toValue: 1, duration: 300 }),
      Animated.timing(b, { toValue: 1, duration: 200 }),
    ]).start();`);
    expect(c).toContain('er_anim_value_animate(s_av_a,');
    expect(c).toContain('er_anim_value_animate(s_av_b,');
    expect(c).toMatch(/\.on_complete = er_seqcb_/); // step 0 chains to step 1
    expect(c).toMatch(/static void er_seqcb_\w+\(bool finished/); // step 1 emitted as a completion callback
  });

  it('sequences the SAME value (out-and-back) without the steps cancelling each other', () => {
    const c = app(`Animated.sequence([
      Animated.timing(a, { toValue: 1, duration: 300 }),
      Animated.timing(a, { toValue: 0, duration: 300 }),
    ]).start();`);
    // Both steps drive s_av_a; the second runs in an on_complete callback, NOT synchronously (which would
    // cancel the first via cancel_value_anim). Two animate calls on the same value must survive.
    const calls = (c.match(/er_anim_value_animate\(s_av_a,/g) || []).length;
    expect(calls).toBe(2);
    expect(c).toMatch(/\.on_complete = er_seqcb_/);
  });

  it('Animated.delay inside a sequence folds into the next step delay_ms', () => {
    const c = app(`Animated.sequence([
      Animated.timing(a, { toValue: 1, duration: 300 }),
      Animated.delay(100),
      Animated.timing(b, { toValue: 1, duration: 200 }),
    ]).start();`);
    expect(c).toContain('.delay_ms = 100;'); // b starts 100ms after a completes
  });

  it('Animated.parallel starts every entry together (no offset)', () => {
    const c = app(`Animated.parallel([
      Animated.timing(a, { toValue: 1, duration: 300 }),
      Animated.timing(b, { toValue: 1, duration: 200 }),
    ]).start();`);
    expect(c).not.toContain('delay_ms'); // both start at 0
    expect(c).toContain('er_anim_value_animate(s_av_a,');
    expect(c).toContain('er_anim_value_animate(s_av_b,');
  });

  it('Animated.stagger spaces entries by i * stagger_ms', () => {
    const c = app(`Animated.stagger(80, [
      Animated.timing(a, { toValue: 1, duration: 200 }),
      Animated.timing(b, { toValue: 1, duration: 200 }),
    ]).start();`);
    expect(c).toContain('cfg1.delay_ms = 80;');
  });

  it('Animated.loop around a single timing sets cfg.loop', () => {
    const c = app(
      `Animated.loop(Animated.timing(a, { toValue: 360, duration: 1000, easing: Easing.linear })).start();`,
    );
    expect(c).toContain('cfg0.loop = true;');
    expect(c).toContain('ER_EASE_LINEAR');
  });

  it('Animated.decay lowers to ER_ANIM_DECAY with velocity + deceleration', () => {
    const c = app(
      `Animated.decay(b, { velocity: 0.5, deceleration: 0.997 }).start();`,
    );
    expect(c).toContain('ER_ANIM_DECAY');
    expect(c).toContain('cfg0.velocity = 0.5f;');
    expect(c).toContain('cfg0.deceleration = 0.997f;');
  });

  it('maps Easing.inOut(Easing.quad) and Easing.bezier(...)', () => {
    const c = app(`Animated.sequence([
      Animated.timing(a, { toValue: 1, duration: 100, easing: Easing.inOut(Easing.quad) }),
      Animated.timing(b, { toValue: 1, duration: 100, easing: Easing.bezier(0.2, 0, 0.4, 1) }),
    ]).start();`);
    expect(c).toContain('ER_EASE_QUAD_IN_OUT');
    expect(c).toContain('ER_EASE_BEZIER');
    expect(c).toContain('.bezier_x1 = 0.2f;');
    expect(c).toContain('.bezier_y2 = 1.0f;');
  });

  it('binds interpolate() on a style prop and a transform via er_anim_value_bind_interpolated', () => {
    const c = app(
      `Animated.timing(a, { toValue: 1, duration: 200 }).start();`,
      `{ opacity: a.interpolate({ inputRange: [0, 1], outputRange: [0.2, 1], extrapolate: 'clamp' }), transform: [{ translateX: b.interpolate({ inputRange: [0, 1], outputRange: [0, 100] }) }] }`,
    );
    expect(c).toContain('ERInterpolation');
    expect(c).toContain('er_anim_value_bind_interpolated(s_av_a,');
    expect(c).toContain('ER_PROP_OPACITY');
    expect(c).toContain('ER_EXTRAPOLATE_CLAMP');
    expect(c).toContain('er_anim_value_bind_interpolated(s_av_b,');
    expect(c).toContain('ER_PROP_TRANSLATE_X');
  });

  it('supports a spring inside a sequence (chained, no fixed duration needed)', () => {
    const c = app(`Animated.sequence([
      Animated.spring(a, { toValue: 1 }),
      Animated.timing(b, { toValue: 1, duration: 200 }),
    ]).start();`);
    expect(c).toContain('ER_ANIM_SPRING');
    expect(c).toContain('er_anim_value_animate(s_av_b,');
    expect(c).toMatch(/\.on_complete = er_seqcb_/);
  });

  it('rejects the same value driven twice in a parallel (they would cancel)', () => {
    expect(() =>
      app(`Animated.parallel([
      Animated.timing(a, { toValue: 1, duration: 100 }),
      Animated.timing(a, { toValue: 0, duration: 100 }),
    ]).start();`),
    ).toThrow(/same animated value/);
  });

  it('rejects Animated.loop around a multi-step sequence', () => {
    expect(() =>
      app(`Animated.loop(Animated.sequence([
      Animated.timing(a, { toValue: 1, duration: 100 }),
      Animated.timing(b, { toValue: 1, duration: 100 }),
    ])).start();`),
    ).toThrow(/loop currently wraps a single/);
  });

  it('rejects mismatched interpolate ranges', () => {
    expect(() =>
      app(
        `Animated.timing(a, { toValue: 1, duration: 200 }).start();`,
        `{ opacity: a.interpolate({ inputRange: [0, 1], outputRange: [0.2, 0.5, 1] }) }`,
      ),
    ).toThrow(/same length/);
  });
});

describe('AOT responsive layout', () => {
  // A percentage dimension lowers to the engine's *_pct field (% of parent), not the absolute pixel field.
  it('lowers percentage width/height to the *_pct fields', () => {
    const c = gen(`${PRE}
      export function App() {
        return (<View style={{ width: '50%', height: '100%' }}><Text>x</Text></View>);
      }`);
    expect(c).toContain('p.width_pct = 50.0f;');
    expect(c).toContain('p.height_pct = 100.0f;');
    expect(c).not.toMatch(/p\.width = \(int16_t\)50;/);
  });

  // Insets take percentages too, and on their own axis: left/right of the width, top/bottom of the height.
  it('lowers percentage insets to the *_pct fields', () => {
    const c = gen(`${PRE}
      export function App() {
        return (
          <View style={{ position: 'absolute', left: '10%', top: '25%', right: 4, bottom: '-5%' }}>
            <Text>x</Text>
          </View>
        );
      }`);
    expect(c).toContain('p.left_pct = 10.0f;');
    expect(c).toContain('p.top_pct = 25.0f;');
    expect(c).toContain('p.right = 4;');
    expect(c).toContain('p.bottom_pct = -5.0f;');
  });

  // 0% means 0px, and 0.0 is the engine's "not set" sentinel for a percentage field — so an authored
  // 0% has to land on the pixel field, or the inset would read as absent and fall back elsewhere.
  it('lowers a 0% inset onto the pixel field', () => {
    const c = gen(`${PRE}
      export function App() {
        return (
          <View style={{ position: 'absolute', left: '0%', right: '0%', flexBasis: '0%' }}>
            <Text>x</Text>
          </View>
        );
      }`);
    expect(c).toContain('p.left = 0;');
    expect(c).toContain('p.right = 0;');
    expect(c).toContain('p.flex_basis = 0;');
    expect(c).not.toContain('_pct');
  });

  it('keeps absolute pixel widths on the pixel field', () => {
    const c = gen(`${PRE}
      export function App() {
        return (<View style={{ width: 120 }}><Text>x</Text></View>);
      }`);
    expect(c).toContain('p.width = 120;');
    expect(c).not.toContain('width_pct');
  });

  // The `screen` global is a compile-time constant; a top-level `if` on it folds to one branch per build.
  it('folds a compile-time screen branch to the WIDE layout (default 800x480)', () => {
    const src = `${PRE}
      export function App() {
        const compact = screen.width < 400;
        if (compact) return (<View><Text>small</Text></View>);
        return (<View><Text>wide</Text></View>);
      }`;
    const c = compileSource(src, 'test').c;
    expect(c).toContain('"wide"');
    expect(c).not.toContain('"small"');
  });

  it('folds the same source to the COMPACT layout for a 240x320 screen', () => {
    const src = `${PRE}
      export function App() {
        const compact = screen.width < 400;
        if (compact) return (<View><Text>small</Text></View>);
        return (<View><Text>wide</Text></View>);
      }`;
    const c = compileSource(src, 'test', {screen: {width: 240, height: 320}}).c;
    expect(c).toContain('"small"');
    expect(c).not.toContain('"wide"');
  });

  it('stamps the header with the screen it actually folded against', () => {
    // The board examples _Static_assert ER_AOT_SCREEN_W/H against their panel, so this metadata has to
    // come from the size the layout was folded at — not from the environment default, which would make a
    // correctly generated app fail the board's check.
    const src = `${PRE}
      export function App() {
        const compact = screen.width < 400;
        if (compact) return (<View><Text>small</Text></View>);
        return (<View><Text>wide</Text></View>);
      }`;
    const res = compileSource(src, 'test', {screen: {width: 240, height: 320}});
    expect(res.c).toContain('"small"');
    expect(res.h).toContain('#define ER_AOT_SCREEN_W 240');
    expect(res.h).toContain('#define ER_AOT_SCREEN_H 320');
    expect(res.h).toContain('#define ER_AOT_DEMO "test"');
  });

  // A board that calls a demo's useHostValue setters otherwise fails on implicit declarations of setters
  // that were never generated. The marker macro lets its main.c #ifndef the mismatch by name.
  it('stamps a demo marker macro, sanitised into a C identifier', () => {
    const src = `${PRE}
      export function App() { return (<View><Text>x</Text></View>); }`;
    expect(compileSource(src, 'watch-face').h).toContain(
      '#define ER_AOT_DEMO_watch_2d_face 1',
    );
    expect(compileSource(src, 'thermostat').h).toContain(
      '#define ER_AOT_DEMO_thermostat 1',
    );
  });

  it('throws on a top-level if whose test is not compile-time constant', () => {
    const src = `${PRE}
      export function App() {
        const [n, setN] = useState(0);
        if (n > 5) return (<View><Text>a</Text></View>);
        return (<View><Text>b</Text></View>);
      }`;
    expect(() => compileSource(src, 'test')).toThrow(
      /compile-time-constant test/,
    );
  });
});

describe('AOT inline-Svg gradients', () => {
  const GRAD = `strokeGrad={{type: 3, ax: 100, ay: 100, r: (lo * Math.PI) / 180,
      stops: [{color: '#F2A64B', offset: 0}, {color: '#4FA9F5', offset: 0.25}]}}`;
  // endAngle is state-driven, so the <Svg> takes the dynamic path — the only one that supports
  // gradients on inline shapes. A fully static <Svg> ignores them (see emitSvgStatic).
  const arc = extra => `${PRE}
    export function App() {
      const [lo, setLo] = useState(68);
      return (<View><Svg width={200} height={200}>
        <Arc cx={100} cy={100} r={80} startAngle={240} endAngle={240 + lo} fill="none"
             strokeWidth={12} stroke="#F2A64B" ${extra} />
      </Svg></View>);
    }`;

  it('emits a MUTABLE gradient table when a field is state-driven, and drives it from state', () => {
    const c = gen(arc(GRAD));
    // Mutable, not const — the conic start angle is rebuilt from state on every update.
    expect(c).toContain('static ERVectorGradient s_svg0_grads[1];');
    expect(c).toMatch(/s_svg0_grads\[0\]\.type = 3;/);
    expect(c).toMatch(/s_svg0_grads\[0\]\.r = .*s_state\.lo/);
    // 1-based index on the paint, and the table reaches the engine on BOTH build and update.
    expect(c).toContain('s_svg0_paints[0].stroke_grad = 1;');
    expect(
      c.match(/er_node_set_vector_ops\([^)]*s_svg0_grads, 1\)/g),
    ).toHaveLength(2);
  });

  it('applies a CONDITIONAL gradient by switching the paint index, not by omission', () => {
    const c = gen(
      arc(`strokeGrad={lo > 60 ? {type: 3, ax: 100, ay: 100, r: 0,
        stops: [{color: '#F2A64B', offset: 0}, {color: '#4FA9F5', offset: 0.25}]} : null}`),
    );
    // The table entry always exists; the INDEX is what varies, so the shape falls back to its solid
    // stroke when the condition is false instead of the gradient leaking into every other state.
    expect(c).toMatch(
      /s_svg0_paints\[0\]\.stroke_grad = \(\(.*s_state\.lo.*\) \? 1 : 0\);/,
    );
  });

  it('accepts the `cond && { … }` form too', () => {
    const c = gen(
      arc(`strokeGrad={lo > 60 && {type: 3, ax: 100, ay: 100, r: 0,
        stops: [{color: '#F2A64B', offset: 0}, {color: '#4FA9F5', offset: 0.25}]}}`),
    );
    expect(c).toMatch(/stroke_grad = \(\(.*s_state\.lo.*\) \? 1 : 0\);/);
  });

  it('rejects a conditional whose other branch is not null', () => {
    expect(() =>
      gen(
        arc(`strokeGrad={lo > 60
          ? {type: 3, ax: 0, ay: 0, r: 0, stops: [{color: '#F2A64B', offset: 0}, {color: '#4FA9F5', offset: 1}]}
          : {type: 1, ax: 0, ay: 0, bx: 1, by: 0, stops: [{color: '#F2A64B', offset: 0}, {color: '#4FA9F5', offset: 1}]}}`),
      ),
    ).toThrow(/conditional "strokeGrad"/);
  });

  it('bakes a CONST gradient table when every field folds', () => {
    const c = gen(
      arc(`strokeGrad={{type: 1, ax: 0, ay: 0, bx: 200, by: 0,
        stops: [{color: '#F2A64B', offset: 0}, {color: '#4FA9F5', offset: 1}]}}`),
    );
    expect(c).toContain('static const ERVectorGradient s_svg0_grads[]');
    expect(c).not.toContain('static ERVectorGradient s_svg0_grads[1];');
  });

  it('leaves shapes without a gradient solid (index 0, no table)', () => {
    const c = gen(arc(''));
    expect(c).not.toContain('s_svg0_grads');
    expect(c).toMatch(/er_node_set_vector_ops\([^)]*NULL, 0\)/);
  });

  it('rejects a non-constant gradient type — it decides the emitted table shape', () => {
    expect(() =>
      gen(
        arc(`strokeGrad={{type: lo, ax: 0, ay: 0,
          stops: [{color: '#F2A64B', offset: 0}, {color: '#4FA9F5', offset: 1}]}}`),
      ),
    ).toThrow(/type.*compile-time constant/);
  });

  it('rejects a stop count the engine table cannot hold', () => {
    const many = Array.from(
      {length: 9},
      (_, i) => `{color: '#F2A64B', offset: ${i / 9}}`,
    ).join(', ');
    expect(() =>
      gen(
        arc(
          `strokeGrad={{type: 1, ax: 0, ay: 0, bx: 1, by: 0, stops: [${many}]}}`,
        ),
      ),
    ).toThrow(/stops.*2\.\.8/);
  });
});

// <Svg> children are shape descriptions, not nodes, so the compiler walks the JSX itself — and used to
// see only direct JSXElements. A fragment around a shape matched nothing and was dropped from the
// generated C without a word, on both the static and the state-driven path.
describe('AOT <Svg> child unwrapping', () => {
  const svg = body => `${PRE}
    import { Svg, Circle, Line, Arc, G } from 'embedded-react';
    export function App() {
      const [t, setT] = useState(0);
      return (
        <Pressable onPress={() => setT(t + 1)}>
          <Svg width={100} height={100}>${body}</Svg>
        </Pressable>
      );
    }`;

  it('inlines a fragment in a static <Svg>, in source order', () => {
    const c = gen(
      svg(`
        <><Circle cx={50} cy={50} r={40} fill="#ff0000" /></>
        <Line x1={0} y1={0} x2={10} y2={10} stroke="#00ff00" />
      `),
    );
    // Both shapes reach the paint table — the fragment used to cost the Circle.
    expect(c).toContain('static const float s_svg0_ops[]'); // the static path (jsxToSvgElement)
    expect(c).toMatch(/s_svg0_paints, 2, NULL, 0\);/);
    expect(c).toContain('.fill = 4294901760u'); // the fragment's Circle (#ff0000)
    expect(c).toContain('.stroke = 4278255360u'); // the Line after it (#00ff00)
    expect(c.indexOf('4294901760u')).toBeLessThan(c.indexOf('4278255360u'));
  });

  it('inlines nested fragments, and one inside a <G>', () => {
    const c = gen(
      svg(`
        <><><Circle cx={10} cy={10} r={5} fill="#ff0000" /></></>
        <G fill="#0000ff"><><Circle cx={20} cy={20} r={5} /></></G>
      `),
    );
    expect(c).toContain('static const float s_svg0_ops[]');
    expect(c).toMatch(/s_svg0_paints, 2, NULL, 0\);/);
    expect(c).toContain('.fill = 4278190335u'); // the <G> fill inherited through the fragment
  });

  it('a fragment does not hide a state-driven attribute from the dynamic-path check', () => {
    // svgHasDynamic decides static vs state-driven; if it cannot see into the fragment the <Svg> goes
    // down the static path and evalStatic throws on the state reference.
    const c = gen(
      svg(
        `<><Arc cx={50} cy={50} r={40} startAngle={-135} endAngle={t * 2} stroke="#f4a261" strokeWidth={8} /></>`,
      ),
    );
    expect(c).toContain('static void build_svg0(void)');
    expect(c).toMatch(/static float s_svg0_ops\[\d+\];/); // mutable tape → the dynamic path ran
    expect(c).toContain('app_mul(s_state.t, 2)');
  });

  it('inlines a fragment in a state-driven <Svg>', () => {
    const c = gen(
      svg(`
        <Arc cx={50} cy={50} r={40} startAngle={-135} endAngle={t * 2} stroke="#f4a261" strokeWidth={8} />
        <><Line x1={0} y1={0} x2={10} y2={10} stroke="#00ff00" /></>
      `),
    );
    expect(c).toContain('static void build_svg0(void)');
    expect(c).toMatch(/s_svg0_paints, 2, NULL, 0\);/); // the fragment's Line is in the table
    expect(c).toContain('.stroke = 4278255360u');
  });

  it('throws on a child it cannot lower instead of dropping it', () => {
    const dynamic = /dynamic <Svg> children/;
    // static path
    expect(() =>
      gen(svg(`{[1, 2].map(i => <Circle cx={i} cy={i} r={1} fill="#fff" />)}`)),
    ).toThrow(dynamic);
    // state-driven path (a `t`-driven sibling forces it) — used to compile, silently short a shape
    expect(() =>
      gen(
        svg(`
          <Arc cx={50} cy={50} r={40} startAngle={-135} endAngle={t * 2} stroke="#f4a261" strokeWidth={8} />
          {[1, 2].map(i => <Circle cx={i} cy={i} r={1} fill="#fff" />)}
        `),
      ),
    ).toThrow(dynamic);
    expect(() =>
      gen(svg(`hello<Circle cx={1} cy={1} r={1} fill="#fff" />`)),
    ).toThrow(/cannot draw text/);
  });

  it('still skips whitespace and JSX comments', () => {
    const c = gen(
      svg(`
        {/* the dial face */}
        <Circle cx={50} cy={50} r={40} fill="#ff0000" />
      `),
    );
    expect(c).toMatch(/s_svg0_paints, 1, NULL, 0\);/);
  });
});

describe('AOT arithmetic semantics', () => {
  // JS `/` is always floating point. Emitting it verbatim gave C INTEGER division whenever both sides
  // happened to be ints, which silently zeroed every ratio built from integer state: a dial driven by
  // `(sp - MINF) / (MAXF - MINF)` sat at the bottom of its sweep for every value below the maximum and
  // snapped to the top at it.
  it('divides as float even when both operands are integers', () => {
    const c = gen(`${PRE}
      export function App() {
        const [sp, setSp] = useState(72);
        return (<View style={{ width: ((sp - 50) / (90 - 50)) * 240 }}><Text>x</Text></View>);
      }`);
    expect(c).toContain('(float)(app_sub(s_state.sp, 50)) / (float)(40)');
    // The bare int division that produced the bug must not survive.
    expect(c).not.toContain('app_sub(s_state.sp, 50) / 40');
  });

  it('keeps + - * on integers whole, through the saturating helpers', () => {
    const c = gen(`${PRE}
      export function App() {
        const [n, setN] = useState(3);
        return (<View style={{ width: (n + 2) * 4 }}><Text>x</Text></View>);
      }`);
    expect(c).toContain('app_mul(app_add(s_state.n, 2), 4)');
  });

  // C leaves a signed overflow undefined, and JS would give a number an int cannot hold. The helpers
  // saturate instead: the nearest value the type has, with the sign and order JS would have.
  it('lowers + - * and negation on whole numbers to saturating helpers', () => {
    const c = gen(`${PRE}
      export function App() {
        const [n, setN] = useState(3);
        return (<Pressable onPress={() => setN(-(n * 3 - 1) + 2)}><Text>{n}</Text></Pressable>);
      }`);
    expect(c).toContain(
      's_state.n = app_add(app_neg(app_sub(app_mul(s_state.n, 3), 1)), 2);',
    );
    expect(c).toContain('static int app_add(int a, int b)');
    expect(c).toContain(
      'return v > INT_MAX ? INT_MAX : v < INT_MIN ? INT_MIN : (int)v;',
    );
    expect(c).toContain('return a == INT_MIN ? INT_MAX : -a;');
    expect(c).toContain('#include <limits.h>');
  });

  it('emits only the helpers the app calls, and no <limits.h> without them', () => {
    const one = gen(`${PRE}
      export function App() {
        const [n, setN] = useState(3);
        return (<Pressable onPress={() => setN(n + 1)}><Text>{n}</Text></Pressable>);
      }`);
    expect(one).toContain('static int app_add(int a, int b)');
    expect(one).not.toMatch(/static int(64_t)? app_(sub|mul|neg|add64)/);
    const none = gen(`${PRE}
      export function App() {
        const [n, setN] = useState(3);
        return (<Pressable onPress={() => setN(0)}><Text>{n}</Text></Pressable>);
      }`);
    expect(none).not.toMatch(/app_(add|sub|mul|neg)/);
    expect(none).not.toContain('#include <limits.h>');
  });

  it('steps a whole-number ref through the helpers, and leaves float math alone', () => {
    const c = gen(`${PRE}
      import { useRef } from 'react';
      export function App() {
        const [f, setF] = useState(0.5);
        const acc = useRef(0);
        const pos = useRef(0.5);
        return (<Pressable onPress={() => { acc.current += 2; acc.current -= f; acc.current *= 3; acc.current--; pos.current += 1; pos.current++; setF(f * 2 - 1); }}><Text>{f}</Text></Pressable>);
      }`);
    expect(c).toContain('s_ref_acc = app_add(s_ref_acc, 2);');
    expect(c).toContain('s_ref_acc = app_f2i((s_ref_acc - s_state.f));');
    expect(c).toContain('s_ref_acc = app_mul(s_ref_acc, 3);');
    expect(c).toContain('s_ref_acc = app_sub(s_ref_acc, 1);');
    expect(c).toContain('s_ref_pos += 1;');
    expect(c).toContain('s_ref_pos++;');
    expect(c).toContain('s_state.f = ((s_state.f * 2) - 1);');
  });

  it('works out constant math at compile time, exactly as JS does', () => {
    const c = gen(`${PRE}
      const LOW = -2147483647 - 1;
      const STEP = 2;
      export function App() {
        const [n, setN] = useState(0);
        return (<Pressable onPress={() => { setN(n + STEP * 3); setN(LOW); setN(-LOW - 1); }}><Text>{n}</Text></Pressable>);
      }`);
    expect(c).toContain('s_state.n = app_add(s_state.n, 6);');
    // `-2147483648` would be `-` applied to 2147483648, which does not fit an int.
    expect(c).toContain('s_state.n = (-2147483647 - 1);');
    // -LOW is 2^31, past an int, but only on the way to a result that fits.
    expect(c).toContain('s_state.n = 2147483647;');
    expect(c).toContain('    int n;');
  });

  it('types a constant past the int range as 64-bit, and as a plain number beside a float', () => {
    const c = gen(`${PRE}
      const BIG = 3000000000;
      export function App() {
        const [n, setN] = useState(0);
        const [f, setF] = useState(0.5);
        return (<Pressable onPress={() => { setN(n * (BIG - 1000)); setF(f * BIG); }}><Text>{n}</Text></Pressable>);
      }`);
    // Widened to hold the product, as a slot is for a timestamp.
    expect(c).toContain('    int64_t n;');
    expect(c).toContain('s_state.n = app_mul64(s_state.n, 2999999000);');
    expect(c).toContain('s_state.f = (s_state.f * ((float)3000000000));');
  });

  // JS gives NaN or ±Infinity for a zero divisor, which an int cannot hold, and C traps or overflows. The
  // answer is JS's, kept whole the way a float is: NaN → 0, ±Infinity → the int limit.
  it('keeps integer % and /= by a runtime divisor defined', () => {
    const c = gen(`${PRE}
      import { useRef } from 'react';
      export function App() {
        const [n, setN] = useState(7);
        const [d, setD] = useState(0);
        const q = useRef(9);
        return (<Pressable onPress={() => { setN(n % d); q.current /= d; q.current %= d; setD(n % 24); setN(n % -1); q.current /= 4; q.current %= 5; q.current /= -1; q.current %= 0; }}><Text>{n}</Text></Pressable>);
      }`);
    expect(c).toContain('s_state.n = app_mod(s_state.n, s_state.d);');
    expect(c).toContain('s_ref_q = app_div(s_ref_q, s_state.d);');
    expect(c).toContain('s_ref_q = app_mod(s_ref_q, s_state.d);');
    // A constant divisor other than 0 and -1 needs no check...
    expect(c).toContain('s_state.d = (s_state.n % 24);');
    expect(c).toContain('s_ref_q /= 4;');
    expect(c).toContain('s_ref_q %= 5;');
    // ...and those two leave no remainder, or only a quotient that saturates.
    expect(c).toContain('s_state.n = 0;');
    expect(c).toContain('s_ref_q = 0;');
    expect(c).toContain('s_ref_q = app_div(s_ref_q, -1);');
    expect(c).toContain('return (b == 0 || b == -1) ? 0 : a % b;');
    expect(c).toContain('return a > 0 ? INT_MAX : a < 0 ? INT_MIN : 0;');
  });

  it('stores a float in an int slot through a saturating conversion', () => {
    const c = gen(`${PRE}
      import { useRef } from 'react';
      export function App() {
        const [n, setN] = useState(0);
        const [f, setF] = useState(0.5);
        const [items, setItems] = useState([{w: 1}]);
        const r = useRef(0);
        return (<Pressable onPress={() => { setN(f * 2); r.current = f; r.current += f; r.current /= f; setItems([...items, {w: f}]); setItems(items.slice(0, f)); }}><Text>{n}</Text></Pressable>);
      }`);
    expect(c).toContain('s_state.n = app_f2i((s_state.f * 2));');
    expect(c).toContain('s_ref_r = app_f2i(s_state.f);');
    expect(c).toContain('s_ref_r = app_f2i((s_ref_r + s_state.f));');
    expect(c).toContain(
      's_ref_r = app_f2i(((float)(s_ref_r) / (float)(s_state.f)));',
    );
    expect(c).toMatch(/s_items\[[^\]]+\]\.w = app_f2i\(s_state\.f\);/);
    expect(c).toContain(
      's_items_count = app_slice_len(s_items_count, app_f2i(s_state.f));',
    );
    expect(c).toContain('if (v >= 2147483648.0f)');
    expect(c).toContain('if (v != v)');
  });

  it('rounds a float to an int like JS, and leaves an int as it is', () => {
    const c = gen(`${PRE}
      export function App() {
        const [n, setN] = useState(3);
        const [f, setF] = useState(0.5);
        return (<View><Text>{Math.round(f)}</Text><Text>{Math.floor(f)}</Text><Text>{Math.ceil(f)}</Text><Text>{Math.round(n) + Math.floor(n)}</Text></View>);
      }`);
    expect(c).toContain('"%d", app_f2i(app_roundf((float)(s_state.f)))');
    expect(c).toContain('"%d", app_f2i(floorf((float)(s_state.f)))');
    expect(c).toContain('"%d", app_f2i(ceilf((float)(s_state.f)))');
    // No float round trip for an int, which would lose digits past 2^24.
    expect(c).toContain('"%d", app_add(s_state.n, s_state.n)');
    // Halves go up, as JS's Math.round does: floor, then one more at a remainder of 0.5.
    expect(c).toContain('return v - f >= 0.5f ? f + 1.0f : f;');
    expect(c).toContain('#include <math.h>');
  });

  it('converts a float opacity, timer delay and dirty rect the way Flow A does', () => {
    const c = gen(`${PRE}
      import { useRef } from 'react';
      import { Svg } from 'embedded-react';
      export function App() {
        const [f, setF] = useState(0.5);
        const bar = useRef(null);
        return (
          <View style={{ opacity: f }}>
            <Pressable onPress={() => { setTimeout(() => setF(0), f * 1000); updateVector(bar, [{ rect: [0, 0, f * 100, 10], fill: '#ffffff' }], [0, 0, f * 100, 10]); }}>
              <Svg ref={bar} width={100} height={10} />
            </Pressable>
          </View>
        );
      }`);
    expect(c).toContain('p.opacity = app_opacity(s_state.f);');
    expect(c).toContain('return (uint8_t)(v * 255.0f + 0.5f);');
    expect(c).toContain(
      'er_timer_add((int)(app_delay_msf((s_state.f * 1000))), false, er_timer_fn_0)',
    );
    expect(c).toMatch(
      /er_node_set_vector_dirty_rect\(\w+, 0, 0, app_f2i\(\(s_state\.f \* 100\)\), 10\);/,
    );
  });

  // A dimension folded at compile time goes through Math.round; one driven by state used to be handed to
  // int16 with a plain C cast, which truncates toward zero. That made the AOT disagree with ITSELF (and
  // with Flow A's bridge, which rounds) for any state-driven fractional size — the same half-pixel split
  // that put the thermostat's dial a pixel off across the two flows in issue #187.
  it('rounds a state-driven fractional size instead of truncating it', () => {
    const c = gen(`${PRE}
      export function App() {
        const [n, setN] = useState(3);
        return (<View style={{ width: n * 0.8875 }}><Text>x</Text></View>);
      }`);
    expect(c).toMatch(/p\.width = app_round_dim\(/);
    expect(c).not.toMatch(/p\.width = \(int16_t\)\(/);
    // The helper it calls has to come with it, and has to be floor(x + 0.5) — JS's rule, halves up.
    expect(c).toContain('static int16_t app_round_dim(double v)');
    expect(c).toContain('const double r = v + 0.5;');
    expect(c).toContain('if (v != v)');
    expect(c).toContain('if (v < -32768.0)');
    expect(c).toContain('if (v > 32767.0)');
  });

  it('leaves the rounding helper out of an app with no state-driven sizes', () => {
    const c = gen(`${PRE}
      export function App() {
        const [n, setN] = useState(3);
        return (<View style={{ width: 40 }}><Text>{n}</Text></View>);
      }`);
    expect(c).not.toContain('app_round_dim');
  });
});

describe('AOT touch drag', () => {
  it('wires onLayout / onTouchStart / onTouchMove and lowers e.layout.* + e.x to EREventData fields', () => {
    const c = gen(`${PRE}
      import { useRef, useCallback } from 'react';
      export function App() {
        const [v, setV] = useState(70);
        const cx = useRef(0);
        const onDrag = useCallback((e) => setV(e.x - cx.current), []);
        return (
          <View
            onLayout={(e) => { cx.current = e.layout.x + e.layout.width / 2; }}
            onTouchStart={onDrag}
            onTouchMove={onDrag}
          ><Text>{v}</Text></View>
        );
      }`);
    expect(c).toContain('ER_EVENT_LAYOUT');
    expect(c).toContain('ER_EVENT_TOUCH_START');
    expect(c).toContain('ER_EVENT_TOUCH_MOVE');
    // onLayout rect: x/y stay, width/height map to ERRect w/h
    expect(c).toContain(
      's_ref_cx = app_f2i((data->layout_rect.x + ((float)(data->layout_rect.w) / (float)(2))));',
    );
    // touch coord + ref read in the shared drag handler
    expect(c).toContain('static void er_cb_onDrag(');
    expect(c).toContain('s_state.v = app_sub(data->x, s_ref_cx);');
    // onTouchStart + onTouchMove reuse the one useCallback handler
    expect(
      c.match(
        /er_event_set\([^,]+, ER_EVENT_TOUCH_(START|MOVE), er_cb_onDrag, NULL\);/g,
      ),
    ).toHaveLength(2);
  });

  it('treats useState(70.0) as a FLOAT slot (decimal literal forces float, value stays sub-integer)', () => {
    const c = gen(`${PRE}
      export function App() {
        const [v, setV] = useState(70.0);
        return (<Pressable onPress={() => setV(v + 0.5)}><Text>{Math.round(v)}</Text></Pressable>);
      }`);
    expect(c).toContain('float v;'); // float struct field, not int
    expect(c).toContain('.v = 70.0f'); // valid C float literal (not 70f)
    expect(c).toContain('s_state.v = (s_state.v + 0.5f);');
    expect(c).toContain('app_f2i(app_roundf((float)(s_state.v)))'); // displayed rounded
  });

  it('keeps useState(70) an int slot (no decimal → no float widening)', () => {
    const c = gen(`${PRE}
      export function App() {
        const [v, setV] = useState(70);
        return (<Pressable onPress={() => setV(v + 1)}><Text>{v}</Text></Pressable>);
      }`);
    expect(c).toContain('int v;');
    expect(c).toContain('.v = 70');
    expect(c).not.toContain('float v;');
  });

  it('folds the negation of a negative constant, never the decrement token --135', () => {
    const c = gen(`${PRE}
      const A = -135;
      export function App() {
        const [v, setV] = useState(0);
        return (<Pressable onPress={(e) => setV(e.x > -A ? -A : e.x)}><Text>{v}</Text></Pressable>);
      }`);
    expect(c).toContain('s_state.v = ((data->x > 135) ? 135 : data->x);');
    expect(c).not.toContain('--135');
  });
});

describe('AOT PanResponder (lowered onto the engine responder system)', () => {
  const PAN_PRE = `import { useState, useRef } from 'react';
import { View, Pressable, ScrollView, Text, PanResponder } from 'embedded-react';
`;

  it('lowers a should-set predicate to a responder QUERY and the callbacks to responder EVENTS', () => {
    const c = gen(`${PAN_PRE}
      export function App() {
        const [x, setX] = useState(0);
        const pan = useRef(PanResponder.create({
          onStartShouldSetPanResponder: () => true,
          onPanResponderMove: (e, g) => setX(g.dx),
        })).current;
        return (<View {...pan.panHandlers}><Text>{x}</Text></View>);
      }`);
    expect(c).toContain(
      'static bool er_pan_pan_start_should_set(ERNode* node, const EREventData* data, void* user_data)',
    );
    expect(c).toMatch(
      /er_responder_query_set\(n\d+, ER_QUERY_START_SHOULD_SET, er_pan_pan_start_should_set, NULL\);/,
    );
    expect(c).toMatch(
      /er_event_set\(n\d+, ER_EVENT_RESPONDER_MOVE, er_pan_pan_move, NULL\);/,
    );
    // No transpiled state machine: the gesture is the engine's, not a JS one copied into C.
    expect(c).not.toContain('ER_EVENT_TOUCH_MOVE');
  });

  it('anchors g.dx on the GRANT so a claim that cost slop opens at zero', () => {
    const c = gen(`${PAN_PRE}
      export function App() {
        const [x, setX] = useState(0);
        const pan = useRef(PanResponder.create({
          onMoveShouldSetPanResponder: (e, g) => Math.abs(g.dx) > 8,
          onPanResponderMove: (e, g) => setX(g.dx),
        })).current;
        return (<View {...pan.panHandlers}><Text>{x}</Text></View>);
      }`);
    expect(c).toContain('static int s_pan_pan_base_dx = 0;');
    expect(c).toContain('s_pan_pan_base_dx = data->dx;'); // set at the grant
    expect(c).toContain('s_state.x = (data->dx - s_pan_pan_base_dx);'); // read relative to it
    expect(c).toContain('s_pan_pan_base_dx = 0;'); // cleared when the gesture ends
  });

  it('always emits grant/release/terminate, even with only a move callback (they own the anchor)', () => {
    const c = gen(`${PAN_PRE}
      export function App() {
        const [x, setX] = useState(0);
        const pan = useRef(PanResponder.create({
          onPanResponderMove: (e, g) => setX(g.dx),
        })).current;
        return (<View {...pan.panHandlers}><Text>{x}</Text></View>);
      }`);
    for (const evt of ['GRANT', 'RELEASE', 'TERMINATE', 'MOVE'])
      expect(c).toContain(`ER_EVENT_RESPONDER_${evt}`);
    expect(c).not.toContain('ER_EVENT_RESPONDER_REJECT'); // not configured → not wired
  });

  it('maps every gestureState field, including the engine-supplied velocity and finger count', () => {
    const c = gen(`${PAN_PRE}
      export function App() {
        const [x, setX] = useState(0);
        const pan = useRef(PanResponder.create({
          onPanResponderRelease: (e, g) =>
            setX(g.moveX + g.moveY + g.x0 + g.y0 + g.dy + g.numberActiveTouches + g.stateID),
          onPanResponderTerminate: (e, g) => setX(g.vx > 0.4 ? 1 : 0),
        })).current;
        return (<View {...pan.panHandlers}><Text>{x}</Text></View>);
      }`);
    expect(c).toContain('app_add(data->x, data->y)'); // moveX, moveY
    expect(c).toContain('s_pan_pan_x0), s_pan_pan_y0)'); // x0, y0 — the grant point
    expect(c).toContain('er_touch_active_count()'); // numberActiveTouches
    expect(c).toContain('(data->dy - s_pan_pan_base_dy)'); // dy, grant-relative
    expect(c).toContain('(data->vx > 0.4f)'); // vx straight off the payload
  });

  it('wires the capture predicates and the termination request', () => {
    const c = gen(`${PAN_PRE}
      export function App() {
        const [x, setX] = useState(0);
        const pan = useRef(PanResponder.create({
          onStartShouldSetPanResponderCapture: () => false,
          onMoveShouldSetPanResponderCapture: (e) => e.y < 40,
          onPanResponderTerminationRequest: () => false,
          onPanResponderReject: () => setX(1),
        })).current;
        return (<View {...pan.panHandlers}><Text>{x}</Text></View>);
      }`);
    expect(c).toContain('ER_QUERY_START_SHOULD_SET_CAPTURE');
    expect(c).toContain('ER_QUERY_MOVE_SHOULD_SET_CAPTURE');
    expect(c).toContain('ER_QUERY_TERMINATION_REQUEST');
    expect(c).toContain('ER_EVENT_RESPONDER_REJECT');
    expect(c).toContain('return ((data->y < 40)) != 0;');
  });

  it('emits ONE set of callbacks however many nodes spread the same responder', () => {
    const c = gen(`${PAN_PRE}
      export function App() {
        const [x, setX] = useState(0);
        const pan = useRef(PanResponder.create({
          onPanResponderMove: (e, g) => setX(g.dx),
        })).current;
        return (
          <View>
            <View style={{ width: 10, height: 10 }} {...pan.panHandlers} />
            <View style={{ width: 10, height: 10 }} {...pan.panHandlers} />
          </View>
        );
      }`);
    expect(c.match(/static void er_pan_pan_move\(/g)).toHaveLength(1);
    expect(c.match(/static int s_pan_pan_granted/g)).toHaveLength(1);
    expect(
      c.match(/ER_EVENT_RESPONDER_MOVE, er_pan_pan_move, NULL/g),
    ).toHaveLength(2);
  });

  it('costs nothing when a responder is declared but never spread', () => {
    const c = gen(`${PAN_PRE}
      export function App() {
        const [x] = useState(0);
        const pan = useRef(PanResponder.create({ onPanResponderMove: () => {} })).current;
        return (<View><Text>{x}</Text></View>);
      }`);
    expect(c).not.toContain('s_pan_');
    expect(c).not.toContain('er_pan_');
  });

  it('accepts the useRef(...) form read back as pan.current.panHandlers', () => {
    const c = gen(`${PAN_PRE}
      export function App() {
        const [x, setX] = useState(0);
        const pan = useRef(PanResponder.create({
          onPanResponderMove: (e, g) => setX(g.dx),
        }));
        return (<View {...pan.current.panHandlers}><Text>{x}</Text></View>);
      }`);
    expect(c).toContain('ER_EVENT_RESPONDER_MOVE');
  });

  it('accepts shorthand-method callbacks as well as arrow properties', () => {
    const c = gen(`${PAN_PRE}
      export function App() {
        const [x, setX] = useState(0);
        const pan = useRef(PanResponder.create({
          onStartShouldSetPanResponder() { return true; },
          onPanResponderMove(e, g) { setX(g.dx); },
        })).current;
        return (<View {...pan.panHandlers}><Text>{x}</Text></View>);
      }`);
    expect(c).toContain('return (1) != 0;');
    expect(c).toContain('s_state.x = (data->dx - s_pan_pan_base_dx);');
  });

  it('gives each instance of a child component its own gesture state', () => {
    const c = gen(`${PAN_PRE}
      function Knob() {
        const [x, setX] = useState(0);
        const pan = useRef(PanResponder.create({
          onPanResponderMove: (e, g) => setX(g.dx),
        })).current;
        return (<View style={{ width: 40, height: 40 }} {...pan.panHandlers}><Text>{x}</Text></View>);
      }
      export function App() {
        return (<View><Knob /><Knob /></View>);
      }`);
    expect(c).toContain('static int s_pan_c0_pan_granted');
    expect(c).toContain('static int s_pan_c1_pan_granted');
  });

  it('still rejects a spread that is not a PanResponder', () => {
    expect(() =>
      gen(`${PAN_PRE}
      const extra = {};
      export function App() { return (<View {...extra} />); }`),
    ).toThrow(/a spread \{\.\.\.\} on <View> is not supported/);
  });

  it('rejects a PanResponder that is not kept in a useRef', () => {
    expect(() =>
      gen(`${PAN_PRE}
      export function App() {
        const [x, setX] = useState(0);
        const pan = PanResponder.create({ onPanResponderMove: (e, g) => setX(g.dx) });
        return (<View {...pan.panHandlers}><Text>{x}</Text></View>);
      }`),
    ).toThrow(/must be kept in a useRef/);
  });

  it('rejects an unknown config key instead of silently dropping it', () => {
    expect(() =>
      gen(`${PAN_PRE}
      export function App() {
        const pan = useRef(PanResponder.create({ onPanResponderWobble: () => {} })).current;
        return (<View {...pan.panHandlers} />);
      }`),
    ).toThrow(/unknown PanResponder config key "onPanResponderWobble"/);
  });

  it('names the Flow-A-only multi-finger callbacks rather than ignoring them', () => {
    expect(() =>
      gen(`${PAN_PRE}
      export function App() {
        const pan = useRef(PanResponder.create({ onPanResponderStart: () => {} })).current;
        return (<View {...pan.panHandlers} />);
      }`),
    ).toThrow(/"onPanResponderStart" is not supported in Flow B/);
  });

  it('rejects a should-set predicate that is not a single expression', () => {
    expect(() =>
      gen(`${PAN_PRE}
      export function App() {
        const [x, setX] = useState(0);
        const pan = useRef(PanResponder.create({
          onStartShouldSetPanResponder: (e, g) => { setX(1); return true; },
        })).current;
        return (<View {...pan.panHandlers}><Text>{x}</Text></View>);
      }`),
    ).toThrow(/must be a single boolean expression/);
  });

  it('rejects an unknown gestureState field', () => {
    expect(() =>
      gen(`${PAN_PRE}
      export function App() {
        const [x, setX] = useState(0);
        const pan = useRef(PanResponder.create({
          onPanResponderMove: (e, g) => setX(g.dz),
        })).current;
        return (<View {...pan.panHandlers}><Text>{x}</Text></View>);
      }`),
    ).toThrow(/unknown gestureState field "dz"/);
  });

  it('rejects a spread whose shape disagrees with how the responder was declared', () => {
    expect(() =>
      gen(`${PAN_PRE}
      export function App() {
        const [x, setX] = useState(0);
        const pan = useRef(PanResponder.create({
          onPanResponderMove: (e, g) => setX(g.dx),
        }));
        return (<View {...pan.panHandlers}><Text>{x}</Text></View>);
      }`),
    ).toThrow(/does not match how it was declared/);
  });

  it('rejects a PanResponder spread onto a component instance', () => {
    expect(() =>
      gen(`${PAN_PRE}
      function Box() { return (<View style={{ width: 10, height: 10 }} />); }
      export function App() {
        const [x, setX] = useState(0);
        const pan = useRef(PanResponder.create({
          onPanResponderMove: (e, g) => setX(g.dx),
        })).current;
        return (<View><Box {...pan.panHandlers} /><Text>{x}</Text></View>);
      }`),
    ).toThrow(/can only be spread onto a host element/);
  });
});

describe('AOT raw touch gesture fields', () => {
  it('lowers e.vx / e.vy so a plain onTouchEnd can recognise a flick', () => {
    const c = gen(`${PRE}
      export function App() {
        const [p, setP] = useState(0);
        return (
          <View onTouchEnd={(e) => setP(e.vx > 0.4 ? 1 : 0)}><Text>{p}</Text></View>
        );
      }`);
    expect(c).toContain('ER_EVENT_TOUCH_END');
    expect(c).toContain('s_state.p = ((data->vx > 0.4f) ? 1 : 0);');
  });

  it('lowers e.dx / e.dy on a raw touch handler (the engine fills them now)', () => {
    const c = gen(`${PRE}
      export function App() {
        const [d, setD] = useState(0);
        return (<View onTouchMove={(e) => setD(e.dx)}><Text>{d}</Text></View>);
      }`);
    expect(c).toContain('s_state.d = data->dx;');
  });
});

describe('AOT diagnostics', () => {
  it('locates an unsupported construct with file:line:col + a code-frame caret', () => {
    const src = `${PRE}
      export function App() {
        const [n, setN] = useState(0);
        return (<Pressable onPress={() => setN(window.x)}><Text>{n}</Text></Pressable>);
      }`;
    let err;
    try {
      compileSource(src, 'demo', {filename: 'demos/demo/App.jsx'});
    } catch (e) {
      err = e;
    }
    expect(err).toBeDefined();
    expect(err.message).toContain('demos/demo/App.jsx:'); // file:line:col
    expect(err.aotLoc).toBeTruthy(); // structured location preserved
    expect(err.message).toContain('^'); // code-frame caret
  });

  // A responsive app folds its layout from `screen`, so the size baked into the build picks the branch
  // the compiler walks. Compile the thermostat at the default 800x480 and it walks the split layout —
  // Flow A only — and reports whatever unsupported thing lives there, with nothing tying the failure
  // back to the size. Every located error names the size now.
  // SCREEN_W/H are read once at import, so each case re-imports the compiler under a stubbed env —
  // which also keeps these from depending on what ER_AOT_SCREEN_W/H happen to be in the shell.
  const errAtScreen = async (w, h) => {
    vi.stubEnv('ER_AOT_SCREEN_W', w);
    vi.stubEnv('ER_AOT_SCREEN_H', h);
    vi.resetModules();
    const {compileSource: compileAtScreen} = await import('../compile.mjs');
    try {
      compileAtScreen(
        `${PRE}\nexport function App() { const [n, setN] = useState(0); return (<Pressable onPress={() => setN(window.x)}><Text>{n}</Text></Pressable>); }`,
        'demo',
      );
    } catch (e) {
      return e;
    } finally {
      vi.unstubAllEnvs();
      vi.resetModules();
    }
  };

  it('names the screen size the layout was folded at, and flags the default', async () => {
    const err = await errAtScreen(undefined, undefined);
    expect(err.message).toContain('screen: 800×480');
    expect(err.message).toMatch(/did not supply both dimensions/);
    expect(err.message).toMatch(/branch meant for another board/);
    expect(err.message.trimEnd().endsWith('another board.')).toBe(true); // a footer, below the hint
  });

  it('credits ER_AOT_SCREEN_W/H when the size came from the environment', async () => {
    const err = await errAtScreen('240', '320');
    expect(err.message).toContain('screen: 240×320 (from ER_AOT_SCREEN_W/H).');
    expect(err.message).not.toMatch(/did not supply both dimensions/);
  });

  it('does not credit the environment when only one dimension is usable', async () => {
    const err = await errAtScreen('nonsense', '320');
    expect(err.message).toContain('screen: 800×320'); // width fell back, height did not
    expect(err.message).toMatch(/did not supply both dimensions/);
  });

  it('attaches a rewrite hint to a known error', () => {
    let err;
    try {
      compileSource(
        `${PRE}\nexport function App() { return (<View><Gauge /></View>); }`,
        'demo',
      );
    } catch (e) {
      err = e;
    }
    expect(err.message).toContain('hint:');
    expect(err.message).toContain('built-in');
  });

  it('points at the default demo path when no filename is given', () => {
    let err;
    try {
      compileSource(
        `${PRE}\nexport function App() { return (<Nope />); }`,
        'mydemo',
      );
    } catch (e) {
      err = e;
    }
    expect(err.message).toContain('demos/mydemo/App.jsx:');
  });

  // A spread on a HOST element used to be silently dropped (the style/event loops only read named
  // attributes) — it must now error clearly, like the typed components already do.
  it('rejects a spread on a host element instead of silently dropping it', () => {
    expect(() =>
      gen(
        `${PRE}\nexport function App() { const o = window.x; return (<View {...o} />); }`,
      ),
    ).toThrow(/spread \{\.\.\.\} on <View> is not supported/);
  });

  it('locates + hints a non-constant useState initial (was a bare evalStatic leak)', () => {
    let err;
    try {
      compileSource(
        `${PRE}\nexport function App() { const [s] = useState(window.y); return (<Text>{s}</Text>); }`,
        'demo',
        {
          filename: 'demos/demo/App.jsx',
        },
      );
    } catch (e) {
      err = e;
    }
    expect(err.message).toContain('must be a compile-time constant');
    expect(err.message).toContain('demos/demo/App.jsx:'); // located, not a bare leak
    expect(err.message).toContain('hint:');
  });

  it('locates a malformed list-state item shape at the initial', () => {
    let err;
    try {
      compileSource(
        `${PRE}\nexport function App() { const [a] = useState([1, 2]); return (<View>{a.map((x) => (<Text key={x}>{x}</Text>))}</View>); }`,
        'demo',
        {
          filename: 'demos/demo/App.jsx',
        },
      );
    } catch (e) {
      err = e;
    }
    expect(err.message).toMatch(/elements must be objects/);
    expect(err.message).toContain('demos/demo/App.jsx:');
    expect(err.message).toContain('hint:');
  });
});

describe('AOT effects & timers', () => {
  it('runs useEffect(fn, []) once on mount and registers a setInterval timer + er_app_tick', () => {
    const c = gen(`${PRE}
      export function App() {
        const [t, setT] = useState(0);
        useEffect(() => { const id = setInterval(() => setT((p) => p + 1), 250); return () => clearInterval(id); }, []);
        return (<Text>{t}</Text>);
      }`);
    expect(c).toContain('void er_app_tick(int dt_ms)'); // host-tick timer driver
    expect(c).toContain('er_timer_add((int)(250), true, er_timer_fn_0)'); // repeating timer
    expect(c).toContain('static void er_timer_fn_0(void)'); // callback → parameterless C fn
    expect(c).toContain('s_state.t = app_add(s_state.t, 1);'); // setT(p => p+1) in the callback
    expect(c).toContain('/* useEffect(fn, []) — run once on mount. */'); // body runs in er_app_build
    expect(c).not.toContain('clearInterval'); // the cleanup return is dropped (never unmounts)
  });

  it('lowers setTimeout to a one-shot timer and clearTimeout(id) to er_timer_clear', () => {
    const c = gen(`${PRE}
      export function App() {
        const [show, setShow] = useState(true);
        return (<Pressable onPress={() => { const id = setTimeout(() => setShow(false), 3000); clearTimeout(id); }}><Text>x</Text></Pressable>);
      }`);
    expect(c).toContain('er_timer_add((int)(3000), false, er_timer_fn_0)'); // one-shot (repeat=false)
    expect(c).toContain('er_timer_clear(l_id);');
  });

  it('emits a no-op er_app_tick when the app uses no timers', () => {
    const c = gen(
      `${PRE}\nexport function App() { return (<Text>hi</Text>); }`,
    );
    expect(c).toContain('void er_app_tick(int dt_ms)');
    expect(c).toContain('(void)dt_ms;');
    expect(c).not.toContain('er_timer_add');
  });

  it('emits er_timer_clear only when something calls it', () => {
    // A mount effect's cleanup is dropped (the app never unmounts), so nothing calls the clear here.
    const mountOnly = gen(`${PRE}
      export function App() {
        const [n, setN] = useState(0);
        useEffect(() => { const id = setInterval(() => setN((v) => v + 1), 1000); return () => clearInterval(id); }, []);
        return (<Text>{n}</Text>);
      }`);
    expect(mountOnly).toContain(
      'er_timer_add((int)(1000), true, er_timer_fn_0)',
    );
    expect(mountOnly).not.toContain('er_timer_clear');

    const fromHandler = gen(`${PRE}
      export function App() {
        const [n, setN] = useState(0);
        return (<Pressable onPress={() => { const id = setInterval(() => setN((v) => v + 1), 1000); clearInterval(id); }}><Text>{n}</Text></Pressable>);
      }`);
    expect(fromHandler).toContain('static void er_timer_clear(int id)');
    expect(fromHandler).toContain('er_timer_clear(l_id);');

    const fromDepCleanup = gen(`${PRE}
      export function App() {
        const [page, setPage] = useState(0);
        const [n, setN] = useState(0);
        useEffect(() => { const id = setInterval(() => setN((v) => v + 1), 1000); return () => clearInterval(id); }, [page]);
        return (<Pressable onPress={() => setPage(page + 1)}><Text>{n}</Text></Pressable>);
      }`);
    expect(fromDepCleanup).toContain('static void er_timer_clear(int id)');
    expect(fromDepCleanup).toContain('er_timer_clear(s_eff0_l_id);');
  });

  it('lowers an early return in a dependency-driven useEffect to a real `return`', () => {
    const c = gen(`${PRE}
      export function App() {
        const [page, setPage] = useState(0);
        const [t, setT] = useState(0);
        useEffect(() => {
          if (page !== 2) return undefined;
          const id = setInterval(() => setT((v) => (v + 1) % 24), 90);
          return () => clearInterval(id);
        }, [page]);
        return (<Pressable onPress={() => setPage(page + 1)}><Text>{t}</Text></Pressable>);
      }`);
    // The guard has to exit the effect — an empty `if` would start the timer on every page.
    expect(c).toMatch(
      /if \(\(s_state\.page != 2\)\)\n    \{\n        return;\n    \}/,
    );
    expect(c).toContain('er_timer_add((int)(90), true, er_timer_fn_0)');
  });

  it('runs a dependency-driven useEffect cleanup before re-running the body', () => {
    const c = gen(`${PRE}
      export function App() {
        const [page, setPage] = useState(0);
        const [t, setT] = useState(0);
        useEffect(() => {
          if (page !== 2) return undefined;
          const id = setInterval(() => setT((v) => (v + 1) % 24), 90);
          return () => clearInterval(id);
        }, [page]);
        return (<Pressable onPress={() => setPage(page + 1)}><Text>{t}</Text></Pressable>);
      }`);
    // The timer id has to outlive the call the cleanup closed over, so it becomes a file-scope slot.
    expect(c).toContain('static int s_eff0_l_id;');
    expect(c).toContain('static void er_effect_0_cleanup(void)');
    expect(c).toContain('er_timer_clear(s_eff0_l_id);');
    // The re-run clears the previous timer first, and only a run that reached the return arms a cleanup.
    expect(c).toMatch(
      /static void er_effect_0\(void\)\n\{\n    if \(s_eff0_armed\)\n    \{\n        s_eff0_armed = 0;\n        er_effect_0_cleanup\(\);\n    \}/,
    );
    expect(c).toMatch(/s_eff0_l_id = er_timer_add[\s\S]*?s_eff0_armed = 1;/);
  });

  it('arms no cleanup for a dependency-driven effect that returns nothing', () => {
    const c = gen(`${PRE}
      export function App() {
        const [n, setN] = useState(0);
        const [doubled, setDoubled] = useState(0);
        useEffect(() => { setDoubled(n * 2); }, [n]);
        return (<Pressable onPress={() => setN(n + 1)}><Text>{doubled}</Text></Pressable>);
      }`);
    expect(c).not.toContain('s_eff0_armed');
    expect(c).not.toContain('er_effect_0_cleanup');
  });

  it('rejects a dependency-driven cleanup returned from inside an if', () => {
    let err;
    try {
      gen(`${PRE}
      export function App() {
        const [page, setPage] = useState(0);
        const [t, setT] = useState(0);
        useEffect(() => {
          if (page === 2) {
            const id = setInterval(() => setT((v) => v + 1), 90);
            return () => clearInterval(id);
          }
        }, [page]);
        return (<Pressable onPress={() => setPage(page + 1)}><Text>{t}</Text></Pressable>);
      }`);
    } catch (e) {
      err = e;
    }
    expect(err.message).toMatch(
      /cleanup must be the last statement of the effect body/,
    );
    expect(err.message).toContain('demos/test/App.jsx:');
    expect(err.message).toContain('hint:');
  });

  it('tags timer ids with a generation so a stale clear cannot kill a reused slot', () => {
    const c = gen(`${PRE}
      export function App() {
        const [n, setN] = useState(0);
        return (<Pressable onPress={() => { const id = setTimeout(() => setN(1), 50); clearTimeout(id); }}><Text>{n}</Text></Pressable>);
      }`);
    expect(c).toContain('return (s_timers[i].gen * ER_AOT_MAX_TIMERS) + i;');
    expect(c).toContain(
      'if (s_timers[i].active && s_timers[i].gen == id / ER_AOT_MAX_TIMERS)',
    );
  });

  it('gives an early-returning mount effect a C function of its own', () => {
    const c = gen(`${PRE}
      export function App() {
        const [n, setN] = useState(0);
        useEffect(() => { if (n > 1) return; setN(2); }, []);
        useEffect(() => { setN(3); }, []);
        return (<Text>{n}</Text>);
      }`);
    // A mount body is inlined into er_app_build, where a bare `return` would skip the second effect.
    expect(c).toContain('static void er_effect_0(void)');
    expect(c).toMatch(
      /er_effect_0\(\);\n    app_update\(\);\n    s_state\.n = 3;/,
    );
  });

  it('keeps a plain mount effect inlined in er_app_build', () => {
    const c = gen(`${PRE}
      export function App() {
        const [n, setN] = useState(0);
        useEffect(() => { const id = setInterval(() => setN((v) => v + 1), 250); return () => clearInterval(id); }, []);
        return (<Text>{n}</Text>);
      }`);
    expect(c).not.toContain('static void er_effect_0(void)'); // a tail cleanup return needs no function
    expect(c).toMatch(/run once on mount\. \*\/\n    int l_id = er_timer_add/);
  });

  it('rejects an early return in an event handler with a located error', () => {
    let err;
    try {
      gen(`${PRE}
      export function App() {
        const [n, setN] = useState(0);
        return (<Pressable onPress={() => { if (n > 1) return; setN(n + 1); }}><Text>{n}</Text></Pressable>);
      }`);
    } catch (e) {
      err = e;
    }
    expect(err.message).toMatch(
      /`return` is only supported inside a useEffect body/,
    );
    expect(err.message).toContain('demos/test/App.jsx:');
    expect(err.message).toContain('hint:');
  });

  it('re-runs a dependency-driven useEffect from app_update when the dep changes', () => {
    const c = gen(`${PRE}
      export function App() {
        const [n, setN] = useState(0);
        const [doubled, setDoubled] = useState(0);
        useEffect(() => { setDoubled(n * 2); }, [n]);
        return (<Pressable onPress={() => setN(n + 1)}><Text>{doubled}</Text></Pressable>);
      }`);
    expect(c).toContain('static int s_eff0_d0;'); // a stored previous value for the dep
    expect(c).toContain('static void er_effect_0(void)'); // the effect body as a C fn
    expect(c).toContain('er_effect_0();'); // run at mount + on change
    // app_update detects the change and runs the effect
    expect(c).toMatch(/er_d0 = s_state\.n; if \(er_d0 != s_eff0_d0\)/);
    expect(c).toContain('if (er_changed) er_effect_0();');
  });

  it('compares a string useEffect dependency with strcmp', () => {
    const c = gen(`${PRE}
      export function App() {
        const [name, setName] = useState('Ada');
        const [len, setLen] = useState(0);
        useEffect(() => { setLen(1); }, [name]);
        return (<Pressable onPress={() => setName('Bob')}><Text>{len}</Text></Pressable>);
      }`);
    expect(c).toContain('static char s_eff0_d0['); // string prev buffer
    expect(c).toContain('strcmp(s_eff0_d0, s_state.name)'); // strcmp-based change detection
  });
});

describe('AOT Switch', () => {
  it('lowers a controlled <Switch> to ER_NODE_SWITCH; toggle fires PRESS and sets state to !value', () => {
    const c = gen(`${PRE}
      import { Switch } from 'embedded-react';
      export function App() {
        const [on, setOn] = useState(false);
        return (<Switch value={on} onValueChange={(v) => setOn(v)} />);
      }`);
    expect(c).toContain('er_node_create(ER_NODE_SWITCH)');
    expect(c).toContain('p.switch_value = (uint8_t)((s_state.on) ? 1 : 0);'); // state-driven value
    expect(c).toMatch(/er_event_set\(n\d+, ER_EVENT_PRESS,/); // engine toggles + fires PRESS
    expect(c).toContain('s_state.on = (!(s_state.on));'); // onValueChange's v param = the toggled value
    expect(c).toContain('p.width = 51;'); // default RN box (renderer scales track/thumb to it)
  });

  it('bakes static trackColor/thumbColor and lets a style size override the default box', () => {
    const c = gen(`${PRE}
      import { Switch } from 'embedded-react';
      export function App() {
        const [on, setOn] = useState(true);
        return (<Switch value={on} onValueChange={(v) => setOn(v)} trackColor={{ false: '#3a3f4a', true: '#2a9d8f' }} thumbColor="#ffffff" style={{ width: 64, height: 36 }} />);
      }`);
    expect(c).toContain('track_color_false');
    expect(c).toContain('track_color_true');
    expect(c).toContain('thumb_color');
    expect(c).toContain('p.width = 64;'); // style overrides the 51 default
    expect(c).not.toContain('p.width = 51;');
  });

  it('throws when a Switch has onValueChange but no value prop', () => {
    expect(() =>
      gen(`${PRE}
        import { Switch } from 'embedded-react';
        export function App() { const [on, setOn] = useState(false); return (<Switch onValueChange={(v) => setOn(v)} />); }`),
    ).toThrow(/needs a value prop/);
  });
});

describe('AOT Dial', () => {
  it('lowers a controlled <Dial> to ER_NODE_ARC; onChange binds its param to data->value', () => {
    const c = gen(`${PRE}
      import { Dial } from 'embedded-react';
      export function App() {
        const [temp, setTemp] = useState(21);
        return (<Dial value={temp} min={10} max={30} step={0.5} thickness={14} cap="round"
                      knob="circle" adjustable trackColor="#30343a" indicatorColor="#ff8800"
                      onChange={(v) => setTemp(v)} style={{ width: 200, height: 200 }} />);
      }`);
    expect(c).toContain('er_node_create(ER_NODE_ARC)');
    expect(c).toContain('p.arc_value = (float)(s_state.temp);'); // state-driven value (app_update)
    expect(c).toContain('p.arc_min = 10.0f;');
    expect(c).toContain('p.arc_max = 30.0f;');
    expect(c).toContain('p.arc_step = 0.5f;');
    expect(c).toContain('p.arc_width = 14;');
    expect(c).toContain('p.arc_cap = ER_ARC_CAP_ROUND;');
    expect(c).toContain('p.arc_knob = ER_ARC_KNOB_CIRCLE;');
    expect(c).toContain('p.arc_adjustable = 1;');
    expect(c).toContain('p.arc_track_color = 0xFF30343Au;');
    expect(c).toContain('p.arc_indicator_color = 0xFFFF8800u;');
    expect(c).toMatch(/er_event_set\(n\d+, ER_EVENT_VALUE_CHANGE,/);
    expect(c).toContain('s_state.temp = app_f2i(data->value);'); // onChange's v param = the engine's new value
    expect(c).toContain('p.width = 200;');
  });

  it('binds an animated value natively and bakes a conic indicator gradient', () => {
    const c = gen(`${PRE}
      import { Dial } from 'embedded-react';
      export function App() {
        const level = useAnimatedValue(0);
        return (<Dial value={level} max={100}
                      indicatorGradient={{ type: 'conic', stops: [{ color: '#0000ff' }, { color: '#ff0000' }] }} />);
      }`);
    expect(c).toMatch(/er_anim_value_bind\(\w+, n\d+, ER_PROP_ARC_VALUE\);/);
    expect(c).not.toContain('arc_value ='); // the value is the binding, never a marshalled prop
    expect(c).toContain('p.gradient_type = ER_GRADIENT_CONIC;');
    expect(c).toContain('p.gradient_stop_count = 2;');
    expect(c).toContain('p.gradient_stops[1].color = 0xFFFF0000u;');
    expect(c).toContain('p.gradient_stops[1].position = 1.0f;');
    expect(c).toContain('p.width = 120;'); // default box
  });

  // A percentage size lowers to width_pct, so the built-in 120x120 default must stand down for it —
  // the engine reads the pixel field first, and an injected default would win over the percentage.
  it('does not inject its default box over a percentage size', () => {
    const c = gen(`${PRE}
      import { Dial } from 'embedded-react';
      export function App() {
        return (<Dial value={50} style={{ width: '50%', height: '50%' }} />);
      }`);
    expect(c).toContain('p.width_pct = 50.0f;');
    expect(c).not.toContain('p.width = 120;');
    expect(c).not.toContain('p.height = 120;');
  });

  it('lowers RANGE mode: two ends, a state-driven knob/range, and a two-arg onChange', () => {
    const c = gen(`${PRE}
      import { Dial } from 'embedded-react';
      export function App() {
        const [lo, setLo] = useState(60);
        const [hi, setHi] = useState(76);
        const [mode, setMode] = useState('auto');
        return (<Dial range={mode === 'auto'} valueStart={lo} value={hi} min={50} max={90} minSpan={4}
                      knob={mode === 'off' ? 'none' : 'circle'} adjustable={mode !== 'off'}
                      indicatorGradient={mode === 'auto' ? {type: 'conic', stops: [{color: '#f2a64b'}, {color: '#4fa9f5'}]} : null}
                      onChange={(v, vLo) => { setHi(v); setLo(vLo); }} />);
      }`);
    expect(c).toContain('p.arc_value_start = (float)(s_state.lo);');
    expect(c).toContain('p.arc_min_span = 4.0f;'); // the pair keeps a minimum separation
    expect(c).toContain('p.arc_value = (float)(s_state.hi);');
    // range / adjustable / knob are all state-driven here, so they re-apply in app_update.
    expect(c).toMatch(/p\.arc_range = \(uint8_t\)\(\(.*\) \? 1 : 0\);/);
    expect(c).toMatch(/p\.arc_adjustable = \(uint8_t\)\(\(.*\) \? 1 : 0\);/);
    expect(c).toContain('ER_ARC_KNOB_NONE');
    expect(c).toContain('ER_ARC_KNOB_CIRCLE');
    // A conditional gradient bakes its stops and switches the COUNT (0 = no gradient).
    expect(c).toMatch(
      /p\.gradient_stop_count = \(uint8_t\)\(\(.*\) \? 2 : 0\);/,
    );
    // Both handler params bind to the event payload — no object allocated on device.
    expect(c).toContain('s_state.hi = app_f2i(data->value);');
    expect(c).toContain('s_state.lo = app_f2i(data->value_start);');
  });

  it('accepts a useCallback onChange and binds an animated valueStart', () => {
    const c = gen(`${PRE}
      import { useCallback } from 'react';
      import { Dial } from 'embedded-react';
      export function App() {
        const [v, setV] = useState(1);
        const lo = useAnimatedValue(0);
        const onChange = useCallback((n) => setV(n), []);
        return (<Dial range valueStart={lo} value={v} onChange={onChange} />);
      }`);
    expect(c).toContain('p.arc_range = 1;');
    expect(c).toMatch(
      /er_anim_value_bind\(\w+, n\d+, ER_PROP_ARC_VALUE_START\);/,
    );
    expect(c).toContain('s_state.v = app_f2i(data->value);');
  });

  it('rejects an unsupported prop and a bad enum token', () => {
    expect(() =>
      gen(`${PRE}
        import { Dial } from 'embedded-react';
        export function App() { return (<Dial value={1} bogus={2} />); }`),
    ).toThrow(/prop "bogus" is not supported/);
    expect(() =>
      gen(`${PRE}
        import { Dial } from 'embedded-react';
        export function App() { return (<Dial value={1} knob="triangle" />); }`),
    ).toThrow(/unsupported <Dial knob>/);
  });
});

describe('AOT ActivityIndicator', () => {
  it('lowers <ActivityIndicator> to ER_NODE_ACTIVITY_INDICATOR with color + a size-derived box', () => {
    const c = gen(`${PRE}
      import { ActivityIndicator } from 'embedded-react';
      export function App() { return (<ActivityIndicator size="large" color="#2a9d8f" />); }`);
    expect(c).toContain('er_node_create(ER_NODE_ACTIVITY_INDICATOR)');
    expect(c).toContain('p.indicator_color = 0xFF2A9D8Fu;');
    expect(c).toContain('p.width = 36;'); // large
    expect(c).toContain('p.height = 36;');
  });

  it('drives animating from state (start/stop)', () => {
    const c = gen(`${PRE}
      import { ActivityIndicator } from 'embedded-react';
      export function App() {
        const [busy, setBusy] = useState(true);
        return (<ActivityIndicator animating={busy} />);
      }`);
    expect(c).toContain('p.animating = (uint8_t)((s_state.busy) ? 1 : 0);');
  });

  it('maps size="small" → 20px and a numeric size verbatim', () => {
    const c = gen(`${PRE}
      import { ActivityIndicator } from 'embedded-react';
      export function App() { return (<View><ActivityIndicator size="small" /><ActivityIndicator size={48} /></View>); }`);
    expect(c).toContain('p.width = 20;');
    expect(c).toContain('p.width = 48;');
  });
});

describe('AOT Modal', () => {
  it('lowers <Modal> to ER_NODE_MODAL: state-driven visibility, backdrop, full-screen-centred overlay defaults', () => {
    const c = gen(`${PRE}
      import { Modal } from 'embedded-react';
      export function App() {
        const [show, setShow] = useState(false);
        return (<Modal visible={show} backdropColor="#000000cc"><Text>Hi</Text></Modal>);
      }`);
    expect(c).toContain('er_node_create(ER_NODE_MODAL)');
    expect(c).toContain('p.modal_visible = (uint8_t)((s_state.show) ? 1 : 0);');
    // <Modal>'s `visible` is its OWN show/hide prop — the engine derives layout.display from
    // modal_visible itself. The generic visible->display alias must not also fire here, or the node
    // carries two independent encodings of the same state that can disagree.
    expect(c).not.toMatch(/p\.display\s*=/);
    expect(c).toContain('p.backdrop_color = 0xCC000000u;');
    expect(c).toContain('p.position = ER_POS_ABSOLUTE;'); // overlay defaults…
    expect(c).toContain('p.right = 0;'); // …filled via 4 insets
    expect(c).toContain('p.align_items = ER_ALIGN_CENTER;');
    expect(c).toContain('"Hi"'); // content (children) emitted
  });

  it('throws when a Modal has no visible prop', () => {
    expect(() =>
      gen(`${PRE}
        import { Modal } from 'embedded-react';
        export function App() { return (<Modal><Text>x</Text></Modal>); }`),
    ).toThrow(/needs a visible prop/);
  });

  it('lowers position:absolute + left/top to ERProps (new style keys)', () => {
    const c = gen(`${PRE}
      export function App() { return (<View style={{ position: 'absolute', left: 10, top: 20 }}><Text>x</Text></View>); }`);
    expect(c).toContain('p.position = ER_POS_ABSOLUTE;');
    expect(c).toContain('p.left = 10;');
    expect(c).toContain('p.top = 20;');
  });
});

describe('AOT dynamic enum styles', () => {
  it('lowers a state-driven flexDirection to an ER_FLEX_* ternary (re-layouts on change)', () => {
    const c = gen(`${PRE}
      export function App() {
        const [row, setRow] = useState(false);
        return (<View style={{ flexDirection: row ? 'row' : 'column' }}><Pressable onPress={() => setRow(!row)}><Text>x</Text></Pressable></View>);
      }`);
    expect(c).toContain(
      'p.flex_direction = ((s_state.row) ? ER_FLEX_ROW : ER_FLEX_COL);',
    );
  });

  it('lowers state-driven alignItems and justifyContent', () => {
    const c = gen(`${PRE}
      export function App() {
        const [c2, setC2] = useState(true);
        return (<View style={{ alignItems: c2 ? 'center' : 'flex-start', justifyContent: c2 ? 'center' : 'flex-end' }}><Text>x</Text></View>);
      }`);
    expect(c).toContain(
      'p.align_items = ((s_state.c2) ? ER_ALIGN_CENTER : ER_ALIGN_FLEX_START);',
    );
    expect(c).toContain(
      'p.justify_content = ((s_state.c2) ? ER_JUSTIFY_CENTER : ER_JUSTIFY_FLEX_END);',
    );
  });

  it('throws on an unknown enum value in a dynamic enum style', () => {
    expect(() =>
      gen(`${PRE}
        export function App() {
          const [r, setR] = useState(false);
          return (<View style={{ flexDirection: r ? 'sideways' : 'row' }}><Text>x</Text></View>);
        }`),
    ).toThrow(/unsupported enum value "sideways"/);
  });
});

describe('AOT nested Text spans', () => {
  it('lowers a <Text> with nested <Text> to inline ERTextSpans (styled + inherit segments)', () => {
    const c = gen(`${PRE}
      export function App() {
        return (<Text style={{ color: '#ffffff', fontSize: 18 }}>Hi <Text style={{ fontWeight: 'bold', color: '#f4a261' }}>there</Text></Text>);
      }`);
    expect(c).toContain('static const ERTextSpan spans_n');
    expect(c).toContain('{ "Hi ", 0u, 0, 0xFF, 0xFF, 0xFF, ER_LAYOUT_AUTO }'); // inherit segment
    expect(c).toContain(
      '{ "there", 0xFFF4A261u, 0, 1, 0xFF, 0xFF, ER_LAYOUT_AUTO }',
    ); // bold + amber span
    expect(c).toMatch(/er_node_set_text_spans\(n\d+, spans_n\d+, 2\);/);
  });

  it('throws when a <Text> exceeds the engine span limit (4)', () => {
    expect(() =>
      gen(`${PRE}
        export function App() {
          return (<Text>a <Text style={{ fontWeight: 'bold' }}>b</Text> c <Text style={{ fontWeight: 'bold' }}>d</Text> e</Text>);
        }`),
    ).toThrow(/renders at most 4/);
  });

  it('keeps a plain <Text> (no nested Text) on the single-string path', () => {
    const c = gen(`${PRE}
      export function App() { const [n, setN] = useState(3); return (<Text>Count {n}</Text>); }`);
    expect(c).not.toContain('er_node_set_text_spans');
    expect(c).toContain('snprintf(p.text');
  });
});

describe('AOT FlatList (thin rewrite → ScrollView + .map)', () => {
  it('rewrites a static FlatList to a ScrollView with rows unrolled + renderItem style applied', () => {
    const c = gen(`${PRE}
      import { FlatList } from 'embedded-react';
      const ITEMS = [{ id: 1, name: 'A' }, { id: 2, name: 'B' }];
      export function App() {
        return (<FlatList style={{ flex: 1 }} data={ITEMS} keyExtractor={(it) => it.id} renderItem={({ item }) => (<View style={{ padding: 8 }}><Text>{item.name}</Text></View>)} />);
      }`);
    expect(c).toContain('er_node_create(ER_NODE_SCROLL_VIEW)'); // FlatList IS a ScrollView in the engine
    expect(c).toContain('"A"');
    expect(c).toContain('"B"');
    expect(c).toContain('p.padding = 8;'); // renderItem's per-row style
  });

  it('rewrites a state-list FlatList to a dynamic list inside a ScrollView (index param ok)', () => {
    const c = gen(`${PRE}
      import { FlatList } from 'embedded-react';
      export function App() {
        const [items, setItems] = useState([{ id: 1, label: 'Row' }]);
        return (<FlatList data={items} renderItem={({ item, index }) => (<View><Text>{item.label}</Text></View>)} />);
      }`);
    expect(c).toContain('er_node_create(ER_NODE_SCROLL_VIEW)');
    expect(c).toContain('s_items'); // the list-state pool array
  });

  it('throws when renderItem is not a ({ item }) destructuring function', () => {
    expect(() =>
      gen(`${PRE}
        import { FlatList } from 'embedded-react';
        export function App() { const d = []; return (<FlatList data={d} renderItem={(x) => <Text>x</Text>} />); }`),
    ).toThrow(/must destructure/);
  });
});

// The package exports three RN wrappers the AOT cannot lower yet (issue #114 shipped them Flow-A only).
// The failure mode that matters is the MESSAGE: the import resolves, the simulator renders them, and the
// spelling is right, so a bare "unknown element" sends people hunting for a typo that isn't there.
describe('AOT Flow-A-only components', () => {
  it.each([
    ['Button', /<Pressable/],
    ['ImageBackground', /<View/],
    ['SectionList', /<ScrollView/],
  ])(
    'rejects <%s> by name, pointing at the tree to write instead',
    (tag, hint) => {
      let thrown;
      try {
        gen(`${PRE}
        import { ${tag} } from 'embedded-react';
        export function App() { return (<${tag} />); }`);
      } catch (e) {
        thrown = e;
      }
      expect(thrown, `<${tag}> compiled instead of throwing`).toBeDefined();
      expect(thrown.message).toContain(
        `<${tag}> is not supported in Flow B yet`,
      );
      expect(thrown.message).not.toContain('unknown element');
      expect(thrown.message).toMatch(hint);
    },
  );

  // The catch-all must still fire for a name that really is a typo / a missing import.
  it('still reports a genuinely unknown element as unknown', () => {
    expect(() =>
      gen(`${PRE}
        export function App() { return (<Vieww />); }`),
    ).toThrow(/unknown element <Vieww>/);
  });
});

describe('AOT TouchableOpacity (press-to-dim on the native driver)', () => {
  const T = `import { useState, useCallback } from 'react';
import { View, Text, Pressable, TouchableOpacity, useAnimatedValue } from 'embedded-react';
`;
  /** The body of the handler wired to `event` on the (single) node, so a test can read its animate call. */
  const handlerFor = (c, event) => {
    const name = c.match(
      new RegExp(`er_event_set\\([^,]+, ${event}, (\\w+),`),
    )?.[1];
    if (!name) return '';
    const at = c.indexOf(`static void ${name}(`);
    return c.slice(at, c.indexOf('\n}', at));
  };

  it('emits ONE Pressable node with an animated value bound to its opacity', () => {
    const c = gen(`${T}
      export function App() {
        return (<TouchableOpacity><Text>tap</Text></TouchableOpacity>);
      }`);
    expect(c.match(/er_node_create\(ER_NODE_PRESSABLE\)/g)).toHaveLength(1);
    expect(c).toContain('er_anim_value_create(1.0f)');
    expect(c).toMatch(
      /er_anim_value_bind\(s_av_press_\w+, \w+, ER_PROP_OPACITY\)/,
    );
  });

  it('dims with no ramp on press-in and fades back over RN’s 250 ms on press-out', () => {
    const c = gen(`${T}
      export function App() {
        return (<TouchableOpacity><Text>tap</Text></TouchableOpacity>);
      }`);
    const down = handlerFor(c, 'ER_EVENT_PRESS_IN');
    const up = handlerFor(c, 'ER_EVENT_PRESS_OUT');
    expect(down).toContain('cfg.duration_ms = 0;');
    expect(down).toMatch(/er_anim_value_animate\(s_av_press_\w+, 0.2f/);
    expect(up).toContain('cfg.duration_ms = 250;');
    expect(up).toMatch(/er_anim_value_animate\(s_av_press_\w+, 1.0f/);
  });

  it('rests at the style’s own opacity and dims to activeOpacity', () => {
    const c = gen(`${T}
      export function App() {
        return (<TouchableOpacity activeOpacity={0.5} style={{opacity: 0.8}}><Text>x</Text></TouchableOpacity>);
      }`);
    expect(c).toContain('er_anim_value_create(0.8f)');
    expect(handlerFor(c, 'ER_EVENT_PRESS_IN')).toMatch(/animate\(\w+, 0.5f/);
    expect(handlerFor(c, 'ER_EVENT_PRESS_OUT')).toMatch(/animate\(\w+, 0.8f/);
  });

  it('wraps the app’s own press handler instead of replacing it', () => {
    const c = gen(`${T}
      export function App() {
        const [n, setN] = useState(0);
        return (<TouchableOpacity onPressIn={() => setN(n + 1)}><Text>{n}</Text></TouchableOpacity>);
      }`);
    const down = handlerFor(c, 'ER_EVENT_PRESS_IN');
    expect(down).toContain('er_anim_value_animate');
    expect(down).toMatch(/er_handler_\d+\(node, data, user_data\);/);
    expect(c).toContain('s_state.n = app_add(s_state.n, 1);');
  });

  it('shares one useCallback handler between the two ends rather than duplicating it', () => {
    const c = gen(`${T}
      export function App() {
        const [n, setN] = useState(0);
        const bump = useCallback(() => setN(n + 1), [n]);
        return (<TouchableOpacity onPressIn={bump} onPressOut={bump}><Text>{n}</Text></TouchableOpacity>);
      }`);
    expect(c.match(/static void er_cb_bump\(/g)).toHaveLength(1);
    expect(handlerFor(c, 'ER_EVENT_PRESS_IN')).toContain('er_cb_bump(');
    expect(handlerFor(c, 'ER_EVENT_PRESS_OUT')).toContain('er_cb_bump(');
  });

  it('compiles a disabled one away entirely — no handlers, no dim', () => {
    const c = gen(`${T}
      export function App() {
        return (<TouchableOpacity disabled onPress={() => {}}><Text>x</Text></TouchableOpacity>);
      }`);
    expect(c).toContain('er_node_create(ER_NODE_PRESSABLE)');
    expect(c).not.toContain('er_event_set(');
    expect(c).not.toContain('er_anim_value_bind(');
  });

  it('rejects a state-driven opacity, which the feedback would overwrite', () => {
    expect(() =>
      gen(`${T}
        export function App() {
          const [o, setO] = useState(1);
          return (<TouchableOpacity style={{opacity: o}} onPress={() => setO(0.5)}><Text>x</Text></TouchableOpacity>);
        }`),
    ).toThrow(/cannot take a state-driven opacity/);
  });

  it('rejects a state-driven disabled, which it cannot compile away', () => {
    expect(() =>
      gen(`${T}
        export function App() {
          const [d, setD] = useState(true);
          return (<TouchableOpacity disabled={d} onPress={() => setD(false)}><Text>x</Text></TouchableOpacity>);
        }`),
    ).toThrow(/disabled> must be a compile-time constant/);
  });

  // Everything here reaches floatLit, which would happily emit `NaNf` (does not compile) or `1.0f` for a
  // bare `activeOpacity` (no dim at all) — a wrong build, or no build, from a prop the app got wrong.
  it.each([
    ['a bare flag', 'activeOpacity', 'true'],
    ['a boolean', 'activeOpacity={false}', 'false'],
    ['NaN', 'activeOpacity={0 / 0}', 'NaN'],
    ['a numeric string', 'activeOpacity="0.4"', '"0.4"'],
    ['over 1', 'activeOpacity={2}', '2'],
    ['under 0', 'activeOpacity={-1}', '-1'],
    ['a percentage', 'activeOpacity={20}', '20'],
  ])(
    'rejects %s as activeOpacity, naming what it got',
    (_what, attr, shown) => {
      let thrown;
      try {
        gen(`${T}
        export function App() {
          return (<TouchableOpacity ${attr}><Text>x</Text></TouchableOpacity>);
        }`);
      } catch (e) {
        thrown = e;
      }
      expect(thrown, `${attr} compiled instead of throwing`).toBeDefined();
      expect(thrown.message).toContain(
        'activeOpacity> must be a number between 0 and 1',
      );
      expect(thrown.message).toContain(`(got ${shown})`);
    },
  );

  it.each([0, 0.5, 1])('accepts activeOpacity={%s}', v => {
    const c = gen(`${T}
      export function App() {
        return (<TouchableOpacity activeOpacity={${v}}><Text>x</Text></TouchableOpacity>);
      }`);
    expect(handlerFor(c, 'ER_EVENT_PRESS_IN')).toContain(
      `er_anim_value_animate(s_av_press_n0, ${v === 1 ? '1.0f' : v === 0 ? '0.0f' : '0.5f'},`,
    );
  });

  // An Animated.Value opacity is collected as a `binds` entry, NOT a dynAssign — a different list, so
  // the state-driven check above misses it entirely. Left alone it emits a SECOND
  // er_anim_value_bind on the same node+prop: the engine's duplicate guard is per-value, so both
  // register and whichever changed last wins the frame.
  it.each([
    ['an Animated.Value', 'style={{opacity: fade}}'],
    [
      'an interpolation',
      'style={{opacity: fade.interpolate({inputRange: [0, 1], outputRange: [0.3, 1]})}}',
    ],
  ])('rejects %s driving a TouchableOpacity opacity', (_what, attr) => {
    expect(() =>
      gen(`${T}
        export function App() {
          const fade = useAnimatedValue(1);
          return (<TouchableOpacity ${attr}><Text>x</Text></TouchableOpacity>);
        }`),
    ).toThrow(/cannot take an Animated opacity/);
  });

  it('leaves an animated TRANSFORM alone — only opacity collides with the dim', () => {
    const c = gen(`${T}
      export function App() {
        const s = useAnimatedValue(1);
        return (<TouchableOpacity style={{transform: [{scale: s}]}}><Text>x</Text></TouchableOpacity>);
      }`);
    expect(c).toContain('ER_PROP_SCALE_X');
    expect(c).toContain('ER_PROP_SCALE_Y');
    // …and the press dim is still its own, separate binding.
    expect(c).toMatch(
      /er_anim_value_bind\(s_av_press_\w+, \w+, ER_PROP_OPACITY\)/,
    );
  });

  it('still allows an animated opacity on a plain <Pressable>', () => {
    const c = gen(`${T}
      export function App() {
        const fade = useAnimatedValue(1);
        return (<Pressable style={{opacity: fade}}><Text>x</Text></Pressable>);
      }`);
    expect(
      c.match(/er_anim_value_bind\(\w+, \w+, ER_PROP_OPACITY\)/g),
    ).toHaveLength(1);
  });

  it('keeps a disabled one’s children, layout and style — only the press is gone', () => {
    const c = gen(`${T}
      export function App() {
        return (<TouchableOpacity disabled style={{padding: 8, opacity: 0.8}}><Text>label</Text></TouchableOpacity>);
      }`);
    expect(c).toContain('er_node_create(ER_NODE_TEXT)'); // children still mount
    expect(c).toContain('"label"');
    expect(c).toContain('p.padding = 8;'); // …and still lay out
    expect(c).toContain('p.opacity = 204;'); // …at the opacity the style asked for
    expect(c).not.toContain('er_event_set(');
    expect(c).not.toContain('er_anim_value_bind(');
  });
});

describe('AOT callback props', () => {
  const C = `import { useState } from 'react';
import { View, Text, Pressable } from 'embedded-react';
`;

  it('inlines each instance’s callback prop as the child handler, against the caller state', () => {
    const c = gen(`${C}
      function StepButton({ label, onTap }) {
        return (<Pressable onPress={onTap}><Text>{label}</Text></Pressable>);
      }
      export function App() {
        const [n, setN] = useState(0);
        return (<View>
          <StepButton label="-" onTap={() => setN(n - 1)} />
          <StepButton label="+" onTap={() => setN(n + 1)} />
        </View>);
      }`);
    expect(c).toContain('s_state.n = app_sub(s_state.n, 1);');
    expect(c).toContain('s_state.n = app_add(s_state.n, 1);');
    // two distinct handlers, one per instance, each wired via er_event_set
    expect((c.match(/static void er_handler_\d+\(/g) || []).length).toBe(2);
    expect((c.match(/er_event_set\(\w+, ER_EVENT_PRESS,/g) || []).length).toBe(
      2,
    );
  });

  it('accepts a useCallback identifier passed as a callback prop', () => {
    const c = gen(`${C}
      import { useCallback } from 'react';
      function Btn({ onTap }) { return (<Pressable onPress={onTap}><Text>x</Text></Pressable>); }
      export function App() {
        const [n, setN] = useState(0);
        const inc = useCallback(() => setN(n + 1), [n]);
        return (<Btn onTap={inc} />);
      }`);
    expect(c).toContain('s_state.n = app_add(s_state.n, 1);');
    expect(c).toContain('er_event_set(');
  });

  it('forwards a callback prop through an intermediate component', () => {
    const c = gen(`${C}
      function Inner({ onTap }) { return (<Pressable onPress={onTap}><Text>x</Text></Pressable>); }
      function Outer({ onTap }) { return (<Inner onTap={onTap} />); }
      export function App() {
        const [n, setN] = useState(0);
        return (<Outer onTap={() => setN(n + 1)} />);
      }`);
    expect(c).toContain('s_state.n = app_add(s_state.n, 1);');
    expect(c).toContain('er_event_set(');
  });
});

describe('AOT TextInput', () => {
  const T = `import { useState } from 'react';
import { View, Text, TextInput } from 'embedded-react';
`;

  it('compiles a controlled TextInput (value + onChangeText) to ER_NODE_TEXT_INPUT + CHANGE_TEXT', () => {
    const c = gen(`${T}
      export function App() {
        const [name, setName] = useState('');
        return (<TextInput value={name} onChangeText={(t) => setName(t)} placeholder="Name" placeholderTextColor="#888" />);
      }`);
    expect(c).toContain('er_node_create(ER_NODE_TEXT_INPUT)');
    expect(c).toContain('ER_EVENT_CHANGE_TEXT');
    // onChangeText param binds to the new text; setName(t) → snprintf from data->changed_text
    expect(c).toContain(
      'snprintf(s_state.name, sizeof(s_state.name), "%s", data->changed_text);',
    );
    // value drives the buffer (synced in app_update), placeholder + color applied
    expect(c).toContain(
      'snprintf(p.text, sizeof(p.text), "%s", s_state.name);',
    );
    expect(c).toContain(
      'snprintf(p.placeholder, sizeof(p.placeholder), "%s", "Name");',
    );
    expect(c).toContain('p.placeholder_color = 0xFF888888u;');
  });

  it('compiles a static-value TextInput inline (no app_update)', () => {
    const c = gen(`${T}
      export function App() {
        return (<TextInput value="hi" placeholder="type" editable={false} />);
      }`);
    expect(c).toContain('er_node_create(ER_NODE_TEXT_INPUT)');
    expect(c).toContain('snprintf(p.text, sizeof(p.text), "%s", "hi");');
    expect(c).toContain('p.editable = 0;');
  });

  it('rejects a non-function onChangeText', () => {
    expect(() =>
      gen(`${T}
        export function App() { const [n, setN] = useState(''); return (<TextInput value={n} onChangeText={n} />); }`),
    ).toThrow(/onChangeText must be an inline function/);
  });

  it('rejects an unsupported TextInput prop', () => {
    expect(() =>
      gen(`${T}
        export function App() { return (<TextInput selectionColor="#fff" />); }`),
    ).toThrow(/not supported/);
  });
});

describe('AOT images', () => {
  // `import x from './x.png'` + <Image source={x}> → the node's image_name is the file's basename, and
  // compileSource returns the import so the CLI can bake it (er_register_assets).
  const IMG = `import { View, Image } from 'embedded-react';
import logo from './assets/logo.png';
`;
  it('resolves an imported image to image_name + returns it for baking', () => {
    const r = compileSource(
      `${IMG}\nexport function App() { return (<Image source={logo} style={{ width: 40, height: 40 }} />); }`,
      'demo',
    );
    expect(r.c).toContain(
      'snprintf(p.image_name, sizeof(p.image_name), "%s", "logo");',
    );
    expect(r.images).toEqual([{name: 'logo', importPath: './assets/logo.png'}]);
  });

  it('lowers resizeMode to the ERResizeMode enum', () => {
    const r = compileSource(
      `${IMG}\nexport function App() { return (<Image source={logo} resizeMode="contain" />); }`,
      'demo',
    );
    expect(r.c).toContain('p.resize_mode = ER_RESIZE_CONTAIN;');
  });

  it('lowers tintColor to an ARGB literal', () => {
    const r = compileSource(
      `${IMG}\nexport function App() { return (<Image source={logo} tintColor="#ff0000" />); }`,
      'demo',
    );
    expect(r.c).toContain('p.tint_color = 0xFFFF0000u;');
  });

  it('accepts source={{ uri }} as a bare asset name (no import to bake)', () => {
    const r = compileSource(
      `import { Image } from 'embedded-react';\nexport function App() { return (<Image source={{ uri: 'wx_sun' }} />); }`,
      'demo',
    );
    expect(r.c).toContain(
      'snprintf(p.image_name, sizeof(p.image_name), "%s", "wx_sun");',
    );
    expect(r.images).toEqual([]);
  });

  it('folds a static <Image source={item.icon}> in an unrolled .map to literal asset names + bakes them', () => {
    const r = compileSource(
      `import { View, Image } from 'embedded-react';\nimport wxSun from './a/wx_sun.png';\nimport wxRain from './a/wx_rain.png';\nconst DAYS = [{ icon: wxSun }, { icon: wxRain }];\nexport function App() { return (<View>{DAYS.map((f, i) => (<Image key={i} source={f.icon} />))}</View>); }`,
      'demo',
    );
    expect(r.c).toContain(
      'snprintf(p.image_name, sizeof(p.image_name), "%s", "wx_sun");',
    );
    expect(r.c).toContain(
      'snprintf(p.image_name, sizeof(p.image_name), "%s", "wx_rain");',
    );
    expect(r.images.map(i => i.name).sort()).toEqual(['wx_rain', 'wx_sun']);
  });

  it('emits a dynamic <Image source> from a list-state field (set in app_update)', () => {
    const c = compileSource(
      `import { View, Image } from 'embedded-react';\nimport { useState } from 'react';\nexport function App() { const [items] = useState([{ icon: 'wx_sun' }]); return (<View>{items.map((d, i) => (<Image key={i} source={d.icon} />))}</View>); }`,
      'demo',
    ).c;
    expect(c).toContain(
      'snprintf(p.image_name, sizeof(p.image_name), "%s", s_items[0].icon);',
    );
  });

  it('emits a dynamic <Image source> from a state ternary + bakes both branches', () => {
    const r = compileSource(
      `import { Image, Pressable } from 'embedded-react';\nimport sun from './a/sun.png';\nimport moon from './a/moon.png';\nimport { useState } from 'react';\nexport function App() { const [day, setDay] = useState(true); return (<Pressable onPress={() => setDay(!day)}><Image source={day ? sun : moon} /></Pressable>); }`,
      'demo',
    );
    expect(r.c).toContain(
      'snprintf(p.image_name, sizeof(p.image_name), "%s", (s_state.day ? "sun" : "moon"));',
    );
    expect(r.images.map(i => i.name).sort()).toEqual(['moon', 'sun']);
  });

  it('rejects an <Image source> that is not a string (e.g. a number)', () => {
    expect(() =>
      compileSource(
        `import { Image } from 'embedded-react';\nimport { useState } from 'react';\nexport function App() { const [n] = useState(5); return (<Image source={n} />); }`,
        'demo',
      ),
    ).toThrow(/must resolve to an asset NAME/);
  });

  it('rejects an unsupported resizeMode', () => {
    expect(() =>
      compileSource(
        `${IMG}\nexport function App() { return (<Image source={logo} resizeMode="squish" />); }`,
        'demo',
      ),
    ).toThrow(/unsupported <Image resizeMode>/);
  });
});

describe('AOT per-instance child state', () => {
  const C = `import { View, Text, Pressable } from 'embedded-react';
import { useState } from 'react';
function Counter({ label }) {
  const [n, setN] = useState(0);
  return (<Pressable onPress={() => setN(n + 1)}><Text>{label}: {n}</Text></Pressable>);
}
`;
  it('gives a stateful child its own namespaced field in ErAppState', () => {
    const c = compileSource(
      `${C}\nexport function App() { return (<View><Counter label="A" /></View>); }`,
      'demo',
    ).c;
    expect(c).toContain('int c0_n;');
    expect(c).toContain('s_state.c0_n = app_add(s_state.c0_n, 1);'); // the child's setter mutates its own field
  });

  it('keeps two instances of the same component independent', () => {
    const c = compileSource(
      `${C}\nexport function App() { return (<View><Counter label="A" /><Counter label="B" /></View>); }`,
      'demo',
    ).c;
    expect(c).toContain('int c0_n;');
    expect(c).toContain('int c1_n;'); // distinct storage per instance
    expect(c).toContain('s_state.c0_n = app_add(s_state.c0_n, 1);');
    expect(c).toContain('s_state.c1_n = app_add(s_state.c1_n, 1);');
    // each instance's text reads its OWN field
    expect(c).toMatch(/"A: %d",\s*s_state\.c0_n/);
    expect(c).toMatch(/"B: %d",\s*s_state\.c1_n/);
  });

  it('folds a child useState initial against a static prop', () => {
    const src = `import { View, Text, Pressable } from 'embedded-react';
import { useState } from 'react';
function Counter({ start }) { const [n, setN] = useState(start); return (<Pressable onPress={() => setN(n + 1)}><Text>{n}</Text></Pressable>); }
export function App() { return (<View><Counter start={5} /></View>); }`;
    expect(compileSource(src, 'demo').c).toContain('.c0_n = 5');
  });
});

describe('AOT per-instance child hooks (self-contained components)', () => {
  it('gives each instance its own useAnimatedValue handle', () => {
    const src = `import { View, Text, Pressable, Animated, useAnimatedValue } from 'embedded-react';
function Card({ label }) {
  const scale = useAnimatedValue(1);
  return (<Pressable onPressIn={() => Animated.spring(scale, { toValue: 0.7 }).start()} style={{ transform: [{ scale: scale }] }}><Text>{label}</Text></Pressable>);
}
export function App() { return (<View><Card label="A" /><Card label="B" /></View>); }`;
    const c = compileSource(src, 'demo').c;
    expect(c).toContain('s_av_c0_scale = er_anim_value_create(1.0f);');
    expect(c).toContain('s_av_c1_scale = er_anim_value_create(1.0f);');
    // each instance's bind targets its OWN value
    expect(c).toContain('er_anim_value_bind(s_av_c0_scale,');
    expect(c).toContain('er_anim_value_bind(s_av_c1_scale,');
  });

  it('gives each instance its own useRef slot', () => {
    const src = `import { View, Text, Pressable } from 'embedded-react';
import { useRef } from 'react';
function Tally() { const t = useRef(0); return (<Pressable onPress={() => { t.current = t.current + 1; }}><Text>x</Text></Pressable>); }
export function App() { return (<View><Tally /><Tally /></View>); }`;
    const c = compileSource(src, 'demo').c;
    expect(c).toContain('static int s_ref_c0_t = 0;');
    expect(c).toContain('static int s_ref_c1_t = 0;');
    expect(c).toContain('s_ref_c0_t = app_add(s_ref_c0_t, 1);');
    expect(c).toContain('s_ref_c1_t = app_add(s_ref_c1_t, 1);');
  });

  it('compiles each instance useCallback into its own distinct handler', () => {
    const src = `import { View, Text, Pressable } from 'embedded-react';
import { useState, useCallback } from 'react';
function Btn() { const [n, setN] = useState(0); const tap = useCallback(() => setN(n + 1), [n]); return (<Pressable onPress={tap}><Text>{n}</Text></Pressable>); }
export function App() { return (<View><Btn /><Btn /></View>); }`;
    const c = compileSource(src, 'demo').c;
    expect(c).toContain('static void er_cb_c0_tap(');
    expect(c).toContain('static void er_cb_c1_tap(');
    expect(c).toContain('s_state.c0_n = app_add(s_state.c0_n, 1);'); // c0's handler mutates c0's state
    expect(c).toContain('s_state.c1_n = app_add(s_state.c1_n, 1);');
  });
});

describe('AOT handler follow-ons', () => {
  it('inlines a helper call in a handler (component-local arrow), binding its args', () => {
    const c = compileSource(
      `import { View, Text, Pressable } from 'embedded-react';
import { useState } from 'react';
function App() {
  const [a, setA] = useState(0);
  const [b, setB] = useState(0);
  const reset = () => { setA(0); setB(0); };
  const bump = (k) => setA(a + k);
  return (<Pressable onPress={() => { reset(); bump(5); }}><Text>{a}</Text></Pressable>);
}
export { App };`,
      'demo',
    ).c;
    expect(c).toContain('s_state.a = 0;');
    expect(c).toContain('s_state.b = 0;');
    expect(c).toContain('s_state.a = app_add(s_state.a, 5);'); // bump(5): arg bound
  });

  it('detects a recursive helper and errors instead of looping forever', () => {
    expect(() =>
      compileSource(
        `import { Text, Pressable } from 'embedded-react';
function App() { const loop = () => { loop(); }; return (<Pressable onPress={() => { loop(); }}><Text>x</Text></Pressable>); }
export { App };`,
        'demo',
      ),
    ).toThrow(/recursive/);
  });

  it('wires a .start(onComplete) callback to the animation on_complete', () => {
    const c = compileSource(
      `import { Text, Pressable, Animated, useAnimatedValue } from 'embedded-react';
import { useState } from 'react';
function App() {
  const [done, setDone] = useState(false);
  const x = useAnimatedValue(0);
  return (<Pressable onPress={() => Animated.timing(x, { toValue: 1, duration: 100 }).start(() => setDone(true))}><Text>x</Text></Pressable>);
}
export { App };`,
      'demo',
    ).c;
    expect(c).toContain('.on_complete = er_donecb_0;');
    expect(c).toContain(
      'static void er_donecb_0(bool finished, void* user_data)',
    );
    expect(c).toContain('s_state.done = 1;'); // completion sets state
  });

  it('rejects a completion callback on a parallel animation (not yet supported)', () => {
    expect(() =>
      compileSource(
        `import { Text, Pressable, Animated, useAnimatedValue } from 'embedded-react';
function App() {
  const a = useAnimatedValue(0); const b = useAnimatedValue(0);
  return (<Pressable onPress={() => Animated.parallel([Animated.timing(a, { toValue: 1 }), Animated.timing(b, { toValue: 1 })]).start(() => {})}><Text>x</Text></Pressable>);
}
export { App };`,
        'demo',
      ),
    ).toThrow(/parallel\/stagger animation is not yet supported/);
  });
});

describe('AOT image baking (usage-based)', () => {
  it('does not bake an image import used only in a folded-away (responsive) branch', () => {
    // wide=screen.width>=600 is folded false at 320px, so the <Image> below is never emitted → not baked.
    const src = `import { View, Text, Image } from 'embedded-react';
import wxSun from './a/wx_sun.png';
const wide = screen.width >= 600;
export function App() {
  if (wide) { return (<View><Image source={wxSun} /></View>); }
  return (<View><Text>compact</Text></View>);
}`;
    const r = compileSource(src, 'demo', {screen: {width: 320, height: 480}});
    expect(r.images).toEqual([]); // wxSun is imported but unreached → not baked
    expect(r.c).not.toContain('wx_sun');
  });

  it("bakes ALL imports when a reached source is dynamic (can't be enumerated)", () => {
    const r = compileSource(
      `import { Image, Pressable } from 'embedded-react';\nimport a from './a/a.png';\nimport b from './a/b.png';\nimport { useState } from 'react';\nexport function App() { const [f, setF] = useState(true); return (<Pressable onPress={() => setF(!f)}><Image source={f ? a : b} /></Pressable>); }`,
      'demo',
    );
    expect(r.images.map(i => i.name).sort()).toEqual(['a', 'b']);
  });
});

describe('AOT version-pin', () => {
  it('stamps a compile-time engine-version assert into the generated C', () => {
    const c = compileSource(
      `${PRE}\nexport function App() { return (<Text>hi</Text>); }`,
      'demo',
    );
    expect(c.c).toContain('#include "er_version.h"');
    expect(c.c).toMatch(
      /_Static_assert\(ER_VERSION_MAJOR == \d+ && ER_VERSION_MINOR == \d+,/,
    );
    expect(c.c).toContain('version mismatch');
  });
});

describe('AOT useHostValue (host-fed input)', () => {
  const HOST_PRE = `import { useHostValue } from 'embedded-react';
`;
  it('lowers useHostValue to an s_state field + a public setter, read like state', () => {
    const c = gen(`${HOST_PRE}import { Text } from 'embedded-react';
      export function App() {
        const steps = useHostValue(0);
        return (<Text>{steps}</Text>);
      }`);
    expect(c).toContain('int steps;'); // field in ErAppState
    expect(c).toContain('.steps = 0'); // initializer
    expect(c).toContain(
      'snprintf(p.text, sizeof(p.text), "%d", s_state.steps);',
    ); // read into Text
    expect(c).toContain('void er_app_set_steps(int v)'); // generated public setter
    expect(c).toContain('s_state.steps = v;');
    expect(c).toContain('app_update();'); // setter refreshes dependent nodes
  });

  it('exposes the setter prototype in the generated header', () => {
    const {h} = compileSource(
      `import { useHostValue, Text } from 'embedded-react';
      export function App() { const bpm = useHostValue(60); return (<Text>{bpm}</Text>); }`,
      'test',
    );
    expect(h).toContain('void er_app_set_bpm(int v);');
  });

  it('uses a float setter for a float initial', () => {
    const c = gen(`${HOST_PRE}import { Text } from 'embedded-react';
      export function App() { const temp = useHostValue(0.0); return (<Text>{temp}</Text>); }`);
    expect(c).toContain('float temp;');
    expect(c).toContain('void er_app_set_temp(float v)');
  });

  it('rejects a string host value', () => {
    expect(() =>
      gen(`${HOST_PRE}import { Text } from 'embedded-react';
      export function App() { const name = useHostValue("hi"); return (<Text>{name}</Text>); }`),
    ).toThrow(/useHostValue.*must be a number/);
  });

  it('rejects a boolean host value (cTypeOfValue would otherwise map it to int)', () => {
    expect(() =>
      gen(`${HOST_PRE}import { Text } from 'embedded-react';
      export function App() { const on = useHostValue(true); return (<Text>{on}</Text>); }`),
    ).toThrow(/useHostValue.*must be a number/);
  });

  it('rejects a non-finite host value (NaN)', () => {
    expect(() =>
      gen(`${HOST_PRE}import { Text } from 'embedded-react';
      export function App() { const x = useHostValue(0 / 0); return (<Text>{x}</Text>); }`),
    ).toThrow(/useHostValue.*must be a number/);
  });

  it('accepts an integer and a float initial', () => {
    const ci = gen(`${HOST_PRE}import { Text } from 'embedded-react';
      export function App() { const n = useHostValue(5); return (<Text>{n}</Text>); }`);
    expect(ci).toContain('int n;');
    expect(ci).toContain('void er_app_set_n(int v)');
    const cf = gen(`${HOST_PRE}import { Text } from 'embedded-react';
      export function App() { const f = useHostValue(1.5); return (<Text>{f}</Text>); }`);
    expect(cf).toContain('float f;');
    expect(cf).toContain('void er_app_set_f(float v)');
  });
});

// Name resolution has been a recurring source of silently-wrong text: the constant fold reaching past a
// runtime binding, App's locals leaking into a child, a child's own const not shadowing. These pin the
// rule rather than one instance of it — a child component is declared at module level, so its body sees
// MODULE scope plus its own props and consts, never the caller's locals.
describe('AOT const scoping', () => {
  const D = `import {useState} from 'react';
import {View, Text} from 'embedded-react';
`;
  const texts = c =>
    (
      c.match(/snprintf\(p\.text, sizeof\(p\.text\), "%s", "([^"]*)"\);/g) || []
    ).map(l => l.match(/"%s", "([^"]*)"/)[1]);

  it("App's local const shadows a module const of the same name", () => {
    const c = gen(`${D}const L = 'module';
      export function App() {
        const L = 'applocal';
        return (<Text>{'a=' + L}</Text>);
      }`);
    expect(texts(c)).toContain('a=applocal');
  });

  it("App's local does NOT leak into a child component", () => {
    const c = gen(`${D}const L = 'module';
      function Child() { return (<Text>{'c=' + L}</Text>); }
      export function App() {
        const L = 'applocal';
        return (<View><Text>{'a=' + L}</Text><Child /></View>);
      }`);
    expect(texts(c)).toEqual(
      expect.arrayContaining(['a=applocal', 'c=module']),
    );
  });

  it("a child's own const shadows the module one", () => {
    const c = gen(`${D}const L = 'module';
      function Child() { const L = 'childlocal'; return (<Text>{'c=' + L}</Text>); }
      export function App() { return (<View><Child /></View>); }`);
    expect(texts(c)).toContain('c=childlocal');
  });

  it('a prop shadows the module const', () => {
    const c = gen(`${D}const L = 'module';
      function Child({L}) { return (<Text>{'c=' + L}</Text>); }
      export function App() { return (<View><Child L="prop" /></View>); }`);
    expect(texts(c)).toContain('c=prop');
  });

  it('a nested child still resolves to module scope', () => {
    const c = gen(`${D}const L = 'module';
      function Inner() { return (<Text>{'i=' + L}</Text>); }
      function Outer() { return (<View><Inner /></View>); }
      export function App() { const L = 'applocal'; return (<View><Outer /></View>); }`);
    expect(texts(c)).toContain('i=module');
  });

  // Styles fold constants through the same scope as text, so a runtime binding has to beat a same-named
  // module const there too — otherwise the fold silently emits the module value where the prop or row
  // item was meant. `width` is a supported dynamic style, so a correct lowering is observable.
  it('a dynamic child prop beats a same-named module const in a style', () => {
    const c = gen(`${D}const w = 10;
      function Child({w}) { return (<View style={{width: w}} />); }
      export function App() {
        const [n, setN] = useState(3);
        return (<View><Child w={n * 2} /></View>);
      }`);
    expect(c).toContain('p.width = app_round_dim(app_mul(s_state.n, 2));');
    expect(c).not.toContain('p.width = 10;');
  });

  it("a .map row's item beats a same-named module const in a style", () => {
    const c = gen(`${D}const it = {w: 10};
      export function App() {
        const [items, setItems] = useState([{w: 4}]);
        return (<View>{items.map(it => (<View style={{width: it.w}} />))}</View>);
      }`);
    expect(c).toMatch(/p\.width = app_round_dim\(s_items\[\d+\]\.w\);/);
    expect(c).not.toContain('p.width = 10;');
  });

  // A child is a module-level function: App's runtime locals (memos, dynamic consts) are not in its scope.
  it("App's memo does NOT leak into a child, and a child's const beats it", () => {
    const c = gen(`${D}import {useMemo} from 'react';
      const total = 5;
      function Child() { return (<Text>{'c=' + total}</Text>); }
      function Own() { const total = 3; return (<Text>{'o=' + total}</Text>); }
      export function App() {
        const [n, setN] = useState(1);
        const total = useMemo(() => n * 2, [n]);
        return (<View><Text>{'a=' + total}</Text><Child /><Own /></View>);
      }`);
    expect(c).toContain('"a=%d", (app_mul(s_state.n, 2))');
    expect(texts(c)).toEqual(expect.arrayContaining(['c=5', 'o=3']));
  });

  it("App's memo beats a same-named module const in a style and as a prop", () => {
    const c = gen(`${D}import {useMemo} from 'react';
      const w = 10;
      function Child({w}) { return (<Text>{w}</Text>); }
      export function App() {
        const [n, setN] = useState(1);
        const w = useMemo(() => n * 3, [n]);
        return (<View style={{width: w}}><Child w={w} /></View>);
      }`);
    expect(c).toContain('p.width = app_round_dim((app_mul(s_state.n, 3)));');
    expect(c).toContain('"%d", (app_mul(s_state.n, 3))');
    expect(c).not.toContain('p.width = 10;');
  });

  it('an event or gesture param beats a same-named module const', () => {
    const c = gen(`${D}import {Pressable} from 'embedded-react';
      const e = 'MOD';
      export function App() {
        const [label, setLabel] = useState('');
        return (<Pressable onTouchMove={e => setLabel('x=' + e.x)}><Text>{label}</Text></Pressable>);
      }`);
    expect(c).toContain('"x=%d", data->x');
    expect(c).not.toContain('"x=undefined"');
  });

  it('a child const can seed its own useState initial', () => {
    const c = gen(`${D}
      function Child() { const START = 5; const [n, setN] = useState(START); return (<Text>{n}</Text>); }
      export function App() { return (<View><Child /></View>); }`);
    expect(c).toMatch(/c0_n = 5/);
  });

  it('a handler param shadows a module const in a string setter', () => {
    const c = gen(`${D}import {Pressable} from 'embedded-react';
      const label = 'MODULE';
      export function App() {
        const [s, setS] = useState('x');
        const [n, setN] = useState(0);
        const pick = (label) => setS(label);
        return (<Pressable onPress={() => pick(n > 0 ? 'a' : 'b')}><Text>{s}</Text></Pressable>);
      }`);
    expect(c).toContain('((s_state.n > 0) ? "a" : "b")');
    expect(c).not.toContain('"MODULE"');
  });
});

// Everything here used to reach the C compiler as something GCC rejects under -Werror (ESP-IDF's
// default): a string in arithmetic, a `<` on two char* (address compare), a char[] as a condition
// (-Werror=address), and `int l_t = s_state.label`.
describe('AOT strings in C-hostile positions', () => {
  const D = `import {useState} from 'react';
import {View, Text, Pressable, Switch} from 'embedded-react';
`;
  it('rejects arithmetic other than + on a string', () => {
    for (const op of ['-', '*', '/', '%']) {
      expect(() =>
        gen(
          `${D}export function App() { const [n] = useState(1); return (<Text>{'5' ${op} n}</Text>); }`,
        ),
      ).toThrow(new RegExp(`"\\${op}" on a string is not supported`));
    }
  });

  // Equality through strcmp is exact for UTF-8. Ordering is not — JS compares UTF-16 code units, the
  // device holds UTF-8 bytes, and they disagree outside the BMP — so it is refused. A strict compare of a
  // string with a number never coerces, so it folds to its constant.
  it('compares strings for equality only, and decides strict mixed compares statically', () => {
    const c = gen(`${D}export function App() {
      const [label] = useState('a');
      const [n] = useState(5);
      return (<View><Text>{label === 'a' ? 'y' : 'n'}</Text><Text>{'v=' + (label !== n)}</Text></View>);
    }`);
    expect(c).toContain('(strcmp(s_state.label, "a") == 0)');
    expect(c).toContain('((1) ? "true" : "false")');
    for (const expr of [`label < 'm'`, `label >= 'm'`])
      expect(() =>
        gen(
          `${D}export function App() { const [label] = useState('a'); return (<Text>{${expr} ? 'a' : 'b'}</Text>); }`,
        ),
      ).toThrow(/ordering strings with/);
    expect(() =>
      gen(
        `${D}export function App() { const [label] = useState('a'); return (<Text>{label == 5 ? 'a' : 'b'}</Text>); }`,
      ),
    ).toThrow(/cannot be compared with a number/);
  });

  it('tests a string condition for non-empty, never its address', () => {
    const c = gen(`${D}export function App() {
      const [label, setLabel] = useState('');
      const [n, setN] = useState(0);
      return (
        <Pressable onPress={() => { if (label) setN(1); }}>
          <Text>{label ? 'set' : 'empty'}</Text>
          <Text>{'v=' + !label}</Text>
        </Pressable>
      );
    }`);
    expect(c).toContain(`(s_state.label[0] != '\\0') ? "set" : "empty"`);
    expect(c).toContain(`(!((s_state.label[0] != '\\0')))`);
    expect(c).toContain(`if ((s_state.label[0] != '\\0'))`);
    expect(c).not.toMatch(/\(s_state\.label \?/);
  });

  it('gives a string handler local its own buffer, so the setter never aliases', () => {
    const c = gen(`${D}export function App() {
      const [label, setLabel] = useState('ab');
      return (<Pressable onPress={() => { const t = label; setLabel(t + '!'); }}><Text>{label}</Text></Pressable>);
    }`);
    expect(c).toContain('char l_t[48];');
    expect(c).toContain('snprintf(l_t, sizeof(l_t), "%s", s_state.label);');
    expect(c).toContain(
      'snprintf(s_state.label, sizeof(s_state.label), "%s!", l_t);',
    );
    expect(c).not.toContain('int l_t');
  });

  it('a <Switch> callback value prints as a boolean', () => {
    const c = gen(`${D}export function App() {
      const [on, setOn] = useState(false);
      const [label, setLabel] = useState('');
      return (<View><Switch value={on} onValueChange={v => { setOn(v); setLabel('on=' + v); }} /><Text>{label}</Text></View>);
    }`);
    expect(c).toMatch(/"on=%s", \(\(.*\) \? "true" : "false"\)/);
  });
});

// The global `undefined` folds like any constant — text needs it (`s + undefined` is "…undefined") — so
// every TYPED slot has to refuse it (and null / NaN) itself, with a location, or it reaches C as `NaN`.
describe('AOT typed slots refuse nothing-values', () => {
  const D = `import {useState, useRef} from 'react';
import {View, Text, useAnimatedValue} from 'embedded-react';
`;
  const err = src => {
    try {
      gen(src);
    } catch (e) {
      return e;
    }
    throw new Error('expected a compile error');
  };
  it.each([
    [
      'useState(undefined)',
      'const [n] = useState(undefined);',
      /initial value of state "n" must be a compile-time constant/,
    ],
    [
      'useState(null)',
      'const [n] = useState(null);',
      /initial value of state "n" is null/,
    ],
    [
      'useState(NaN)',
      'const [n] = useState(0 / 0);',
      /initial value of state "n" is NaN/,
    ],
    [
      'useAnimatedValue(undefined)',
      'const v = useAnimatedValue(undefined);',
      /initial value of useAnimatedValue "v" must be a compile-time constant/,
    ],
    [
      'useRef(undefined)',
      'const r = useRef(undefined);',
      /useRef initial for "r"/,
    ],
    ['useRef(NaN)', 'const r = useRef(0 / 0);', /useRef initial for "r"/],
  ])('%s is a located error, never C', (_n, decl, re) => {
    const e = err(
      `${D}export function App() { ${decl} return (<Text>x</Text>); }`,
    );
    expect(e.message).toMatch(re);
    expect(e.aotLoc).toBeTruthy();
  });

  it('a nullish style value is skipped, not judged as a key', () => {
    const c = gen(`${D}export function App() {
      return (<View style={{transform: undefined, width: null, height: 5}} />);
    }`);
    expect(c).toContain('p.height = 5;');
    expect(c).not.toMatch(/p\.width = [0-9]/); // the root's screen_w width is expected; a literal is not
  });

  it('undefined still renders as JS does in text', () => {
    const c = gen(`${D}export function App() {
      const [s] = useState('a');
      return (<View><Text>{undefined}</Text><Text>{s + undefined}</Text></View>);
    }`);
    expect(c).toContain('"%s", ""');
    expect(c).toContain('"%sundefined", s_state.s');
  });
});

describe('AOT demo marker encoding', () => {
  it('is one-to-one: names differing only in punctuation get distinct markers', () => {
    const h = name =>
      compileSource(`${PRE}export function App() { return (<View />); }`, name)
        .h;
    expect(h('foo-bar')).toContain('#define ER_AOT_DEMO_foo_2d_bar 1');
    expect(h('foo_bar')).toContain('#define ER_AOT_DEMO_foo_5f_bar 1');
    expect(h('thermostat')).toContain('#define ER_AOT_DEMO_thermostat 1');
    expect(h('my app')).toContain('#define ER_AOT_DEMO_my_20_app 1');
    expect(h('café')).toContain('#define ER_AOT_DEMO_caf_e9_ 1');
  });
});

// State, refs and animated values are runtime bindings. They follow the rule every other binding already
// did — beat a same-named module const in EVERY fold, not only the ones that consult env — which went
// unenforced for them: a style, prop, conditional or svg attribute silently used the module value.
describe('AOT hook bindings beat module consts', () => {
  const D = `import {useState, useRef} from 'react';
import {View, Text, Svg, Circle} from 'embedded-react';
`;
  const withState = jsx =>
    gen(`${D}const r = 10;
      export function App() { const [r, setR] = useState(4); return (${jsx}); }`);

  it('in a style', () => {
    const c = withState(`<View style={{width: r, borderTopLeftRadius: r}} />`);
    expect(c).toContain('p.width = app_round_dim(s_state.r);');
    expect(c).toContain('p.border_top_left_radius = app_round_dim(s_state.r);');
    expect(c).not.toMatch(/p\.(width|border_top_left_radius) = 10;/);
  });

  it('in a conditional child', () => {
    expect(withState(`<View>{r > 5 && <Text>big</Text>}</View>`)).toContain(
      '(s_state.r > 5)',
    );
  });

  it('as a prop into a child', () => {
    const c = gen(`${D}const r = 10;
      function C({v}) { return (<Text>{v}</Text>); }
      export function App() { const [r, setR] = useState(4); return (<View><C v={r} /></View>); }`);
    expect(c).toContain('"%d", s_state.r');
    expect(c).not.toContain('"%s", "10"');
  });

  it('in an svg attribute', () => {
    expect(
      withState(
        `<Svg width={40} height={40}><Circle cx={20} cy={20} r={r} fill="#fff" /></Svg>`,
      ),
    ).toContain('(float)(s_state.r)');
  });

  it('for a ref', () => {
    const c = gen(`${D}const w = 10;
      export function App() { const w = useRef(4); return (<View style={{width: w.current}} />); }`);
    expect(c).toContain('p.width = app_round_dim(s_ref_w);');
  });

  // A dynamic child const is unsupported, as it is in App — so it must fail loudly, never quietly fall
  // back to the module const it shadows.
  it("a child's dynamic const is a located error, never the module value", () => {
    const child = body =>
      `${D}const k = 99;
      function C({n}) { const k = n * 2; ${body} }
      export function App() { const [n] = useState(3); return (<View><C n={n} /></View>); }`;
    expect(() => gen(child(`return (<Text>{'k=' + k}</Text>);`))).toThrow(
      /cannot resolve identifier "k"/,
    );
    expect(() =>
      gen(child(`const [v] = useState(k); return (<Text>{v}</Text>);`)),
    ).toThrow(/initial value of state "v" must be a compile-time constant/);
  });
});

describe('AOT a string used as a condition', () => {
  const D = `import {useState} from 'react';
import {View, Text, Switch, Svg, Circle} from 'embedded-react';
`;

  it('a string in {cond && <X/>} is tested for non-empty, not its address', () => {
    const c = gen(`${D}export function App() {
      const [label] = useState('');
      return (<View>{label && <Text>x</Text>}</View>);
    }`);
    expect(c).toContain(`(s_state.label[0] != '\\0')`);
    expect(c).not.toMatch(/\(\(s_state\.label\) \?/);
  });

  it('a <Switch> driven by a string state tests non-empty, not the address', () => {
    const c = gen(`${D}export function App() {
      const [label] = useState('x');
      const [on, setOn] = useState(false);
      return (<Switch value={label} onValueChange={v => setOn(v)} />);
    }`);
    expect(c).toContain("(!((s_state.label[0] != '\\0')))");
    expect(c).toContain("(uint8_t)(((s_state.label[0] != '\\0')) ? 1 : 0)");
  });

  it('a conditional gradient driven by a string state tests non-empty', () => {
    const grad = `{type: 1, stops: [{color: '#ff0000', offset: 0}, {color: '#0000ff', offset: 1}], bx: 40}`;
    const svg = attr =>
      gen(`${D}export function App() {
        const [label] = useState('x');
        return (<Svg width={40} height={40}><Circle cx={20} cy={20} r={10} fillGrad={${attr}} /></Svg>);
      }`);
    for (const attr of [`label ? null : ${grad}`, `label && ${grad}`])
      expect(svg(attr)).toContain("s_state.label[0] != '\\0'");
  });
});

describe('AOT a string where a style needs a number', () => {
  it('a string state cannot drive a numeric style', () => {
    expect(() =>
      gen(`${PRE}export function App() {
        const [radius] = useState('10');
        return (<View style={{borderTopLeftRadius: radius}} />);
      }`),
    ).toThrow(/style "borderTopLeftRadius" needs a number/);
  });
});

describe('AOT boolean semantics', () => {
  const D = `import {useState} from 'react';
import {View, Text} from 'embedded-react';
`;

  it('a boolean is never strictly equal to a number', () => {
    const app = cond =>
      gen(`${D}export function App() {
        const [flag] = useState(true);
        return (<View>{${cond} && <Text>a</Text>}</View>);
      }`);
    expect(app('flag === 1')).not.toContain('s_state.flag == 1');
    expect(app('flag !== 1')).not.toContain('s_state.flag != 1');
    expect(app('flag == 1')).toContain('(s_state.flag == 1)');
    expect(app('flag === true')).toContain('(s_state.flag == 1)');
  });

  it('a nested span drops a boolean child', () => {
    const c = gen(`${D}export function App() {
      return (<Text>a<Text>{true}</Text>{false}</Text>);
    }`);
    expect(c).not.toContain('"true"');
    expect(c).not.toContain('"false"');
  });
});

describe('AOT name shadowing', () => {
  const D = `import {useState, useCallback} from 'react';
import {View, Text, Pressable} from 'embedded-react';
`;

  // A .map callback's params shadow outer bindings of the same name, as a JS arrow param does.
  it('a static .map item and index shadow a same-named state', () => {
    const c = gen(`${D}const ITEMS = [{key: 'a'}, {key: 'b'}];
      export function App() {
        const [it] = useState(0);
        const [i] = useState(7);
        return (<View>{ITEMS.map((it, i) => (<Text>{it.key + i}</Text>))}</View>);
      }`);
    expect(c).toContain('"%s", "a0"');
    expect(c).toContain('"%s", "b1"');
    expect(c).not.toContain('s_state.i');
  });

  it('a pooled .map row index shadows a same-named state', () => {
    const c = gen(`${D}export function App() {
      const [items] = useState([{k: 'a'}]);
      const [i] = useState(7);
      return (<View>{items.map((it, i) => (<Text>{'i=' + i}</Text>))}</View>);
    }`);
    expect(c).toContain('"%s", "i=0"');
    expect(c).not.toContain('s_state.i)');
  });

  it('a .map parameter shadows a callback of the same name', () => {
    expect(() =>
      gen(`${D}export function App() {
        const [n, setN] = useState(0);
        const onTap = useCallback(() => setN(n + 1), [n]);
        return (<View>{['a'].map(onTap => <Pressable key={onTap} onPress={onTap}><Text>x</Text></Pressable>)}</View>);
      }`),
    ).toThrow(/onPress must be an inline function/);
  });

  it('a hook binding shadows a module const before the local consts fold', () => {
    expect(() =>
      gen(`${D}const r = 10;
        export function App() { const [r] = useState(4); const copy = r; return (<Text>{copy}</Text>); }`),
    ).toThrow(/cannot resolve identifier "copy"/);
    expect(() =>
      gen(`${D}const r = 10;
        function C() { const [r] = useState(4); const copy = r; return (<Text>{copy}</Text>); }
        export function App() { return (<View><C /></View>); }`),
    ).toThrow(/cannot resolve identifier "copy"/);
  });

  it("App's dynamic const is a located error, never the module value it shadows", () => {
    const src = use =>
      `${D}const L = 'module';
      export function App() { const [n] = useState(3); const L = n; return (${use}); }`;
    for (const use of [
      `<Text>{'L=' + L}</Text>`,
      `<View style={{width: L}} />`,
    ])
      expect(() => gen(src(use))).toThrow(/cannot resolve identifier "L"/);
  });
});

describe('AOT undefined as a value', () => {
  const D = `import {useState} from 'react';
import {View, Text, TextInput} from 'embedded-react';
`;

  it('a const that is undefined folds like any other', () => {
    const c = gen(`${D}const EMPTY = undefined;
      export function App() {
        const empty = undefined;
        return (<View><Text>{EMPTY}</Text><Text>{empty}</Text></View>);
      }`);
    expect(c).toContain('"%s", ""');
    expect(() =>
      gen(`${D}const EMPTY = undefined;
        export function App() { const [n] = useState(EMPTY); return (<Text>x</Text>); }`),
    ).toThrow(/initial value of state "n" is undefined/);
  });

  // Flow A omits an undefined prop; so does Flow B — every reader sees the attribute as absent.
  it('an attribute set to undefined is omitted, as in Flow A', () => {
    const c = gen(`${D}export function App() {
      const [v, setV] = useState('');
      return (
        <View>
          <View visible={undefined} style={{width: 5}} />
          <TextInput value={v} placeholder={undefined} onChangeText={t => setV(t)} />
        </View>
      );
    }`);
    expect(c).toContain('p.width = 5;');
    expect(c).not.toContain('ER_DISPLAY_NONE');
    expect(c).not.toContain('placeholder');
  });

  it('an undefined or null style, or style-array entry, means no style', () => {
    expect(() =>
      gen(`${D}export function App() { return (<View style={undefined} />); }`),
    ).not.toThrow();
    expect(() =>
      gen(
        `${D}const s = null;\nexport function App() { return (<View style={s} />); }`,
      ),
    ).not.toThrow();
    expect(
      gen(
        `${D}const s = {width: 7};\nexport function App() { return (<View style={[s, undefined]} />); }`,
      ),
    ).toContain('p.width = 7;');
  });

  it('a binding named undefined is a located error', () => {
    let e;
    try {
      gen(`${D}export function App() {
        const [undefined] = useState('x');
        return (<Text>{undefined}</Text>);
      }`);
    } catch (x) {
      e = x;
    }
    expect(e?.message).toMatch(/`undefined` cannot be used as a name/);
    expect(e?.aotLoc).toBeTruthy();
  });
});

describe('AOT unary arithmetic on a string', () => {
  it.each(['+', '-'])('rejects unary %s on a string', op => {
    expect(() =>
      gen(
        `${PRE}export function App() { const [label] = useState('5'); return (<Text>{'v=' + (${op}label)}</Text>); }`,
      ),
    ).toThrow(new RegExp(`unary "\\${op}" on a string is not supported`));
  });
});

// An attribute that FOLDS to undefined — a prop a child was never given — is omitted like a literal
// {undefined}, as in Flow A. Read as a value, `visible` hid the node and `placeholder` printed the word
// "undefined"; both did so on master too.
describe('AOT props that fold to undefined are omitted', () => {
  const D = `import {useState} from 'react';
import {View, Text, Pressable, TextInput} from 'embedded-react';
`;
  const withChild = child =>
    gen(`${D}${child}
      export function App() { return (<View><C /></View>); }`);

  it('visible from an absent prop leaves the node visible', () => {
    const c = withChild(
      `function C({x}) { return (<View visible={x} style={{width: 5}} />); }`,
    );
    expect(c).not.toContain('ER_DISPLAY_NONE');
    expect(c).toContain('p.width = 5;');
  });

  it('placeholder from an absent prop is not the word "undefined"', () => {
    const c = withChild(
      `function C({x}) { const [v, setV] = useState(''); return (<TextInput value={v} placeholder={x} onChangeText={t => setV(t)} />); }`,
    );
    expect(c).not.toContain('"undefined"');
  });

  it('a handler from an absent prop is simply not attached', () => {
    const c = withChild(
      `function C({x}) { return (<Pressable onPress={x}><Text>t</Text></Pressable>); }`,
    );
    expect(c).not.toContain('ER_EVENT_PRESS,');
  });

  it('a statically-decided undefined branch is omitted too', () => {
    const c = gen(`${D}const SHOW = false;
      export function App() { return (<View><View visible={SHOW ? true : undefined} style={{width: 5}} /></View>); }`);
    expect(c).not.toContain('p.display');
  });

  // The rule copies the element rather than editing the shared JSX node, so one instance's answer must
  // not leak into another's.
  it('the same JSX in two scopes gets each its own answer', () => {
    const c =
      gen(`${D}function C({x}) { return (<View visible={x} style={{width: 5}} />); }
      export function App() { return (<View><C /><C x={false} /></View>); }`);
    expect((c.match(/p\.display = ER_DISPLAY_NONE;/g) || []).length).toBe(1);
  });
});

describe('AOT fixed-slot truncation', () => {
  // Every string the generated file writes lands in a fixed-size slot, so an over-long value truncates by
  // design. GCC reports that intent for any format mixing %s with anything else, and ESP-IDF builds with
  // -Werror — so without this the ordinary `{'n=' + name}` fails on the device. Clang has no such warning,
  // which is why a clang-only smoke test called the same code clean.
  it('tells GCC the truncation is intended, and keeps clang out of it', () => {
    const c = gen(`${PRE}
      export function App() {
        const [name, setName] = useState('x');
        return (<Text>{'n=' + name}</Text>);
      }`);
    expect(c).toContain('#if defined(__GNUC__) && !defined(__clang__)');
    expect(c).toContain('#pragma GCC diagnostic ignored "-Wformat-truncation"');
    // The suppression has to precede the code it covers.
    expect(c.indexOf('#pragma GCC diagnostic ignored')).toBeLessThan(
      c.indexOf('snprintf(p.text'),
    );
  });
});

describe('AOT generated-C portability', () => {
  it('emits a guarded M_PI fallback when the app uses math (M_PI is not in ISO C99 <math.h>)', () => {
    const c = compileSource(
      `import { Text } from 'embedded-react';
import { useState } from 'react';
export function App() { const [n] = useState(0); return (<Text>{Math.round(n * Math.PI)}</Text>); }`,
      'demo',
    ).c;
    expect(c).toContain('#include <math.h>');
    expect(c).toContain('#ifndef M_PI');
    expect(c).toContain('#define M_PI 3.14159');
  });
});

describe('AOT delayLongPress', () => {
  const D = `import { View, Text, Pressable, TouchableOpacity } from 'embedded-react';
`;

  it('bakes the hold time into the node props', () => {
    const c = gen(`${D}
      export function App() {
        return (<Pressable delayLongPress={800} onLongPress={() => {}}><Text>x</Text></Pressable>);
      }`);
    expect(c).toContain('p.long_press_ms = 800;');
    expect(c).toContain('er_event_set(n0, ER_EVENT_LONG_PRESS');
  });

  it('folds a module-level constant, and takes the prop on a TouchableOpacity too', () => {
    const c = gen(`${D}
      const HOLD = 250;
      export function App() {
        return (<TouchableOpacity delayLongPress={HOLD} onLongPress={() => {}}><Text>x</Text></TouchableOpacity>);
      }`);
    expect(c).toContain('p.long_press_ms = 250;');
  });

  // 0 is the engine's "no delayLongPress set" sentinel, so it cannot mean zero — 1 ms is the next tick,
  // which is what RN's setTimeout(0) amounts to.
  it('clamps 0 up to the next tick rather than reading as unset', () => {
    const c = gen(`${D}
      export function App() {
        return (<Pressable delayLongPress={0} onLongPress={() => {}}><Text>x</Text></Pressable>);
      }`);
    expect(c).toContain('p.long_press_ms = 1;');
  });

  it('rejects a value that does not fold', () => {
    expect(() =>
      gen(`${D}import { useState } from 'react';
      export function App() {
        const [ms] = useState(700);
        return (<Pressable delayLongPress={ms} onLongPress={() => {}}><Text>x</Text></Pressable>);
      }`),
    ).toThrow(/delayLongPress> must fold to a number/);
  });
});

describe('AOT string concatenation in text', () => {
  const D = `import { useState } from 'react';
import { View, Text, TextInput } from 'embedded-react';
`;

  it('lowers a `+` chain in a <Text> child to a printf format + args', () => {
    const c = gen(`${D}
      const PAGES = 4;
      export function App() {
        const [page, setPage] = useState(0);
        return (<Text>{'render-check ' + (page + 1) + '/' + PAGES}</Text>);
      }`);
    expect(c).toContain(
      'snprintf(p.text, sizeof(p.text), "render-check %d/4", app_add(s_state.page, 1));',
    );
  });

  it('picks the specifier from the part, not the chain', () => {
    const c = gen(`${D}
      export function App() {
        const [hit, setHit] = useState('none');
        const [t, setT] = useState(0.5);
        return (<View><Text>{'hit: ' + hit}</Text><Text>{'t=' + t}</Text></View>);
      }`);
    expect(c).toContain('"hit: %s", s_state.hit');
    expect(c).toContain('"t=%g", s_state.t');
  });

  // JS types a `+` left to right, so the string only starts at the first string operand: `n + 1` still
  // adds. Flattening the chain blindly would emit "%d%d ms" and print "51 ms" where JS prints "6 ms".
  it('keeps arithmetic that runs before the first string operand', () => {
    const c = gen(`${D}
      export function App() {
        const [n, setN] = useState(5);
        return (<Text>{n + 1 + ' ms'}</Text>);
      }`);
    expect(c).toContain('"%d ms", app_add(s_state.n, 1)');
  });

  it('appends past the first string operand, as JS does', () => {
    const c = gen(`${D}
      export function App() {
        const [n, setN] = useState(5);
        return (<Text>{'a' + n + 2}</Text>);
      }`);
    expect(c).toContain('"a%d2", s_state.n');
  });

  // `"a" + null === "anull"` in JS, but a standalone {null} child draws nothing. The two contexts need
  // different handling of the same value.
  it('stringifies a nullish concat operand, but not a standalone child', () => {
    const c = gen(`${D}
      export function App() {
        const [label, setLabel] = useState('hi');
        return (<View><Text>{label + null}</Text><Text>{null}</Text></View>);
      }`);
    expect(c).toContain('"%snull", s_state.label');
    expect(c).toContain('snprintf(p.text, sizeof(p.text), "%s", "");');
  });

  // The AOT stores a boolean in an int slot, so the type alone cannot tell useState(false) from
  // useState(0). JS prints a boolean as "true"/"false"; React draws nothing for one as a child.
  it('prints a boolean operand the way JS does', () => {
    const c = gen(`${D}
      export function App() {
        const [on, setOn] = useState(false);
        return (<Text>{'enabled: ' + on}</Text>);
      }`);
    expect(c).toContain('"enabled: %s", ((s_state.on) ? "true" : "false")');
  });

  it('draws nothing for a boolean as a standalone child, like React', () => {
    const c = gen(`${D}
      export function App() {
        const [on, setOn] = useState(false);
        return (<View><Text>{on}</Text><Text>{false}</Text></View>);
      }`);
    expect(c).not.toContain('%d');
    expect(c.match(/snprintf\(p\.text[^\n]*/g)).toEqual([
      'snprintf(p.text, sizeof(p.text), "%s", "");',
      'snprintf(p.text, sizeof(p.text), "%s", "");',
    ]);
  });

  it.each([
    ['a comparison', 'n > 5'],
    ['a negation', '!on'],
    ['a ternary of booleans', 'n ? on : !on'],
    ['&& of two booleans', 'on && n > 5'],
  ])('treats %s as a boolean', (_name, expr) => {
    const c = gen(`${D}
      export function App() {
        const [n, setN] = useState(0);
        const [on, setOn] = useState(false);
        return (<Text>{'v: ' + (${expr})}</Text>);
      }`);
    expect(c).toContain('? "true" : "false"');
  });

  // `on && 'yes'` is 'yes' or false in JS, and C's && only has 0/1 — neither value survives. Printing
  // "1" (or relabelling it "true") is a wrong answer either way, so text refuses the shape instead.
  const textErr = body => {
    try {
      gen(`${D}
      export function App() {
        const [n, setN] = useState(0);
        const [m, setM] = useState(1);
        const [on, setOn] = useState(false);
        return (${body});
      }`);
    } catch (e) {
      return e;
    }
    throw new Error('expected a compile error');
  };

  it.each([
    ['&& yielding a string', `<Text>{'x: ' + (on && 'yes')}</Text>`],
    ['&& as a bare child', `<Text>{on && 'yes'}</Text>`],
    ['|| yielding a string', `<Text>{'x: ' + (on || 'no')}</Text>`],
    ['&& of two numbers', `<Text>{'x: ' + (n && m)}</Text>`],
  ])('refuses %s in text, with a location', (_name, body) => {
    const e = textErr(body);
    expect(e.message).toMatch(/evaluates to one of its operands/);
    expect(e.aotLoc).toBeTruthy();
    expect(e.message).toContain('^');
  });

  // `(ok ? 1 : "none")` is ill-typed C — int against char* — and has no single printf spec either.
  it.each([
    ['numeric then string', `on ? 1 : 'none'`],
    ['string then numeric', `on ? 'none' : 1`],
  ])('refuses a ternary mixing %s', (_name, expr) => {
    const e = textErr(`<Text>{'x: ' + (${expr})}</Text>`);
    expect(e.message).toMatch(/cannot mix a string branch with a numeric one/);
    expect(e.aotLoc).toBeTruthy();
  });

  it('refuses the same mix outside text, e.g. in a setter', () => {
    let err;
    try {
      gen(`${D}import {Pressable} from 'embedded-react';
      export function App() {
        const [ok, setOk] = useState(false);
        const [label, setLabel] = useState('hi');
        return (<Pressable onPress={() => setLabel(ok ? 1 : 'none')}><Text>{label}</Text></Pressable>);
      }`);
    } catch (e) {
      err = e;
    }
    expect(err.message).toMatch(
      /cannot mix a string branch with a numeric one/,
    );
  });

  it('still allows a ternary whose branches agree', () => {
    const c = gen(`${D}
      export function App() {
        const [ok, setOk] = useState(false);
        return (<View><Text>{'a: ' + (ok ? 'x' : 'y')}</Text><Text>{'b: ' + (ok ? 1 : 2.5)}</Text></View>);
      }`);
    expect(c).toContain('"a: %s", (s_state.ok ? "x" : "y")');
    expect(c).toContain('"b: %g", (s_state.ok ? 1 : 2.5f)');
  });

  // A branch that concatenates would need a format of its own; one snprintf has one. The generic
  // "not supported in this position" used to fire first, with a hint that text buffers are fine — which
  // is exactly wrong inside a text buffer.
  it('names a ternary whose branch concatenates, instead of the generic position error', () => {
    const e = textErr(`<Text>{'x: ' + (on ? 'a' + n : 'b')}</Text>`);
    expect(e.message).toMatch(
      /ternary in text cannot concatenate inside its test or a branch/,
    );
    expect(e.message).not.toMatch(/not supported in this position/);
    expect(e.aotLoc).toBeTruthy();
  });

  it('names a logical whose operand concatenates, and a ternary whose test does', () => {
    const e1 = textErr(`<Text>{on && ('n=' + n)}</Text>`);
    expect(e1.message).toMatch(
      /"&&" in text cannot concatenate inside an operand/,
    );
    const e2 = textErr(`<Text>{('a' + n) ? 'x' : 'y'}</Text>`);
    expect(e2.message).toMatch(
      /cannot concatenate inside its test or a branch/,
    );
  });

  it('refuses a ternary mixing a boolean branch with a non-boolean one', () => {
    const e = textErr(`<Text>{'x: ' + (on ? on : 5)}</Text>`);
    expect(e.message).toMatch(/mixes a boolean branch/);
    expect(e.aotLoc).toBeTruthy();
  });

  it('still lowers a logical whose operands are both boolean', () => {
    const c = gen(`${D}
      export function App() {
        const [n, setN] = useState(0);
        const [on, setOn] = useState(false);
        return (<Text>{'x: ' + (on && n > 1)}</Text>);
      }`);
    expect(c).toContain('? "true" : "false"');
  });

  // Unary +/- are numeric coercions: `+flag` is 0 or 1 in JS, never true/false.
  it.each([
    ['+', `+on`],
    ['-', `-on`],
  ])('does not carry boolean-ness through unary %s', (_op, expr) => {
    const c = gen(`${D}
      export function App() {
        const [on, setOn] = useState(false);
        return (<Text>{'v=' + (${expr})}</Text>);
      }`);
    expect(c).toContain('"v=%d"');
    expect(c).not.toContain('? "true" : "false"');
  });

  it('renders a coerced boolean as a number child, not an empty one', () => {
    const c = gen(`${D}
      export function App() {
        const [on, setOn] = useState(false);
        return (<Text>{+on}</Text>);
      }`);
    expect(c).toContain('"%d", (+(s_state.on))');
  });

  it('keeps a boolean local through a handler const', () => {
    const c = gen(`${D}import {Pressable} from 'embedded-react';
      export function App() {
        const [n, setN] = useState(0);
        const [label, setLabel] = useState('');
        return (<Pressable onPress={() => { const hot = n > 5; setLabel('hot: ' + hot); }}><Text>{label}</Text></Pressable>);
      }`);
    expect(c).toContain('"hot: %s", ((l_hot) ? "true" : "false")');
  });

  it('still prints numbers, floats and strings by type', () => {
    const c = gen(`${D}
      export function App() {
        const [n, setN] = useState(0);
        const [f, setF] = useState(1.5);
        const [s, setS] = useState('x');
        return (<View><Text>{'a' + n}</Text><Text>{'b' + f}</Text><Text>{'c' + s}</Text></View>);
      }`);
    expect(c).toContain('"a%d", s_state.n');
    expect(c).toContain('"b%g", s_state.f');
    expect(c).toContain('"c%s", s_state.s');
  });

  // snprintf may not read and write overlapping objects (C11 7.21.6.6), and a self-referential setter
  // feeds the slot its own contents.
  it('builds a self-referential string setter in a temporary', () => {
    const c = gen(`${D}import {Pressable} from 'embedded-react';
      export function App() {
        const [label, setLabel] = useState('hi');
        return (<Pressable onPress={() => setLabel(label + '!')}><Text>{label}</Text></Pressable>);
      }`);
    expect(c).toContain('char next[sizeof(s_state.label)];');
    expect(c).toContain('snprintf(next, sizeof(next), "%s!", s_state.label);');
    expect(c).toContain('memcpy(s_state.label, next, strlen(next) + 1);');
    expect(c).not.toContain(
      'snprintf(s_state.label, sizeof(s_state.label), "%s!"',
    );
  });

  it('writes a non-self-referential setter straight to the slot', () => {
    const c = gen(`${D}import {Pressable} from 'embedded-react';
      export function App() {
        const [n, setN] = useState(0);
        const [label, setLabel] = useState('hi');
        return (<Pressable onPress={() => setLabel('x' + n)}><Text>{label}</Text></Pressable>);
      }`);
    expect(c).toContain(
      'snprintf(s_state.label, sizeof(s_state.label), "x%d", s_state.n);',
    );
    expect(c).not.toContain('char next[');
  });

  // A near-miss name must not be mistaken for the destination.
  it('does not take a different slot with a shared prefix as self-reference', () => {
    const c = gen(`${D}import {Pressable} from 'embedded-react';
      export function App() {
        const [label, setLabel] = useState('hi');
        const [label2, setLabel2] = useState('yo');
        return (<Pressable onPress={() => setLabel(label2 + '!')}><Text>{label}</Text></Pressable>);
      }`);
    expect(c).not.toContain('char next[');
  });

  it('escapes a literal % so it survives the format string', () => {
    const c = gen(`${D}
      export function App() {
        const [pct, setPct] = useState(0);
        return (<Text>{pct + '% full'}</Text>);
      }`);
    expect(c).toContain('"%d%% full", s_state.pct');
  });

  it('folds a fully static chain into the literal', () => {
    const c = gen(`${D}
      const NAME = 'watch';
      export function App() {
        return (<Text>{'demo: ' + NAME}</Text>);
      }`);
    expect(c).toContain('"demo: watch"');
    expect(c).not.toContain('s_state');
  });

  it('builds a string state slot from a concatenation', () => {
    const c = gen(`${D}
      export function App() {
        const [n, setN] = useState(0);
        const [label, setLabel] = useState('');
        return (<Text onPress={() => setLabel('hit ' + n)}>x</Text>);
      }`);
    expect(c).toContain(
      'snprintf(s_state.label, sizeof(s_state.label), "hit %d", s_state.n);',
    );
  });

  it('builds a <TextInput value> from a concatenation', () => {
    const c = gen(`${D}
      export function App() {
        const [n, setN] = useState(0);
        return (<TextInput value={'#' + n} />);
      }`);
    expect(c).toContain('"#%d", s_state.n');
  });

  // Outside a text buffer there is no format string to lower into, so it must be a located error rather
  // than C that only the host compiler rejects.
  it('rejects a concatenation where the value is not text', () => {
    let err;
    try {
      gen(`${D}
      export function App() {
        const [n, setN] = useState(0);
        const [s, setS] = useState('x');
        return (<Text onPress={() => setN('a' + s)}>x</Text>);
      }`);
    } catch (e) {
      err = e;
    }
    expect(err.message).toMatch(/string concatenation is not supported/);
    expect(err.aotLoc).toBeTruthy();
    expect(err.message).toContain('^');
  });
});

describe('AOT per-corner border radii', () => {
  const D = `import { useState } from 'react';
import { View, StyleSheet } from 'embedded-react';
`;

  it('lowers the four corners to their ERProps fields', () => {
    const c = gen(`${D}
      export function App() {
        return (<View style={{borderTopLeftRadius: 2, borderTopRightRadius: 10,
                              borderBottomRightRadius: 20, borderBottomLeftRadius: 0}} />);
      }`);
    expect(c).toContain('p.border_top_left_radius = 2;');
    expect(c).toContain('p.border_top_right_radius = 10;');
    expect(c).toContain('p.border_bottom_right_radius = 20;');
    expect(c).toContain('p.border_bottom_left_radius = 0;');
  });

  it('takes them state-driven too', () => {
    const c = gen(`${D}
      export function App() {
        const [r, setR] = useState(4);
        return (<View style={{borderTopLeftRadius: r}} />);
      }`);
    expect(c).toContain('p.border_top_left_radius = app_round_dim(s_state.r);');
  });

  it('takes them from a StyleSheet', () => {
    const c = gen(`${D}
      const styles = StyleSheet.create({ card: { borderTopLeftRadius: 6 } });
      export function App() {
        return (<View style={styles.card} />);
      }`);
    expect(c).toContain('p.border_top_left_radius = 6;');
  });
});

describe('AOT style diagnostics', () => {
  const D = `import { useState } from 'react';
import { View, StyleSheet } from 'embedded-react';
`;
  const err = src => {
    try {
      gen(src);
    } catch (e) {
      return e;
    }
    throw new Error('expected a compile error');
  };

  // A key the static lowering does not know used to fall through to the dynamic branch and be reported
  // as state-driven, which sent the author looking for state that isn't there.
  it('names an unknown style key as unknown, not as state-driven', () => {
    const e = err(`${D}
      export function App() {
        return (<View style={{transform: [{scale: 2}]}} />);
      }`);
    expect(e.message).toMatch(/style "transform" is not supported/);
    expect(e.message).not.toMatch(/state-driven/);
    expect(e.aotHint).toMatch(/Flow B lowers these:/);
  });

  it('says the same for an unknown key given a state-driven value', () => {
    const e = err(`${D}
      export function App() {
        const [n, setN] = useState(0);
        return (<View style={{shadowRadius: n}} />);
      }`);
    expect(e.message).toMatch(/style "shadowRadius" is not supported/);
    expect(e.message).not.toMatch(/state-driven/);
  });

  it('still reports a state-driven value on a known key as state-driven', () => {
    const e = err(`${D}
      export function App() {
        const [n, setN] = useState(0);
        return (<View style={{flex: n}} />);
      }`);
    expect(e.message).toMatch(/state-driven value for style "flex"/);
  });

  it('reports a bad value on a known key as a bad value, with a location', () => {
    const e = err(`${D}
      export function App() {
        return (<View style={{alignItems: 'baseline'}} />);
      }`);
    expect(e.message).toMatch(/unsupported value for style "alignItems"/);
    expect(e.message).toMatch(/one of auto, flex-start/);
    expect(e.aotLoc).toBeTruthy();
  });

  // DYN_FIELDS/KEYS are plain objects, so `style={{toString: n}}` used to find Object.prototype's method,
  // look like a known key, and emit `p.undefined = …` — invalid C, from the compiler that is supposed to
  // reject unsupported styles up front.
  it.each(['toString', 'constructor', 'valueOf', 'hasOwnProperty'])(
    'rejects the inherited Object.prototype key "%s" as an unknown style',
    key => {
      const e = err(`${D}
      export function App() {
        const [n, setN] = useState(0);
        return (<View style={{${key}: n}} />);
      }`);
      expect(e.message).toMatch(new RegExp(`style "${key}" is not supported`));
    },
  );

  it('does not emit an undefined ERProps field for a prototype key', () => {
    expect(() =>
      gen(`${D}
      export function App() {
        const [n, setN] = useState(0);
        return (<View style={{toString: n}} />);
      }`),
    ).toThrow();
    // And the static path too, which goes through lowerStyle's own key lookup.
    expect(() =>
      gen(`${D}
      export function App() {
        return (<View style={{valueOf: 4}} />);
      }`),
    ).toThrow(/style "valueOf" is not supported/);
  });

  it('locates an unknown key that arrived through a StyleSheet', () => {
    const e = err(`${D}
      const styles = StyleSheet.create({ card: { shadowOpacity: 0.5 } });
      export function App() {
        return (<View style={styles.card} />);
      }`);
    expect(e.message).toMatch(/style "shadowOpacity" is not supported/);
    expect(e.aotLoc).toBeTruthy();
  });
});

describe('AOT list setters', () => {
  const app = body =>
    gen(`${PRE}
      export function App() {
        const [k, setK] = useState(-2);
        const [f, setF] = useState(0.5);
        const [items, setItems] = useState([{w: 1}, {w: 2}, {w: 3}]);
        return (<Pressable onPress={() => { ${body} }}><Text>x</Text></Pressable>);
      }`);

  // A negative count would send the next append to s_items[-n], outside the array.
  it('keeps the count in range for a runtime slice end, counting a negative one back from the length', () => {
    const c = app('setItems(items.slice(0, k));');
    expect(c).toContain(
      's_items_count = app_slice_len(s_items_count, s_state.k);',
    );
    expect(c).toContain('static int app_slice_len(int len, int end)');
    expect(c).toContain('return end <= -len ? 0 : len + end;');
  });

  it('settles a constant slice end at compile time', () => {
    const c = app(
      'setItems(items.slice(0, 2)); setItems(items.slice(0, -1)); setItems(items.slice(0, -2)); setItems(items.slice(0, 1e12));',
    );
    expect(c).toContain(
      's_items_count = (s_items_count < 2) ? s_items_count : 2;',
    );
    expect(c).toContain('if (s_items_count > 0) s_items_count--;');
    expect(c).toContain('s_items_count = app_slice_len(s_items_count, -2);');
    // Past the capacity an end keeps every item, so a huge one clamps there and needs no 64-bit math.
    expect(c).toContain(
      's_items_count = (s_items_count < 16) ? s_items_count : 16;',
    );
  });

  it('truncates a float slice end the way JS does', () => {
    expect(app('setItems(items.slice(0, f));')).toContain(
      's_items_count = app_slice_len(s_items_count, app_f2i(s_state.f));',
    );
  });

  it('keeps every item for slice(0) and slice()', () => {
    const c = app('setItems(items.slice(0)); setItems(items.slice());');
    expect(c).not.toMatch(/^\s+s_items_count\b/m);
    expect(c).not.toContain('app_slice_len');
  });

  it('refuses a slice that drops items from the front', () => {
    expect(() => app('setItems(items.slice(1, 3));')).toThrow(
      /only items\.slice\(0, end\) is supported/,
    );
    expect(() => app('setItems(items.slice(k));')).toThrow(
      /only items\.slice\(0, end\) is supported/,
    );
  });
});
