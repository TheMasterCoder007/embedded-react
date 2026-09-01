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

// Runtime e2e: PanResponder through the real bridge + engine. The unit test drives the state machine
// by calling the handlers directly; this one proves the wiring — that spreading `panHandlers` onto a
// component registers real engine events, that a touch on a CHILD bubbles up to them, that velocity
// reads the engine clock, and that the engine's cancel path lands on onPanResponderTerminate.
//
// Touches come from the runner's __touch(phase, x, y): 0 down / 1 move / 2 up / 3 cancel.
import {createRoot} from '../../src/renderer.js';
import {View, PanResponder} from 'embedded-react';
import {NativeUI} from '../../src/native-ui.js';
import {check, report} from './harness.js';

const log = [];
const record = name => (e, g) =>
  log.push({name, dx: g.dx, dy: g.dy, vx: g.vx, x0: g.x0, type: e.type});

const pan = PanResponder.create({
  onStartShouldSetPanResponder: () => true,
  onPanResponderGrant: record('grant'),
  onPanResponderMove: record('move'),
  onPanResponderRelease: record('release'),
  onPanResponderTerminate: record('terminate'),
});

// The responder lives on the OUTER view; the inner one is what the touch actually hits, so a pass here
// is also a check that raw touches bubble to an ancestor's handlers.
function App() {
  return (
    <View style={{width: 480, height: 320}} {...pan.panHandlers}>
      <View style={{width: 200, height: 200, backgroundColor: '#1a1a2e'}} />
    </View>
  );
}

const root = createRoot({width: screen.width, height: screen.height});
root.render(<App />);

const names = () => log.map(entry => entry.name).join(',');

// --- A drag on the child, with the clock advanced between moves ------------------------------------
__touch(0, 40, 40);
check(names() === 'grant', `touch-down on the child grants (${names()})`);
check(
  log[0].x0 === 40 && log[0].dx === 0,
  `grant anchors at the touch point (x0=${log[0].x0}, dx=${log[0].dx})`,
);
check(
  log[0].type === 'touchStart',
  `the raw event reaches the callback (type=${log[0].type})`,
);

NativeUI.tick(20);
__touch(1, 100, 70);
check(names() === 'grant,move', `a move is reported (${names()})`);
check(
  log[1].dx === 60 && log[1].dy === 30,
  `travel is measured from the grant point (dx=${log[1].dx}, dy=${log[1].dy})`,
);
check(
  log[1].vx === 3,
  `velocity is px/ms off the engine clock (60px / 20ms = 3, got ${log[1].vx})`,
);

NativeUI.tick(20);
__touch(1, 160, 70); // 60px over 20ms → vx 3 again
NativeUI.tick(20); // a frame with the finger resting where it landed, then the lift
__touch(2, 160, 70);
check(
  names() === 'grant,move,move,release',
  `lifting the finger releases (${names()})`,
);
check(
  log[3].dx === 120,
  `release carries the full travel (dx=${log[3].dx}, want 120)`,
);
check(
  log[3].vx === 3,
  `release keeps the fling speed rather than re-measuring across the lift (vx=${log[3].vx})`,
);

// --- The engine cancelling the sequence lands on terminate, not release -----------------------------
log.length = 0;
__touch(0, 40, 40);
NativeUI.tick(20);
__touch(1, 60, 40);
__touch(3, 60, 40);
check(
  names() === 'grant,move,terminate',
  `a cancelled touch terminates rather than releasing (${names()})`,
);

// A fresh gesture after the cancel starts clean.
log.length = 0;
__touch(0, 200, 100);
NativeUI.tick(20);
__touch(1, 230, 100);
__touch(2, 230, 100);
check(
  names() === 'grant,move,release',
  `the next gesture runs normally after a cancel (${names()})`,
);
check(
  log[1].dx === 30 && log[1].x0 === 200,
  `and re-anchors (dx=${log[1].dx}, x0=${log[1].x0})`,
);

// --- A touch-down that the engine never saw ended cancels the stale sequence first -------------------
log.length = 0;
__touch(0, 40, 40);
NativeUI.tick(20);
__touch(0, 300, 200); // second down, no up in between
check(
  names() === 'grant,terminate,grant',
  `a re-entrant touch-down cancels the stale gesture first (${names()})`,
);
check(
  log[2].x0 === 300,
  `the new gesture anchors at the new point (x0=${log[2].x0})`,
);
__touch(2, 300, 200);

report('pan-responder');
