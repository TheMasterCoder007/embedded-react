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

// A percentage inset — `left: '50%'` — has to survive the whole Flow A path: the bridge parses the
// string into ERProps' float field instead of dropping it on the floor, and the engine resolves it
// against the containing block. It used to reach a numeric-only reader that ignored strings, so a
// percent-positioned node silently stacked in the corner. Expected rects come from `yoga-layout`.
import {createRoot} from '../../src/renderer.js';
import {View} from 'embedded-react';
import {check, report} from './harness.js';

const seen = {};
const box = (name, style) => (
  <View key={name} style={style} onLayout={e => (seen[name] = e.layout)} />
);

function App() {
  return (
    // The containing block is this node's padding box (200x100); its content box (180x80, at 10,10)
    // is what the RELATIVE child's percentage measures against instead.
    <View style={{width: 200, height: 100, padding: 10}}>
      {box('abs', {
        position: 'absolute',
        left: '10%',
        top: '25%',
        width: 20,
        height: 20,
      })}
      {box('far', {
        position: 'absolute',
        right: '10%',
        bottom: '20%',
        width: 20,
        height: 20,
      })}
      {box('pair', {
        position: 'absolute',
        left: '10%',
        right: '25%',
        top: '10%',
        bottom: '30%',
      })}
      {box('rel', {left: '10%', top: '50%', width: 20, height: 20})}
      {box('zero', {
        position: 'absolute',
        left: '0%',
        right: '0%',
        top: '0%',
        bottom: '0%',
      })}
    </View>
  );
}

createRoot({width: screen.width, height: screen.height}).render(<App />);

const rect = name => {
  const l = seen[name];
  return l ? `${l.x},${l.y},${l.width},${l.height}` : 'missing';
};

check(seen.abs != null, 'percent-inset boxes laid out (onLayout fired)');
check(
  rect('abs') === '20,25,20,20',
  `left/top percent of the padding box (${rect('abs')})`,
);
check(
  rect('far') === '160,60,20,20',
  `right/bottom percent anchor the far edges (${rect('far')})`,
);
check(
  rect('pair') === '20,10,130,60',
  `opposing percent insets size the node (${rect('pair')})`,
);
check(
  rect('rel') === '28,50,20,20',
  `a relative percent offset measures the content box (${rect('rel')})`,
);
check(
  rect('zero') === '0,0,200,100',
  `0% insets pin all four edges rather than reading as unset (${rect('zero')})`,
);

report('pct-inset');
