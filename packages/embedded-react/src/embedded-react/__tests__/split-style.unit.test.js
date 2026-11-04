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
import {splitAnimatedStyle} from '../split-style.js';

// Stand-in for an Animated.Value / interpolation (anything with the __animated marker).
const anim = id => ({__animated: true, id, __bind() {}});

describe('splitAnimatedStyle', () => {
  it('keeps a fully static style static, with no bindings', () => {
    const {staticStyle, bindings} = splitAnimatedStyle({width: 10, opacity: 1});
    expect(staticStyle).toEqual({width: 10, opacity: 1});
    expect(bindings).toEqual([]);
  });

  it('extracts an animated top-level prop into a binding', () => {
    const opacity = anim('o');
    const {staticStyle, bindings} = splitAnimatedStyle({
      width: 10,
      opacity: opacity,
    });
    expect(staticStyle).toEqual({width: 10});
    expect(bindings).toEqual([{prop: 'opacity', value: opacity}]);
  });

  it('extracts an animated transform axis, keeping static transform entries', () => {
    const translateX = anim('tx');
    const {staticStyle, bindings} = splitAnimatedStyle({
      transform: [{translateX}, {rotate: '10deg'}],
    });
    expect(staticStyle).toEqual({transform: [{rotate: '10deg'}]});
    expect(bindings).toEqual([{prop: 'translateX', value: translateX}]);
  });

  it('animated scale binds both axes', () => {
    const scale = anim('s');
    const {bindings} = splitAnimatedStyle({transform: [{scale}]});
    expect(bindings).toEqual([
      {prop: 'scaleX', value: scale},
      {prop: 'scaleY', value: scale},
    ]);
  });

  it('flattens nested style arrays before splitting', () => {
    const opacity = anim('o');
    const {staticStyle, bindings} = splitAnimatedStyle([
      {width: 10},
      {opacity},
    ]);
    expect(staticStyle).toEqual({width: 10});
    expect(bindings).toEqual([{prop: 'opacity', value: opacity}]);
  });
});
