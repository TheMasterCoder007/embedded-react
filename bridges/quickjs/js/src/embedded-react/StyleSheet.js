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

// StyleSheet — the React Native analog. `create` is an identity pass-through (styles are plain
// objects the host config flattens at setProps time); `flatten` collapses nested arrays/objects.
export const StyleSheet = {
  create(styles) {
    return styles;
  },

  flatten(style) {
    if (!style) return {};
    if (Array.isArray(style)) {
      const out = {};
      for (const s of style) Object.assign(out, StyleSheet.flatten(s));
      return out;
    }
    return style;
  },

  hairlineWidth: 1,
  absoluteFill: { position: 'absolute', top: 0, left: 0, right: 0, bottom: 0 },
};
