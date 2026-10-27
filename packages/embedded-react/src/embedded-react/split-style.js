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
import {flattenStyle} from '../props.js';

export function splitAnimatedStyle(style) {
  const staticStyle = {};
  const bindings = [];

  const flat = {};
  flattenStyle(style, flat);

  for (const key in flat) {
    const val = flat[key];

    if (key === 'transform' && Array.isArray(val)) {
      const staticTransform = [];
      for (const entry of val) {
        const axis = Object.keys(entry)[0];
        const v = entry[axis];
        if (v && v.__animated) {
          if (axis === 'scale') {
            bindings.push({prop: 'scaleX', value: v});
            bindings.push({prop: 'scaleY', value: v});
          } else {
            bindings.push({prop: axis, value: v});
          }
        } else {
          staticTransform.push(entry);
        }
      }
      if (staticTransform.length > 0) staticStyle.transform = staticTransform;
    } else if (val && val.__animated) {
      bindings.push({prop: key, value: val});
    } else {
      staticStyle[key] = val;
    }
  }

  return {staticStyle, bindings};
}
