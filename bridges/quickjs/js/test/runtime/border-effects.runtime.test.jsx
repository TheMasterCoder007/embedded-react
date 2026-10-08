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

// Runtime e2e: the border ring effects of a View through the real bridge + engine.
//   • borderSweep* draws a light along the inside of the rounded edge, over the content, and
//     borderSweepPhase moves it on the native driver,
//   • borderGradient {type: 'conic'} shows a conic gradient through the ring, and
//     borderGradientAngle turns it on the native driver,
//   • borderGradient {type: 'radial', size} moves a radial gradient across the ring with the angle,
//   • dropping the style removes the ring.
import {createRoot} from '../../src/renderer.js';
import {Animated, Easing, View} from 'embedded-react';
import {check, report} from './harness.js';

const root = createRoot({width: screen.width, height: screen.height});

const W = 100;
const H = 60;
const BLACK = 0xff000000;

/** Sum of the red channel along row y (inside the box), a measure of how much ring light it holds. */
function rowLight(y) {
  let sum = 0;
  for (let x = 0; x < W; x++) sum += (__pixel(x, y) >> 16) & 0xff;
  return sum;
}

/** The box under test, on an opaque backdrop so a repaint clears what it drew outside its corners. */
function box(extra) {
  return (
    <View style={{width: W + 20, height: H + 20, backgroundColor: '#000000'}}>
      <View
        style={{
          width: W,
          height: H,
          backgroundColor: '#000000',
          borderRadius: 8,
          ...extra,
        }}
      />
    </View>
  );
}

const SWEEP = {
  borderSweepColor: '#ffffff',
  borderSweepWidth: 4,
  borderSweepLength: 0.3,
};

root.render(box({}));
check(rowLight(1) === 0, 'no ring without borderSweepWidth');

const phase = new Animated.Value(0.15);
root.render(box({...SWEEP, borderSweepPhase: phase}));
check(
  __pixel(W / 2, H / 2) === BLACK,
  'the ring leaves the content inside it alone',
);
check(rowLight(1) > 0, 'a head on the top edge lights the top of the ring');
check(
  rowLight(1) > rowLight(H - 2),
  'the ring is lit near its head, not all around',
);

Animated.timing(phase, {
  toValue: 0.65,
  duration: 32,
  easing: Easing.linear,
  useNativeDriver: true,
}).start();
NativeUI.tick(48);
NativeUI.commit();
check(
  rowLight(H - 2) > rowLight(1),
  'animating borderSweepPhase to 0.65 moves the light to the bottom edge',
);

root.render(box({}));
check(
  rowLight(1) === 0 && rowLight(H - 2) === 0,
  'dropping the sweep removes the ring',
);

/** Red channel at (x, y). */
const red = (x, y) => (__pixel(x, y) >> 16) & 0xff;

// Clear, white at a quarter turn, clear again: from 0 (up) the white points right.
const CONIC = {
  type: 'conic',
  width: 4,
  stops: [
    {color: '#ffffff00', offset: 0},
    {color: '#ffffff', offset: 0.25},
    {color: '#ffffff00', offset: 0.5},
  ],
};
const turn = new Animated.Value(0);
root.render(box({borderGradient: CONIC, borderGradientAngle: turn}));
check(
  red(W - 2, H / 2) > 0xc0 && red(1, H / 2) === 0,
  'a conic ring is lit where its stops point (the right edge)',
);
check(
  __pixel(W / 2, H / 2) === BLACK,
  'the conic ring leaves the content alone',
);
turn.setValue(180);
NativeUI.commit();
check(
  red(1, H / 2) > 0xc0 && red(W - 2, H / 2) === 0,
  'borderGradientAngle 180 turns the light to the left edge',
);

// White at the centre of a background 3x the box, clear at its farthest corner. At angle 0 the
// background sits at 0% 0%, so its centre lies past the bottom-right corner; at 180, past the top-left.
const shine = new Animated.Value(0);
root.render(
  box({
    borderGradient: {
      type: 'radial',
      width: 4,
      size: 3,
      stops: [{color: '#ffffff'}, {color: '#ffffff00'}],
    },
    borderGradientAngle: shine,
  }),
);
const bottomRight = () => red(W - 12, H - 2);
const topLeft = () => red(12, 1);
check(
  bottomRight() > topLeft() + 0x40,
  'a radial ring at angle 0 is lit at the bottom right',
);
shine.setValue(180);
NativeUI.commit();
check(
  topLeft() > bottomRight() + 0x40,
  'at angle 180 the radial light has moved to the top left',
);

root.render(box({}));
check(red(1, H / 2) === 0, 'dropping borderGradient removes the ring');

report('border-effects');
