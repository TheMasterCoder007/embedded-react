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

// An animated color reaches the node only through an interpolation onto colors, and animated
// opacity fades a Text as it does a View. Both are read back from the framebuffer.
import {createRef} from 'react';
import {createRoot} from '../../src/renderer.js';
import {Animated, Text, View} from 'embedded-react';
import {check, report} from './harness.js';

const root = createRoot({width: screen.width, height: screen.height});
const BLUE = 0xff0000ff;
const BACKDROP = {width: 200, height: 200, backgroundColor: '#0000ff'};

const warnings = [];
console.warn = msg => warnings.push(String(msg));

/** True when any pixel of the 200x40 strip at the top of the backdrop is not the backdrop. */
function inked() {
  for (let y = 0; y < 40; y++) {
    for (let x = 0; x < 200; x++) {
      if (__pixel(x, y) !== BLUE) return true;
    }
  }
  return false;
}

// --- backgroundColor through a color interpolation ------------------------------------------------
const c = new Animated.Value(0);
root.render(
  <View style={BACKDROP}>
    <Animated.View
      style={{
        width: 100,
        height: 100,
        backgroundColor: c.interpolate({
          inputRange: [0, 1],
          outputRange: ['#ff0000', 'rgb(0, 255, 0)'],
        }),
      }}
    />
  </View>,
);
check(__pixel(50, 50) === 0xffff0000, 'the color binds at its current value');
c.setValue(1);
NativeUI.commit();
check(__pixel(50, 50) === 0xff00ff00, 'and follows the value to the end color');
c.setValue(0.5);
NativeUI.commit();
check(__pixel(50, 50) === 0xff808000, 'with each channel interpolated between');

// --- a plain value on a color prop is refused, with a warning -------------------------------------
const raw = new Animated.Value(1);
root.render(
  <View style={BACKDROP}>
    <Animated.View style={{width: 100, height: 100, backgroundColor: raw}} />
  </View>,
);
check(__pixel(50, 50) === BLUE, 'a plain value does not write a color');
check(
  warnings.some(w => w.includes('"backgroundColor" needs colors')),
  'and says how to animate one',
);

// --- colors on a numeric prop are refused, with a warning ---------------------------------------
const fade = new Animated.Value(1);
root.render(
  <View style={BACKDROP}>
    <Animated.View
      style={{
        width: 100,
        height: 100,
        backgroundColor: '#ff0000',
        opacity: fade.interpolate({
          inputRange: [0, 1],
          outputRange: ['#000', '#fff'],
        }),
      }}
    />
  </View>,
);
check(__pixel(50, 50) === 0xffff0000, 'colors do not write an opacity');
check(
  warnings.some(w => w.includes('"opacity" takes an outputRange of numbers')),
  'and the warning names the prop',
);

// The bridge refuses them too, for a caller that skips the JS check: read as numbers they would be NaN,
// a fully transparent opacity.
const box = createRef();
root.render(
  <View style={BACKDROP}>
    <View
      ref={box}
      style={{width: 100, height: 100, backgroundColor: '#ff0000'}}
    />
  </View>,
);
NativeUI.animValueBindInterpolated(fade._handle, box.current, 'opacity', {
  inputRange: [0, 1],
  outputRange: ['#000', '#fff'],
});
NativeUI.commit();
check(
  __pixel(50, 50) === 0xffff0000,
  'the bridge does not bind colors to an opacity',
);

// --- opacity on a Text ----------------------------------------------------------------------------
const o = new Animated.Value(0);
root.render(
  <View style={BACKDROP}>
    <Animated.Text style={{color: '#ffffff', fontSize: 20, opacity: o}}>
      MMMM
    </Animated.Text>
  </View>,
);
check(!inked(), 'a Text at an animated opacity of 0 paints nothing');
o.setValue(1);
NativeUI.commit();
check(inked(), 'and paints once the value reaches 1');

root.render(
  <View style={BACKDROP}>
    <Text style={{color: '#ffffff', fontSize: 20, opacity: 0}}>MMMM</Text>
  </View>,
);
check(!inked(), 'a static opacity of 0 hides a Text too');

report('animated-color-opacity');
