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

// Runtime e2e: a `backgroundImage` style through the real bridge and engine, read back as pixels.
import {createRoot} from '../../src/renderer.js';
import {View} from 'embedded-react';
import {check, report} from './harness.js';

declare function __pixel(x: number, y: number): number;
declare const screen: {width: number; height: number};

const WIDTH = 200;
const HEIGHT = 80;

const red = (pixel: number): number => (pixel >> 16) & 0xff;
const blue = (pixel: number): number => pixel & 0xff;

/** Whether two pixels are the same color, give or take a dither step. */
const close = (first: number, second: number): boolean =>
  Math.abs(red(first) - red(second)) <= 2 &&
  Math.abs(blue(first) - blue(second)) <= 2;

const root = createRoot({width: screen.width, height: screen.height});
const show = (backgroundImage: unknown): void => {
  root.render(
    <View style={{width: 480, height: 320, backgroundColor: '#000000'}}>
      <View
        style={{
          width: WIDTH,
          height: HEIGHT,
          backgroundColor: '#00ff00',
          backgroundImage: backgroundImage as string,
        }}
      />
    </View>,
  );
};

const topLeft = (): number => __pixel(0, 0);
const topRight = (): number => __pixel(WIDTH - 1, 0);
const bottomLeft = (): number => __pixel(0, HEIGHT - 1);
const bottomRight = (): number => __pixel(WIDTH - 1, HEIGHT - 1);

show('linear-gradient(to right, #ff0000, #0000ff)');
check(topLeft() === 0xffff0000, 'to right starts on the first stop');
check(topRight() === 0xff0000ff, 'to right ends on the last stop');
check(bottomLeft() === 0xffff0000, 'to right is the same down a column');

show('linear-gradient(#ff0000, #0000ff)');
check(
  topLeft() === 0xffff0000 && topRight() === 0xffff0000,
  'the default direction starts at the top',
);
check(bottomLeft() === 0xff0000ff, 'the default direction ends at the bottom');

show('linear-gradient(0deg, #ff0000, #0000ff)');
check(bottomLeft() === 0xffff0000, '0deg starts at the bottom');
check(topLeft() === 0xff0000ff, '0deg ends at the top');

show('linear-gradient(to bottom right, #ff0000, #0000ff)');
check(topLeft() === 0xffff0000, 'a corner starts opposite it');
check(bottomRight() === 0xff0000ff, 'a corner ends on it');
check(
  close(topRight(), bottomLeft()) && close(topRight(), 0xff7f007f),
  'the two other corners share the middle color',
);

show([
  {
    type: 'linear-gradient',
    direction: 'to right',
    colorStops: [
      {color: '#ff0000', positions: ['0%', '50%']},
      {color: '#0000ff'},
    ],
  },
]);
check(
  __pixel(WIDTH / 4, 10) === 0xffff0000,
  'the object form holds a color between its two positions',
);
check(topRight() === 0xff0000ff, 'the object form ends on the last stop');

show('radial-gradient(circle, #ff0000, #0000ff)');
check(
  close(__pixel(WIDTH / 2, HEIGHT / 2), 0xffff0000),
  'a radial gradient starts at the center',
);
check(topLeft() === 0xff0000ff, 'a radial gradient ends at the corners');

show('radial-gradient(#ff0000, #0000ff)');
check(
  topLeft() === 0xff00ff00 && __pixel(WIDTH / 2, HEIGHT / 2) === 0xff00ff00,
  'a gradient the engine does not draw leaves the backgroundColor',
);

show(undefined);
check(topLeft() === 0xff00ff00, 'removing the gradient paints the color');

report('background-image');
