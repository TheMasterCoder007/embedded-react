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

// Runtime e2e: an alert covers the app, takes its touches, and closes from its button; a cancelable
// one closes on a tap outside the sheet but not on the sheet.
//
// Touches come from the runner's __touch(phase, x, y): 0 down / 2 up.
import {Alert, AppRegistry, Pressable} from 'embedded-react';
import {check, report} from './harness.ts';

const tap = (x: number, y: number) => {
  __touch(0, x, y);
  __touch(2, x, y);
};

// Taps a grid over the screen until `done` says to stop. The sheet's layout is not readable from
// here, so this is how a test finds a button.
const tapUntil = (done: () => boolean) => {
  for (let y = 4; y < screen.height; y += 8) {
    for (let x = 4; x < screen.width; x += 8) {
      tap(x, y);
      if (done()) return;
    }
  }
};

let appPresses = 0;
const centerX = screen.width / 2;
const centerY = screen.height / 2;

// The app is one full-screen button, so any tap that gets past an alert is counted.
AppRegistry.registerComponent('alert-test', () => () => (
  <Pressable
    style={{width: screen.width, height: screen.height}}
    onPress={() => appPresses++}
  />
));

tap(centerX, centerY);
check(appPresses === 1, 'the app takes a tap with no alert up');

// ---- a plain alert ---------------------------------------------------------------------------
let okPresses = 0;
Alert.alert('Saved', 'Your settings were saved.', [
  {text: 'OK', onPress: () => okPresses++},
]);
NativeUI.tick(0); // the alert renders on the next pump

tap(2, 2);
tap(centerX, centerY);
check(
  appPresses === 1,
  'an alert keeps taps from the app, outside the sheet and on it',
);
check(okPresses === 0, 'a tap off the button does not press it');

tapUntil(() => okPresses > 0);
check(okPresses === 1, 'the button is on screen and takes a tap');

tap(centerX, centerY);
check(appPresses === 2, 'the app takes taps again once the alert closes');

// ---- a cancelable alert ----------------------------------------------------------------------
let dismissals = 0;
Alert.alert('Note', 'Tap outside to close.', [{text: 'OK'}], {
  cancelable: true,
  onDismiss: () => dismissals++,
});
NativeUI.tick(0);

tap(centerX, centerY);
check(
  dismissals === 0,
  'a tap on the sheet does not dismiss a cancelable alert',
);
check(appPresses === 2, 'and does not reach the app');

tap(2, 2);
check(dismissals === 1, 'a tap outside the sheet dismisses it');

tap(centerX, centerY);
check(appPresses === 3, 'the app takes taps again once it is dismissed');

report('alert');
