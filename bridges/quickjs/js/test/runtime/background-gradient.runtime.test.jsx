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

// Runtime e2e: a View's `backgroundGradient` style through the real bridge + engine.
//   • with ERUI_GRADIENT, a CSS angle / `to` side lands on the right edges (0deg points to the top),
//   • without it, the View paints its backgroundColor instead,
//   • dropping the style returns the View to its backgroundColor,
//   • malformed gradients (unknown type, too many stops, descending offsets) neither crash nor leak.
import {createRoot} from '../../src/renderer.js';
import {View} from 'embedded-react';
import {check, report} from './harness.js';

const root = createRoot({width: screen.width, height: screen.height});

const RED = 0xffff0000;
const BLUE = 0xff0000ff;
const GREEN = 0xff00ff00;
const STOPS = [{color: '#ff0000'}, {color: '#0000ff'}];

/** Renders one 100x100 box at the origin, green under its gradient. */
function paint(backgroundGradient) {
  root.render(
    <View
      style={{
        width: 100,
        height: 100,
        backgroundColor: '#00ff00',
        backgroundGradient,
      }}
    />,
  );
}

paint({type: 'linear', to: 'right', stops: STOPS});
const gradients = __pixel(0, 50) !== GREEN;
if (gradients) {
  check(
    __pixel(0, 50) === RED && __pixel(99, 50) === BLUE,
    "`to: 'right'` runs from the left edge to the right edge",
  );
  const mid = __pixel(50, 50);
  check(
    ((mid >> 16) & 0xff) > 0x60 && (mid & 0xff) > 0x60,
    'the middle of a red→blue ramp mixes both',
  );

  paint({type: 'linear', angle: 0, stops: STOPS});
  check(
    __pixel(50, 99) === RED && __pixel(50, 0) === BLUE,
    'angle 0 points to the top, as in CSS',
  );

  paint({
    type: 'linear',
    to: 'right',
    stops: [
      {color: '#ff0000', offset: 0.5},
      {color: '#0000ff', offset: 0.1},
    ],
  });
  check(
    __pixel(40, 50) === RED && __pixel(60, 50) === BLUE,
    'a descending offset is raised to the one before it (a hard stop at 50%)',
  );
} else {
  check(
    __pixel(50, 50) === GREEN,
    'without ERUI_GRADIENT the View paints its backgroundColor',
  );
}

paint(undefined);
check(
  __pixel(0, 50) === GREEN && __pixel(99, 50) === GREEN,
  'dropping backgroundGradient returns the View to its backgroundColor',
);

paint({type: 'conic', stops: STOPS});
check(__pixel(50, 50) === GREEN, 'an unsupported type is ignored');

paint({
  type: 'linear',
  stops: ['#100', '#200', '#300', '#400', '#500', '#600'].map(color => ({
    color,
  })),
});
check(true, 'more stops than the engine holds did not crash');

report('background-gradient');
