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

// Runtime e2e: the RAW touch events carry the gesture. `onTouchMove`/`End`/`Cancel` report the travel
// since touch-down (dx/dy) and the speed the finger was last measured at (vx/vy, px per ms) — the same
// numbers the responder events carry, marshalled by the bridge onto the plain event object. That makes
// a flick `onTouchEnd={e => e.vx > 0.4 && next()}`, with no recogniser and no clock.
//
// Touches come from the runner's __touch(phase, x, y[, finger]): 0 down / 1 move / 2 up / 3 cancel.
import {createRoot} from '../../src/renderer.js';
import {View} from 'embedded-react';
import {NativeUI} from '../../src/native-ui.js';
import {check, report} from './harness.js';

const root = createRoot({width: screen.width, height: screen.height});

const log = [];
const rec = name => e =>
  log.push({name, x: e.x, dx: e.dx, dy: e.dy, vx: e.vx, vy: e.vy});

const tree = (
  <View
    style={{width: 480, height: 320}}
    onTouchStart={rec('start')}
    onTouchMove={rec('move')}
    onTouchEnd={rec('end')}
    onTouchCancel={rec('cancel')}>
    <View style={{width: 200, height: 200, backgroundColor: '#1a1a2e'}} />
  </View>
);

// ====================================================================================================
// 1. A flick: travel accumulates from touch-down, velocity is the last leg, and the lift keeps it
// ====================================================================================================
{
  root.render(tree);

  __touch(0, 40, 40);
  check(log.length === 1 && log[0].name === 'start', 'touch-down dispatches');
  check(
    log[0].dx === 0 && log[0].vx === 0,
    `touch-down opens at zero travel and zero speed (dx=${log[0].dx}, vx=${log[0].vx})`,
  );

  NativeUI.tick(20);
  __touch(1, 100, 70); // +60, +30 in 20 ms
  check(
    log[1].dx === 60 && log[1].dy === 30,
    `a raw move carries travel from touch-down (dx=${log[1].dx}, dy=${log[1].dy})`,
  );
  check(
    log[1].vx === 3 && log[1].vy === 1.5,
    `a raw move carries px/ms velocity (vx=${log[1].vx}, vy=${log[1].vy})`,
  );

  NativeUI.tick(20);
  __touch(1, 160, 70); // +60 more in 20 ms: travel accumulates, speed is the last leg only
  check(
    log[2].dx === 120 && log[2].vx === 3 && log[2].vy === 0,
    `travel accumulates while velocity measures the last move (dx=${log[2].dx}, vx=${log[2].vx})`,
  );

  NativeUI.tick(20); // a frame of rest before the finger lifts — a real fling always has one
  __touch(2, 160, 70);
  check(
    log[3].name === 'end' && log[3].dx === 120,
    `touch-end carries the gesture's total travel (dx=${log[3].dx})`,
  );
  check(
    log[3].vx === 3,
    `touch-end keeps the flick's speed instead of re-measuring across the rest (vx=${log[3].vx})`,
  );
}

// ====================================================================================================
// 2. A cancelled sequence reports the same fields (a panel driver dropping the touch, say)
// ====================================================================================================
{
  log.length = 0;
  root.render(tree);

  __touch(0, 100, 100);
  NativeUI.tick(20);
  __touch(1, 100, 60); // -40 in 20 ms
  __touch(3, 100, 60);
  check(
    log[2].name === 'cancel' && log[2].dy === -40 && log[2].vy === -2,
    `touch-cancel carries travel and speed (dy=${log[2].dy}, vy=${log[2].vy})`,
  );
}

// ====================================================================================================
// 3. A new touch-down starts a fresh gesture — no travel or speed leaks across sequences
// ====================================================================================================
{
  log.length = 0;
  root.render(tree);

  __touch(0, 50, 50);
  NativeUI.tick(20);
  __touch(1, 150, 50);
  __touch(2, 150, 50);

  log.length = 0;
  __touch(0, 60, 60);
  check(
    log[0].dx === 0 && log[0].vx === 0 && log[0].vy === 0,
    `the next touch-down resets travel and speed (dx=${log[0].dx}, vx=${log[0].vx})`,
  );
  __touch(2, 60, 60);
}

report('touch-gesture');
