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
    expect(c).toContain('s_state.on = (!s_state.on);');
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
