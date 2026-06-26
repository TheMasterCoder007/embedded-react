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
import {StyleSheet} from '../StyleSheet.js';
import {Platform} from '../Platform.js';

describe('StyleSheet', () => {
  it('create returns the styles unchanged (identity)', () => {
    const styles = {box: {width: 10}, label: {color: 'white'}};
    expect(StyleSheet.create(styles)).toBe(styles);
  });

  it('flatten collapses nested arrays, later entries winning', () => {
    expect(StyleSheet.flatten([{a: 1}, [{b: 2}, {a: 3}]])).toEqual({
      a: 3,
      b: 2,
    });
  });

  it('flatten of null/undefined is an empty object', () => {
    expect(StyleSheet.flatten(null)).toEqual({});
    expect(StyleSheet.flatten(undefined)).toEqual({});
  });

  it('flatten passes a plain object through', () => {
    expect(StyleSheet.flatten({width: 5})).toEqual({width: 5});
  });
});

describe('Platform', () => {
  it('OS is "embedded"', () => {
    expect(Platform.OS).toBe('embedded');
  });

  it('select picks the embedded entry', () => {
    expect(Platform.select({embedded: 'e', ios: 'i', default: 'd'})).toBe('e');
  });

  it('select falls back to default when no embedded entry', () => {
    expect(Platform.select({ios: 'i', default: 'd'})).toBe('d');
  });
});
