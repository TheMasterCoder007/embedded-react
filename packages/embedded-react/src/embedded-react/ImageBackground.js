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

// ImageBackground — RN's "content on top of a picture" wrapper, as a plain component. No engine node
// of its own:
//
//   <ImageBackground source={wallpaper} style={{flex: 1}}><Text>hi</Text></ImageBackground>
//     renders the same tree as
//   <View style={{flex: 1}}>
//     <Image source={wallpaper} style={StyleSheet.absoluteFill} />
//     <Text>hi</Text>
//   </View>
//
// <Image> cannot take children (it is a leaf node in the engine, as in RN), which is the whole reason
// this component exists. The image is absolutely positioned, so it is outside the flow: the children
// lay out against the <View> as if it weren't there, and they paint over it because the engine paints
// in tree order.
//
// One deliberate difference from upstream, because our <Image> is a plain scene node: RN re-proxies
// the container's width/height onto the image to undo <Image>'s own sizing. Ours doesn't size itself,
// so the inset alone fills the box — and since an absolute child resolves its insets against the
// container's PADDING box, as in RN, the picture fills the whole container even when it has padding.
import {createElement, forwardRef} from 'react';
import {View, Image} from './components.js';
import {StyleSheet} from './StyleSheet.js';

/**
 * A <View> with an <Image> stretched behind its children.
 *
 * `style` sizes and lays out the container; every OTHER prop goes to the <Image>, exactly as upstream
 * — `source`, `resizeMode` and `tintColor` are the ones worth setting. Note that this puts touch and
 * `onLayout` handlers on the image rather than the container, and the children paint on top of it, so
 * a handler meant for the whole area belongs on a <Pressable> around this component.
 *
 * @param {object} props Image props, plus the four below.
 * @param {*} [props.style] Container style — the box the picture fills.
 * @param {*} [props.imageStyle] Style merged over the fill, for `borderRadius`, `opacity`, `tintColor`.
 * @param {*} [props.imageRef] Ref to the <Image> node (the outer `ref` goes to the <View>).
 * @param {*} [props.children] Rendered above the picture, laid out against the container.
 * @returns {*} A <View> whose first child is the backing <Image>.
 */
export const ImageBackground = forwardRef(function ImageBackground(props, ref) {
  const {children, style, imageStyle, imageRef, ...imageProps} = props;
  return createElement(
    View,
    {style, ref},
    createElement(Image, {
      ...imageProps,
      style: [StyleSheet.absoluteFill, imageStyle],
      ref: imageRef,
    }),
    children,
  );
});
