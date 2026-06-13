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

import { describe, it, expect } from 'vitest';
import { toEngineConfig, create, Presets, Types, Properties } from '../layout-anim-config.js';

describe('toEngineConfig', () => {
  it('defaults to a 300ms ease-in-out timing', () => {
    expect(toEngineConfig(undefined)).toEqual({ type: 'timing', duration: 300, easing: 'easeInOut' });
    expect(toEngineConfig({})).toEqual({ type: 'timing', duration: 300, easing: 'easeInOut' });
  });

  it('carries the duration through', () => {
    expect(toEngineConfig({ duration: 500 })).toEqual({ type: 'timing', duration: 500, easing: 'easeInOut' });
  });

  it('maps update.type to the engine easing token', () => {
    expect(toEngineConfig({ update: { type: Types.linear } }).easing).toBe('linear');
    expect(toEngineConfig({ update: { type: Types.easeIn } }).easing).toBe('easeIn');
    expect(toEngineConfig({ update: { type: Types.easeOut } }).easing).toBe('easeOut');
    expect(toEngineConfig({ update: { type: Types.easeInEaseOut } }).easing).toBe('easeInOut');
    expect(toEngineConfig({ update: { type: Types.keyboard } }).easing).toBe('easeInOut');
  });

  it('produces a spring config (no easing) for spring updates', () => {
    expect(toEngineConfig({ duration: 700, update: { type: Types.spring } })).toEqual({
      type: 'spring',
      duration: 700,
    });
  });

  it('accepts a top-level type when there is no update block', () => {
    expect(toEngineConfig({ type: Types.linear }).easing).toBe('linear');
    expect(toEngineConfig({ type: Types.spring })).toEqual({ type: 'spring', duration: 300 });
  });
});

describe('create / Presets', () => {
  it('create builds an RN-shaped config', () => {
    expect(create(400, Types.linear, Properties.opacity)).toEqual({
      duration: 400,
      create: { type: 'linear', property: 'opacity' },
      update: { type: 'linear' },
      delete: { type: 'linear', property: 'opacity' },
    });
  });

  it('presets reduce to the expected engine configs', () => {
    expect(toEngineConfig(Presets.easeInEaseOut)).toEqual({ type: 'timing', duration: 300, easing: 'easeInOut' });
    expect(toEngineConfig(Presets.linear)).toEqual({ type: 'timing', duration: 500, easing: 'linear' });
    expect(toEngineConfig(Presets.spring)).toEqual({ type: 'spring', duration: 700 });
  });
});
