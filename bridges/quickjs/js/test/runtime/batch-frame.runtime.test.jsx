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

// Runtime e2e: a frame's state updates cost one render and one commit, however many callbacks made
// them. Nothing here is inside a React event system — these are host-initiated updates (timers,
// promise continuations, a native touch handler), which is exactly the case LegacyRoot renders and
// commits one at a time unless the bridge batches them.
//
// Commits are counted through __layoutPasses(): every update below resizes a node, so a commit that
// runs pays a layout pass and one that is coalesced away does not.
import {createRoot} from '../../src/renderer.js';
import {View} from 'embedded-react';
import {useState} from 'react';
import {NativeUI} from '../../src/native-ui.js';
import {check, report} from './harness.js';

const root = createRoot({width: screen.width, height: screen.height});

let setA;
let setB;
let setC;
let renders = 0;

function App() {
  const [a, sa] = useState(10);
  const [b, sb] = useState(10);
  const [c, sc] = useState(10);
  setA = sa;
  setB = sb;
  setC = sc;
  renders++;
  return (
    <View
      style={{width: 200, height: 200}}
      onTouchStart={() => (setA(60), setB(60))}>
      <View style={{width: a, height: 20}} />
      <View style={{width: b, height: 20}} />
      <View style={{width: c, height: 20}} />
    </View>
  );
}

root.render(<App />);

/** Runs `fn`, then one host frame, and reports the renders and commits it cost. */
function frame(fn) {
  const r0 = renders;
  const p0 = __layoutPasses();
  fn();
  NativeUI.tick(16);
  return {renders: renders - r0, commits: __layoutPasses() - p0};
}

// ====================================================================================================
// 1. Three timers due on the same frame — the "N concurrent animations" case
// ====================================================================================================
{
  const cost = frame(() => {
    setTimeout(() => setA(30), 1);
    setTimeout(() => setB(30), 1);
    setTimeout(() => setC(30), 1);
  });
  check(
    cost.commits === 1,
    `three timers on one frame commit once (got ${cost.commits})`,
  );
  check(
    cost.renders === 1,
    `three timers on one frame render once (got ${cost.renders})`,
  );
}

// ====================================================================================================
// 2. A promise continuation is part of the same frame, so it paints with it
//
// It costs a second render pass, which the batch cannot remove: React queues its own flush microtask
// when the timer sets state, and that lands ahead of the app's continuation in the job queue. The
// commit is what the batch is there to collapse, and it does.
// ====================================================================================================
{
  const cost = frame(() => {
    setTimeout(() => {
      setA(40);
      Promise.resolve().then(() => (setB(40), setC(40)));
    }, 1);
  });
  check(
    cost.commits === 1,
    `a timer and its promise continuation commit once (got ${cost.commits})`,
  );
  check(
    cost.renders === 2,
    `...over the two renders their ordering forces (got ${cost.renders})`,
  );
}

// ====================================================================================================
// 3. A native touch handler setting two things: one render, one commit — and the commit lands
//    eagerly, so the layout the next hit test reads is already the new one
// ====================================================================================================
{
  const r0 = renders;
  const p0 = __layoutPasses();
  __touch(0, 10, 10);
  check(
    __layoutPasses() - p0 === 1,
    `a handler setting two states commits once (got ${__layoutPasses() - p0})`,
  );
  check(
    renders - r0 === 1,
    `a handler setting two states renders once (got ${renders - r0})`,
  );
  __touch(2, 10, 10);
}

// ====================================================================================================
// 4. A render outside any frame still commits immediately — layout is never left stale
// ====================================================================================================
{
  const p0 = __layoutPasses();
  root.render(<App key="remount" />);
  check(
    __layoutPasses() - p0 === 1,
    `a top-level render commits on the spot (got ${__layoutPasses() - p0})`,
  );
}

report('batch-frame');
