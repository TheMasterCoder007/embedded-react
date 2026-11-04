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
const calls = {
  setProps: [],
  setTextSpans: [],
  setVectorOps: [],
  setEvent: [],
  animUnbind: [],
};
// Every recorded bridge call in order, so a test can pin the ones whose ORDER is the contract.
const order = [];
let lastHandle = 0;
globalThis.IS_REACT_ACT_ENVIRONMENT = false;
globalThis.NativeUI = {
  createNode: () => ++lastHandle,
  setProps: (handle, props) => {
    calls.setProps.push({handle, props});
    order.push(`setProps:${handle}`);
  },
  setRoot: () => {},
  appendChild: () => {},
  insertBefore: () => {},
  removeChild: () => {},
  destroyNode: () => {},
  setEvent: (handle, key, handler) =>
    calls.setEvent.push({handle, key, handler}),
  setTextSpans: (handle, spans) => calls.setTextSpans.push({handle, spans}),
  setVectorOps: (handle, ops) => calls.setVectorOps.push({handle, ops}),
  animUnbind: (handle, prop) => {
    calls.animUnbind.push({handle, prop});
    order.push(`animUnbind:${handle}:${prop}`);
  },
  commit: () => {},
  now: () => 0,
  maxVectorOps: 4096,
  maxVectorPaints: 1024,
  maxVectorGrads: 256,
};

const Reconciler = (await import('react-reconciler')).default;
const {hostConfig} = await import('../host-config.js');
const {createElement} = (await import('react')).default;

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
  animUnbind: calls.animUnbind.length,
});
const NOTHING = {
  setProps: 0,
  setTextSpans: 0,
  setVectorOps: 0,
  setEvent: 0,
  animUnbind: 0,
};

beforeEach(() => {
  for (const recorded of Object.values(calls)) recorded.length = 0;
  order.length = 0;
});

// A minimal Animated.Value stand-in: a CLASS instance (deepEqualProps compares those by identity)
// with the __animated marker splitAnimatedStyle looks for and a recordable __bind.
class FakeAnimatedValue {
  constructor() {
    this.__animated = true;
    this.bound = [];
  }
  __bind(handle, prop) {
    this.bound.push({handle, prop});
    order.push(`bind:${handle}:${prop}`);
  }
}

describe('prepareUpdate diffing: unchanged re-renders are bridge-free', () => {
  it('a re-render with recreated-but-identical props makes zero NativeUI calls', () => {
    const onPress = () => {};
    const ui = () =>
      createElement(
        'View',
        {style: {width: 100, padding: 4}},
        createElement('Text', {style: {color: '#ffffff'}}, 'Hello ', 'World'),
        createElement('View', {
          style: [{flex: 1}, {backgroundColor: '#101010'}],
        }),
        createElement('View', {onPress, style: {height: 20}}),
      );
    const update = mount(ui());
    Object.values(calls).forEach(recorded => (recorded.length = 0));

    update(ui()); // every props/style object is a NEW identity with the same values

    expect(counts()).toEqual(NOTHING);
  });

  it('a changed style re-serializes only that node, and only its props section', () => {
    const ui = width =>
      createElement(
        'View',
        {style: {width}},
        createElement('Text', {style: {color: '#ffffff'}}, 'Hi'),
      );
    const update = mount(ui(100));
    Object.values(calls).forEach(recorded => (recorded.length = 0));

    update(ui(120));

    expect(counts()).toEqual({...NOTHING, setProps: 1});
    expect(calls.setProps[0].props.width).toBe(120);
  });

  it('a new inline handler re-registers the event WITHOUT re-serializing props', () => {
    const ui = () =>
      createElement('View', {onPress: () => {}, style: {width: 50}});
    const update = mount(ui());
    Object.values(calls).forEach(recorded => (recorded.length = 0));

    update(ui()); // fresh closure each render — its captures may be stale, must re-store

    expect(counts()).toEqual({...NOTHING, setEvent: 1});
    expect(calls.setEvent[0].key).toBe('onPress');
    expect(typeof calls.setEvent[0].handler).toBe('function');
  });

  it('removing a handler clears it on the node', () => {
    const update = mount(createElement('View', {onPress: () => {}}));
    Object.values(calls).forEach(recorded => (recorded.length = 0));

    update(createElement('View', {}));

    expect(counts()).toEqual({...NOTHING, setEvent: 1});
    expect(calls.setEvent[0].handler).toBe(null);
  });

  it('changed text content re-applies the text', () => {
    const ui = count =>
      createElement('Text', {style: {fontSize: 14}}, 'count: ', count);
    const update = mount(ui(0));
    Object.values(calls).forEach(recorded => (recorded.length = 0));

    update(ui(0));
    expect(counts()).toEqual(NOTHING);

    update(ui(1));
    expect(calls.setProps).toHaveLength(1);
    expect(calls.setProps[0].props.text).toBe('count: 1');
    expect(calls.setVectorOps).toHaveLength(0);
  });

  it('nested styled <Text> spans: unchanged spans are skipped, changed spans re-upload', () => {
    const ui = color =>
      createElement(
        'Text',
        {style: {color: '#ffffff'}},
        'temp ',
        createElement('Text', {style: {color}}, '72°'),
      );
    const update = mount(ui('#ff0000'));
    Object.values(calls).forEach(recorded => (recorded.length = 0));

    update(ui('#ff0000'));
    expect(counts()).toEqual(NOTHING);

    update(ui('#00ff00'));
    expect(calls.setTextSpans).toHaveLength(1);
    expect(
      calls.setTextSpans[0].spans.some(span => span.color === '#00ff00'),
    ).toBe(true);
  });

  it('a simultaneous text-content AND span-style change resolves both correctly from one flatten', () => {
    // Exercises commitUpdate's payload.props + payload.spans path together: applyProps and
    // applyTextSpans must each see the CURRENT (post-change) style even though the flattened style
    // is computed once and shared between them.
    const ui = (count, color) =>
      createElement(
        'Text',
        {style: {color: '#ffffff'}},
        `count: ${count} `,
        createElement('Text', {style: {color}}, 'live'),
      );
    const update = mount(ui(0, '#ff0000'));
    Object.values(calls).forEach(recorded => (recorded.length = 0));

    update(ui(1, '#00ff00'));

    expect(calls.setProps).toHaveLength(1);
    expect(calls.setProps[0].props.text).toBe('count: 1 live');
    expect(calls.setTextSpans).toHaveLength(1);
    const spans = calls.setTextSpans[0].spans;
    expect(spans.find(span => span.text === 'count: 1 ').color).toBe('#ffffff');
    expect(spans.find(span => span.text === 'live').color).toBe('#00ff00');
  });
});

describe('prepareUpdate diffing: <Svg> op-tape uploads track their real inputs', () => {
  const shapes = d => [
    createElement('Circle', {
      key: 'c',
      cx: 50,
      cy: 50,
      r: 40,
      stroke: '#ffffff',
    }),
    createElement('Path', {key: 'p', d, stroke: '#ff0000'}),
  ];

  it('identical declarative children (recreated elements) do not re-flatten', () => {
    const ui = () =>
      createElement(
        'Svg',
        {width: 100, height: 100, viewBox: '0 0 100 100'},
        ...shapes('M0 0 L10 10'),
      );
    const update = mount(ui());
    Object.values(calls).forEach(recorded => (recorded.length = 0));

    update(ui());

    expect(counts()).toEqual(NOTHING);
  });

  it('a position-only style move (the drag hot path) skips the tape upload', () => {
    const ui = left =>
      createElement(
        'Svg',
        {width: 100, height: 100, style: {left}},
        ...shapes('M0 0 L10 10'),
      );
    const update = mount(ui(0));
    Object.values(calls).forEach(recorded => (recorded.length = 0));

    update(ui(25));

    expect(counts()).toEqual({...NOTHING, setProps: 1});
    expect(calls.setProps[0].props.left).toBe(25);
  });

  it('a changed shape re-uploads the tape', () => {
    const ui = d =>
      createElement('Svg', {width: 100, height: 100}, ...shapes(d));
    const update = mount(ui('M0 0 L10 10'));
    Object.values(calls).forEach(recorded => (recorded.length = 0));

    update(ui('M0 0 L20 20'));

    expect(calls.setVectorOps).toHaveLength(1);
  });

  it('a changed <Svg> paint re-uploads the tape', () => {
    const ui = fill =>
      createElement(
        'Svg',
        {width: 100, height: 100, fill},
        ...shapes('M0 0 L10 10'),
      );
    const update = mount(ui('#ff0000'));
    Object.values(calls).forEach(recorded => (recorded.length = 0));

    update(ui('#00ff00'));

    expect(calls.setVectorOps).toHaveLength(1);
  });

  it('<Svg source>: same artifact and box skip the upload; a resized box re-uploads', () => {
    const art = {kind: 'vector', ops: [], paints: [], width: 24, height: 24};
    const ui = width => createElement('Svg', {source: art, width, height: 24});
    const update = mount(ui(24));
    Object.values(calls).forEach(recorded => (recorded.length = 0));

    update(ui(24));
    expect(counts()).toEqual(NOTHING);

    update(ui(48));
    expect(calls.setVectorOps).toHaveLength(1);
    expect(calls.setProps).toHaveLength(1); // width also folds into the resolved style box
  });

  it('<Svg source>: a changed root paint skips the upload, since the artifact ignores it', () => {
    const art = {kind: 'vector', ops: [], paints: [], width: 24, height: 24};
    const ui = fill =>
      createElement('Svg', {source: art, width: 24, height: 24, fill});
    const update = mount(ui('#ff0000'));
    Object.values(calls).forEach(recorded => (recorded.length = 0));

    update(ui('#00ff00'));

    expect(calls.setVectorOps).toHaveLength(0);
  });

  it('<Svg source> raster fallback: unchanged props are bridge-free, a style change re-applies image props', () => {
    const art = {kind: 'raster', name: 'logo.png', width: 24, height: 24};
    const ui = top => createElement('Svg', {source: art, style: {top}});
    const update = mount(ui(0));
    Object.values(calls).forEach(recorded => (recorded.length = 0));

    update(ui(0));
    expect(counts()).toEqual(NOTHING);

    update(ui(10));
    expect(calls.setProps).toHaveLength(1);
    expect(calls.setProps[0].props.imageName).toBe('logo.png');
    expect(calls.setVectorOps).toHaveLength(0); // raster never uploads a tape
  });

  it('<Image> with a constant source: a style change re-applies its imageName', () => {
    const ui = width =>
      createElement('Image', {source: 'logo', style: {width}});
    const update = mount(ui(20));
    Object.values(calls).forEach(recorded => (recorded.length = 0));

    update(ui(40));
    expect(calls.setProps).toHaveLength(1);
    expect(calls.setProps[0].props.imageName).toBe('logo');
  });
});

describe('prepareUpdate diffing: Animated.Value binding stays correct', () => {
  it('the same value instance is not re-bound; a fresh instance is', () => {
    const firstValue = new FakeAnimatedValue();
    const ui = value =>
      createElement('View', {style: {opacity: value, width: 80}});
    const update = mount(ui(firstValue));
    expect(firstValue.bound).toHaveLength(1); // bound on mount
    Object.values(calls).forEach(recorded => (recorded.length = 0));

    update(ui(firstValue)); // same instance — already bound, nothing to do
    expect(counts()).toEqual(NOTHING);
    expect(firstValue.bound).toHaveLength(1);

    const secondValue = new FakeAnimatedValue();
    update(ui(secondValue)); // a NEW instance must be treated as changed and bound
    expect(calls.setProps).toHaveLength(1);
    expect(secondValue.bound).toHaveLength(1);
  });
});

// A binding outlives the render that made it — the engine re-pushes it over the static props of every
// later setProps — so a prop that stops being animated has to be released explicitly, or it keeps
// following its old value and the static value React just set never sticks.
describe('animated bindings are released when a prop stops being animated', () => {
  it('animated → static unbinds the prop, and unbinds it BEFORE re-applying the props', () => {
    const animatedValue = new FakeAnimatedValue();
    const ui = opacity => createElement('View', {style: {opacity, width: 80}});
    const update = mount(ui(animatedValue));
    const handle = animatedValue.bound[0].handle;
    Object.values(calls).forEach(recorded => (recorded.length = 0));
    order.length = 0;

    update(ui(0.5));

    expect(calls.animUnbind).toEqual([{handle, prop: 'opacity'}]);
    expect(calls.setProps[0].props.opacity).toBe(0.5);
    // setProps re-pushes every value STILL bound to the node (er_animation_reapply_bound), so unbinding
    // afterwards would leave the last animated float on the node instead of the 0.5 just written.
    expect(order).toEqual([
      `animUnbind:${handle}:opacity`,
      `setProps:${handle}`,
    ]);
  });

  it('a prop dropped from the style entirely is unbound too', () => {
    const animatedValue = new FakeAnimatedValue();
    const update = mount(
      createElement('View', {style: {opacity: animatedValue, width: 80}}),
    );
    const handle = animatedValue.bound[0].handle;
    Object.values(calls).forEach(recorded => (recorded.length = 0));

    update(createElement('View', {style: {width: 80}})); // no opacity at all — reverts to the engine default

    expect(calls.animUnbind).toEqual([{handle, prop: 'opacity'}]);
  });

  it('swapping the value unbinds the old one before binding the new one', () => {
    const firstValue = new FakeAnimatedValue();
    const secondValue = new FakeAnimatedValue();
    const ui = value =>
      createElement('View', {style: {opacity: value, width: 80}});
    const update = mount(ui(firstValue));
    const handle = firstValue.bound[0].handle;
    Object.values(calls).forEach(recorded => (recorded.length = 0));
    order.length = 0;

    update(ui(secondValue));

    // Without the unbind both values keep writing opacity — whichever ticks last wins.
    expect(calls.animUnbind).toEqual([{handle, prop: 'opacity'}]);
    expect(secondValue.bound).toEqual([{handle, prop: 'opacity'}]);
    expect(order).toEqual([
      `animUnbind:${handle}:opacity`,
      `setProps:${handle}`,
      `bind:${handle}:opacity`,
    ]);
  });

  it('an unrelated prop change re-applies the props without touching a live binding', () => {
    const animatedValue = new FakeAnimatedValue();
    const ui = width =>
      createElement('View', {style: {opacity: animatedValue, width}});
    const update = mount(ui(80));
    Object.values(calls).forEach(recorded => (recorded.length = 0));

    update(ui(90));

    expect(counts()).toEqual({...NOTHING, setProps: 1});
    expect(animatedValue.bound).toHaveLength(1); // still bound from the mount, not re-bound
  });

  it('`transform: [{scale}]` releases both axes it bound', () => {
    const animatedValue = new FakeAnimatedValue();
    const ui = scale =>
      createElement('View', {style: {transform: [{scale}], width: 80}});
    const update = mount(ui(animatedValue));
    const handle = animatedValue.bound[0].handle;
    expect(animatedValue.bound.map(binding => binding.prop)).toEqual([
      'scaleX',
      'scaleY',
    ]);
    Object.values(calls).forEach(recorded => (recorded.length = 0));

    update(ui(1));

    expect(calls.animUnbind).toEqual([
      {handle, prop: 'scaleX'},
      {handle, prop: 'scaleY'},
    ]);
  });

  it('<Dial value>: animated → static unbinds the arc value and marshals the number', () => {
    const animatedValue = new FakeAnimatedValue();
    const ui = value =>
      createElement('Dial', {style: {width: 100, height: 100}, value});
    const update = mount(ui(animatedValue));
    const handle = animatedValue.bound[0].handle;
    expect(animatedValue.bound[0].prop).toBe('value');
    Object.values(calls).forEach(recorded => (recorded.length = 0));

    update(ui(42));

    expect(calls.animUnbind).toEqual([{handle, prop: 'value'}]);
    expect(calls.setProps[0].props.value).toBe(42);
  });

  it('a deleted descendant leaves no record, so its handle would bind from scratch', () => {
    // React hands removeChild only the TOP of a deleted subtree; destroyNode reclaims the descendants
    // in C. Their records go when React detaches them, or the map would keep one per destroyed node,
    // and the handle would look already-bound if its generation ever wrapped back round to it.
    const animatedValue = new FakeAnimatedValue();
    const update = mount(
      createElement(
        'View',
        {},
        createElement('View', {style: {opacity: animatedValue}}),
      ),
    );
    const inner = animatedValue.bound[0].handle;

    update(null); // only the outer View goes through removeChildFromContainer
    lastHandle = inner - 1; // the engine hands the freed handle to the next node
    update(createElement('View', {style: {opacity: animatedValue}}));

    expect(animatedValue.bound).toEqual([
      {handle: inner, prop: 'opacity'},
      {handle: inner, prop: 'opacity'},
    ]);
  });
});

// An interpolation is a throwaway object rebuilt by every `value.interpolate(...)` call in render, so
// binding identity can't be object identity — it's the parent value plus the mapping.
describe('interpolated bindings compare by parent + config', () => {
  class FakeInterpolation {
    constructor(parent, config) {
      this.__animated = true;
      this._parent = parent;
      this._config = config;
      this.bound = [];
    }
    __bind(handle, prop) {
      this.bound.push({handle, prop});
      order.push(`bind:${handle}:${prop}`);
    }
    __bindEq(other) {
      return (
        other instanceof FakeInterpolation &&
        other._parent === this._parent &&
        JSON.stringify(other._config) === JSON.stringify(this._config)
      );
    }
  }

  const parent = new FakeAnimatedValue();
  const interp = outputRange =>
    new FakeInterpolation(parent, {
      inputRange: [0, 1],
      outputRange: outputRange,
    });

  it('a re-render with an equal interpolation leaves the binding alone', () => {
    const ui = (width, outputRange) =>
      createElement('View', {
        style: {opacity: interp(outputRange), width},
      });
    const update = mount(ui(80, [0, 1]));
    Object.values(calls).forEach(recorded => (recorded.length = 0));

    update(ui(90, [0, 1])); // fresh interpolation object, same mapping

    expect(counts()).toEqual({...NOTHING, setProps: 1});
  });

  it('a changed range is re-bound — the engine ignores a duplicate bind, so the old one must go', () => {
    const ui = outputRange =>
      createElement('View', {style: {opacity: interp(outputRange), width: 80}});
    const update = mount(ui([0, 1]));
    Object.values(calls).forEach(recorded => (recorded.length = 0));
    order.length = 0;

    update(ui([0, 0.5]));

    expect(calls.animUnbind).toHaveLength(1);
    expect(calls.animUnbind[0].prop).toBe('opacity');
    expect(order[order.length - 1]).toContain('bind:'); // released, then bound with the new mapping
  });
});

describe('<Dial>: the native arc widget marshals its props and binds an animated value', () => {
  it('passes the widget props through and registers onChange', () => {
    const onChange = () => {};
    mount(
      createElement('Dial', {
        style: {width: 200, height: 200},
        value: 42,
        min: 0,
        max: 100,
        thickness: 14,
        knob: 'circle',
        adjustable: true,
        indicatorGradient: {
          type: 'conic',
          stops: [{color: '#0000ff'}, {color: '#ff0000'}],
        },
        onChange,
      }),
    );
    const props = calls.setProps.find(
      call => call.props.thickness === 14,
    ).props;
    expect(props.value).toBe(42);
    expect(props.max).toBe(100);
    expect(props.knob).toBe('circle');
    expect(props.adjustable).toBe(true);
    expect(props.indicatorGradient.type).toBe('conic');
    expect(
      calls.setEvent.some(
        call => call.key === 'onChange' && call.handler === onChange,
      ),
    ).toBe(true);
  });

  it('an Animated.Value as `value` is bound to the node, not marshalled as a prop', () => {
    const animatedValue = new FakeAnimatedValue();
    const update = mount(
      createElement('Dial', {
        style: {width: 100, height: 100},
        value: animatedValue,
      }),
    );
    expect(animatedValue.bound).toHaveLength(1);
    expect(animatedValue.bound[0].prop).toBe('value');
    const props = calls.setProps.find(call => call.props.width === 100).props;
    expect(props.value).toBeUndefined();
    Object.values(calls).forEach(recorded => (recorded.length = 0));
    update(
      createElement('Dial', {
        style: {width: 100, height: 100},
        value: animatedValue,
      }),
    );
    expect(counts()).toEqual(NOTHING); // same instance: nothing to re-send or re-bind
  });
});
