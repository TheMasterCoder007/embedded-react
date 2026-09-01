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

import {describe, it, expect, beforeEach, afterEach, vi} from 'vitest';
import {PanResponder} from '../PanResponder.js';

// PanResponder is pure JS over the onTouch* events, so the whole state machine is testable by calling
// the handlers directly with the event shape the bridge builds ({type, x, y, dx, dy}). Velocity reads
// performance.now() — the engine's clock on device — so the tests drive a stub of it.

const realPerformance = globalThis.performance;
let clock = 0;

beforeEach(() => {
  clock = 0;
  globalThis.performance = {now: () => clock};
});

afterEach(() => {
  globalThis.performance = realPerformance;
});

const ev = (type, x, y) => ({type, x, y, dx: 0, dy: 0});
const down = (h, x, y) => h.onTouchStart(ev('touchStart', x, y));
const move = (h, x, y) => h.onTouchMove(ev('touchMove', x, y));
const up = (h, x, y) => h.onTouchEnd(ev('touchEnd', x, y));
const cancel = (h, x, y) => h.onTouchCancel(ev('touchCancel', x, y));

/** A recorder that logs every callback as [name, {dx, dy, vx, vy, x0, y0, numberActiveTouches}]. */
function recorder(extra = {}) {
  const log = [];
  const snap = g => ({
    dx: g.dx,
    dy: g.dy,
    vx: g.vx,
    vy: g.vy,
    x0: g.x0,
    y0: g.y0,
    moveX: g.moveX,
    moveY: g.moveY,
    numberActiveTouches: g.numberActiveTouches,
  });
  const tap = name => (e, g) => log.push([name, snap(g)]);
  return {
    log,
    names: () => log.map(entry => entry[0]),
    config: {
      onPanResponderGrant: tap('grant'),
      onPanResponderMove: tap('move'),
      onPanResponderRelease: tap('release'),
      onPanResponderTerminate: tap('terminate'),
      ...extra,
    },
  };
}

describe('PanResponder.create', () => {
  it('returns the four onTouch* handlers to spread onto a component', () => {
    const {panHandlers} = PanResponder.create({});
    expect(Object.keys(panHandlers).sort()).toEqual([
      'onTouchCancel',
      'onTouchEnd',
      'onTouchMove',
      'onTouchStart',
    ]);
  });

  it('never grants when neither should-set callback is supplied (RN parity)', () => {
    const r = recorder();
    const {panHandlers} = PanResponder.create(r.config);
    down(panHandlers, 10, 10);
    move(panHandlers, 60, 10);
    up(panHandlers, 60, 10);
    expect(r.names()).toEqual([]);
  });

  it('gives each responder its own stateID', () => {
    const seen = [];
    const spy = () =>
      PanResponder.create({
        onStartShouldSetPanResponder: (e, g) => {
          seen.push(g.stateID);
          return true;
        },
      }).panHandlers.onTouchStart(ev('touchStart', 0, 0));
    spy();
    spy();
    expect(seen).toHaveLength(2);
    expect(seen[0]).not.toBe(seen[1]);
  });
});

describe('start-should-set gestures', () => {
  it('grants on touch-down, then reports travel per move and on release', () => {
    const r = recorder({onStartShouldSetPanResponder: () => true});
    const {panHandlers: h} = PanResponder.create(r.config);

    down(h, 100, 50);
    move(h, 130, 60);
    move(h, 160, 90);
    up(h, 160, 90);

    expect(r.names()).toEqual(['grant', 'move', 'move', 'release']);
    // dx/dy are measured from the grant point (100, 50).
    expect(r.log[1][1]).toMatchObject({dx: 30, dy: 10, x0: 100, y0: 50});
    expect(r.log[2][1]).toMatchObject({dx: 60, dy: 40, moveX: 160, moveY: 90});
    expect(r.log[3][1]).toMatchObject({dx: 60, dy: 40});
  });

  it('folds the release point in, so dx is the full travel with no final move', () => {
    const r = recorder({onStartShouldSetPanResponder: () => true});
    const {panHandlers: h} = PanResponder.create(r.config);

    down(h, 0, 0);
    up(h, 45, -12); // finger lifted somewhere else without an intervening move

    expect(r.names()).toEqual(['grant', 'release']);
    expect(r.log[1][1]).toMatchObject({dx: 45, dy: -12});
  });

  it('resets the gesture state between gestures', () => {
    const r = recorder({onStartShouldSetPanResponder: () => true});
    const {panHandlers: h} = PanResponder.create(r.config);

    down(h, 0, 0);
    move(h, 80, 80);
    up(h, 80, 80);

    down(h, 5, 5);
    move(h, 15, 5);

    const last = r.log[r.log.length - 1][1];
    expect(last).toMatchObject({dx: 10, dy: 0, x0: 5, y0: 5});
  });

  it('does not grant when the predicate returns false', () => {
    const r = recorder({onStartShouldSetPanResponder: () => false});
    const {panHandlers: h} = PanResponder.create(r.config);
    down(h, 0, 0);
    move(h, 40, 0);
    up(h, 40, 0);
    expect(r.names()).toEqual([]);
  });
});

describe('move-should-set gestures', () => {
  it('claims the pan past the slop and re-anchors dx/dy at the grant point', () => {
    const r = recorder({
      onMoveShouldSetPanResponder: (e, g) => Math.abs(g.dx) > 10,
    });
    const {panHandlers: h} = PanResponder.create(r.config);

    down(h, 100, 0);
    move(h, 105, 0); // inside the slop — nothing yet
    expect(r.names()).toEqual([]);

    move(h, 120, 0); // crosses it: grants, and this move is NOT reported as a move
    expect(r.names()).toEqual(['grant']);
    expect(r.log[0][1]).toMatchObject({dx: 0, dy: 0, x0: 120, y0: 0});

    move(h, 130, 0); // first real move, measured from the grant point
    expect(r.names()).toEqual(['grant', 'move']);
    expect(r.log[1][1]).toMatchObject({dx: 10});
  });
});

describe('events outside a gesture', () => {
  it('ignores moves once every finger has lifted', () => {
    const r = recorder({onStartShouldSetPanResponder: () => true});
    const {panHandlers: h} = PanResponder.create(r.config);
    down(h, 0, 0);
    up(h, 0, 0);
    move(h, 50, 50);
    expect(r.names()).toEqual(['grant', 'release']);
  });

  it('ignores a touch-end that no touch-down preceded', () => {
    const r = recorder({onStartShouldSetPanResponder: () => true});
    const {panHandlers: h} = PanResponder.create(r.config);
    up(h, 10, 10);
    expect(r.names()).toEqual([]);
  });
});

describe('velocity', () => {
  it('reports pixels per millisecond from the last move', () => {
    const r = recorder({onStartShouldSetPanResponder: () => true});
    const {panHandlers: h} = PanResponder.create(r.config);

    down(h, 0, 0);
    clock = 20;
    move(h, 40, 10); // 40 px / 20 ms = 2 px/ms, 10 px / 20 ms = 0.5 px/ms
    expect(r.log[1][1]).toMatchObject({vx: 2, vy: 0.5});

    clock = 30;
    move(h, 45, 10); // 5 px / 10 ms
    expect(r.log[2][1]).toMatchObject({vx: 0.5, vy: 0});
  });

  it('keeps the fling speed on release instead of re-measuring across the lift', () => {
    const r = recorder({onStartShouldSetPanResponder: () => true});
    const {panHandlers: h} = PanResponder.create(r.config);

    down(h, 0, 0);
    clock = 10;
    move(h, 40, 0); // thrown at 4 px/ms
    clock = 26; // a frame passes with the finger resting where it landed
    up(h, 40, 0);

    // Re-measuring 0 px over that frame would report vx 0 and lose the throw.
    expect(r.log[2][1].vx).toBe(4);
    expect(r.log[2][1].dx).toBe(40);
  });

  it('keeps the last velocity when the clock has not advanced (no Infinity/NaN)', () => {
    const r = recorder({onStartShouldSetPanResponder: () => true});
    const {panHandlers: h} = PanResponder.create(r.config);

    down(h, 0, 0);
    clock = 10;
    move(h, 30, 0);
    expect(r.log[1][1].vx).toBe(3);

    move(h, 60, 0); // same frame: dt === 0
    expect(r.log[2][1].vx).toBe(3);
    expect(Number.isFinite(r.log[2][1].vx)).toBe(true);
    expect(r.log[2][1].dx).toBe(60); // travel still tracks
  });
});

describe('termination', () => {
  it('reports a cancelled touch as terminate, not release', () => {
    const r = recorder({onStartShouldSetPanResponder: () => true});
    const {panHandlers: h} = PanResponder.create(r.config);

    down(h, 0, 0);
    move(h, 20, 0);
    cancel(h, 20, 0);

    expect(r.names()).toEqual(['grant', 'move', 'terminate']);
  });

  it('stays quiet when the gesture was never granted', () => {
    const r = recorder({onStartShouldSetPanResponder: () => false});
    const {panHandlers: h} = PanResponder.create(r.config);
    down(h, 0, 0);
    cancel(h, 0, 0);
    expect(r.names()).toEqual([]);
  });

  it('starts clean after a cancel', () => {
    const r = recorder({onStartShouldSetPanResponder: () => true});
    const {panHandlers: h} = PanResponder.create(r.config);

    down(h, 0, 0);
    move(h, 50, 0);
    cancel(h, 50, 0);
    down(h, 200, 200);
    move(h, 210, 200);

    expect(r.names()).toEqual(['grant', 'move', 'terminate', 'grant', 'move']);
    expect(r.log[4][1]).toMatchObject({
      dx: 10,
      x0: 200,
      numberActiveTouches: 1,
    });
  });
});

describe('a second finger', () => {
  it('joins the gesture in flight instead of restarting it, and the last lift ends it', () => {
    const r = recorder({onStartShouldSetPanResponder: () => true});
    const {panHandlers: h} = PanResponder.create(r.config);

    down(h, 100, 0);
    down(h, 300, 0); // second finger: no second grant, anchor unchanged
    expect(r.names()).toEqual(['grant']);

    move(h, 140, 0);
    expect(r.log[1][1]).toMatchObject({
      dx: 40,
      x0: 100,
      numberActiveTouches: 2,
    });

    up(h, 300, 0); // one finger up — the gesture continues
    expect(r.names()).toEqual(['grant', 'move']);

    up(h, 140, 0); // last finger up — now it releases
    expect(r.names()).toEqual(['grant', 'move', 'release']);
  });

  it('drops every finger on a cancel', () => {
    const r = recorder({onStartShouldSetPanResponder: () => true});
    const {panHandlers: h} = PanResponder.create(r.config);

    down(h, 0, 0);
    down(h, 50, 0);
    cancel(h, 0, 0);
    expect(r.names()).toEqual(['grant', 'terminate']);

    up(h, 50, 0); // the stale second lift must not fire a release
    expect(r.names()).toEqual(['grant', 'terminate']);
  });
});

describe('unsupported config', () => {
  it('warns once, naming the responder-negotiation keys it ignores', async () => {
    vi.resetModules(); // the warn-once latch is module state
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => {});
    const {PanResponder: Fresh} = await import('../PanResponder.js');

    Fresh.create({
      onStartShouldSetPanResponder: () => true,
      onMoveShouldSetPanResponderCapture: () => true,
      onPanResponderTerminationRequest: () => true,
    });
    Fresh.create({onShouldBlockNativeResponder: () => true});

    expect(warn).toHaveBeenCalledTimes(1);
    expect(warn.mock.calls[0][0]).toContain(
      'onMoveShouldSetPanResponderCapture',
    );
    expect(warn.mock.calls[0][0]).toContain('onPanResponderTerminationRequest');
    warn.mockRestore();
  });
});
