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

// Separates a style object into the static props (passed straight to the node) and the animated
// bindings (Animated.Value / interpolation, bound to the node's prop so the engine drives them).
// Pure (no NativeUI), so it's unit-testable. An animated entry is anything with an `__animated`
// marker — `transform: [{ translateX: value }]` entries are handled too (scale binds both axes).
import {flattenStyle} from '../props.mts';
import type {Style} from '../props.mts';

/** An Animated.Value or an interpolation of one, as the host config binds it to a node's prop. */
export interface Bindable {
  __animated: boolean;
  __bind(node: number, prop: string): void;
  /** Whether `other` binds a prop the same way. */
  __bindEq?(other: unknown): boolean;
}

/** A node prop and the animated value that drives it. */
export interface Binding {
  prop: string;
  value: Bindable;
}

export function splitAnimatedStyle(style: unknown): {
  staticStyle: Style;
  bindings: Binding[];
} {
  const staticStyle: Style = {};
  const bindings: Binding[] = [];

  const flat: Style = {};
  flattenStyle(style, flat);

  for (const key in flat) {
    const value = flat[key];

    if (key === 'transform' && Array.isArray(value)) {
      const staticTransform = [];
      for (const entry of value) {
        const axis = Object.keys(entry)[0];
        const axisValue = entry[axis];
        if (axisValue && axisValue.__animated) {
          if (axis === 'scale') {
            bindings.push({prop: 'scaleX', value: axisValue});
            bindings.push({prop: 'scaleY', value: axisValue});
          } else {
            bindings.push({prop: axis, value: axisValue});
          }
        } else {
          staticTransform.push(entry);
        }
      }
      if (staticTransform.length > 0) staticStyle.transform = staticTransform;
    } else if (value && value.__animated) {
      bindings.push({prop: key, value});
    } else {
      staticStyle[key] = value;
    }
  }

  return {staticStyle, bindings};
}
