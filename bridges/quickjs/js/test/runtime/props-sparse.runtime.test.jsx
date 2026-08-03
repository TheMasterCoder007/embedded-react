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

// Runtime e2e for native_ui_bridge.c's apply_props(): it now gathers a setProps object's own keys
// via JS_GetOwnPropertyNames instead of probing ~90 fixed names, so this exercises exactly what
// that rewrite could get wrong — statement-order semantics (flex shorthand vs. explicit overrides),
// the shared static s_prop_slots scratch table not leaking a previous call's values into a later
// sparse object, and unrecognized keys being ignored rather than crashing.
import {createRoot} from '../../src/renderer.js';
import {View} from 'embedded-react';
import {check, report} from './harness.js';

let rowRect = null;
let aRect = null;

function FlexRow({styleA}) {
  return (
    <View
      style={{width: 300, height: 50, flexDirection: 'row'}}
      onLayout={e => (rowRect = e.layout)}>
      <View style={styleA} onLayout={e => (aRect = e.layout)} />
      <View style={{width: 100}} />
    </View>
  );
}

const root = createRoot({width: screen.width, height: screen.height});

// `flex` shorthand alone: flex:2 -> grow=2/shrink=1/basis=0, so A fills the remaining 200px
// (row 300 - sibling 100). Confirms apply_flex_shorthand still runs off the gathered slot.
root.render(<FlexRow styleA={{flex: 2}} />);
check(rowRect && rowRect.width === 300, 'row keeps its explicit width (300)');
check(
  aRect && aRect.width === 200,
  `flex:2 shorthand fills the remaining space (got ${aRect && aRect.width})`,
);

// Explicit flexGrow/flexBasis must still win over the shorthand — apply_props() runs the shorthand
// before the explicit overrides, and the rewrite preserves that statement order.
root.render(<FlexRow styleA={{flex: 2, flexGrow: 0, flexBasis: 40}} />);
check(
  aRect.width === 40,
  `explicit flexGrow/flexBasis override the flex shorthand (got ${aRect.width})`,
);

// Percentage width resolves against the row's content box, independent of the px path.
root.render(<FlexRow styleA={{width: '50%', height: 10}} />);
check(
  aRect.width === 150,
  `width: "50%" resolves against the 300px row (got ${aRect.width})`,
);

// This render's style has neither `flex` nor `width` as a percent — a numeric width alone must
// fully take effect. If a previous call's slot values weren't cleared, stale flex/percent state
// from the renders above could leak through the shared static s_prop_slots table.
root.render(<FlexRow styleA={{width: 60}} />);
check(
  aRect.width === 60,
  `numeric width replaces a prior percent width (got ${aRect.width})`,
);

// An unrecognized key alongside known ones must be ignored, not crash or shadow real props.
root.render(<FlexRow styleA={{width: 70, bogusProp: 'nope'}} />);
check(
  aRect.width === 70,
  `unknown keys are ignored alongside known ones (got ${aRect.width})`,
);

report('props-sparse');
