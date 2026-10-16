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

// Pressable — RN's core press target, as a forwardRef component over the host <Pressable> element.
//
// Setting `disabled` withholds all press-related event handlers (`onPress`, `onLongPress`, `onPressIn`,
// `onPressOut`) so that NativeUI never registers callbacks on the native node. The C hit-testing
// logic skips nodes without press handlers, preventing disabled pressables from claiming the gesture responder.
import {createElement, forwardRef} from 'react';
import {Pressable as HostPressable} from './components.js';

/**
 * A container that responds to touch interactions.
 *
 * When `disabled` is true, all press handlers are withheld so the node will not intercept touches
 * or fire callbacks.
 *
 * @param {object} [props] Pressable props.
 * @param {boolean} [props.disabled] Whether touch interactions are disabled.
 * @param {Function} [props.onPress] Called when a press is activated.
 * @param {Function} [props.onLongPress] Called when a long press is activated.
 * @param {Function} [props.onPressIn] Called immediately when a touch is pressed down.
 * @param {Function} [props.onPressOut] Called when a touch is released or canceled.
 * @param {*} [props.children] Children elements.
 * @returns {*} A host <Pressable> element.
 */
export const Pressable = forwardRef(function Pressable(props = {}, ref) {
  const {
    disabled,
    onPress,
    onLongPress,
    onPressIn,
    onPressOut,
    ...rest
  } = props;

  return createElement(HostPressable, {
    ...rest,
    ref,
    disabled,
    onPress: disabled ? undefined : onPress,
    onLongPress: disabled ? undefined : onLongPress,
    onPressIn: disabled ? undefined : onPressIn,
    onPressOut: disabled ? undefined : onPressOut,
  });
});
