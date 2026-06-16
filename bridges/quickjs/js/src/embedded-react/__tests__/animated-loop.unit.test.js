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

import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';

// Stub the C bridge so animations complete SYNCHRONOUSLY — i.e., NativeUI.animValueAnimate calls its
// completion callback before returning. This is what the engine effectively does on a large catch-up
// frame in the WASM simulator (a long-duration timing finishes inside a single pump). A robust
// Animated.loop must not turn that into unbounded synchronous recursion (stack overflow).
function installSyncNativeUI() {
  let h = 0;
  globalThis.NativeUI = {
    animValueCreate: () => ++h,
    animValueSet: () => {},
    animValueGet: () => 1,
    animValueDestroy: () => {},
    animValueBind: () => {},
    animStop: () => {},
    animValueAnimate: (_handle, _to, _cfg, cb) => {
      if (cb) cb(true); // complete synchronously
      return ++h;
    },
  };
}

describe('Animated.loop with synchronous completion', () => {
  beforeEach(() => {
    installSyncNativeUI();
    vi.resetModules();
    vi.useFakeTimers();
  });
  afterEach(() => {
    vi.useRealTimers();
    delete globalThis.NativeUI;
  });

  it('does not overflow the stack on loop(sequence([...])) when completions fire synchronously', async () => {
    const { loop, sequence, timing, AnimatedValue } = await import('../Animated.js');
    const v = new AnimatedValue(1);
    const anim = loop(
      sequence([
        timing(v, { toValue: 1.12, duration: 800 }),
        timing(v, { toValue: 1.0, duration: 800 }),
      ]),
    ); // infinite loop — the scaffolder's pulsing-logo pattern
    // start() must return (scheduling the next iteration asynchronously); a recursive loop throws
    // RangeError: Maximum call stack size exceeded here.
    expect(() => anim.start()).not.toThrow();
    anim.stop();
  });
});
