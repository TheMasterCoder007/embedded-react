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

// Easing tokens. The engine implements a fixed set of easing curves (ERAnimEasing), so unlike RN's
// composable Easing functions these are string tokens the bridge maps to the engine enum. `bezier`
// returns a control-point object the bridge reads as a custom cubic-bezier curve.
export const Easing = {
  linear: 'linear',
  ease: 'ease',
  easeIn: 'easeIn',
  easeOut: 'easeOut',
  easeInOut: 'easeInOut',
  quadIn: 'quadIn',
  quadOut: 'quadOut',
  quadInOut: 'quadInOut',
  cubicIn: 'cubicIn',
  cubicOut: 'cubicOut',
  cubicInOut: 'cubicInOut',
  bounceOut: 'bounceOut',
  elasticOut: 'elasticOut',

  /** Custom cubic-bezier curve via control points. */
  bezier(x1, y1, x2, y2) {
    return { x1, y1, x2, y2 };
  },
};
