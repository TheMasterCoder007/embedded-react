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

import { describe, it, expect } from 'vitest';
import { compileSource } from '../compile.mjs';

// The Flow B AOT compiler turns an App.jsx source string into C. These tests assert on the generated C
// (compileSource is pure — no file I/O), so each fixture is a complete minimal app. `gen` returns the
// .c text; `PRE` is the usual import preamble (the compiler pattern-matches names, it does not resolve
// imports, so the import line is cosmetic but kept for realism).
const PRE = `import { useState } from 'react';
import { View, Text, Pressable, StyleSheet, Animated, useAnimatedValue } from 'embedded-react';
`;
const gen = (src) => compileSource(src, 'test').c;

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
    expect(c).toContain('s_state.n = (s_state.n + 1);');
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

  it('compiles multiple sequential setters in one handler with a single app_update', () => {
    const c = gen(`${PRE}
      export function App() {
        const [a, setA] = useState(0);
        const [b, setB] = useState(0);
        return (<Pressable onPress={() => { setA(a + 1); setB(b - 1); }}><Text>x</Text></Pressable>);
      }`);
    expect(c).toContain('s_state.a = (s_state.a + 1);');
    expect(c).toContain('s_state.b = (s_state.b - 1);');
    // exactly one app_update per handler body
    const handler = c.slice(c.indexOf('er_handler_0'));
    expect(handler.slice(0, handler.indexOf('}')).match(/app_update\(\);/g)).toHaveLength(1);
  });

  it('compiles a handler local const used by a later setter', () => {
    const c = gen(`${PRE}
      export function App() {
        const [n, setN] = useState(0);
        return (<Pressable onPress={() => { const step = 5; setN(n + step); }}><Text>x</Text></Pressable>);
      }`);
    expect(c).toContain('int l_step = 5;');
    expect(c).toContain('s_state.n = (s_state.n + l_step);');
  });

  it('compiles a branching (if/else) handler', () => {
    const c = gen(`${PRE}
      export function App() {
        const [n, setN] = useState(0);
        return (<Pressable onPress={() => { if (n > 10) { setN(0); } else { setN(n + 1); } }}><Text>x</Text></Pressable>);
      }`);
    expect(c).toContain('if ((s_state.n > 10))');
    expect(c).toContain('s_state.n = 0;');
    expect(c).toContain('s_state.n = (s_state.n + 1);');
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
    expect(c).toContain('s_ref_taps = (s_ref_taps + 1);');
    expect(c).toContain('s_state.n = s_ref_taps;');
  });

  it('a ref-only handler does not emit app_update (refs do not re-render)', () => {
    const c = gen(`${PRE}
      import { useRef } from 'react';
      export function App() {
        const acc = useRef(0);
        return (<Pressable onPress={() => { acc.current++; }}><Text>x</Text></Pressable>);
      }`);
    expect(c).toContain('s_ref_acc++;');
    const handler = c.slice(c.indexOf('er_handler_0('));
    expect(handler.slice(0, handler.indexOf('\n}')).includes('app_update();')).toBe(false);
  });

  it('lowers a string useState to a char buffer + snprintf setter + %s text', () => {
    const c = gen(`${PRE}
      export function App() {
        const [label, setLabel] = useState('Idle');
        return (<Pressable onPress={() => setLabel('Active')}><Text>{label}</Text></Pressable>);
      }`);
    expect(c).toMatch(/char label\[\d+\];/);
    expect(c).toContain('.label = "Idle"');
    expect(c).toContain('snprintf(s_state.label, sizeof(s_state.label), "%s", "Active");');
    // the Text node renders the string with %s
    expect(c).toContain('snprintf(p.text, sizeof(p.text), "%s", s_state.label);');
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
    expect(c.match(/er_event_set\([^,]+, ER_EVENT_PRESS, er_cb_onTap, NULL\);/g)).toHaveLength(2);
  });

  it('inlines a state-dependent useMemo at its use site', () => {
    const c = gen(`${PRE}
      import { useMemo } from 'react';
      export function App() {
        const [n, setN] = useState(2);
        const doubled = useMemo(() => n * 2, [n]);
        return (<Text>Value {doubled}</Text>);
      }`);
    expect(c).toContain('(s_state.n * 2)');
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
    expect(c).toMatch(/er_node_set_vector_ops\(n\d+, s_svg0_ops, \d+, s_svg0_paints, 2\);/);
    expect(c).toContain('.stroke_w = 3.0f');           // circle stroke width baked
    expect(c).toContain('p.width = (int16_t)100;');     // node box from Svg width
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
    expect(c).toMatch(/static float s_svg0_ops\[\d+\];/);   // mutable, not const
    expect(c).toContain('static void build_svg0(void)');
    expect(c).toContain('cosf(');                            // arc trig in C
    expect(c).toContain('(s_state.temp * 2)');               // dynamic endAngle expression
    expect(c).toContain('build_svg0();');
    expect(c).toMatch(/er_node_set_vector_ops\(s_n\d+, s_svg0_ops, \d+, s_svg0_paints, 2\);/); // re-upload in app_update
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
    expect(c).toMatch(/static ERVectorPaint s_svg0_paints\[1\];/);      // MUTABLE table (not const)
    expect(c).not.toMatch(/static const ERVectorPaint s_svg0_paints/);  // ...specifically not const
    // dynamic stroke color → an ARGB ternary, assigned in build_svg0
    expect(c).toContain('s_svg0_paints[0].stroke = (((strcmp(s_state.mode, "cool") == 0)) ? 0xFF4CC9F0u : 0xFFF4A261u);');
    // dynamic stroke width → a numeric ternary cast to float
    expect(c).toContain('s_svg0_paints[0].stroke_w = (float)(((strcmp(s_state.mode, "off") == 0) ? 2 : 8));');
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
    expect(c).not.toContain('s_svg0_paints[0].stroke =');                   // paint NOT reassigned per update
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
    expect(c).toContain('static ERNode* s_ref_dial = NULL;');     // node ref slot
    expect(c).toMatch(/s_ref_dial = n\d+;/);                       // captured at build
    expect(c).toContain('#include <math.h>');                     // arc trig
    expect(c).toMatch(/static float s_uv0_ops\[\d+\];/);          // imperative op buffer
    expect(c).toContain('s_uv0_ops[0] = ER_VOP_SHAPE;');
    expect(c).toContain('data->x');                                // event coord in arc endAngle
    expect(c).toMatch(/er_node_set_vector_ops\(s_ref_dial, s_uv0_ops, \d+, s_uv0_paints, 1\);/);
    expect(c).toContain('er_node_set_vector_dirty_rect(s_ref_dial, 0, 0, 200, 200);');
  });

  it('recognizes a memo()-wrapped component', () => {
    const c = gen(`${PRE}
      import { memo } from 'react';
      const Badge = memo(function Badge({ label }) { return (<Text>{label}</Text>); });
      export function App() { return (<View><Badge label="hi" /></View>); }`);
    expect(c).toContain('"hi"');                    // inlined, prop substituted
    expect(c).toContain('er_node_create(ER_NODE_TEXT)');
  });

  it('emits props.children (destructured) passed to a component', () => {
    const c = gen(`${PRE}
      function Card({ children }) { return (<View style={{ padding: 8 }}>{children}</View>); }
      export function App() { return (<Card><Text>inside</Text></Card>); }`);
    expect(c).toContain('"inside"');                // the child Text is emitted
    expect(c).toContain('er_node_create(ER_NODE_VIEW)');
  });

  it('emits props.children via the whole-props parameter', () => {
    const c = gen(`${PRE}
      function Card(props) { return (<View>{props.children}</View>); }
      export function App() { return (<Card><Text>P</Text></Card>); }`);
    expect(c).toContain('"P"');
  });

  it('merges a static spread {...obj} into a component\'s props', () => {
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
    expect(c).toContain('(int)roundf(');                         // Math.round → int cast
    expect(c).toContain('strcmp(s_state.sel, "a")');             // string equality + it.key folded to "a"
    expect(c).toContain('strcmp(s_state.sel, "b")');             // second unrolled .map iteration
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

describe('AOT animation completeness (Phase 11)', () => {
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
    const c = app(`Animated.loop(Animated.timing(a, { toValue: 360, duration: 1000, easing: Easing.linear })).start();`);
    expect(c).toContain('cfg0.loop = true;');
    expect(c).toContain('ER_EASE_LINEAR');
  });

  it('Animated.decay lowers to ER_ANIM_DECAY with velocity + deceleration', () => {
    const c = app(`Animated.decay(b, { velocity: 0.5, deceleration: 0.997 }).start();`);
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
    expect(() => app(`Animated.parallel([
      Animated.timing(a, { toValue: 1, duration: 100 }),
      Animated.timing(a, { toValue: 0, duration: 100 }),
    ]).start();`)).toThrow(/same animated value/);
  });

  it('rejects Animated.loop around a multi-step sequence', () => {
    expect(() => app(`Animated.loop(Animated.sequence([
      Animated.timing(a, { toValue: 1, duration: 100 }),
      Animated.timing(b, { toValue: 1, duration: 100 }),
    ])).start();`)).toThrow(/loop currently wraps a single/);
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
    const c = compileSource(src, 'test', { screen: { width: 240, height: 320 } }).c;
    expect(c).toContain('"small"');
    expect(c).not.toContain('"wide"');
  });

  it('throws on a top-level if whose test is not compile-time constant', () => {
    const src = `${PRE}
      export function App() {
        const [n, setN] = useState(0);
        if (n > 5) return (<View><Text>a</Text></View>);
        return (<View><Text>b</Text></View>);
      }`;
    expect(() => compileSource(src, 'test')).toThrow(/compile-time-constant test/);
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
    expect(c).toContain('s_ref_cx = (data->layout_rect.x + (data->layout_rect.w / 2));');
    // touch coord + ref read in the shared drag handler
    expect(c).toContain('static void er_cb_onDrag(');
    expect(c).toContain('s_state.v = (data->x - s_ref_cx);');
    // onTouchStart + onTouchMove reuse the one useCallback handler
    expect(c.match(/er_event_set\([^,]+, ER_EVENT_TOUCH_(START|MOVE), er_cb_onDrag, NULL\);/g)).toHaveLength(2);
  });

  it('treats useState(70.0) as a FLOAT slot (decimal literal forces float, value stays sub-integer)', () => {
    const c = gen(`${PRE}
      export function App() {
        const [v, setV] = useState(70.0);
        return (<Pressable onPress={() => setV(v + 0.5)}><Text>{Math.round(v)}</Text></Pressable>);
      }`);
    expect(c).toContain('float v;');             // float struct field, not int
    expect(c).toContain('.v = 70.0f');           // valid C float literal (not 70f)
    expect(c).toContain('s_state.v = (s_state.v + 0.5f);');
    expect(c).toContain('(int)roundf((float)(s_state.v))'); // displayed rounded
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

  it('negates a negative constant as (-(-135)), never the decrement token --135', () => {
    const c = gen(`${PRE}
      const A = -135;
      export function App() {
        const [v, setV] = useState(0);
        return (<Pressable onPress={(e) => setV(e.x > -A ? -A : e.x)}><Text>{v}</Text></Pressable>);
      }`);
    expect(c).toContain('(-(-135))');
    expect(c).not.toContain('--135');
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
      compileSource(src, 'demo', { filename: 'demos/demo/App.jsx' });
    } catch (e) {
      err = e;
    }
    expect(err).toBeDefined();
    expect(err.message).toContain('demos/demo/App.jsx:'); // file:line:col
    expect(err.aotLoc).toBeTruthy(); // structured location preserved
    expect(err.message).toContain('^'); // code-frame caret
  });

  it('attaches a rewrite hint to a known error', () => {
    let err;
    try {
      compileSource(`${PRE}\nexport function App() { return (<View><Gauge /></View>); }`, 'demo');
    } catch (e) {
      err = e;
    }
    expect(err.message).toContain('hint:');
    expect(err.message).toContain('built-in');
  });

  it('points at the default demo path when no filename is given', () => {
    let err;
    try {
      compileSource(`${PRE}\nexport function App() { return (<Nope />); }`, 'mydemo');
    } catch (e) {
      err = e;
    }
    expect(err.message).toContain('demos/mydemo/App.jsx:');
  });

  // A spread on a HOST element used to be silently dropped (the style/event loops only read named
  // attributes) — it must now error clearly, like the typed components already do.
  it('rejects a spread on a host element instead of silently dropping it', () => {
    expect(() => gen(`${PRE}\nexport function App() { const o = window.x; return (<View {...o} />); }`)).toThrow(
      /spread \{\.\.\.\} on <View> is not supported/,
    );
  });

  it('locates + hints a non-constant useState initial (was a bare evalStatic leak)', () => {
    let err;
    try {
      compileSource(`${PRE}\nexport function App() { const [s] = useState(window.y); return (<Text>{s}</Text>); }`, 'demo', {
        filename: 'demos/demo/App.jsx',
      });
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
      compileSource(`${PRE}\nexport function App() { const [a] = useState([1, 2]); return (<View>{a.map((x) => (<Text key={x}>{x}</Text>))}</View>); }`, 'demo', {
        filename: 'demos/demo/App.jsx',
      });
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
    expect(c).toContain('s_state.t = (s_state.t + 1);'); // setT(p => p+1) in the callback
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
    const c = gen(`${PRE}\nexport function App() { return (<Text>hi</Text>); }`);
    expect(c).toContain('void er_app_tick(int dt_ms)');
    expect(c).toContain('(void)dt_ms;');
    expect(c).not.toContain('er_timer_add');
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
    expect(c).toContain('p.flex_direction = ((s_state.row) ? ER_FLEX_ROW : ER_FLEX_COL);');
  });

  it('lowers state-driven alignItems and justifyContent', () => {
    const c = gen(`${PRE}
      export function App() {
        const [c2, setC2] = useState(true);
        return (<View style={{ alignItems: c2 ? 'center' : 'flex-start', justifyContent: c2 ? 'center' : 'flex-end' }}><Text>x</Text></View>);
      }`);
    expect(c).toContain('p.align_items = ((s_state.c2) ? ER_ALIGN_CENTER : ER_ALIGN_FLEX_START);');
    expect(c).toContain('p.justify_content = ((s_state.c2) ? ER_JUSTIFY_CENTER : ER_JUSTIFY_FLEX_END);');
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
    expect(c).toContain('{ "there", 0xFFF4A261u, 0, 1, 0xFF, 0xFF, ER_LAYOUT_AUTO }'); // bold + amber span
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
    expect(c).toContain('s_state.n = (s_state.n - 1);');
    expect(c).toContain('s_state.n = (s_state.n + 1);');
    // two distinct handlers, one per instance, each wired via er_event_set
    expect((c.match(/static void er_handler_\d+\(/g) || []).length).toBe(2);
    expect((c.match(/er_event_set\(\w+, ER_EVENT_PRESS,/g) || []).length).toBe(2);
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
    expect(c).toContain('s_state.n = (s_state.n + 1);');
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
    expect(c).toContain('s_state.n = (s_state.n + 1);');
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
    expect(c).toContain('snprintf(s_state.name, sizeof(s_state.name), "%s", data->changed_text);');
    // value drives the buffer (synced in app_update), placeholder + color applied
    expect(c).toContain('snprintf(p.text, sizeof(p.text), "%s", s_state.name);');
    expect(c).toContain('snprintf(p.placeholder, sizeof(p.placeholder), "%s", "Name");');
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
    const r = compileSource(`${IMG}\nexport function App() { return (<Image source={logo} style={{ width: 40, height: 40 }} />); }`, 'demo');
    expect(r.c).toContain('snprintf(p.image_name, sizeof(p.image_name), "%s", "logo");');
    expect(r.images).toEqual([{ name: 'logo', importPath: './assets/logo.png' }]);
  });

  it('lowers resizeMode to the ERResizeMode enum', () => {
    const r = compileSource(`${IMG}\nexport function App() { return (<Image source={logo} resizeMode="contain" />); }`, 'demo');
    expect(r.c).toContain('p.resize_mode = ER_RESIZE_CONTAIN;');
  });

  it('lowers tintColor to an ARGB literal', () => {
    const r = compileSource(`${IMG}\nexport function App() { return (<Image source={logo} tintColor="#ff0000" />); }`, 'demo');
    expect(r.c).toContain('p.tint_color = 0xFFFF0000u;');
  });

  it('accepts source={{ uri }} as a bare asset name (no import to bake)', () => {
    const r = compileSource(`import { Image } from 'embedded-react';\nexport function App() { return (<Image source={{ uri: 'wx_sun' }} />); }`, 'demo');
    expect(r.c).toContain('snprintf(p.image_name, sizeof(p.image_name), "%s", "wx_sun");');
    expect(r.images).toEqual([]);
  });

  it('folds a static <Image source={item.icon}> in an unrolled .map to literal asset names + bakes them', () => {
    const r = compileSource(
      `import { View, Image } from 'embedded-react';\nimport wxSun from './a/wx_sun.png';\nimport wxRain from './a/wx_rain.png';\nconst DAYS = [{ icon: wxSun }, { icon: wxRain }];\nexport function App() { return (<View>{DAYS.map((f, i) => (<Image key={i} source={f.icon} />))}</View>); }`,
      'demo',
    );
    expect(r.c).toContain('snprintf(p.image_name, sizeof(p.image_name), "%s", "wx_sun");');
    expect(r.c).toContain('snprintf(p.image_name, sizeof(p.image_name), "%s", "wx_rain");');
    expect(r.images.map((i) => i.name).sort()).toEqual(['wx_rain', 'wx_sun']);
  });

  it('emits a dynamic <Image source> from a list-state field (set in app_update)', () => {
    const c = compileSource(
      `import { View, Image } from 'embedded-react';\nimport { useState } from 'react';\nexport function App() { const [items] = useState([{ icon: 'wx_sun' }]); return (<View>{items.map((d, i) => (<Image key={i} source={d.icon} />))}</View>); }`,
      'demo',
    ).c;
    expect(c).toContain('snprintf(p.image_name, sizeof(p.image_name), "%s", s_items[0].icon);');
  });

  it('emits a dynamic <Image source> from a state ternary + bakes both branches', () => {
    const r = compileSource(
      `import { Image, Pressable } from 'embedded-react';\nimport sun from './a/sun.png';\nimport moon from './a/moon.png';\nimport { useState } from 'react';\nexport function App() { const [day, setDay] = useState(true); return (<Pressable onPress={() => setDay(!day)}><Image source={day ? sun : moon} /></Pressable>); }`,
      'demo',
    );
    expect(r.c).toContain('snprintf(p.image_name, sizeof(p.image_name), "%s", (s_state.day ? "sun" : "moon"));');
    expect(r.images.map((i) => i.name).sort()).toEqual(['moon', 'sun']);
  });

  it('rejects an <Image source> that is not a string (e.g. a number)', () => {
    expect(() =>
      compileSource(`import { Image } from 'embedded-react';\nimport { useState } from 'react';\nexport function App() { const [n] = useState(5); return (<Image source={n} />); }`, 'demo'),
    ).toThrow(/must resolve to an asset NAME/);
  });

  it('rejects an unsupported resizeMode', () => {
    expect(() => compileSource(`${IMG}\nexport function App() { return (<Image source={logo} resizeMode="squish" />); }`, 'demo')).toThrow(/unsupported <Image resizeMode>/);
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
    const c = compileSource(`${C}\nexport function App() { return (<View><Counter label="A" /></View>); }`, 'demo').c;
    expect(c).toContain('int c0_n;');
    expect(c).toContain('s_state.c0_n = (s_state.c0_n + 1);'); // the child's setter mutates its own field
  });

  it('keeps two instances of the same component independent', () => {
    const c = compileSource(`${C}\nexport function App() { return (<View><Counter label="A" /><Counter label="B" /></View>); }`, 'demo').c;
    expect(c).toContain('int c0_n;');
    expect(c).toContain('int c1_n;'); // distinct storage per instance
    expect(c).toContain('s_state.c0_n = (s_state.c0_n + 1);');
    expect(c).toContain('s_state.c1_n = (s_state.c1_n + 1);');
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
    expect(c).toContain('s_ref_c0_t = (s_ref_c0_t + 1);');
    expect(c).toContain('s_ref_c1_t = (s_ref_c1_t + 1);');
  });

  it('compiles each instance useCallback into its own distinct handler', () => {
    const src = `import { View, Text, Pressable } from 'embedded-react';
import { useState, useCallback } from 'react';
function Btn() { const [n, setN] = useState(0); const tap = useCallback(() => setN(n + 1), [n]); return (<Pressable onPress={tap}><Text>{n}</Text></Pressable>); }
export function App() { return (<View><Btn /><Btn /></View>); }`;
    const c = compileSource(src, 'demo').c;
    expect(c).toContain('static void er_cb_c0_tap(');
    expect(c).toContain('static void er_cb_c1_tap(');
    expect(c).toContain('s_state.c0_n = (s_state.c0_n + 1);'); // c0's handler mutates c0's state
    expect(c).toContain('s_state.c1_n = (s_state.c1_n + 1);');
  });
});

describe('AOT handler follow-ons', () => {
  it('inlines a helper call in a handler (component-local arrow), binding its args', () => {
    const c = compileSource(`import { View, Text, Pressable } from 'embedded-react';
import { useState } from 'react';
function App() {
  const [a, setA] = useState(0);
  const [b, setB] = useState(0);
  const reset = () => { setA(0); setB(0); };
  const bump = (k) => setA(a + k);
  return (<Pressable onPress={() => { reset(); bump(5); }}><Text>{a}</Text></Pressable>);
}
export { App };`, 'demo').c;
    expect(c).toContain('s_state.a = 0;');
    expect(c).toContain('s_state.b = 0;');
    expect(c).toContain('s_state.a = (s_state.a + 5);'); // bump(5): arg bound
  });

  it('detects a recursive helper and errors instead of looping forever', () => {
    expect(() =>
      compileSource(`import { Text, Pressable } from 'embedded-react';
function App() { const loop = () => { loop(); }; return (<Pressable onPress={() => { loop(); }}><Text>x</Text></Pressable>); }
export { App };`, 'demo'),
    ).toThrow(/recursive/);
  });

  it('wires a .start(onComplete) callback to the animation on_complete', () => {
    const c = compileSource(`import { Text, Pressable, Animated, useAnimatedValue } from 'embedded-react';
import { useState } from 'react';
function App() {
  const [done, setDone] = useState(false);
  const x = useAnimatedValue(0);
  return (<Pressable onPress={() => Animated.timing(x, { toValue: 1, duration: 100 }).start(() => setDone(true))}><Text>x</Text></Pressable>);
}
export { App };`, 'demo').c;
    expect(c).toContain('.on_complete = er_donecb_0;');
    expect(c).toContain('static void er_donecb_0(bool finished, void* user_data)');
    expect(c).toContain('s_state.done = 1;'); // completion sets state
  });

  it('rejects a completion callback on a parallel animation (not yet supported)', () => {
    expect(() =>
      compileSource(`import { Text, Pressable, Animated, useAnimatedValue } from 'embedded-react';
function App() {
  const a = useAnimatedValue(0); const b = useAnimatedValue(0);
  return (<Pressable onPress={() => Animated.parallel([Animated.timing(a, { toValue: 1 }), Animated.timing(b, { toValue: 1 })]).start(() => {})}><Text>x</Text></Pressable>);
}
export { App };`, 'demo'),
    ).toThrow(/parallel\/stagger animation is not yet supported/);
  });
});
