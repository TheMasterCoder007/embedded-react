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

// Runtime e2e for §2: the host pump must drain Promise microtasks and fire setTimeout/setInterval
// callbacks off the engine clock. NativeUI.tick(ms) advances the clock and pumps, so this test is
// fully deterministic — each tick fires exactly the timers whose deadline has passed.
import {check, report} from './harness.js';

// --- Promises: a .then resolves only after a pump, not synchronously ---------------------------
let promiseFlag = false;
Promise.resolve().then(() => {
  promiseFlag = true;
});
check(promiseFlag === false, 'promise .then does not run synchronously');
NativeUI.tick(0); // a zero-delta tick still pumps the job queue
check(promiseFlag === true, 'promise .then runs after a pump');

// --- async/await resolves across a pump --------------------------------------------------------
let awaited = null;
(async () => {
  awaited = await Promise.resolve('hi');
})();
NativeUI.tick(0);
check(awaited === 'hi', 'async/await resolves after a pump');

// --- setTimeout fires only once its delay has elapsed ------------------------------------------
let fired = 0;
setTimeout(() => {
  fired++;
}, 50);
NativeUI.tick(30); // 30 < 50 — not yet
check(fired === 0, 'setTimeout has not fired before its delay (t=30 < 50)');
NativeUI.tick(40); // 70 >= 50 — fires
check(fired === 1, 'setTimeout fires once its delay elapses');
NativeUI.tick(100);
check(fired === 1, 'one-shot setTimeout fires exactly once');

// --- clearTimeout cancels a pending timer ------------------------------------------------------
let cancelled = 0;
const cid = setTimeout(() => {
  cancelled++;
}, 20);
clearTimeout(cid);
NativeUI.tick(50);
check(cancelled === 0, 'clearTimeout prevents the callback from firing');

// --- extra args are forwarded to the callback (web spec) ---------------------------------------
let argSum = 0;
setTimeout(
  (a, b) => {
    argSum = a + b;
  },
  10,
  3,
  4,
);
NativeUI.tick(15);
check(argSum === 7, 'setTimeout forwards extra args to the callback');

// --- setInterval repeats every period; clearInterval stops it ----------------------------------
let intervalTicks = 0;
const iid = setInterval(() => {
  intervalTicks++;
}, 25);
NativeUI.tick(25); // fire 1
NativeUI.tick(25); // fire 2
NativeUI.tick(25); // fire 3
check(
  intervalTicks === 3,
  `setInterval fires each period (ticks=${intervalTicks})`,
);
clearInterval(iid);
NativeUI.tick(100);
check(intervalTicks === 3, 'clearInterval stops further firing');

// --- a timer scheduled from inside a timer callback is honored ---------------------------------
let chained = 0;
setTimeout(() => {
  chained++;
  setTimeout(() => {
    chained++;
  }, 10);
}, 10);
NativeUI.tick(10); // outer fires, schedules inner
check(chained === 1, 'outer timer fired and scheduled an inner timer');
NativeUI.tick(10); // inner fires
check(chained === 2, 'timer scheduled from a callback fires on a later tick');

report('timers');
