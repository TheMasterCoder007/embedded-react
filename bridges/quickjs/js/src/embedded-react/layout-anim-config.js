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

// Pure LayoutAnimation config helpers — no NativeUI dependency, so they're unit-testable in plain
// Node (see __tests__/layout-animation.unit.test.js). LayoutAnimation.js wires configureNext to the
// bridge using these.

/** RN-compatible animation type tokens. */
export const Types = {
  spring: 'spring',
  linear: 'linear',
  easeInEaseOut: 'easeInEaseOut',
  easeIn: 'easeIn',
  easeOut: 'easeOut',
  keyboard: 'keyboard',
};

/** RN-compatible animated-property tokens (accepted for API parity; the engine tweens position). */
export const Properties = {
  opacity: 'opacity',
  scaleX: 'scaleX',
  scaleY: 'scaleY',
  scaleXY: 'scaleXY',
};

/** Maps an RN animation-type token to an engine easing token. */
function easingForType(type) {
  switch (type) {
    case Types.linear:
      return 'linear';
    case Types.easeIn:
      return 'easeIn';
    case Types.easeOut:
      return 'easeOut';
    case Types.easeInEaseOut:
    case Types.keyboard:
    default:
      return 'easeInOut';
  }
}

/**
 * Reduces an RN LayoutAnimation config ({ duration, create, update, delete }) to the engine's flat
 * config. The engine tweens the *position transition*, which is RN's `update`; create/delete opacity
 * are accepted for parity but not separately animated.
 */
export function toEngineConfig(config) {
  const c = config || {};
  const duration = typeof c.duration === 'number' ? c.duration : 300;
  const update = c.update || {};
  const type = update.type || c.type || Types.easeInEaseOut;
  if (type === Types.spring) {
    return {type: 'spring', duration};
  }
  return {type: 'timing', duration, easing: easingForType(type)};
}

/** Builds an RN-shaped config (duration + create/update/delete) from a type + creation property. */
export function create(
  duration,
  type = Types.easeInEaseOut,
  creationProp = Properties.opacity,
) {
  return {
    duration,
    create: {type, property: creationProp},
    update: {type},
    delete: {type, property: creationProp},
  };
}

/** RN-compatible preset configs. */
export const Presets = {
  easeInEaseOut: create(300, Types.easeInEaseOut, Properties.opacity),
  linear: create(500, Types.linear, Properties.opacity),
  spring: {
    duration: 700,
    create: {type: Types.linear, property: Properties.opacity},
    update: {type: Types.spring, springDamping: 0.4},
    delete: {type: Types.linear, property: Properties.opacity},
  },
};
