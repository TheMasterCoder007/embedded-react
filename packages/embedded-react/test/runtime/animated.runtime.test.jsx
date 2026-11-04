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

// Runtime e2e: an Animated.Value driven by Animated.timing must advance in the engine as the host
// ticks — no per-frame JS. We start a linear 0→100 over 100ms and step the clock via NativeUI.tick.
import {Animated, Easing} from 'embedded-react';
import {check, report} from './harness.js';

const value = new Animated.Value(0);
check(value.__getValue() === 0, 'value starts at its initial (0)');

value.setValue(25);
check(value.__getValue() === 25, 'setValue updates the engine value');
value.setValue(0);

Animated.timing(value, {
  toValue: 100,
  duration: 100,
  easing: Easing.linear,
}).start();

NativeUI.tick(50); // halfway
const mid = value.__getValue();
check(mid > 5 && mid < 95, `value animates over time (mid=${mid})`);

NativeUI.tick(60); // past the end (110ms total)
const end = value.__getValue();
check(Math.abs(end - 100) < 0.5, `value reaches toValue (end=${end})`);

report('animated');
