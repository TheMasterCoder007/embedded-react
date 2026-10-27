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

// Runtime e2e for a render past ERUI_MAX_NODES: createNode reports the full pool on stderr, once per run
// of failures rather than once per dropped node, and unmounting the render frees every node it took,
// including the children of a parent that did not fit.
//
// @runtime-stderr: createNode: node pool full, raise ERUI_MAX_NODES
// @runtime-stderr: createNode: node pool full, raise ERUI_MAX_NODES
// @runtime-stderr: createNode: node pool full, raise ERUI_MAX_NODES
import {createRoot} from '../../src/renderer.js';
import {View} from 'embedded-react';
import {check, report} from './harness.js';

const root = createRoot({width: screen.width, height: screen.height});

// --- a run of failures logs once (first line) ------------------------------------------------
const handles = [];
for (
  let h = NativeUI.createNode('View');
  h !== 0;
  h = NativeUI.createNode('View')
) {
  handles.push(h);
}
check(
  handles.length > 0,
  `nodes are created until the pool is full (got ${handles.length})`,
);
check(
  NativeUI.createNode('View') === 0,
  'creation keeps failing while the pool is full',
);

// --- a success ends the run, so the next failure logs again (second line) --------------------
NativeUI.destroyNode(handles.pop());
const reused = NativeUI.createNode('View');
check(reused !== 0, 'a freed slot is reused');
handles.push(reused);
check(NativeUI.createNode('View') === 0, 'the pool is full again');
handles.forEach(h => NativeUI.destroyNode(h));

// --- a render far past the limit logs one line, not one per dropped node (third line) ---------
root.render(
  <View style={{flexDirection: 'row', flexWrap: 'wrap'}}>
    {Array.from({length: 600}, (_, i) => (
      <View key={i} style={{width: 4, height: 4}} />
    ))}
  </View>,
);
check(NativeUI.createNode('View') === 0, 'the pool is full after the render');

// --- unmounting frees the children whose parent did not fit ----------------------------------
root.render(null);
NativeUI.tick(0); // deleted instances are detached on a pump
const probe = NativeUI.createNode('View');
check(probe !== 0, 'unmounting the render frees the pool');
NativeUI.destroyNode(probe);

let laidOut = 0;
root.render(
  <View style={{flexDirection: 'row', flexWrap: 'wrap'}}>
    {Array.from({length: 10}, (_, i) => (
      <View key={i} style={{width: 4, height: 4}} onLayout={() => laidOut++} />
    ))}
  </View>,
);
check(
  laidOut === 10,
  `a render that fits lays out afterwards (got ${laidOut})`,
);

report('node-pool');
