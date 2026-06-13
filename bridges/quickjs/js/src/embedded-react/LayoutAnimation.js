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

// LayoutAnimation — the React Native analog. Calling configureNext(...) before a state update that
// changes layout makes the engine tween every node whose computed rect moved (instead of snapping)
// on the next commit. Backed by the engine's er_layout_anim_configure_next; the tween advances in C
// each frame (er_layout_anim_tick), so there's no per-frame JS.
import { NativeUI } from '../native-ui.js';
import { Types, Properties, Presets, toEngineConfig, create } from './layout-anim-config.js';

/**
 * Arms a layout animation for the next commit. Call right before the setState that changes layout.
 * onAnimationDidEnd is approximated with a timer (the engine config carries no completion hook).
 */
export function configureNext(config, onAnimationDidEnd) {
  NativeUI.configureNextLayoutAnimation(toEngineConfig(config));
  if (typeof onAnimationDidEnd === 'function') {
    const ms = config && typeof config.duration === 'number' ? config.duration : 300;
    setTimeout(onAnimationDidEnd, ms);
  }
}

export const LayoutAnimation = {
  configureNext,
  create,
  Types,
  Properties,
  Presets,
  easeInEaseOut: (onDone) => configureNext(Presets.easeInEaseOut, onDone),
  linear: (onDone) => configureNext(Presets.linear, onDone),
  spring: (onDone) => configureNext(Presets.spring, onDone),
};
