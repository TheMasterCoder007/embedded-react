// Runtime e2e for interpolate `extrapolate`. The interpolated output binds to a node visual prop
// (opacity), which isn't observable from JS — the extend/clamp/identity *math* is covered by the
// engine's test_interpolate.c. Here we exercise the bridge marshalling end-to-end: each mode (and a
// per-end override) must bind and drive past the input range without error. A fresh node per mode
// avoids the engine's duplicate-(node,prop) bind guard.
import { Animated } from 'embedded-react';
import { check, report } from './harness.js';

const v = new Animated.Value(0);

function driveMode(cfg, label) {
  const node = NativeUI.createNode('View');
  NativeUI.setProps(node, { width: 50, height: 50 });
  const interp = v.interpolate(cfg);
  interp.__bind(node, 'opacity'); // output applied in C; not readable from JS
  v.setValue(-50); // below inputRange[0]
  NativeUI.tick(1);
  v.setValue(150); // above the last breakpoint
  NativeUI.tick(1);
  check(true, `bound + driven past the range without error (${label})`);
}

const range = { inputRange: [0, 100], outputRange: [0, 1] };
driveMode({ ...range }, 'default (extend)');
driveMode({ ...range, extrapolate: 'clamp' }, 'extrapolate: clamp');
driveMode({ ...range, extrapolate: 'identity' }, 'extrapolate: identity');
driveMode({ ...range, extrapolateLeft: 'clamp', extrapolateRight: 'extend' }, 'per-end override');

// The raw value still tracks regardless of any interpolation bindings.
v.setValue(42);
check(Math.abs(v.__getValue() - 42) < 0.001, 'raw value still tracks alongside interpolation bindings');

report('interpolate-extrapolate');
