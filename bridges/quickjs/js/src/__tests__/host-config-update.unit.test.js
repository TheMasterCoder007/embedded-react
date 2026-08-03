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

// prepareUpdate must diff, not rubber-stamp. Re-marshaling props across the JS->C bridge is the
// dominant cost of a Flow A re-render (~10ms/node on an ESP32-S3), and a parent's state change
// re-renders every non-memoized child with recreated-but-identical props — so a diff that returns
// null for those nodes (React then skips commitUpdate entirely) is the single biggest reconcile
// lever. These tests pin the contract at the bridge: which NativeUI calls a given prop change is
// allowed to produce, and — just as important — which it must NOT.

import {describe, it, expect, beforeEach} from 'vitest';

// One mock installed before host-config.js is imported (native-ui.js captures globalThis.NativeUI
// at import time, so a per-test mock object would be ignored by the cached module). Counters are
// cleared per test instead.
const calls = {setProps: [], setTextSpans: [], setVectorOps: [], setEvent: []};
let idc = 0;
globalThis.IS_REACT_ACT_ENVIRONMENT = false;
globalThis.NativeUI = {
  createNode: () => ++idc,
  setProps: (h, p) => calls.setProps.push({h, p}),
  setRoot: () => {},
  appendChild: () => {},
  insertBefore: () => {},
  removeChild: () => {},
  destroyNode: () => {},
  setEvent: (h, key, fn) => calls.setEvent.push({h, key, fn}),
  setTextSpans: (h, spans) => calls.setTextSpans.push({h, spans}),
  setVectorOps: (h, ops) => calls.setVectorOps.push({h, ops}),
  commit: () => {},
  now: () => 0,
  maxVectorOps: 4096,
  maxVectorPaints: 1024,
  maxVectorGrads: 256,
};

const Reconciler = (await import('react-reconciler')).default;
const {hostConfig} = await import('../host-config.js');
const {createElement: el} = (await import('react')).default;

const reconciler = Reconciler(hostConfig);

/** Mounts a tree into a fresh legacy (synchronous) root; returns an update(nextTree) function. */
function mount(tree) {
  const container = globalThis.NativeUI.createNode('View');
  const root = reconciler.createContainer(
    container,
    0,
    null,
    false,
    null,
    '',
    () => {},
    null,
  );
  reconciler.updateContainer(tree, root, null, null);
  return next => reconciler.updateContainer(next, root, null, null);
}

const counts = () => ({
  setProps: calls.setProps.length,
  setTextSpans: calls.setTextSpans.length,
  setVectorOps: calls.setVectorOps.length,
  setEvent: calls.setEvent.length,
});
const NOTHING = {setProps: 0, setTextSpans: 0, setVectorOps: 0, setEvent: 0};

beforeEach(() => {
  for (const a of Object.values(calls)) a.length = 0;
});

// A minimal Animated.Value stand-in: a CLASS instance (deepEqualProps compares those by identity)
// with the __animated marker splitAnimatedStyle looks for and a recordable __bind.
class FakeAnimatedValue {
  constructor() {
    this.__animated = true;
    this.bound = [];
  }
  __bind(h, prop) {
    this.bound.push({h, prop});
  }
}

describe('prepareUpdate diffing: unchanged re-renders are bridge-free', () => {
  it('a re-render with recreated-but-identical props makes zero NativeUI calls', () => {
    const onPress = () => {};
    const ui = () =>
      el(
        'View',
        {style: {width: 100, padding: 4}},
        el('Text', {style: {color: '#ffffff'}}, 'Hello ', 'World'),
        el('View', {style: [{flex: 1}, {backgroundColor: '#101010'}]}),
        el('View', {onPress, style: {height: 20}}),
      );
    const update = mount(ui());
    Object.values(calls).forEach(a => (a.length = 0));

    update(ui()); // every props/style object is a NEW identity with the same values

    expect(counts()).toEqual(NOTHING);
  });

  it('a changed style re-serializes only that node, and only its props section', () => {
    const ui = w =>
      el(
        'View',
        {style: {width: w}},
        el('Text', {style: {color: '#ffffff'}}, 'Hi'),
      );
    const update = mount(ui(100));
    Object.values(calls).forEach(a => (a.length = 0));

    update(ui(120));

    expect(counts()).toEqual({...NOTHING, setProps: 1});
    expect(calls.setProps[0].p.width).toBe(120);
  });

  it('a new inline handler re-registers the event WITHOUT re-serializing props', () => {
    const ui = () => el('View', {onPress: () => {}, style: {width: 50}});
    const update = mount(ui());
    Object.values(calls).forEach(a => (a.length = 0));

    update(ui()); // fresh closure each render — its captures may be stale, must re-store

    expect(counts()).toEqual({...NOTHING, setEvent: 1});
    expect(calls.setEvent[0].key).toBe('onPress');
    expect(typeof calls.setEvent[0].fn).toBe('function');
  });

  it('removing a handler clears it on the node', () => {
    const update = mount(el('View', {onPress: () => {}}));
    Object.values(calls).forEach(a => (a.length = 0));

    update(el('View', {}));

    expect(counts()).toEqual({...NOTHING, setEvent: 1});
    expect(calls.setEvent[0].fn).toBe(null);
  });

  it('changed text content re-applies the text', () => {
    const ui = n => el('Text', {style: {fontSize: 14}}, 'count: ', n);
    const update = mount(ui(0));
    Object.values(calls).forEach(a => (a.length = 0));

    update(ui(0));
    expect(counts()).toEqual(NOTHING);

    update(ui(1));
    expect(calls.setProps).toHaveLength(1);
    expect(calls.setProps[0].p.text).toBe('count: 1');
    expect(calls.setVectorOps).toHaveLength(0);
  });

  it('nested styled <Text> spans: unchanged spans are skipped, changed spans re-upload', () => {
    const ui = color =>
      el(
        'Text',
        {style: {color: '#ffffff'}},
        'temp ',
        el('Text', {style: {color}}, '72°'),
      );
    const update = mount(ui('#ff0000'));
    Object.values(calls).forEach(a => (a.length = 0));

    update(ui('#ff0000'));
    expect(counts()).toEqual(NOTHING);

    update(ui('#00ff00'));
    expect(calls.setTextSpans).toHaveLength(1);
    expect(calls.setTextSpans[0].spans.some(s => s.color === '#00ff00')).toBe(
      true,
    );
  });

  it('a simultaneous text-content AND span-style change resolves both correctly from one flatten', () => {
    // Exercises commitUpdate's payload.props + payload.spans path together: applyProps and
    // applyTextSpans must each see the CURRENT (post-change) style even though the flattened style
    // is computed once and shared between them.
    const ui = (n, color) =>
      el(
        'Text',
        {style: {color: '#ffffff'}},
        `count: ${n} `,
        el('Text', {style: {color}}, 'live'),
      );
    const update = mount(ui(0, '#ff0000'));
    Object.values(calls).forEach(a => (a.length = 0));

    update(ui(1, '#00ff00'));

    expect(calls.setProps).toHaveLength(1);
    expect(calls.setProps[0].p.text).toBe('count: 1 live');
    expect(calls.setTextSpans).toHaveLength(1);
    const spans = calls.setTextSpans[0].spans;
    expect(spans.find(s => s.text === 'count: 1 ').color).toBe('#ffffff');
    expect(spans.find(s => s.text === 'live').color).toBe('#00ff00');
  });
});

describe('prepareUpdate diffing: <Svg> op-tape uploads track their real inputs', () => {
  const shapes = d => [
    el('Circle', {key: 'c', cx: 50, cy: 50, r: 40, stroke: '#ffffff'}),
    el('Path', {key: 'p', d, stroke: '#ff0000'}),
  ];

  it('identical declarative children (recreated elements) do not re-flatten', () => {
    const ui = () =>
      el(
        'Svg',
        {width: 100, height: 100, viewBox: '0 0 100 100'},
        ...shapes('M0 0 L10 10'),
      );
    const update = mount(ui());
    Object.values(calls).forEach(a => (a.length = 0));

    update(ui());

    expect(counts()).toEqual(NOTHING);
  });

  it('a position-only style move (the drag hot path) skips the tape upload', () => {
    const ui = left =>
      el(
        'Svg',
        {width: 100, height: 100, style: {left}},
        ...shapes('M0 0 L10 10'),
      );
    const update = mount(ui(0));
    Object.values(calls).forEach(a => (a.length = 0));

    update(ui(25));

    expect(counts()).toEqual({...NOTHING, setProps: 1});
    expect(calls.setProps[0].p.left).toBe(25);
  });

  it('a changed shape re-uploads the tape', () => {
    const ui = d => el('Svg', {width: 100, height: 100}, ...shapes(d));
    const update = mount(ui('M0 0 L10 10'));
    Object.values(calls).forEach(a => (a.length = 0));

    update(ui('M0 0 L20 20'));

    expect(calls.setVectorOps).toHaveLength(1);
  });

  it('<Svg source>: same artifact and box skip the upload; a resized box re-uploads', () => {
    const art = {kind: 'vector', ops: [], paints: [], width: 24, height: 24};
    const ui = w => el('Svg', {source: art, width: w, height: 24});
    const update = mount(ui(24));
    Object.values(calls).forEach(a => (a.length = 0));

    update(ui(24));
    expect(counts()).toEqual(NOTHING);

    update(ui(48));
    expect(calls.setVectorOps).toHaveLength(1);
    expect(calls.setProps).toHaveLength(1); // width also folds into the resolved style box
  });

  it('<Svg source> raster fallback: unchanged props are bridge-free, a style change re-applies image props', () => {
    const art = {kind: 'raster', name: 'logo.png', width: 24, height: 24};
    const ui = top => el('Svg', {source: art, style: {top}});
    const update = mount(ui(0));
    Object.values(calls).forEach(a => (a.length = 0));

    update(ui(0));
    expect(counts()).toEqual(NOTHING);

    update(ui(10));
    expect(calls.setProps).toHaveLength(1);
    expect(calls.setProps[0].p.imageName).toBe('logo.png');
    expect(calls.setVectorOps).toHaveLength(0); // raster never uploads a tape
  });
});

describe('prepareUpdate diffing: Animated.Value binding stays correct', () => {
  it('the same value instance is not re-bound; a fresh instance is', () => {
    const v1 = new FakeAnimatedValue();
    const ui = v => el('View', {style: {opacity: v, width: 80}});
    const update = mount(ui(v1));
    expect(v1.bound).toHaveLength(1); // bound on mount
    Object.values(calls).forEach(a => (a.length = 0));

    update(ui(v1)); // same instance — already bound, nothing to do
    expect(counts()).toEqual(NOTHING);
    expect(v1.bound).toHaveLength(1);

    const v2 = new FakeAnimatedValue();
    update(ui(v2)); // a NEW instance must be treated as changed and bound
    expect(calls.setProps).toHaveLength(1);
    expect(v2.bound).toHaveLength(1);
  });
});
