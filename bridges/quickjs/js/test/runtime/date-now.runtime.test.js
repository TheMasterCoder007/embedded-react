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

// Date.now() in the lite profile (no Date intrinsic): it runs on the engine clock, a host can anchor it
// to real time, and it and performance.now() keep counting past the engine clock's 32-bit wrap.
import {check, report} from './harness.js';

// --- Date.now exists and advances with the engine clock ----------------------------------------
check(
  typeof Date.now === 'function',
  'Date.now exists without the Date intrinsic',
);
const d0 = Date.now();
const p0 = performance.now();
NativeUI.tick(250);
check(
  Date.now() - d0 === 250,
  `Date.now advances with the engine clock (${Date.now() - d0})`,
);
check(performance.now() - p0 === 250, 'performance.now advances with it');
check(Number.isInteger(Date.now()), 'Date.now is whole milliseconds');
check(
  NativeUI.now() === performance.now(),
  'NativeUI.now and performance.now read the same clock',
);

// --- a callback that reads it runs to completion -----------------------------------------------
let stamped = null;
setTimeout(() => {
  stamped = Date.now();
}, 10);
NativeUI.tick(10);
check(stamped === Date.now(), 'Date.now works inside a timer callback');

// --- Date objects still need the intrinsic, and the error says so ------------------------------
// The result is used so the bundler cannot drop the call: esbuild treats `new Date()` as pure.
const thrownBy = make => {
  try {
    return `no throw (${typeof make()})`;
  } catch (e) {
    return String(e && e.message);
  }
};
const viaNew = thrownBy(() => new Date());
check(
  viaNew.includes('ER_JS_INTRINSIC_DATE'),
  `new Date() names the intrinsic it needs (${viaNew})`,
);
const viaCall = thrownBy(() => Date());
check(viaCall.includes('ER_JS_INTRINSIC_DATE'), `Date() does too (${viaCall})`);
check(!({} instanceof Date), 'instanceof Date is false, not a throw');

// --- the host anchors it to wall-clock time ----------------------------------------------------
const perfBefore = performance.now();
__setWallClock(1700000000000);
check(
  Date.now() === 1700000000000,
  `Date.now reads the time the host set (${Date.now()})`,
);
NativeUI.tick(1000);
check(Date.now() === 1700000001000, 'and counts on from it');
check(
  performance.now() - perfBefore === 1000,
  'setting the wall clock does not move performance.now',
);
let rejected = false;
try {
  __setWallClock(Symbol('not a time'));
} catch (e) {
  rejected = e instanceof TypeError;
}
check(rejected, 'a value that cannot become a number throws');
check(Date.now() === 1700000001000, 'and leaves the wall clock alone');

// --- both keep counting past the engine clock's 32-bit wrap (~49.7 days) -----------------------
const dw = Date.now();
const pw = performance.now();
for (let i = 0; i < 5; i++) NativeUI.tick(1000000000); // 5e9 ms in total, past 2^32
check(
  performance.now() - pw === 5e9,
  `performance.now does not wrap (${performance.now() - pw})`,
);
check(Date.now() - dw === 5e9, `Date.now does not wrap (${Date.now() - dw})`);

report('date-now');
