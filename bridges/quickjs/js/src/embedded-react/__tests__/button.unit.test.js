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

import {describe, it, expect, vi, afterEach} from 'vitest';
import {Button} from '../Button.js';
import {flattenStyleObj} from '../../props.js';

// Button is a plain component, so calling it IS the render — no reconciler needed. What matters is the
// element tree it returns and the styles that end up flattened onto those two nodes, since that flatten
// is exactly what buildProps does before the props reach the bridge.

const render = props => Button(props);
/** The <Text> the button wraps. */
const label = el => el.props.children;
/** A node's style as the bridge would see it: the array collapsed to one object. */
const flat = el => flattenStyleObj(el.props.style);

afterEach(() => vi.restoreAllMocks());

describe('Button renders as a Pressable + Text', () => {
  it('returns a Pressable wrapping exactly one Text', () => {
    const el = render({title: 'Save'});
    expect(el.type).toBe('Pressable');
    expect(label(el).type).toBe('Text');
    expect(label(el).props.children).toBe('Save');
  });

  it('does not add RN’s intermediate View (two engine nodes, not three)', () => {
    const el = render({title: 'Save'});
    expect(Array.isArray(label(el))).toBe(false);
    expect(label(el).type).not.toBe('View');
  });

  it('wires onPress onto the Pressable', () => {
    const onPress = () => {};
    expect(render({title: 'x', onPress}).props.onPress).toBe(onPress);
  });

  it('paints the default fill and centres the label', () => {
    const el = render({title: 'x'});
    expect(flat(el)).toMatchObject({backgroundColor: '#2196F3', padding: 8});
    expect(flat(label(el))).toMatchObject({
      color: '#ffffff',
      textAlign: 'center',
    });
  });

  // RN pads the <Text>; an auto-sized text node here measures only its glyph run, so padding on the
  // label would vanish. It has to sit on the <Pressable> or the button hugs its letters.
  it('puts the padding on the Pressable, not on the label', () => {
    const el = render({title: 'x'});
    expect(flat(el).padding).toBe(8);
    expect(flat(label(el)).padding).toBe(undefined);
  });
});

describe('Button color and disabled', () => {
  it('replaces the fill with `color`, keeping the rest of the style', () => {
    const el = render({title: 'x', color: '#ff0000'});
    expect(flat(el)).toMatchObject({
      backgroundColor: '#ff0000',
      borderRadius: 2,
    });
  });

  it('ignores a falsy color rather than painting it', () => {
    for (const color of [undefined, null, '', false]) {
      expect(flat(render({title: 'x', color})).backgroundColor).toBe('#2196F3');
    }
  });

  it('greys both nodes out when disabled', () => {
    const el = render({title: 'x', disabled: true});
    expect(flat(el).backgroundColor).toBe('#dfdfdf');
    expect(flat(label(el)).color).toBe('#a1a1a1');
  });

  // `disabled` is inert on a Pressable node (the engine has no such flag), so withholding the handler
  // is the only thing that actually stops the press.
  it('detaches onPress when disabled', () => {
    const onPress = () => {};
    expect(render({title: 'x', onPress, disabled: true}).props.onPress).toBe(
      undefined,
    );
    expect(render({title: 'x', onPress, disabled: false}).props.onPress).toBe(
      onPress,
    );
  });

  it('lets `color` win over the disabled fill only when disabled is false', () => {
    expect(flat(render({title: 'x', color: '#0f0'})).backgroundColor).toBe(
      '#0f0',
    );
    // Disabled is pushed last, so it overrides an explicit colour — as it does in RN.
    expect(
      flat(render({title: 'x', color: '#0f0', disabled: true})).backgroundColor,
    ).toBe('#dfdfdf');
  });
});

describe('Button title coercion', () => {
  it('renders an empty label instead of throwing on a missing title', () => {
    for (const props of [undefined, {}, {title: null}]) {
      expect(label(Button(props)).props.children).toBe('');
    }
  });

  it('stringifies a non-string title', () => {
    expect(label(render({title: 42})).props.children).toBe('42');
  });

  it('does not upper-case the title (RN does that on Android)', () => {
    expect(label(render({title: 'Save'})).props.children).toBe('Save');
  });
});

describe('Button prop warnings', () => {
  // The warner latches ONCE per module, not per render, so these two run in declaration order on
  // purpose: everything that must stay silent has to be asserted before anything trips it.
  it('says nothing about the supported props, or about RN props that need an OS', () => {
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => {});
    render({title: 'x', onPress: () => {}, color: '#fff', disabled: false});
    render({
      title: 'x',
      accessibilityLabel: 'Save',
      accessibilityHint: 'Saves it',
      testID: 'save-btn',
      touchSoundDisabled: true,
      hasTVPreferredFocus: true,
      nextFocusDown: 3,
    });
    expect(warn).not.toHaveBeenCalled();
  });

  it('warns once about props it ignores, naming them and the way out', () => {
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => {});
    render({title: 'x', style: {flex: 1}, hitSlop: 4});
    expect(warn).toHaveBeenCalledTimes(1);
    expect(warn.mock.calls[0][0]).toContain('style');
    expect(warn.mock.calls[0][0]).toContain('hitSlop');
    expect(warn.mock.calls[0][0]).toContain('<Pressable>');

    // Warn-once: a screen of buttons must not flood the device console.
    render({title: 'x', style: {}});
    render({title: 'x', onLongPress: () => {}});
    expect(warn).toHaveBeenCalledTimes(1);
  });
});
