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

import {afterEach, describe, expect, it, vi} from 'vitest';
import {Dimensions, useWindowDimensions} from '../Dimensions.js';

afterEach(() => {
  vi.unstubAllGlobals();
});

describe('Dimensions', () => {
  it('reports the panel size at a scale of 1', () => {
    vi.stubGlobal('screen', {width: 240, height: 320});
    const size = {width: 240, height: 320, scale: 1, fontScale: 1};
    expect(Dimensions.get('window')).toEqual(size);
    expect(Dimensions.get('screen')).toEqual(size);
  });

  it('reads the panel each time, as the simulator can restart at a new size', () => {
    vi.stubGlobal('screen', {width: 240, height: 320});
    Dimensions.get('window');
    vi.stubGlobal('screen', {width: 480, height: 272});
    expect(Dimensions.get('window').width).toBe(480);
  });

  it('refuses a size it does not have', () => {
    vi.stubGlobal('screen', {width: 240, height: 320});
    // @ts-expect-error -- only 'window' and 'screen' name a size
    expect(() => Dimensions.get('display')).toThrow(/display/);
  });

  it('gives useWindowDimensions the same size', () => {
    vi.stubGlobal('screen', {width: 320, height: 240});
    expect(useWindowDimensions()).toEqual(Dimensions.get('window'));
  });
});
