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

// Runtime e2e: onLayout reports x/y relative to the parent, as React Native does, and carries the
// rect at e.nativeEvent.layout as well as e.layout. Runs in the QuickJS + engine host.
import {createRoot} from '../../src/renderer.js';
import {View} from 'embedded-react';
import {check, report} from './harness.js';

let outer = null;
let inner = null;
let sameObject = false;
let innerFires = 0;

function App({left, padding = 5}) {
  return (
    <View
      style={{
        position: 'absolute',
        left,
        top: 30,
        width: 100,
        height: 80,
        padding,
      }}
      onLayout={e => (outer = e.nativeEvent.layout)}>
      <View
        style={{width: 20, height: 10, marginLeft: 3}}
        onLayout={e => {
          inner = e.nativeEvent.layout;
          sameObject = e.layout === e.nativeEvent.layout;
          innerFires++;
        }}
      />
    </View>
  );
}

const root = createRoot({width: screen.width, height: screen.height});

root.render(<App left={40} />);
check(outer && outer.x === 40 && outer.y === 30, 'outer box at 40,30');
check(
  inner &&
    inner.x === 8 &&
    inner.y === 5 &&
    inner.width === 20 &&
    inner.height === 10,
  `inner box relative to its parent (got ${inner && `${inner.x},${inner.y}`}, want 8,5)`,
);
check(sameObject, 'e.layout is e.nativeEvent.layout');

root.render(<App left={60} />);
check(outer.x === 60, 'outer box moved to 60');
check(
  innerFires === 1,
  `inner box did not fire when only its parent moved (fired ${innerFires})`,
);

root.render(<App left={60} padding={9} />);
check(
  innerFires === 2,
  `inner box fired when the parent's padding moved it (fired ${innerFires})`,
);
check(
  inner.x === 12 && inner.y === 9,
  `inner box at its new place in the parent (got ${inner.x},${inner.y}, want 12,9)`,
);

report('layout-relative');
