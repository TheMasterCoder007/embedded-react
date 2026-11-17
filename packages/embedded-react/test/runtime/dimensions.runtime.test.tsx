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

// Runtime e2e: the host changes the screen size while the app runs. The root takes the new size,
// Dimensions fires 'change', useWindowDimensions renders again, and state is kept.
//
// The size comes from the runner's __setScreen(width, height), which stands in for
// er_runtime_set_screen_size().
import {useState} from 'react';
import {
  AppRegistry,
  Dimensions,
  Pressable,
  View,
  useWindowDimensions,
} from 'embedded-react';
import type {LayoutRectangle} from 'embedded-react';
import {check, report} from './harness.ts';

const tap = (x: number, y: number) => {
  __touch(0, x, y);
  __touch(2, x, y);
};

const startWidth = screen.width;
const startHeight = screen.height;

let presses = 0;
let renders = 0;
let box = null as LayoutRectangle | null;
let fill = null as LayoutRectangle | null;
const changes: string[] = [];

function App() {
  const {width, height} = useWindowDimensions();
  const [count, setCount] = useState(0);
  presses = count;
  renders++;
  return (
    <View
      style={{flex: 1}}
      onLayout={event => (fill = event.nativeEvent.layout)}>
      <Pressable
        style={{width, height: height / 2}}
        onLayout={event => (box = event.nativeEvent.layout)}
        onPress={() => setCount(count + 1)}
      />
    </View>
  );
}

AppRegistry.registerComponent('dimensions-test', () => App);
NativeUI.tick(0);

check(
  box?.width === startWidth && box.height === startHeight / 2,
  `the hook gives the panel's size (${JSON.stringify(box)})`,
);
tap(4, 4);
check(presses === 1, 'a tap changes state');

const subscription = Dimensions.addEventListener('change', ({window, screen}) =>
  changes.push(
    `${window.width}x${window.height} ${screen.width}x${screen.height}`,
  ),
);

// A quarter turn.
const rendersBefore = renders;
__setScreen(startHeight, startWidth);
NativeUI.tick(0);

check(
  changes.join() ===
    `${startHeight}x${startWidth} ${startHeight}x${startWidth}`,
  `'change' fires once with the new size (${changes.join()})`,
);
check(
  Dimensions.get('window').width === startHeight &&
    Dimensions.get('screen').height === startWidth,
  'Dimensions.get reads the new size',
);
check(renders === rendersBefore + 1, 'the hook renders the app again, once');
check(
  box?.width === startHeight && box.height === startWidth / 2,
  `the app lays out at the new size (${JSON.stringify(box)})`,
);
check(
  fill?.width === startHeight && fill.height === startWidth,
  `the root takes the new size (${JSON.stringify(fill)})`,
);
check(presses === 1, 'state is kept');
tap(4, 4);
check(presses === 2, 'the app still takes taps');

// A removed listener hears nothing; the hook and the root still follow.
subscription.remove();
__setScreen(startWidth, startHeight);
NativeUI.tick(0);
check(changes.length === 1, 'a removed listener is not called');
check(
  box?.width === startWidth && fill?.height === startHeight,
  'the app follows the size back',
);

let threw = false;
try {
  // @ts-expect-error -- 'change' is the only event
  Dimensions.addEventListener('resize', () => {});
} catch {
  threw = true;
}
check(threw, 'an unknown event name throws');

report('dimensions');
