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

// Fractional style dimensions must snap to whole pixels the way JavaScript's Math.round does — the rule
// Flow B's AOT compiler folds its constants with. The bridge used to hand ERProps a plain C cast instead,
// which truncates toward zero, so any layout built from a scale factor drew a pixel off in Flow A only:
// the thermostat's 320x480 dial came out 223 px wide against the AOT's 224, and its centre readout sat a
// row high, which is what issue #187 saw. The numbers below are that dial's real geometry (S = 284/320).
import {createRoot} from '../../src/renderer.js';
import {View} from 'embedded-react';
import {check, report} from './harness.js';

const seen = {};
const box = (name, style) => (
  <View
    key={name}
    style={{position: 'absolute', ...style}}
    onLayout={e => (seen[name] = e.layout)}
  />
);

const S = 284 / 320; // the dial's authoring-unit → screen-px scale

function App() {
  return (
    <View style={{width: 480, height: 320}}>
      {/* 223.65 → 224: the dial box itself, a whole pixel of ring geometry. */}
      {box('dial', {left: 0, top: 0, width: 2 * (116 + 10) * S, height: 4})}
      {/* 106.5 → 107: an exact half rounds UP (Math.round), not away from zero. */}
      {box('halfUp', {left: 0, top: 0, width: 120 * S, height: 4})}
      {/* 147.325 → 147: below the half, so this one rounds down either way. */}
      {box('down', {left: 0, top: 0, width: 166 * S, height: 4})}
      {/* 58.575 → 59 as a POSITION: the readout box's top, which used to land on 58. */}
      {box('top', {left: 0, top: 126 * S - 60 * S, width: 4, height: 4})}
      {/* -1.5 → -1 and -0.5 → 0: Math.round is floor(x + 0.5), so halves go up on both signs. */}
      {box('negHalf', {left: -1.5, top: 40, width: 4, height: 4})}
      {box('negTiny', {left: -0.5, top: 60, width: 4, height: 4})}
      {box('huge', {left: 0, top: 80, width: 1e9, height: 4})}
      {box('tiny', {left: 0, top: 100, width: -1e9, height: 4})}
      {box('nan', {left: 0, top: 120, width: 0 / 0, height: 4})}
    </View>
  );
}

createRoot({width: screen.width, height: screen.height}).render(<App />);

const at = (name, key) => (seen[name] ? seen[name][key] : undefined);

check(seen.dial != null, 'fractional boxes laid out (onLayout fired)');
check(
  at('dial', 'width') === 224,
  `223.65 rounds to 224 (${at('dial', 'width')})`,
);
check(
  at('halfUp', 'width') === 107,
  `an exact .5 rounds up: 106.5 -> 107 (${at('halfUp', 'width')})`,
);
check(
  at('down', 'width') === 147,
  `147.325 rounds to 147 (${at('down', 'width')})`,
);
check(
  at('top', 'y') === 59,
  `a fractional top rounds: 58.575 -> 59 (${at('top', 'y')})`,
);
check(
  at('negHalf', 'x') === -1,
  `-1.5 rounds to -1, not -2 (${at('negHalf', 'x')})`,
);
check(
  at('negTiny', 'x') === 0,
  `-0.5 rounds to 0, not -1 (${at('negTiny', 'x')})`,
);
check(
  at('huge', 'width') === 32767,
  `a width past int16 clamps instead of wrapping (${at('huge', 'width')})`,
);
check(
  at('tiny', 'width') === 0,
  `a width past int16 the other way does not wrap positive (${at('tiny', 'width')})`,
);
check(
  at('nan', 'width') === 0,
  `a NaN width resolves to 0 (${at('nan', 'width')})`,
);

report('dim-rounding');
