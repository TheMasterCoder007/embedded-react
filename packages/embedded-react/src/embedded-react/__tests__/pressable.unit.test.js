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

import {describe, it, expect, beforeEach} from 'vitest';
import {createRef, createElement as el} from 'react';

const calls = {
  createNode: [],
  setEvent: [],
  setProps: [],
};
let idc = 0;
globalThis.IS_REACT_ACT_ENVIRONMENT = false;
globalThis.NativeUI = {
  createNode: type => {
    calls.createNode.push(type);
    return ++idc;
  },
  setProps: (handle, props) => {
    calls.setProps.push({handle, props});
  },
  setRoot: () => {},
  appendChild: () => {},
  insertBefore: () => {},
  removeChild: () => {},
  destroyNode: () => {},
  setEvent: (h, key, fn) => calls.setEvent.push({h, key, fn}),
  setTextSpans: () => {},
  setVectorOps: () => {},
  animUnbind: () => {},
  commit: () => {},
  now: () => 0,
};

const Reconciler = (await import('react-reconciler')).default;
const {hostConfig} = await import('../../host-config.js');
const {Pressable, Text} = await import('../index.js');

const reconciler = Reconciler(hostConfig);

function mount(tree) {
  const container = globalThis.NativeUI.createNode('View');
  calls.createNode.length = 0;
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

const handler = key => {
  const found = calls.setEvent.filter(e => e.key === key).pop();
  return found && found.fn;
};

beforeEach(() => {
  calls.createNode.length = 0;
  calls.setEvent.length = 0;
  calls.setProps.length = 0;
  idc = 0;
});

describe('Pressable host node creation', () => {
  it('renders a host Pressable element', () => {
    mount(el(Pressable, {}));
    expect(calls.createNode).toEqual(['Pressable']);
  });

  it('forwards ref to the host node', () => {
    const ref = createRef();
    mount(el(Pressable, {ref}));
    expect(ref.current).toBeTypeOf('number');
  });
});

describe('Pressable press handlers when enabled', () => {
  it('registers all press handlers when enabled or disabled is omitted', () => {
    const onPress = () => {};
    const onLongPress = () => {};
    const onPressIn = () => {};
    const onPressOut = () => {};

    mount(
      el(Pressable, {
        onPress,
        onLongPress,
        onPressIn,
        onPressOut,
      }),
    );

    expect(handler('onPress')).toBe(onPress);
    expect(handler('onLongPress')).toBe(onLongPress);
    expect(handler('onPressIn')).toBe(onPressIn);
    expect(handler('onPressOut')).toBe(onPressOut);
  });

  it('registers handlers when disabled is explicitly false', () => {
    const onPress = () => {};
    mount(el(Pressable, {disabled: false, onPress}));
    expect(handler('onPress')).toBe(onPress);
  });
});

describe('Pressable disabled handler withholding', () => {
  it('registers no press handlers when disabled is true', () => {
    const onPress = () => {};
    const onLongPress = () => {};
    const onPressIn = () => {};
    const onPressOut = () => {};

    mount(
      el(Pressable, {
        disabled: true,
        onPress,
        onLongPress,
        onPressIn,
        onPressOut,
      }),
    );

    const live = calls.setEvent.filter(e => e.fn);
    expect(live).toEqual([]);
  });

  it('clears handlers when dynamically updating to disabled=true, and restores them when re-enabled', () => {
    const onPress = () => {};
    const update = mount(el(Pressable, {disabled: false, onPress}));
    expect(handler('onPress')).toBe(onPress);

    update(el(Pressable, {disabled: true, onPress}));
    expect(handler('onPress')).toBeFalsy();

    update(el(Pressable, {disabled: false, onPress}));
    expect(handler('onPress')).toBe(onPress);
  });

  it('preserves children and style when disabled', () => {
    mount(
      el(
        Pressable,
        {disabled: true, style: {padding: 12}},
        el(Text, {}, 'Child text'),
      ),
    );
    expect(calls.createNode).toEqual(['Text', 'Pressable']);
    expect(calls.setProps[1].props).toMatchObject({padding: 12});
  });
});
