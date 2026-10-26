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

// Runtime e2e: <Dial> — the native arc widget — through the real bridge + engine.
//   • mounts as ER_NODE_ARC and lays out at its style size (onLayout),
//   • a native drag (touch injected by the runner's __touch) moves the value and delivers it to
//     onChange as a NUMBER (ER_EVENT_VALUE_CHANGE → the bridge's value trampoline),
//   • a <Switch> toggle delivers onValueChange as a BOOLEAN through the same event,
//   • an Animated.Value as `value` binds natively (no crash, no prop marshalling of the object),
//   • re-renders with new props (colour / segments / gradient) don't crash.
import {createRoot} from '../../src/renderer.js';
import {View, Dial, Switch, Animated, Easing} from 'embedded-react';
import {check, report} from './harness.js';

const layouts = {};
const changes = [];
const toggles = [];

function App({value, hot}) {
  return (
    <View style={{width: 480, height: 320}}>
      <Dial
        style={{width: 200, height: 200}}
        value={value}
        min={0}
        max={100}
        step={5}
        thickness={16}
        cap="round"
        knob="circle"
        adjustable
        indicatorColor={hot ? '#ff0000' : '#0a84ff'}
        onLayout={e => (layouts.dial = e.layout)}
        onChange={v => changes.push(v)}
      />
      <Switch
        style={{
          position: 'absolute',
          left: 300,
          top: 20,
          width: 51,
          height: 31,
        }}
        value={false}
        onValueChange={on => toggles.push(on)}
      />
    </View>
  );
}

const root = createRoot({width: screen.width, height: screen.height});
root.render(<App value={25} hot={false} />);
check(layouts.dial != null, 'Dial rendered (onLayout fired)');
check(
  layouts.dial && layouts.dial.width === 200 && layouts.dial.height === 200,
  'Dial laid out at its style size (200x200)',
);

// Touch the top of the ring: centre (100,100), mid radius 92 → (100, 8). Default sweep 135°→405°, so the
// top (270°) is 50%.
__touch(0, 100, 8);
check(
  changes.length === 1,
  `touch-down on the ring fires onChange once (${changes.length})`,
);
check(
  changes[0] === 50,
  `onChange receives the quantized value as a number (${changes[0]})`,
);
check(typeof changes[0] === 'number', 'value is delivered as a JS number');
// Move to 315° → 66.7% → step 5 → 65.
__touch(1, 165, 35);
check(
  changes[changes.length - 1] === 65,
  `move tracks the finger (${changes[changes.length - 1]})`,
);
// A React re-render mid-drag (the readout updating) must not snap the value back.
root.render(<App value={25} hot={true} />);
__touch(1, 166, 36); // tiny move: same quantized value → no new event
const n = changes.length;
__touch(2, 166, 36);
check(changes.length === n, 'release does not emit a spurious change');

// Tap in the hole: nothing (the centre is transparent to the dial).
__touch(0, 100, 100);
__touch(2, 100, 100);
check(changes.length === n, 'tap in the hole does not change the value');

// Switch: tap toggles, onValueChange gets a boolean (the bridge maps it to ER_EVENT_VALUE_CHANGE).
__touch(0, 325, 35);
__touch(2, 325, 35);
check(
  toggles.length === 1,
  `Switch tap fires onValueChange once (${toggles.length})`,
);
check(
  toggles[0] === true,
  `Switch delivers the new value as a boolean (${toggles[0]})`,
);

// Animated value bound to the dial's value (native driver): must bind without marshalling the object.
const level = new Animated.Value(0);
root.render(
  <View style={{width: 480, height: 320}}>
    <Dial
      style={{width: 120, height: 120}}
      value={level}
      max={100}
      segments={8}
      gapAngle={3}
      indicatorGradient={{
        type: 'conic',
        stops: [{color: '#0000ff'}, {color: '#ff0000'}],
      }}
    />
  </View>,
);
Animated.timing(level, {
  toValue: 100,
  duration: 100,
  easing: Easing.linear,
}).start();
NativeUI.tick(50);
NativeUI.tick(60);
check(
  Math.abs(level.__getValue() - 100) < 0.5,
  'animated dial value ramps natively',
);
root.render(
  <View style={{width: 480, height: 320}}>
    <Dial
      style={{width: 120, height: 120}}
      value={level}
      max={100}
      indicatorGradient={{
        type: 'radial',
        stops: [{color: '#00ff00'}, {color: '#ff00ff'}],
      }}
      knob="image"
      knobImage="missing-asset"
    />
  </View>,
);
check(true, 're-render with a radial gradient + image knob did not crash');

report('dial');
