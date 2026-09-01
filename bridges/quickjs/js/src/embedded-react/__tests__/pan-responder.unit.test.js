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

// PanResponder's panHandlers are the RN responder props — negotiation queries plus native responder
// events — so this test drives them in the ENGINE's dispatch order (hit_test.c): touch-down bubbles
// the raw touchStart, then negotiation asks the should-set queries and grants; each move dispatches
// responderMove to the owner then re-negotiates; touch-up bubbles touchEnd then responderRelease.
// The events carry engine payloads: x/y absolute, dx/dy CUMULATIVE FROM TOUCH-DOWN (the module
// re-anchors them at the grant). The real engine path is covered by pan-responder.runtime.test.jsx;
// this file pins the state machine. Velocity reads performance.now() — the engine's clock on device —
// so the tests drive a stub of it.

const realPerformance = globalThis.performance;
let clock = 0;

beforeEach(() => {
  clock = 0;
  globalThis.performance = {now: () => clock};
});

afterEach(() => {
  globalThis.performance = realPerformance;
});

/**
 * Drives one responder's panHandlers the way hit_test.c dispatches for a single-node scene: raw
 * touch bubbling, then negotiation (capture before bubble), then the responder events. `start` is
 * remembered so move/up events carry the engine's cumulative dx/dy.
 */
function engineSim(handlers) {
  let startX = 0;
  let startY = 0;
  let granted = false;
  const ev = (type, x, y) => ({type, x, y, dx: x - startX, dy: y - startY});

  return {
    down(x, y) {
      startX = x;
      startY = y;
      handlers.onTouchStart?.(ev('touchStart', x, y));
      if (granted) return; // a later finger: negotiation re-grants below via downJoin
      const e = ev('', x, y);
      if (
        handlers.onStartShouldSetResponderCapture?.(e) === true ||
        handlers.onStartShouldSetResponder?.(e) === true
      ) {
        granted = true;
        handlers.onResponderGrant?.(ev('responderGrant', x, y));
      }
    },
    /** A later finger's touch-down: its own slot re-negotiates and re-grants the same node. */
    downJoin(x, y) {
      handlers.onTouchStart?.(ev('touchStart', x, y));
      if (
        handlers.onStartShouldSetResponderCapture?.(ev('', x, y)) === true ||
        handlers.onStartShouldSetResponder?.(ev('', x, y)) === true
      ) {
        handlers.onResponderGrant?.(ev('responderGrant', x, y));
      }
    },
    move(x, y) {
      if (granted) {
        handlers.onResponderMove?.(ev('responderMove', x, y));
      } else if (handlers.onMoveShouldSetResponder?.(ev('', x, y)) === true) {
        granted = true;
        handlers.onResponderGrant?.(ev('responderGrant', x, y));
      }
    },
    up(x, y) {
      handlers.onTouchEnd?.(ev('touchEnd', x, y));
      if (granted) handlers.onResponderRelease?.(ev('responderRelease', x, y));
      granted = false;
    },
    /** A non-final finger's lift: raw touchEnd bubbles, and ITS slot releases the shared node. */
    upJoin(x, y) {
      handlers.onTouchEnd?.(ev('touchEnd', x, y));
      handlers.onResponderRelease?.(ev('responderRelease', x, y));
    },
    cancel(x, y) {
      handlers.onTouchCancel?.(ev('touchCancel', x, y));
      if (granted)
        handlers.onResponderTerminate?.(ev('responderTerminate', x, y));
      granted = false;
    },
  };
}

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
      onPanResponderReject: tap('reject'),
      onPanResponderStart: tap('start'),
      onPanResponderEnd: tap('end'),
      ...extra,
    },
  };
}

describe('PanResponder.create', () => {
  it('emits the responder props for the callbacks supplied, and no phantom queries', () => {
    const {panHandlers: full} = PanResponder.create({
      onStartShouldSetPanResponder: () => true,
      onMoveShouldSetPanResponder: () => true,
      onStartShouldSetPanResponderCapture: () => true,
      onMoveShouldSetPanResponderCapture: () => true,
      onPanResponderTerminationRequest: () => false,
    });
    expect(Object.keys(full).sort()).toEqual([
      'onMoveShouldSetResponder',
      'onMoveShouldSetResponderCapture',
      'onResponderGrant',
      'onResponderMove',
      'onResponderReject',
      'onResponderRelease',
      'onResponderTerminate',
      'onResponderTerminationRequest',
      'onStartShouldSetResponder',
      'onStartShouldSetResponderCapture',
      'onTouchCancel',
      'onTouchEnd',
      'onTouchStart',
    ]);

    // With no should-set config, no query props exist at all — the engine is never asked, so the
    // responder can never grant. That absence IS the "never grants" mechanism.
    const {panHandlers: bare} = PanResponder.create({});
    expect(Object.keys(bare)).not.toContain('onStartShouldSetResponder');
    expect(Object.keys(bare)).not.toContain('onMoveShouldSetResponder');
    expect(Object.keys(bare)).not.toContain('onResponderTerminationRequest');
  });

  it('gives each responder its own stateID', () => {
    const seen = [];
    const make = () =>
      engineSim(
        PanResponder.create({
          onStartShouldSetPanResponder: (e, g) => {
            seen.push(g.stateID);
            return true;
          },
        }).panHandlers,
      ).down(0, 0);
    make();
    make();
    expect(seen).toHaveLength(2);
    expect(seen[0]).not.toBe(seen[1]);
  });

  it('passes the termination-request answer through as a strict boolean', () => {
    let asked = 0;
    const {panHandlers} = PanResponder.create({
      onStartShouldSetPanResponder: () => true,
      onPanResponderTerminationRequest: () => {
        asked++;
        return false;
      },
    });
    expect(
      panHandlers.onResponderTerminationRequest({x: 0, y: 0, dx: 0, dy: 0}),
    ).toBe(false);
    expect(asked).toBe(1);
    // Truthy-but-not-true does not yield (mirrors the should-set === true discipline).
    const loose = PanResponder.create({
      onPanResponderTerminationRequest: () => 1,
    }).panHandlers;
    expect(
      loose.onResponderTerminationRequest({x: 0, y: 0, dx: 0, dy: 0}),
    ).toBe(false);
  });
});

describe('start-should-set gestures', () => {
  it('grants on touch-down, then reports travel per move and on release', () => {
    const r = recorder({onStartShouldSetPanResponder: () => true});
    const sim = engineSim(PanResponder.create(r.config).panHandlers);

    sim.down(100, 50);
    sim.move(130, 60);
    sim.move(160, 90);
    sim.up(160, 90);

    expect(r.names()).toEqual(['grant', 'move', 'move', 'release']);
    expect(r.log[1][1]).toMatchObject({dx: 30, dy: 10, x0: 100, y0: 50});
    expect(r.log[2][1]).toMatchObject({dx: 60, dy: 40, moveX: 160, moveY: 90});
    expect(r.log[3][1]).toMatchObject({dx: 60, dy: 40});
  });

  it('folds the release point in, so dx is the full travel with no final move', () => {
    const r = recorder({onStartShouldSetPanResponder: () => true});
    const sim = engineSim(PanResponder.create(r.config).panHandlers);

    sim.down(0, 0);
    sim.up(45, -12);

    expect(r.names()).toEqual(['grant', 'release']);
    expect(r.log[1][1]).toMatchObject({dx: 45, dy: -12});
  });

  it('resets the gesture state between gestures', () => {
    const r = recorder({onStartShouldSetPanResponder: () => true});
    const sim = engineSim(PanResponder.create(r.config).panHandlers);

    sim.down(0, 0);
    sim.move(80, 80);
    sim.up(80, 80);

    sim.down(5, 5);
    sim.move(15, 5);

    const last = r.log[r.log.length - 1][1];
    expect(last).toMatchObject({dx: 10, dy: 0, x0: 5, y0: 5});
  });

  it('does not grant when the predicate returns false', () => {
    const r = recorder({onStartShouldSetPanResponder: () => false});
    const sim = engineSim(PanResponder.create(r.config).panHandlers);
    sim.down(0, 0);
    sim.move(40, 0);
    sim.up(40, 0);
    expect(r.names()).toEqual([]);
  });

  it('routes the capture predicate to the capture query prop', () => {
    const r = recorder({onStartShouldSetPanResponderCapture: () => true});
    const {panHandlers} = PanResponder.create(r.config);
    expect(typeof panHandlers.onStartShouldSetResponderCapture).toBe(
      'function',
    );
    expect(panHandlers.onStartShouldSetResponder).toBeUndefined();
    engineSim(panHandlers).down(10, 10);
    expect(r.names()).toEqual(['grant']);
  });
});

describe('move-should-set gestures', () => {
  it('claims the pan past the slop and re-anchors dx/dy at the grant point', () => {
    const r = recorder({
      onMoveShouldSetPanResponder: (e, g) => Math.abs(g.dx) > 10,
    });
    const sim = engineSim(PanResponder.create(r.config).panHandlers);

    sim.down(100, 0);
    sim.move(105, 0); // inside the slop — nothing yet
    expect(r.names()).toEqual([]);

    sim.move(120, 0); // crosses it: the engine grants; travel re-anchors here
    expect(r.names()).toEqual(['grant']);
    expect(r.log[0][1]).toMatchObject({dx: 0, dy: 0, x0: 120, y0: 0});

    sim.move(130, 0); // first real move, measured from the grant point
    expect(r.names()).toEqual(['grant', 'move']);
    expect(r.log[1][1]).toMatchObject({dx: 10});
  });
});

describe('velocity', () => {
  it('reports pixels per millisecond from the last move', () => {
    const r = recorder({onStartShouldSetPanResponder: () => true});
    const sim = engineSim(PanResponder.create(r.config).panHandlers);

    sim.down(0, 0);
    clock = 20;
    sim.move(40, 10); // 40px / 20ms, 10px / 20ms
    expect(r.log[1][1]).toMatchObject({vx: 2, vy: 0.5});

    clock = 30;
    sim.move(45, 10); // 5px / 10ms
    expect(r.log[2][1]).toMatchObject({vx: 0.5, vy: 0});
  });

  it('keeps the fling speed on release instead of re-measuring across the lift', () => {
    const r = recorder({onStartShouldSetPanResponder: () => true});
    const sim = engineSim(PanResponder.create(r.config).panHandlers);

    sim.down(0, 0);
    clock = 10;
    sim.move(40, 0); // thrown at 4 px/ms
    clock = 26; // a frame passes with the finger resting where it landed
    sim.up(40, 0);

    expect(r.log[2][1].vx).toBe(4);
    expect(r.log[2][1].dx).toBe(40);
  });

  it('keeps the last velocity when the clock has not advanced (no Infinity/NaN)', () => {
    const r = recorder({onStartShouldSetPanResponder: () => true});
    const sim = engineSim(PanResponder.create(r.config).panHandlers);

    sim.down(0, 0);
    clock = 10;
    sim.move(30, 0);
    expect(r.log[1][1].vx).toBe(3);

    sim.move(60, 0); // same frame: dt === 0
    expect(r.log[2][1].vx).toBe(3);
    expect(Number.isFinite(r.log[2][1].vx)).toBe(true);
    expect(r.log[2][1].dx).toBe(60);
  });
});

describe('termination and rejection', () => {
  it('reports a cancelled touch as terminate, not release', () => {
    const r = recorder({onStartShouldSetPanResponder: () => true});
    const sim = engineSim(PanResponder.create(r.config).panHandlers);

    sim.down(0, 0);
    sim.move(20, 0);
    sim.cancel(20, 0);

    expect(r.names()).toEqual(['grant', 'move', 'terminate']);
  });

  it('starts clean after a terminate', () => {
    const r = recorder({onStartShouldSetPanResponder: () => true});
    const sim = engineSim(PanResponder.create(r.config).panHandlers);

    sim.down(0, 0);
    sim.move(50, 0);
    sim.cancel(50, 0);
    sim.down(200, 200);
    sim.move(210, 200);

    expect(r.names()).toEqual(['grant', 'move', 'terminate', 'grant', 'move']);
    expect(r.log[4][1]).toMatchObject({
      dx: 10,
      x0: 200,
      numberActiveTouches: 1,
    });
  });

  it('forwards a rejection to onPanResponderReject', () => {
    const r = recorder({onMoveShouldSetPanResponder: () => true});
    const {panHandlers} = PanResponder.create(r.config);
    panHandlers.onResponderReject({
      type: 'responderReject',
      x: 5,
      y: 5,
      dx: 0,
      dy: 0,
    });
    expect(r.names()).toEqual(['reject']);
  });
});

describe('multiple fingers', () => {
  it('folds a later finger into the gesture: start on join, end on its lift, release on the last', () => {
    const r = recorder({onStartShouldSetPanResponder: () => true});
    const sim = engineSim(PanResponder.create(r.config).panHandlers);

    sim.down(100, 0); // finger A → grant
    sim.downJoin(300, 0); // finger B joins → its slot re-grants → start
    expect(r.names()).toEqual(['grant', 'start']);
    expect(r.log[1][1].numberActiveTouches).toBe(2);

    sim.move(140, 0);
    sim.upJoin(300, 0); // finger B lifts → end; the gesture continues
    expect(r.names()).toEqual(['grant', 'start', 'move', 'end']);

    sim.up(140, 0); // last finger → release
    expect(r.names()).toEqual(['grant', 'start', 'move', 'end', 'release']);
    expect(r.log[4][1].numberActiveTouches).toBe(0);
  });
});

describe('unsupported config', () => {
  it('warns once per unknown key, so a second responder with a NEW bad key still warns', async () => {
    vi.resetModules(); // the per-key warn latch is module state
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => {});
    const {PanResponder: Fresh} = await import('../PanResponder.js');

    Fresh.create({onShouldBlockNativeResponder: () => true});
    Fresh.create({onShouldBlockNativeResponder: () => true}); // repeat: no second warning
    Fresh.create({onPanRespnoderMove: () => {}}); // a typo: NEW key, must warn

    expect(warn).toHaveBeenCalledTimes(2);
    expect(warn.mock.calls[0][0]).toContain('onShouldBlockNativeResponder');
    expect(warn.mock.calls[1][0]).toContain('onPanRespnoderMove');
    warn.mockRestore();
  });
});
