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

// A NaN from app math (a 0/0, say) fails every comparison, so a clamp alone passes it on to a float-to-int
// cast, which C leaves undefined. A NaN opacity is fully transparent (as Flow B makes it) and a NaN
// transform component is ignored. Built with -fsanitize=float-cast-overflow, the runner also catches a cast
// that still sees one.
import {createRoot} from '../../src/renderer.js';
import {Animated, useAnimatedValue, View} from 'embedded-react';
import {check, report} from './harness.js';

const root = createRoot({width: screen.width, height: screen.height});
const RED = 0xffff0000;
// An opaque backdrop, so a box that stops painting shows it instead of the last frame's pixels.
const BLUE = 0xff0000ff;
const BACKDROP = {width: 200, height: 200, backgroundColor: '#0000ff'};
const BOX = {width: 100, height: 100, backgroundColor: '#ff0000'};

/** Renders a red 100x100 box on the backdrop and returns the pixel at the box's centre. */
function paint(style) {
  root.render(
    <View style={BACKDROP}>
      <View style={{...BOX, ...style}} />
    </View>,
  );
  return __pixel(50, 50);
}

check(paint({opacity: 1}) === RED, 'an opacity of 1 paints the box');
check(paint({opacity: 0}) === BLUE, 'an opacity of 0 paints nothing');
check(paint({opacity: NaN}) === BLUE, 'a NaN opacity is fully transparent');
check(paint({opacity: -1}) === BLUE, 'a negative opacity is 0');
check(paint({opacity: 2}) === RED, 'an opacity past 1 is 1');

check(
  paint({transform: [{translateX: NaN}]}) === RED,
  'a NaN translateX leaves the box where layout put it',
);
check(
  paint({transform: [{translateX: 1e10}]}) === BLUE,
  'a translateX past the int range moves the box off screen',
);

// The native driver takes the same value through its own path to the opacity byte.
let v = null;
function Fading() {
  v = useAnimatedValue(1);
  return (
    <View style={BACKDROP}>
      <Animated.View style={{...BOX, opacity: v}} />
    </View>
  );
}
root.render(<Fading />);
NativeUI.tick(0); // flush passive effects
check(__pixel(50, 50) === RED, 'an animated opacity of 1 paints the box');
v.setValue(NaN);
NativeUI.commit(); // tick() only advances the engine; a frame is painted on commit
check(
  __pixel(50, 50) === BLUE,
  'an animated opacity set to NaN is fully transparent',
);

report('nan-style');
