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
// No `console` here on purpose: the lib is ES2023 with no DOM, matching the device runtime.
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
    onStartShouldSetPanResponder: (event, gestureState) =>
      event.x > 10 && gestureState.numberActiveTouches === 1,
    onMoveShouldSetPanResponder: (event, gestureState) =>
      Math.abs(gestureState.dx) > 8 && Math.abs(event.dy) < 4,
    onStartShouldSetPanResponderCapture: () => false,
    onMoveShouldSetPanResponderCapture: () => false,
    onPanResponderTerminationRequest: () => false,

    // Lifecycle callbacks receive the engine's responder events.
    onPanResponderGrant: (event, gestureState) => {
      seen.push(event.type === 'responderGrant');
      nums.push(gestureState.x0, gestureState.y0);
    },
    onPanResponderMove: (event, gestureState) => {
      seen.push(event.type === 'responderMove');
      nums.push(
        gestureState.dx,
        gestureState.dy,
        gestureState.vx,
        gestureState.vy,
        gestureState.moveX,
        gestureState.moveY,
        gestureState.stateID,
      );
    },
    onPanResponderRelease: event =>
      seen.push(event.type === 'responderRelease'),
    onPanResponderTerminate: event =>
      seen.push(event.type === 'responderTerminate'),
    onPanResponderReject: event => seen.push(event.type === 'responderReject'),
    onPanResponderStart: (event, gestureState) => {
      seen.push(event.type === 'responderGrant');
      nums.push(gestureState.numberActiveTouches);
    },
    // The odd one out: a non-final finger's lift comes off the RAW touch stream.
    onPanResponderEnd: event => seen.push(event.type === 'touchEnd'),
  });

  // A non-object config is coerced rather than throwing, so calling with nothing must stay legal.
  PanResponder.create();

  return (
    <View
      style={{flex: 1}}
      {...pan.panHandlers}
      // The low-level responder props, wired directly.
      onStartShouldSetResponder={event => event.dx === 0}
      onMoveShouldSetResponder={event => Math.abs(event.dx) > 5}
      onResponderGrant={event => seen.push(event.type === 'responderGrant')}
      onResponderMove={event => {
        seen.push(event.type === 'responderMove');
        nums.push(event.dx, event.dy);
      }}
      onResponderRelease={event => seen.push(event.type === 'responderRelease')}
      onResponderReject={event => seen.push(event.type === 'responderReject')}
      onResponderTerminate={event =>
        seen.push(event.type === 'responderTerminate')
      }
      onResponderTerminationRequest={() => true}
      // Raw touches still bubble alongside the responder system.
      onTouchStart={event => {
        seen.push(event.type === 'touchStart');
        nums.push(event.x, event.y);
      }}
      onTouchCancel={event => seen.push(event.type === 'touchCancel')}
    />
  );
}

// The exported type aliases must stay usable by name — apps annotate their own handlers with them.
export const typedQuery = (
  event: ResponderQueryEvent,
  gestureState: PanResponderGestureState,
): boolean => event.dx > gestureState.dx;
export const typedEvent = (event: ResponderEvent): string => event.type;
export const typedPoint = (point: TouchPoint): number =>
  point.x + point.y + point.dx + point.dy;
