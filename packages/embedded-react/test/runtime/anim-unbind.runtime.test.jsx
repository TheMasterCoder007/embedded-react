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

// Runtime e2e: an animated binding must be RELEASED when a prop stops being animated. The engine owns
// a bound prop until told otherwise — it re-pushes the animated value over the static props of every
// later commit — so a leaked binding silently overrules the value React just set, and a prop that
// swapped Animated.Values ends up written by both.
//
// The node's position is the probe: translateX moves the node's hit box, so where a touch lands says
// which value (if any) is driving the prop, through the real engine rather than a recorded call.
import {createRoot} from '../../src/renderer.js';
import {View, Dial, Animated} from 'embedded-react';
import {check, report} from './harness.js';

// Bridge calls the reconciler makes, still forwarded to the engine.
const unbinds = [];
const realUnbind = NativeUI.animUnbind;
NativeUI.animUnbind = (handle, prop) => {
  unbinds.push(prop);
  realUnbind.call(NativeUI, handle, prop);
};
const unbound = prop => unbinds.filter(p => p === prop).length;

const presses = [];
const x = new Animated.Value(0);
const y = new Animated.Value(0);

// Animated.View, the wrapper the bug was reported against: its style is bound by the host config like
// any other element's. The box is 40 wide at left 0, so translateX t makes it hittable over [t, t+40).
function App({tx}) {
  return (
    <View style={{width: 300, height: 200}}>
      <Animated.View
        style={{
          position: 'absolute',
          left: 0,
          top: 0,
          width: 40,
          height: 40,
          transform: [{translateX: tx}],
        }}
        onPress={() => presses.push(1)}
      />
    </View>
  );
}

/** Taps at (px, py) and reports whether the animated node was the one pressed. */
function pressAt(px, py) {
  const before = presses.length;
  __touch(0, px, py);
  __touch(2, px, py);
  return presses.length > before;
}

const root = createRoot({width: screen.width, height: screen.height});

root.render(<App tx={x} />);
x.setValue(100);
check(pressAt(120, 20), 'the node follows its bound value');
check(!pressAt(20, 20), 'and has left its unshifted layout box');

// --- animated → static ---------------------------------------------------------------------------
root.render(<App tx={0} />);
check(
  unbound('translateX') === 1,
  'the released prop was unbound at the bridge',
);
check(
  pressAt(20, 20),
  'a prop that stopped being animated takes its static value',
);
check(!pressAt(120, 20), 'and no longer sits where the animation left it');
x.setValue(200);
check(pressAt(20, 20), 'the released value no longer writes the prop');

// --- swapping to a different Animated.Value -------------------------------------------------------
root.render(<App tx={y} />);
y.setValue(60);
check(pressAt(80, 20), 'the prop follows the new value');
x.setValue(250);
check(pressAt(80, 20), 'and not the one it was bound to before');
check(unbound('translateX') === 1, 're-binding a free prop needs no unbind');

// A re-render that changes nothing about the binding must leave it alone (an unbind/re-bind per commit
// would be pure bridge traffic on the hot path).
const before = unbound('translateX');
root.render(<App tx={y} />);
check(unbound('translateX') === before, 'an unchanged binding is not churned');

// --- <Dial value>, the same path through a native widget prop -------------------------------------
// The dial's value drives the indicator sweep, which no touch can read back, so this half pins the
// bridge call: ER_PROP_ARC_VALUE is released exactly when the prop stops being animated.
const level = new Animated.Value(0);
const dial = props => (
  <View style={{width: 300, height: 200}}>
    <Dial style={{width: 120, height: 120}} max={100} {...props} />
  </View>
);

root.render(dial({value: level}));
root.render(dial({value: level, indicatorColor: '#ff0000'}));
check(
  unbound('value') === 0,
  '<Dial value>: an unchanged animated value stays bound',
);

root.render(dial({value: 42}));
check(
  unbound('value') === 1,
  '<Dial value>: switching to a number released the binding',
);

report('anim-unbind');
