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

// Runtime e2e for <TouchableOpacity>: the unit tests assert which bridge calls it makes, this proves
// the feedback actually happens — a real touch through the engine's hit test snaps the bound value to
// activeOpacity, and the lift ramps it back over real ticks with no JS in between.
//
// Touches come from the runner's __touch(phase, x, y[, finger]): 0 down / 1 move / 2 up / 3 cancel.
import {createRoot} from '../../src/renderer.js';
import {Text, TouchableOpacity, View} from 'embedded-react';
import {check, report} from './harness.js';

// Spy on the three bridge calls that carry the feedback: the value it creates, the node it binds that
// value to, and the Pressable handle that node ought to be.
let pressable = null;
let handle = null;
let bind = null;
const realCreateNode = NativeUI.createNode;
NativeUI.createNode = type => {
  const h = realCreateNode(type);
  if (type === 'Pressable') pressable = h;
  return h;
};
const realCreate = NativeUI.animValueCreate;
NativeUI.animValueCreate = initial => (handle = realCreate(initial));
const realBind = NativeUI.animValueBind;
NativeUI.animValueBind = (h, node, prop) => {
  bind = {h, node, prop};
  return realBind(h, node, prop);
};

/** The live opacity, straight out of the engine-side value. */
const opacity = () => NativeUI.animValueGet(handle);
/** Float32 round-trips through the engine, so compare with a tolerance. */
const near = (a, b) => Math.abs(a - b) < 0.01;

const root = createRoot({width: screen.width, height: screen.height});

let box = null;
const presses = [];
const App = ({disabled}) => (
  // alignSelf shrink-wraps the wrapper onto the touchable, so its rect IS the touchable's.
  <View>
    <View style={{alignSelf: 'flex-start'}} onLayout={e => (box = e.layout)}>
      <TouchableOpacity
        style={{padding: 8}}
        disabled={disabled}
        onPress={() => presses.push('press')}>
        <Text>Tap</Text>
      </TouchableOpacity>
    </View>
  </View>
);

root.render(<App disabled={false} />);
NativeUI.tick(0); // flush passive effects

const cx = () => box.x + box.width / 2;
const cy = () => box.y + box.height / 2;

// ====================================================================================================
// 1. One node, one binding — the dim is a property of the Pressable, not a wrapper around it
// ====================================================================================================
check(
  box != null && box.width > 0,
  `touchable laid out (${box && box.width}x${box && box.height})`,
);
check(handle != null, 'created an engine-side animated value');
check(
  bind != null && bind.h === handle && bind.node === pressable,
  'bound that value to the Pressable node itself',
);
check(
  bind != null && bind.prop === 'opacity',
  `bound to opacity (${bind && bind.prop})`,
);
check(near(opacity(), 1), `rests fully opaque (${opacity()})`);

// ====================================================================================================
// 2. A real touch dims it, and the lift ramps it back on the native driver
// ====================================================================================================
__touch(0, cx(), cy());
check(
  near(opacity(), 0.2),
  `press-in dims to activeOpacity with no ramp (${opacity()})`,
);

__touch(2, cx(), cy());
check(presses.length === 1, 'the press itself still fired');
check(opacity() < 0.5, `still dim the instant the finger lifts (${opacity()})`);
NativeUI.tick(120);
const mid = opacity();
check(mid > 0.2 && mid < 1, `fading back across ticks (${mid})`);
NativeUI.tick(200);
check(near(opacity(), 1), `back to fully opaque after 250 ms (${opacity()})`);

// ====================================================================================================
// 3. A touch that leaves the box undims it, and a canceled one does not strand it dim
// ====================================================================================================
__touch(0, cx(), cy());
check(near(opacity(), 0.2), 'dims again on the next press');
__touch(1, cx(), cy() + box.height * 4); // drag well clear of the touchable
NativeUI.tick(400);
check(near(opacity(), 1), `undims when the finger slides off (${opacity()})`);
__touch(2, cx(), cy() + box.height * 4);
check(presses.length === 1, 'no press fired for the touch that slid off');

__touch(0, cx(), cy());
check(near(opacity(), 0.2), 'dims for a touch that is about to be canceled');
__touch(3, cx(), cy()); // cancel: the host abandoning the sequence
NativeUI.tick(400);
check(
  near(opacity(), 1),
  `a canceled touch does not strand it dim (${opacity()})`,
);

// ====================================================================================================
// 4. disabled: no press, and no feedback either
// ====================================================================================================
root.render(<App disabled={true} />);
NativeUI.tick(0);
__touch(0, cx(), cy());
check(near(opacity(), 1), `disabled does not dim (${opacity()})`);
__touch(2, cx(), cy());
check(presses.length === 1, 'disabled did not fire onPress');

// Enabling it again brings both back.
root.render(<App disabled={false} />);
NativeUI.tick(0);
__touch(0, cx(), cy());
check(near(opacity(), 0.2), 'dims again once enabled');
__touch(2, cx(), cy());
check(presses.length === 2, 'presses again once enabled');
NativeUI.tick(400);

NativeUI.createNode = realCreateNode;
NativeUI.animValueCreate = realCreate;
NativeUI.animValueBind = realBind;
report('touchable-opacity');
