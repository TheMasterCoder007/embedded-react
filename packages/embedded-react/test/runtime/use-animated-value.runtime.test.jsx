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

// Runtime e2e: useAnimatedValue returns a render-stable Animated.Value backed by the engine's
// native driver, and frees the engine-side value slot when its component unmounts (the value pool is
// fixed-size, so the unmount cleanup is what keeps mounting/unmounting components from leaking slots).
import {createRoot} from '../../src/renderer.js';
import {Animated, Easing, useAnimatedValue, View} from 'embedded-react';
import {check, report} from './harness.js';

// Spy on the destroy bridge call so we can assert the unmount cleanup ran.
let destroyCount = 0;
const realDestroy = NativeUI.animValueDestroy;
NativeUI.animValueDestroy = handle => {
  destroyCount++;
  return realDestroy(handle);
};

let seen = []; // every value instance the component rendered with

function Comp() {
  const value = useAnimatedValue(0);
  seen.push(value);
  return <Animated.View style={{width: 10, height: 10, opacity: value}} />;
}

const root = createRoot({width: screen.width, height: screen.height});

root.render(<Comp />);
NativeUI.tick(0); // flush passive effects (the unmount-cleanup registration)
const value = seen[0];
check(
  value != null && value.__animated === true,
  'hook returns an Animated.Value',
);
check(
  value.__getValue() === 0,
  `value starts at its initial (${value.__getValue()})`,
);

// It drives the native driver like any Animated.Value.
Animated.timing(value, {
  toValue: 100,
  duration: 100,
  easing: Easing.linear,
}).start();
NativeUI.tick(60);
check(
  value.__getValue() > 0,
  `value animates over time (=${value.__getValue()})`,
);

// Re-render: the hook must hand back the SAME instance (no new slot per render).
root.render(<Comp />);
NativeUI.tick(0);
check(seen.length >= 2 && seen[1] === value, 'value is stable across renders');
check(destroyCount === 0, 'value is not destroyed while still mounted');

// Unmount: the cleanup frees the engine-side slot exactly once.
root.render(null);
NativeUI.tick(0);
check(
  destroyCount === 1,
  `value destroyed on unmount (destroys=${destroyCount})`,
);

NativeUI.animValueDestroy = realDestroy;
report('use-animated-value');
