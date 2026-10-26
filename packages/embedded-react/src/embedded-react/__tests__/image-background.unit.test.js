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
import {ImageBackground} from '../ImageBackground.js';
import {StyleSheet} from '../StyleSheet.js';
import {flattenStyleObj} from '../../props.js';

// ImageBackground is a forwardRef component, so `.render(props, ref)` IS the render — the reconciler
// only decides where the two refs land, which the runtime test covers. Here we assert the element tree:
// a <View> carrying the container style, an absolutely-filled <Image> first, children after it.

const render = (props, ref = null) => ImageBackground.render(props, ref);
/** [image, ...children] of the returned <View>. */
const kids = el => el.props.children;
const image = el => kids(el)[0];
const flat = el => flattenStyleObj(el.props.style);

describe('ImageBackground renders as a View + filling Image', () => {
  it('returns a View whose first child is the Image', () => {
    const el = render({source: 'bg', children: 'hi'});
    expect(el.type).toBe('View');
    expect(image(el).type).toBe('Image');
    // The image paints first (tree order), so the children are drawn over it.
    expect(kids(el)[1]).toBe('hi');
  });

  it('gives the image an absolute fill', () => {
    expect(flat(image(render({source: 'bg'})))).toMatchObject(
      StyleSheet.absoluteFill,
    );
  });

  it('puts `style` on the container and leaves it off the image', () => {
    const style = {flex: 1, padding: 8};
    const el = render({source: 'bg', style});
    expect(el.props.style).toBe(style);
    expect(flat(image(el)).flex).toBe(undefined);
  });

  it('merges `imageStyle` over the fill', () => {
    const el = render({
      source: 'bg',
      imageStyle: {opacity: 0.4, borderRadius: 6, top: 4},
    });
    expect(flat(image(el))).toMatchObject({
      position: 'absolute',
      left: 0,
      opacity: 0.4,
      borderRadius: 6,
      top: 4, // imageStyle wins over the fill's own inset
    });
  });

  it('forwards every other prop to the Image, as upstream does', () => {
    const onLayout = () => {};
    const el = render({
      source: {uri: 'bg'},
      resizeMode: 'contain',
      tintColor: '#f00',
      onLayout,
    });
    expect(image(el).props).toMatchObject({
      source: {uri: 'bg'},
      resizeMode: 'contain',
      tintColor: '#f00',
      onLayout,
    });
    // …and none of them leak onto the container.
    expect(el.props.resizeMode).toBe(undefined);
    expect(el.props.onLayout).toBe(undefined);
  });

  it('renders the image alone when there are no children', () => {
    const el = render({source: 'bg'});
    expect(kids(el)[1]).toBe(undefined);
  });

  it('keeps multiple children in order after the image', () => {
    const el = render({source: 'bg', children: ['a', 'b']});
    expect(kids(el)[1]).toEqual(['a', 'b']);
  });
});

describe('ImageBackground refs', () => {
  it('points the outer ref at the View and imageRef at the Image', () => {
    const viewRef = {current: null};
    const imageRef = {current: null};
    const el = render({source: 'bg', imageRef}, viewRef);
    expect(el.ref).toBe(viewRef);
    expect(image(el).ref).toBe(imageRef);
    // imageRef is consumed here, not forwarded on as an Image prop.
    expect(image(el).props.imageRef).toBe(undefined);
  });

  it('leaves both refs null when neither is given', () => {
    const el = render({source: 'bg'});
    expect(el.ref).toBe(null);
    expect(image(el).ref).toBe(null);
  });
});
