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

// The bridge remembers what it last parsed out of a prop's string — the enum a token mapped to, and
// the ARGB a CSS color parsed to — so a commit that repeats the same style doesn't re-parse it. This
// is the churn those caches have to survive: values that keep changing, more distinct colors than the
// cache has room for, and equal text arriving as a different string object each render.
import {createRoot} from '../../src/renderer.js';
import {View} from 'embedded-react';
import {check, report} from './harness.js';

const root = createRoot({width: screen.width, height: screen.height});
const layouts = {};

/** Renders one 100x100 box at the origin and returns the pixel at its centre. */
function paint(style) {
  root.render(<View style={{width: 100, height: 100, ...style}} />);
  return __pixel(50, 50);
}

// Alternating between two colors on the same prop: the second render must not keep the first's ARGB.
check(paint({backgroundColor: '#ff0000'}) === 0xffff0000, 'hex color painted');
check(
  paint({backgroundColor: '#00ff00'}) === 0xff00ff00,
  'a different hex color replaced it',
);
check(
  paint({backgroundColor: '#ff0000'}) === 0xffff0000,
  'and switching back restored the first',
);

// The same color written three ways — each spelling parses to the same ARGB.
check(
  paint({backgroundColor: 'rgb(0,0,255)'}) === 0xff0000ff,
  'rgb() color painted',
);
check(
  paint({backgroundColor: '#0000ff'}) === 0xff0000ff,
  'the hex spelling of it painted the same',
);
check(
  paint({backgroundColor: 'blue'}) === 0xff0000ff,
  'the named spelling of it painted the same',
);

// More distinct colors than the cache holds, cycled twice: every one still parses to itself, whether
// it survived eviction or had to be re-parsed.
const ramp = [];
for (let i = 0; i < 40; i++) ramp.push((0xff000000 | (i * 6)) >>> 0); // >>> 0: `|` yields a signed int32
let rampOk = true;
for (let pass = 0; pass < 2; pass++) {
  for (const argb of ramp) {
    const hex = '#' + (argb & 0xffffff).toString(16).padStart(6, '0');
    if (paint({backgroundColor: hex}) !== argb) rampOk = false;
  }
}
check(
  rampOk,
  `${ramp.length} colors cycled twice each painted their own value`,
);

// Equal text, fresh string object every render — the value is the same, however it was built.
const built = ['#', 'ff', '00', 'ff'].join('');
check(
  paint({backgroundColor: built}) === 0xffff00ff,
  'a computed color string painted',
);
check(
  paint({backgroundColor: ['#', 'ff', '00', 'ff'].join('')}) === 0xffff00ff,
  'and an equal but freshly built one painted the same',
);

// Enum tokens: flexDirection decides whether two children stack or sit side by side, so the layout
// reports whether the second render re-read the token or reused the first one's.
function Row({dir}) {
  return (
    <View style={{width: 100, height: 100, flexDirection: dir}}>
      <View style={{width: 20, height: 20}} />
      <View
        style={{width: 20, height: 20}}
        onLayout={e => (layouts.second = e.layout)}
      />
    </View>
  );
}
root.render(<Row dir="row" />);
check(
  layouts.second.x === 20 && layouts.second.y === 0,
  'flexDirection row laid the second child beside the first',
);
root.render(<Row dir="column" />);
check(
  layouts.second.x === 0 && layouts.second.y === 20,
  'switching to column stacked it below',
);
root.render(<Row dir="row" />);
check(
  layouts.second.x === 20 && layouts.second.y === 0,
  'and switching back restored the row',
);
root.render(<Row dir={'ro' + 'w'} />);
check(
  layouts.second.x === 20 && layouts.second.y === 0,
  'a computed token mapped the same as the literal',
);

report('props-marshalling');
