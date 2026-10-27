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

// A type-level FIXTURE, not a runtime test: `npm run typecheck` compiles it alongside index.d.ts.
// Compiling the declarations alone only proves they are well-formed — it cannot catch a signature that
// is well-formed but WRONG. This file uses the gesture surface the way an app does, so a `type` union
// that drifts from what the bridge actually delivers fails the build here.
//
// The event `type` strings below are the runtime's (k_event_names in native_ui_bridge.c). Comparing
// against a literal is the assertion: TS rejects `===` between types with no overlap, so a union that
// loses 'responderGrant' — or a query that wrongly claims to carry a `type` — breaks this file.
// No `console` here on purpose: the lib is ES2020 with no DOM, matching the device runtime.
import {View, PanResponder} from '../../src/embedded-react/index';
import type {
  ResponderEvent,
  ResponderQueryEvent,
  PanResponderGestureState,
  TouchPoint,
} from '../../src/embedded-react/index';

const seen: boolean[] = [];
const nums: number[] = [];

export function GestureUsage() {
  const pan = PanResponder.create({
    // Queries receive a bare touchpoint — no `type`, because the engine writes none.
    onStartShouldSetPanResponder: (e, g) =>
      e.x > 10 && g.numberActiveTouches === 1,
    onMoveShouldSetPanResponder: (e, g) =>
      Math.abs(g.dx) > 8 && Math.abs(e.dy) < 4,
    onStartShouldSetPanResponderCapture: () => false,
    onMoveShouldSetPanResponderCapture: () => false,
    onPanResponderTerminationRequest: () => false,

    // Lifecycle callbacks receive the engine's responder events.
    onPanResponderGrant: (e, g) => {
      seen.push(e.type === 'responderGrant');
      nums.push(g.x0, g.y0);
    },
    onPanResponderMove: (e, g) => {
      seen.push(e.type === 'responderMove');
      nums.push(g.dx, g.dy, g.vx, g.vy, g.moveX, g.moveY, g.stateID);
    },
    onPanResponderRelease: e => seen.push(e.type === 'responderRelease'),
    onPanResponderTerminate: e => seen.push(e.type === 'responderTerminate'),
    onPanResponderReject: e => seen.push(e.type === 'responderReject'),
    onPanResponderStart: (e, g) => {
      seen.push(e.type === 'responderGrant');
      nums.push(g.numberActiveTouches);
    },
    // The odd one out: a non-final finger's lift comes off the RAW touch stream.
    onPanResponderEnd: e => seen.push(e.type === 'touchEnd'),
  });

  // A non-object config is coerced rather than throwing, so calling with nothing must stay legal.
  PanResponder.create();

  return (
    <View
      style={{flex: 1}}
      {...pan.panHandlers}
      // The low-level responder props, wired directly.
      onStartShouldSetResponder={e => e.dx === 0}
      onMoveShouldSetResponder={e => Math.abs(e.dx) > 5}
      onResponderGrant={e => seen.push(e.type === 'responderGrant')}
      onResponderMove={e => {
        seen.push(e.type === 'responderMove');
        nums.push(e.dx, e.dy);
      }}
      onResponderRelease={e => seen.push(e.type === 'responderRelease')}
      onResponderReject={e => seen.push(e.type === 'responderReject')}
      onResponderTerminate={e => seen.push(e.type === 'responderTerminate')}
      onResponderTerminationRequest={() => true}
      // Raw touches still bubble alongside the responder system.
      onTouchStart={e => {
        seen.push(e.type === 'touchStart');
        nums.push(e.x, e.y);
      }}
      onTouchCancel={e => seen.push(e.type === 'touchCancel')}
    />
  );
}

// The exported type aliases must stay usable by name — apps annotate their own handlers with them.
export const typedQuery = (
  e: ResponderQueryEvent,
  g: PanResponderGestureState,
): boolean => e.dx > g.dx;
export const typedEvent = (e: ResponderEvent): string => e.type;
export const typedPoint = (p: TouchPoint): number => p.x + p.y + p.dx + p.dy;
