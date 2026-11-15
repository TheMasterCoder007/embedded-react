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

// Runtime e2e: Keyboard.dismiss() blurs the focused TextInput, and in a build with the on-screen
// keyboard the show and hide events fire once each as the focus comes and goes.
import {createRoot} from '../../src/renderer.js';
import {View, TextInput, Keyboard} from 'embedded-react';
import {check, report} from './harness.js';

declare const screen: {width: number; height: number};
declare function __touch(phase: number, x: number, y: number): void;

const log: string[] = [];

function tap(x: number, y: number) {
  __touch(0, x, y);
  __touch(2, x, y);
}

const root = createRoot({width: screen.width, height: screen.height});
const App = ({second}: {second: boolean}) => (
  <View style={{width: screen.width, height: screen.height}}>
    <TextInput
      style={{width: 100, height: 30}}
      onFocus={() => log.push('focus 1')}
      onBlur={() => {
        log.push('blur 1');
        // The input has already lost the focus, so this must not blur it again.
        Keyboard.dismiss();
      }}
    />
    {second && (
      <TextInput
        style={{width: 100, height: 30}}
        onFocus={() => log.push('focus 2')}
        onBlur={() => log.push('blur 2')}
      />
    )}
  </View>
);
root.render(<App second={true} />);

Keyboard.addListener('keyboardDidShow', event =>
  log.push(`show ${event.endCoordinates.height > 0}`),
);
const hide = Keyboard.addListener('keyboardDidHide', event =>
  log.push(`hide ${event.endCoordinates.screenY === screen.height}`),
);

check(!Keyboard.isVisible(), 'hidden with nothing focused');
check(Keyboard.metrics() === undefined, 'no metrics while hidden');
Keyboard.dismiss();
check(log.length === 0, 'dismissing with nothing focused does nothing');

// A build with the on-screen keyboard shows it on focus; one without has nothing to show.
tap(50, 15);
const hasKeyboard = Keyboard.isVisible();
console.log(`  on-screen keyboard: ${hasKeyboard ? 'on' : 'off'}`);
const shown = hasKeyboard ? ['show true'] : [];
const hidden = hasKeyboard ? ['hide true'] : [];
const expectLog = (expected: string[], message: string) => {
  check(
    JSON.stringify(log) === JSON.stringify(expected),
    `${message} (${log.join(', ')})`,
  );
  log.length = 0;
};

expectLog(['focus 1', ...shown], 'a tap focuses the input');
if (hasKeyboard) {
  const metrics = Keyboard.metrics();
  check(
    metrics?.screenX === 0 &&
      metrics.width === screen.width &&
      metrics.screenY + metrics.height === screen.height,
    `metrics are the strip at the bottom (${JSON.stringify(metrics)})`,
  );
}

tap(50, 45);
expectLog(['blur 1', 'focus 2'], 'moving the focus keeps the keyboard up');

Keyboard.dismiss();
expectLog(['blur 2', ...hidden], 'dismiss blurs the input');
check(!Keyboard.isVisible(), 'hidden after dismiss');

// Unmounting the focused input hides the keyboard with no blur.
tap(50, 45);
log.length = 0;
root.render(<App second={false} />);
expectLog(hidden, 'unmounting the focused input hides the keyboard');

hide.remove();
tap(50, 15);
Keyboard.dismiss();
expectLog(['focus 1', ...shown, 'blur 1'], 'a removed listener is not called');

report('keyboard');
