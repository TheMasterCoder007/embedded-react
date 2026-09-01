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

// Runtime e2e: PanResponder through the real bridge + engine — the NATIVE responder path. The unit
// test drives the state machine; this proves the wiring: spreading `panHandlers` registers real engine
// events AND negotiation queries, negotiation covers the subtree (capture before bubble), a granted
// pan owns the gesture against a ScrollView, termination requests are honoured, losing claimants get
// rejected, and multi-finger touches fold into one gesture.
//
// Touches come from the runner's __touch(phase, x, y[, finger]): 0 down / 1 move / 2 up / 3 cancel.
import {createRoot} from '../../src/renderer.js';
import {View, ScrollView, PanResponder} from 'embedded-react';
import {NativeUI} from '../../src/native-ui.js';
import {check, report} from './harness.js';

const root = createRoot({width: screen.width, height: screen.height});

/** A recorder-backed responder: logs every lifecycle callback with a gestureState snapshot. */
function recorder(log, tag, extra = {}) {
  const rec = name => (e, g) =>
    log.push({
      tag,
      name,
      x: e.x,
      dx: g.dx,
      dy: g.dy,
      vx: g.vx,
      x0: g.x0,
      type: e.type,
      touches: g.numberActiveTouches,
    });
  return PanResponder.create({
    onPanResponderGrant: rec('grant'),
    onPanResponderMove: rec('move'),
    onPanResponderRelease: rec('release'),
    onPanResponderTerminate: rec('terminate'),
    onPanResponderReject: rec('reject'),
    onPanResponderStart: rec('start'),
    onPanResponderEnd: rec('end'),
    ...extra,
  });
}

const names = log =>
  log.map(e => `${e.tag ? e.tag + ':' : ''}${e.name}`).join(',');

// ====================================================================================================
// 1. The basic drag lifecycle (grant → move → release, velocity, cancel, re-entrant down)
// ====================================================================================================
{
  const log = [];
  const pan = recorder(log, '', {onStartShouldSetPanResponder: () => true});

  // The responder lives on the OUTER view; the inner one is what the touch hits — negotiation walks
  // the ancestor chain, so a wrapper claims its children's touches.
  root.render(
    <View style={{width: 480, height: 320}} {...pan.panHandlers}>
      <View style={{width: 200, height: 200, backgroundColor: '#1a1a2e'}} />
    </View>,
  );

  __touch(0, 40, 40);
  check(
    names(log) === 'grant',
    `touch-down on the child grants (${names(log)})`,
  );
  check(
    log[0].x0 === 40 && log[0].dx === 0,
    `grant anchors at the touch point (x0=${log[0].x0}, dx=${log[0].dx})`,
  );
  check(
    log[0].type === 'responderGrant',
    `the native responder event reaches the callback (type=${log[0].type})`,
  );

  NativeUI.tick(20);
  __touch(1, 100, 70);
  check(names(log) === 'grant,move', `a move is reported (${names(log)})`);
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
    names(log) === 'grant,move,move,release',
    `lifting the finger releases (${names(log)})`,
  );
  check(
    log[3].dx === 120,
    `release carries the full travel (dx=${log[3].dx}, want 120)`,
  );
  check(
    log[3].vx === 3,
    `release keeps the fling speed rather than re-measuring across the lift (vx=${log[3].vx})`,
  );

  log.length = 0;
  __touch(0, 40, 40);
  NativeUI.tick(20);
  __touch(1, 60, 40);
  __touch(3, 60, 40);
  check(
    names(log) === 'grant,move,terminate',
    `a cancelled touch terminates rather than releasing (${names(log)})`,
  );

  log.length = 0;
  __touch(0, 200, 100);
  NativeUI.tick(20);
  __touch(1, 230, 100);
  __touch(2, 230, 100);
  check(
    names(log) === 'grant,move,release',
    `the next gesture runs normally after a cancel (${names(log)})`,
  );
  check(
    log[1].dx === 30 && log[1].x0 === 200,
    `and re-anchors (dx=${log[1].dx}, x0=${log[1].x0})`,
  );

  log.length = 0;
  __touch(0, 40, 40);
  NativeUI.tick(20);
  __touch(0, 300, 200); // second down on the same finger, no up in between
  check(
    names(log) === 'grant,terminate,grant',
    `a re-entrant touch-down cancels the stale gesture first (${names(log)})`,
  );
  check(
    log[2].x0 === 300,
    `the new gesture anchors at the new point (x0=${log[2].x0})`,
  );
  __touch(2, 300, 200);
}

// ====================================================================================================
// 2. A granted pan OWNS the gesture: the ScrollView ancestor must not auto-scroll under it
// ====================================================================================================
{
  const log = [];
  const scrolls = [];
  const pan = recorder(log, '', {onStartShouldSetPanResponder: () => true});

  root.render(
    <ScrollView
      style={{width: 240, height: 200}}
      onScroll={e => scrolls.push(e.scrollY)}>
      <View style={{width: 240, height: 600}}>
        <View style={{width: 240, height: 600}} {...pan.panHandlers} />
      </View>
    </ScrollView>,
  );

  __touch(0, 100, 100);
  NativeUI.tick(20);
  __touch(1, 100, 60); // dy 40 — way past ER_SCROLL_SLOP (5)
  NativeUI.tick(20);
  __touch(1, 100, 20);
  NativeUI.tick(20);
  __touch(2, 100, 20);
  for (let i = 0; i < 30; i++) NativeUI.tick(16); // momentum window

  check(
    names(log) === 'grant,move,move,release',
    `the pan runs its full lifecycle inside the scroller (${names(log)})`,
  );
  check(
    scrolls.length === 0,
    `the ScrollView never scrolled under the granted pan (${scrolls.length} scroll events)`,
  );
}

// ====================================================================================================
// 3. Move-should-set takes the gesture BACK from an auto-scrolling ScrollView
// ====================================================================================================
{
  const log = [];
  const scrolls = [];
  const pan = recorder(log, '', {
    onMoveShouldSetPanResponder: (e, g) => Math.abs(g.dy) > 40,
  });

  root.render(
    <ScrollView
      style={{width: 240, height: 200}}
      onScroll={e => scrolls.push(e.scrollY)}>
      <View style={{width: 240, height: 600}}>
        <View style={{width: 240, height: 600}} {...pan.panHandlers} />
      </View>
    </ScrollView>,
  );

  __touch(0, 100, 150);
  NativeUI.tick(20);
  __touch(1, 100, 130); // dy 20: auto-scroll claims (slop 5) and scrolls
  NativeUI.tick(20);
  __touch(1, 100, 110); // dy 40: still the scroller's
  const scrolledBefore = scrolls.length;
  NativeUI.tick(20);
  __touch(1, 100, 90); // dy 60: our predicate claims — scroller yields (no termination handler)
  const scrollsAtGrant = scrolls.length;
  NativeUI.tick(20);
  __touch(1, 100, 70);
  NativeUI.tick(20);
  __touch(2, 100, 70);
  for (let i = 0; i < 30; i++) NativeUI.tick(16); // momentum window — a yielded scroller must not coast

  check(
    scrolledBefore > 0,
    `the scroller scrolled before the pan claimed (${scrolledBefore} events)`,
  );
  check(
    names(log) === 'grant,move,release',
    `the pan took the gesture mid-drag (${names(log)})`,
  );
  check(
    log[0].dx === 0 && log[0].dy === 0,
    `the takeover re-anchors travel at the grant (dx=${log[0].dx}, dy=${log[0].dy})`,
  );
  check(
    log[1].dy === -20,
    `moves after the takeover measure from the grant (dy=${log[1].dy}, want -20)`,
  );
  check(
    scrolls.length === scrollsAtGrant,
    `the scroller stopped dead when it yielded — no scroll, no coasting (${scrolls.length - scrollsAtGrant} extra events)`,
  );
}

// ====================================================================================================
// 4. Termination request: the holder refuses, the challenger is rejected
// ====================================================================================================
{
  const log = [];
  const inner = recorder(log, 'inner', {
    onStartShouldSetPanResponder: () => true,
    onPanResponderTerminationRequest: () => false,
  });
  const outer = recorder(log, 'outer', {
    onMoveShouldSetPanResponder: () => true,
  });

  root.render(
    <View style={{width: 480, height: 320}} {...outer.panHandlers}>
      <View style={{width: 200, height: 200}} {...inner.panHandlers} />
    </View>,
  );

  __touch(0, 50, 50); // inner claims on start
  NativeUI.tick(20);
  __touch(1, 90, 50); // outer claims on move → inner refuses → outer rejected
  NativeUI.tick(20);
  __touch(1, 130, 50);
  __touch(2, 130, 50);

  const seq = names(log);
  check(
    seq.startsWith('inner:grant,inner:move,outer:reject'),
    `the refused challenger is rejected after the holder's move (${seq})`,
  );
  check(
    log.filter(e => e.tag === 'inner' && e.name === 'move').length === 2 &&
      log.some(e => e.tag === 'inner' && e.name === 'release'),
    `the holder keeps the gesture through both moves and the release (${seq})`,
  );
  check(
    !log.some(e => e.tag === 'outer' && e.name === 'grant'),
    `the challenger is never granted (${seq})`,
  );
}

// ====================================================================================================
// 5. Capture phase: an outer capture predicate beats the inner bubble one
// ====================================================================================================
{
  const log = [];
  const inner = recorder(log, 'inner', {
    onStartShouldSetPanResponder: () => true,
  });
  const outer = recorder(log, 'outer', {
    onStartShouldSetPanResponderCapture: () => true,
  });

  root.render(
    <View style={{width: 480, height: 320}} {...outer.panHandlers}>
      <View style={{width: 200, height: 200}} {...inner.panHandlers} />
    </View>,
  );

  __touch(0, 50, 50);
  NativeUI.tick(20);
  __touch(1, 90, 50);
  __touch(2, 90, 50);

  check(
    names(log) === 'outer:grant,outer:move,outer:release',
    `capture (root→leaf) wins before the inner bubble predicate is even asked (${names(log)})`,
  );
}

// ====================================================================================================
// 6. Multi-finger: later fingers join the gesture; the last one to lift releases it
// ====================================================================================================
{
  const log = [];
  const pan = recorder(log, '', {onStartShouldSetPanResponder: () => true});

  root.render(<View style={{width: 480, height: 320}} {...pan.panHandlers} />);

  __touch(0, 100, 100, 0); // finger 0 down → grant
  NativeUI.tick(20);
  __touch(0, 300, 100, 1); // finger 1 down → joins: start, no second grant
  NativeUI.tick(20);
  __touch(1, 140, 100, 0); // finger 0 drags
  NativeUI.tick(20);
  __touch(2, 300, 100, 1); // finger 1 up → end, gesture continues
  NativeUI.tick(20);
  __touch(1, 180, 100, 0);
  __touch(2, 180, 100, 0); // last finger up → release

  const seq = names(log);
  check(
    seq === 'grant,start,move,end,move,release',
    `two fingers fold into one gesture (${seq})`,
  );
  check(
    log[1].touches === 2,
    `numberActiveTouches counts both fingers (${log[1].touches})`,
  );
  check(
    log[5].touches === 0 && log[5].name === 'release',
    `the last lift releases with no fingers left (${log[5].touches})`,
  );
}

report('pan-responder');
