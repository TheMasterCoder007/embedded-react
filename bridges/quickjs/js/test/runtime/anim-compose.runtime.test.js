// Runtime e2e for Animated composition + completion callbacks. Each child animation runs in C
// (native driver); the composition is JS over start()/stop() + the §2 timer globals. NativeUI.tick
// advances the engine clock and pumps, firing completion callbacks deterministically.
import { Animated, Easing } from 'embedded-react';
import { check, report } from './harness.js';

const lin = { duration: 100, easing: Easing.linear };

// --- .start(callback) fires with { finished: true } on natural completion --------------------
{
  const v = new Animated.Value(0);
  let result = null;
  Animated.timing(v, { toValue: 100, ...lin }).start((r) => (result = r));
  NativeUI.tick(50);
  check(result === null, 'completion callback has not fired mid-animation');
  NativeUI.tick(60); // past the end
  check(result && result.finished === true, 'completion fires { finished: true } at the end');
  check(Math.abs(v.__getValue() - 100) < 0.5, 'value reached toValue');
}

// --- interrupting an animation fires the old callback with { finished: false } ----------------
{
  const v = new Animated.Value(0);
  let first = null;
  Animated.timing(v, { toValue: 100, ...lin }).start((r) => (first = r));
  NativeUI.tick(20);
  Animated.timing(v, { toValue: 0, ...lin }).start(); // supersedes the first
  check(first && first.finished === false, 'interrupted animation reports { finished: false }');
}

// --- sequence: animations run in order, callback fires once at the end ------------------------
{
  const v = new Animated.Value(0);
  let done = null;
  Animated.sequence([
    Animated.timing(v, { toValue: 50, ...lin }),
    Animated.timing(v, { toValue: 200, ...lin }),
  ]).start((r) => (done = r));

  NativeUI.tick(110); // first leg completes → second starts
  check(done === null, 'sequence not done after first leg');
  const mid = v.__getValue();
  check(mid >= 50 && mid <= 200, `sequence advanced into second leg (v=${mid})`);

  NativeUI.tick(110); // second leg completes
  check(done && done.finished === true, 'sequence completes after the last entry');
  check(Math.abs(v.__getValue() - 200) < 0.5, 'sequence ended at the final toValue');
}

// --- parallel: both run at once, callback fires when all finish -------------------------------
{
  const a = new Animated.Value(0);
  const b = new Animated.Value(0);
  let done = null;
  Animated.parallel([
    Animated.timing(a, { toValue: 100, ...lin }),
    Animated.timing(b, { toValue: 100, duration: 200, easing: Easing.linear }),
  ]).start((r) => (done = r));

  NativeUI.tick(110); // a done, b still going
  check(done === null, 'parallel waits for the slowest child');
  check(Math.abs(a.__getValue() - 100) < 0.5, 'fast child (a) finished');
  const bMid = b.__getValue();
  check(bMid > 40 && bMid < 90, `slow child (b) still animating (b=${bMid})`);

  NativeUI.tick(110); // b done
  check(done && done.finished === true, 'parallel completes when all children finish');
}

// --- delay: a pure spacer driven by setTimeout -----------------------------------------------
{
  let done = null;
  Animated.delay(80).start((r) => (done = r));
  NativeUI.tick(50);
  check(done === null, 'delay has not elapsed yet');
  NativeUI.tick(40);
  check(done && done.finished === true, 'delay completes after its time');
}

// --- loop: repeats a fixed number of iterations, resetting the value each time ----------------
{
  const v = new Animated.Value(0);
  let done = null;
  Animated.loop(Animated.timing(v, { toValue: 100, ...lin }), { iterations: 3 }).start((r) => (done = r));

  // Three 100ms legs; step well past each. Reset-before-iteration means each leg goes 0→100.
  NativeUI.tick(110);
  check(done === null, 'loop still running after iteration 1');
  NativeUI.tick(110);
  check(done === null, 'loop still running after iteration 2');
  NativeUI.tick(110);
  check(done && done.finished === true, 'loop completes after the configured iterations');
}

// --- stop() halts a sequence and reports not-finished ----------------------------------------
{
  const v = new Animated.Value(0);
  let done = null;
  const seq = Animated.sequence([
    Animated.timing(v, { toValue: 100, duration: 200, easing: Easing.linear }),
    Animated.timing(v, { toValue: 200, ...lin }),
  ]);
  seq.start((r) => (done = r));
  NativeUI.tick(50);
  seq.stop();
  check(done && done.finished === false, 'stopped sequence reports { finished: false }');
}

report('anim-compose');
