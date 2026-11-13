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

// Dimensions — RN analog over the host-injected `screen` global. A panel has no pixel-density or font
// setting, so `scale` and `fontScale` are 1, and 'window' and 'screen' are the same size.
export const Dimensions = {
  get(dimension) {
    if (dimension !== 'window' && dimension !== 'screen') {
      throw new Error(`Dimensions.get: no dimension named "${dimension}"`);
    }

    return {
      width: screen.width,
      height: screen.height,
      scale: 1,
      fontScale: 1,
    };
  },
};

// The panel's size. It is fixed while the app runs, so this never re-renders.
export function useWindowDimensions() {
  return Dimensions.get('window');
}
