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

// <TouchableOpacity> is a <Pressable> plus a native-driver dim, so what matters is what reaches the
// bridge: one node (not two), one animated value bound to its opacity, and an animate call per press
// end. These run through the REAL reconciler against a mocked NativeUI — the component is all hooks,
// so calling it directly would render nothing.

import {describe, it, expect, beforeEach, vi} from 'vitest';

const calls = {
  createNode: [],
  setEvent: [],
  animValueCreate: [],
  animValueBind: [],
  animValueSet: [],
  animValueAnimate: [],
  animValueDestroy: [],
};
let lastHandle = 0;
let lastValueHandle = 0;
globalThis.IS_REACT_ACT_ENVIRONMENT = false;
globalThis.NativeUI = {
  createNode: type => {
    calls.createNode.push(type);
    return ++lastHandle;
  },
  setProps: () => {},
  setRoot: () => {},
  appendChild: () => {},
  insertBefore: () => {},
  removeChild: () => {},
  destroyNode: () => {},
  setEvent: (handle, key, handler) =>
    calls.setEvent.push({handle, key, handler}),
  setTextSpans: () => {},
  setVectorOps: () => {},
  animUnbind: () => {},
  animValueCreate: initial => {
    calls.animValueCreate.push(initial);
    return ++lastValueHandle;
  },
  animValueBind: (handle, node, prop) =>
    calls.animValueBind.push({handle, node, prop}),
  animValueSet: (handle, value) => calls.animValueSet.push({handle, value}),
  animValueGet: () => 1,
  animValueDestroy: handle => calls.animValueDestroy.push(handle),
  animValueAnimate: (handle, toValue, config) => {
    calls.animValueAnimate.push({handle, toValue, config});
    return ++lastValueHandle;
  },
  animStop: () => {},
  commit: () => {},
  now: () => 0,
  maxVectorOps: 4096,
  maxVectorPaints: 1024,
  maxVectorGrads: 256,
};

const Reconciler = (await import('react-reconciler')).default;
const {hostConfig} = await import('../../host-config.js');
const {createElement} = (await import('react')).default;
const {TouchableOpacity} = await import('../TouchableOpacity.js');

const reconciler = Reconciler(hostConfig);

/** Mounts a tree into a fresh synchronous root; returns update(next) — update(null) unmounts.
 *  Passive effects are flushed by hand: useAnimatedValue frees its engine slot from one, and the
 *  resting-opacity sync is another, so a test that never flushed would be testing half the component. */
function mount(tree) {
  const container = globalThis.NativeUI.createNode('View');
  calls.createNode.length = 0; // the container is the harness's, not the app's
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
  const render = next => {
    reconciler.updateContainer(next, root, null, null);
    reconciler.flushPassiveEffects();
  };
  render(tree);
  return render;
}

/** The handler last registered for `key`, as the engine would hold it. */
const handler = key => {
  const found = calls.setEvent.filter(call => call.key === key).pop();
  return found && found.handler;
};
/** The one animate call, or the nth. */
const animated = (index = 0) => calls.animValueAnimate[index];

beforeEach(() => {
  for (const recorded of Object.values(calls)) recorded.length = 0;
});

describe('TouchableOpacity renders one Pressable with a bound opacity', () => {
  it('costs a single engine node — the dim is a binding, not a wrapper', () => {
    mount(
      createElement(TouchableOpacity, {}, createElement('Text', {}, 'Tap')),
    );
    // Children are created before their parent, so order is the reconciler's, not the tree's.
    expect(calls.createNode).toHaveLength(2);
    expect(calls.createNode).toContain('Pressable');
  });

  it('binds its animated value to the node’s opacity', () => {
    mount(createElement(TouchableOpacity, {}));
    expect(calls.animValueCreate).toEqual([1]);
    expect(calls.animValueBind).toHaveLength(1);
    expect(calls.animValueBind[0].prop).toBe('opacity');
  });

  it('starts at the opacity the style asks for', () => {
    mount(createElement(TouchableOpacity, {style: {opacity: 0.6}}));
    expect(calls.animValueCreate).toEqual([0.6]);
  });

  it('registers both ends of the press so the engine can drive the fade', () => {
    mount(createElement(TouchableOpacity, {}));
    expect(handler('onPressIn')).toBeTypeOf('function');
    expect(handler('onPressOut')).toBeTypeOf('function');
  });
});

describe('TouchableOpacity press feedback', () => {
  it('dims to RN’s 0.2 with no ramp on press-in', () => {
    mount(createElement(TouchableOpacity, {}));
    handler('onPressIn')({});
    expect(animated()).toMatchObject({toValue: 0.2});
    expect(animated().config).toMatchObject({type: 'timing', duration: 0});
  });

  it('fades back to the resting opacity over a quarter second on press-out', () => {
    mount(createElement(TouchableOpacity, {style: {opacity: 0.6}}));
    handler('onPressOut')({});
    expect(animated()).toMatchObject({toValue: 0.6});
    expect(animated().config).toMatchObject({type: 'timing', duration: 250});
  });

  it('honours activeOpacity', () => {
    mount(createElement(TouchableOpacity, {activeOpacity: 0.85}));
    handler('onPressIn')({});
    expect(animated().toValue).toBe(0.85);
  });

  it('drives the same value both ways (one binding, not two animations racing)', () => {
    mount(createElement(TouchableOpacity, {}));
    handler('onPressIn')({});
    handler('onPressOut')({});
    expect(animated(0).handle).toBe(animated(1).handle);
    expect(animated(0).handle).toBe(calls.animValueBind[0].handle);
  });

  it('still calls the app’s own onPressIn/onPressOut', () => {
    const seen = [];
    mount(
      createElement(TouchableOpacity, {
        onPressIn: () => seen.push('in'),
        onPressOut: () => seen.push('out'),
      }),
    );
    handler('onPressIn')({});
    handler('onPressOut')({});
    expect(seen).toEqual(['in', 'out']);
  });
});

describe('TouchableOpacity disabled', () => {
  it('registers no press handler at all, so it neither fires nor dims', () => {
    mount(createElement(TouchableOpacity, {disabled: true, onPress: () => {}}));
    const live = calls.setEvent.filter(call => call.handler);
    expect(live).toEqual([]);
  });

  it('presses and dims again once it is enabled', () => {
    const update = mount(createElement(TouchableOpacity, {disabled: true}));
    update(createElement(TouchableOpacity, {disabled: false}));
    expect(handler('onPressIn')).toBeTypeOf('function');
    handler('onPressIn')({});
    expect(animated().toValue).toBe(0.2);
  });
});

describe('TouchableOpacity with an animated opacity in its style', () => {
  // The press feedback owns this node's opacity. RN drops the app's the same way (its composed style
  // shadows it) but says nothing; Flow B refuses to compile it. Flow A's job is to not be silent.
  // NB the warning is deliberately once-per-process, so the first test here is the one that sees it.
  class FakeAnimatedValue {
    constructor() {
      this.__animated = true;
    }
    __bind() {}
  }
  const animatedStyle = () => ({opacity: new FakeAnimatedValue()});

  it('says so rather than dropping it in silence — once, not once per render', () => {
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => {});
    const update = mount(
      createElement(TouchableOpacity, {style: animatedStyle()}),
    );
    update(createElement(TouchableOpacity, {style: animatedStyle()}));
    mount(createElement(TouchableOpacity, {style: animatedStyle()}));
    expect(warn).toHaveBeenCalledTimes(1);
    expect(warn.mock.calls[0][0]).toMatch(/ignores an animated `opacity`/);
    expect(warn.mock.calls[0][0]).toMatch(/<Pressable>/);
    warn.mockRestore();
  });

  it('binds the press value only — never two things writing one prop', () => {
    mount(createElement(TouchableOpacity, {style: animatedStyle()}));
    expect(calls.animValueBind).toHaveLength(1);
    expect(calls.animValueBind[0].prop).toBe('opacity');
  });

  it('rests fully opaque, since what it was handed is not a number', () => {
    mount(createElement(TouchableOpacity, {style: animatedStyle()}));
    expect(calls.animValueCreate).toEqual([1]);
    handler('onPressOut')({});
    expect(animated().toValue).toBe(1);
  });
});

describe('TouchableOpacity across renders', () => {
  it('keeps its press handlers stable, so an idle re-render re-registers nothing', () => {
    const style = {padding: 8};
    const onPress = () => {};
    const update = mount(createElement(TouchableOpacity, {style, onPress}));
    calls.setEvent.length = 0;
    update(createElement(TouchableOpacity, {style, onPress}));
    expect(calls.setEvent).toEqual([]);
  });

  it('pushes a changed style opacity through the binding that owns it', () => {
    const update = mount(
      createElement(TouchableOpacity, {style: {opacity: 1}}),
    );
    calls.animValueSet.length = 0;
    update(createElement(TouchableOpacity, {style: {opacity: 0.4}}));
    expect(calls.animValueSet).toEqual([
      {handle: calls.animValueBind[0].handle, value: 0.4},
    ]);
    // …and the press-out now restores to the new one.
    handler('onPressOut')({});
    expect(animated().toValue).toBe(0.4);
  });

  it('leaves the value alone mid-press, where the dim owns it', () => {
    const update = mount(
      createElement(TouchableOpacity, {style: {opacity: 1}}),
    );
    handler('onPressIn')({});
    calls.animValueSet.length = 0;
    update(createElement(TouchableOpacity, {style: {opacity: 0.4}}));
    expect(calls.animValueSet).toEqual([]);
  });

  it('frees the engine-side value slot on unmount', () => {
    const update = mount(createElement(TouchableOpacity, {}));
    expect(calls.animValueDestroy).toEqual([]);
    update(null);
    expect(calls.animValueDestroy).toEqual([calls.animValueBind[0].handle]);
  });
});
