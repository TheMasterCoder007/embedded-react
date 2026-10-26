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

// Runtime e2e for the press family: a held finger produces onLongPress, and that long press REPLACES
// the tap rather than arriving alongside it (React Native's isPressCanceledByLongPress). `delayLongPress`
// sets the hold time per node, so one screen can hold a slow destructive action next to a quick one.
//
// The hold is measured by the engine's input clock, which the host advances with NativeUI.tick — so a
// test holds a finger by ticking, not by sleeping. Touches come from the runner's
// __touch(phase, x, y[, finger]): 0 down / 1 move / 2 up / 3 cancel.
import {createRoot} from '../../src/renderer.js';
import {View, Text, Pressable, TouchableOpacity} from 'embedded-react';
import {NativeUI} from '../../src/native-ui.js';
import {check, report} from './harness.js';

const root = createRoot({width: screen.width, height: screen.height});

const log = [];
const rec = name => () => log.push(name);

/** Holds a finger on (x, y) for ms, ticking in 10 ms steps the way a frame loop would, then lifts. */
const hold = (x, y, ms, {lift = true} = {}) => {
  __touch(0, x, y);
  for (let held = 0; held < ms; held += 10) NativeUI.tick(10);
  if (lift) __touch(2, x, y);
};

// ====================================================================================================
// 1. The default 500 ms hold, and a long press replacing the tap it grew out of
// ====================================================================================================
{
  log.length = 0;
  root.render(
    <View style={{width: 480, height: 320}}>
      <Pressable
        style={{width: 200, height: 200}}
        onPress={rec('press')}
        onLongPress={rec('long')}>
        <Text>hold me</Text>
      </Pressable>
    </View>,
  );

  hold(40, 40, 300);
  check(
    log.join() === 'press',
    `a short press is an ordinary tap (${log.join() || 'nothing'})`,
  );

  log.length = 0;
  hold(40, 40, 300, {lift: false});
  check(log.length === 0, 'nothing fires before the 500 ms threshold');
  for (let i = 0; i < 25; i++) NativeUI.tick(10); // past 500 ms
  check(log.join() === 'long', `onLongPress fires on the hold (${log.join()})`);
  __touch(2, 40, 40);
  check(
    log.join() === 'long',
    `a delivered long press replaces the tap (${log.join()})`,
  );
}

// ====================================================================================================
// 2. delayLongPress sets the hold per node — a slow one and a quick one on the same screen
// ====================================================================================================
{
  log.length = 0;
  root.render(
    <View style={{width: 480, height: 320}}>
      <Pressable
        style={{width: 100, height: 100}}
        delayLongPress={1200}
        onPress={rec('slow-press')}
        onLongPress={rec('slow-long')}
      />
      <Pressable
        style={{width: 100, height: 100}}
        delayLongPress={120}
        onPress={rec('quick-press')}
        onLongPress={rec('quick-long')}
      />
    </View>,
  );

  hold(40, 40, 600, {lift: false});
  check(
    log.length === 0,
    `delayLongPress={1200} still holds at 600 ms (${log.join() || 'nothing'})`,
  );
  for (let i = 0; i < 70; i++) NativeUI.tick(10); // past 1200 ms
  check(log.join() === 'slow-long', `the slow hold fires (${log.join()})`);
  __touch(2, 40, 40);

  log.length = 0;
  hold(40, 140, 150);
  check(
    log.join() === 'quick-long',
    `delayLongPress={120} fires well before the default (${log.join()})`,
  );
}

// ====================================================================================================
// 3. Without an onLongPress handler a long hold is still an ordinary tap, and TouchableOpacity — which
//    forwards the prop through to its Pressable — behaves the same as one
// ====================================================================================================
{
  log.length = 0;
  root.render(
    <View style={{width: 480, height: 320}}>
      <Pressable style={{width: 200, height: 200}} onPress={rec('press')} />
    </View>,
  );
  hold(40, 40, 900);
  check(
    log.join() === 'press',
    `a long hold with no onLongPress is still a press (${log.join()})`,
  );

  log.length = 0;
  root.render(
    <View style={{width: 480, height: 320}}>
      <TouchableOpacity
        style={{width: 200, height: 200}}
        delayLongPress={150}
        onPress={rec('press')}
        onLongPress={rec('long')}
      />
    </View>,
  );
  hold(40, 40, 200);
  check(
    log.join() === 'long',
    `<TouchableOpacity delayLongPress> reaches the node (${log.join()})`,
  );
}

report('long-press');
