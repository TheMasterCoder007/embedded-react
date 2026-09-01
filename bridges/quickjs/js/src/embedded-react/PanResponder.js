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

// PanResponder — the React Native analog for drags, swipes and flings.
//
// A raw `onTouchMove` carries an absolute point and nothing else, so every drag ends up re-deriving the
// same three things by hand: where the finger started, how far it has travelled, and how fast it is
// going. This module does that once. `create(config)` returns an object whose `panHandlers` you spread
// onto any component; each callback receives `(event, gestureState)` exactly as in RN:
//
//   const pan = useRef(PanResponder.create({
//     onStartShouldSetPanResponder: () => true,
//     onPanResponderMove: (e, g) => setX(g.dx),
//     onPanResponderRelease: (e, g) => (g.vx > 0.5 ? next() : settle()),
//   })).current;
//
//   <View {...pan.panHandlers} />
//
// The gesture rides the ENGINE's responder system (issue #115 shipped a JS-only precursor over the raw
// onTouch* events; this is the rebase). `panHandlers` is a bag of RN View responder props — the
// should-set predicates become native negotiation QUERIES (the engine calls them synchronously during
// hit-testing and the boolean answer decides who owns the gesture), and grant/move/release/terminate
// arrive as native responder events. That buys real arbitration:
//
//   • A granted PanResponder OWNS the gesture: a ScrollView ancestor no longer auto-scrolls under the
//     pan (auto-scroll only claims a gesture nobody owns), and it takes a slop-claimed scroll BACK the
//     moment `onMoveShouldSetPanResponder` answers true.
//   • Negotiation covers the subtree: the engine asks every node on the hit chain, capture phase
//     root→leaf first (`*Capture` config keys), then bubble leaf→root — so a responder on a wrapper
//     claims touches that land on its children, and an outer responder can pre-empt an inner one.
//   • Losing a negotiation is reported (`onPanResponderReject`), and a challenger asking for the
//     gesture is refusable (`onPanResponderTerminationRequest` — return false to keep it).
//   • `onPanResponderTerminate` is the undo hook for whatever `onPanResponderGrant` began: the engine
//     fires it when the touch sequence is cancelled, when a fresh touch-down arrives on a finger whose
//     previous sequence never ended, or when this responder yields to a challenger.
//
// Remaining limits: `onShouldBlockNativeResponder` is Android-specific and stays unsupported (holding
// the responder already blocks the ScrollView; the built-in adjustable-Arc drag deliberately wins over
// JS claimants). And the engine events carry no finger id, so multiple fingers fold into ONE gesture —
// `numberActiveTouches` counts them, extra fingers fire `onPanResponderStart`/`End`, and the last one
// to lift releases; two independent single-finger gestures need two responders on disjoint subtrees.
//
// Flow A only. The AOT compiler (Flow B) only spreads compile-time-constant objects, so
// `{...pan.panHandlers}` fails the build with "AOT: a spread {...} on <View> is not supported"; Flow B
// support is tracked in #176.

/** Per-instance gesture id, so two responders on screen at once are told apart. */
let s_nextStateID = 1;

/** Unsupported config keys already warned about (each unique key warns once per process). */
const s_warnedKeys = new Set();

/** Config keys this module acts on. Everything else is either RN-Android-specific or a typo. */
const SUPPORTED = [
  'onStartShouldSetPanResponder',
  'onStartShouldSetPanResponderCapture',
  'onMoveShouldSetPanResponder',
  'onMoveShouldSetPanResponderCapture',
  'onPanResponderGrant',
  'onPanResponderMove',
  'onPanResponderRelease',
  'onPanResponderTerminate',
  'onPanResponderTerminationRequest',
  'onPanResponderReject',
  'onPanResponderStart',
  'onPanResponderEnd',
];

/**
 * The engine's monotonic millisecond clock, used for velocity. `performance.now()` is installed by the
 * QuickJS runtime on every context (it is React's scheduler clock) and exists in the simulator and in
 * Node; `Date` is an opt-in intrinsic that device builds usually leave out, so it is never used here.
 * The clock only advances when the host ticks, so a dt of 0 is normal and leaves velocity untouched.
 */
const now = () =>
  typeof performance !== 'undefined' && typeof performance.now === 'function'
    ? performance.now()
    : 0;

/** Warns ONCE PER KEY about config keys this module does not act on. */
function warnUnsupportedConfig(config) {
  const names = Object.keys(config).filter(
    k => !SUPPORTED.includes(k) && !s_warnedKeys.has(k),
  );
  if (names.length === 0) return;
  for (const k of names) s_warnedKeys.add(k);
  console.warn(
    `embedded-react: PanResponder ignores ${names.join(', ')} — ` +
      `supported: ${SUPPORTED.join(', ')}.`,
  );
}

/**
 * Clears the travel/velocity fields between gestures. `numberActiveTouches` is deliberately left alone:
 * it counts fingers on the panel, which outlives the gesture when a second finger is still down.
 *
 * @param {object} g The gesture state to reset in place.
 */
function resetGestureState(g) {
  g.moveX = 0;
  g.moveY = 0;
  g.x0 = 0;
  g.y0 = 0;
  g.dx = 0;
  g.dy = 0;
  g.vx = 0;
  g.vy = 0;
}

/**
 * Builds a gesture recogniser. Call it ONCE per component and keep the result in a ref — the returned
 * handlers close over one mutable gestureState, so re-creating it per render throws the drag away.
 *
 * @param {object} [config] RN-shaped callbacks; see SUPPORTED. Every one is optional.
 * @returns {{panHandlers: object}} `panHandlers` to spread onto a component.
 */
export function create(config = {}) {
  warnUnsupportedConfig(config);

  // The single mutable object handed to every callback — same identity for the responder's lifetime,
  // as in RN, so a handler may stash it but must not expect a snapshot.
  const gestureState = {
    /** Identifies this responder; stable for its lifetime. */
    stateID: s_nextStateID++,
    /** Latest touch point. */
    moveX: 0,
    moveY: 0,
    /** Where the gesture was granted — the anchor dx/dy are measured from. */
    x0: 0,
    y0: 0,
    /** Travel since the grant, in pixels. */
    dx: 0,
    dy: 0,
    /** Velocity in pixels per millisecond. */
    vx: 0,
    vy: 0,
    /** Fingers currently on the panel (within this responder's subtree). */
    numberActiveTouches: 0,
  };

  let granted = false;
  let lastMoveTime = 0;
  // The engine measures e.dx/e.dy from TOUCH-DOWN; RN (and this module) measure g.dx/g.dy from the
  // GRANT — a responder that needed 10px of slop to claim starts its own travel at zero, so the slop
  // never shows up as a jump. The base is what e.dx read at the moment of the grant.
  let baseDx = 0;
  let baseDy = 0;

  const fire = (name, e) => {
    if (typeof config[name] === 'function') config[name](e, gestureState);
  };

  /** Folds one engine payload (e.x/e.y absolute, e.dx/e.dy from touch-down) into the travel fields. */
  const advance = e => {
    gestureState.moveX = e.x;
    gestureState.moveY = e.y;
    gestureState.dx = e.dx - baseDx;
    gestureState.dy = e.dy - baseDy;
  };

  /** A move: travel, plus a fresh velocity reading over the time since the last one. */
  const track = e => {
    const t = now();
    const dt = t - lastMoveTime;
    const prevDx = gestureState.dx;
    const prevDy = gestureState.dy;

    advance(e);

    // dt is 0 whenever two touches land inside one host frame (the clock only moves on er_tick), and
    // the division would be Infinity. Keeping the previous velocity is the honest answer: nothing new
    // has been measured yet.
    if (dt > 0) {
      gestureState.vx = (gestureState.dx - prevDx) / dt;
      gestureState.vy = (gestureState.dy - prevDy) / dt;
    }
    lastMoveTime = t;
  };

  /** Wraps a should-set predicate as an engine query: track the point, ask, absent means "no" (as in RN). */
  const query = name =>
    typeof config[name] === 'function'
      ? e => {
          if (!granted) advance(e);
          return config[name](e, gestureState) === true;
        }
      : undefined;

  /** Ends the gesture: reports it if this responder owned it, then clears the state for the next one. */
  const finish = (e, callback) => {
    const wasGranted = granted;
    granted = false;
    // Fold the final point into the travel so `dx` is the gesture's true total even when the finger
    // lifted without a final move — but leave vx/vy at the last MOVE's reading. A fling ends with the
    // finger resting for a frame or two, and re-measuring across that would report zero and throw the
    // throw away.
    advance(e);
    if (wasGranted) fire(callback, e);
    resetGestureState(gestureState);
    baseDx = 0;
    baseDy = 0;
  };

  const panHandlers = {
    // --- Raw touch bookkeeping (bubbles per finger): finger count + Start/End for extra fingers ---
    onTouchStart(e) {
      gestureState.numberActiveTouches += 1;
      // A later finger joins the gesture in flight (the engine events carry no finger id, so
      // independent tracking is impossible). Its own touch slot re-negotiates and re-grants this
      // node, and THAT grant fires onPanResponderStart — not this raw handler, which runs first.
      if (gestureState.numberActiveTouches > 1) return;
      resetGestureState(gestureState);
      baseDx = 0;
      baseDy = 0;
      gestureState.x0 = e.x;
      gestureState.y0 = e.y;
      gestureState.moveX = e.x;
      gestureState.moveY = e.y;
      lastMoveTime = now();
    },
    onTouchEnd(e) {
      if (gestureState.numberActiveTouches > 0)
        gestureState.numberActiveTouches -= 1;
      // A finger lifted but the gesture continues; the RELEASE for the last one arrives as a
      // responder event below.
      if (granted && gestureState.numberActiveTouches > 0)
        fire('onPanResponderEnd', e);
    },
    onTouchCancel() {
      gestureState.numberActiveTouches = 0;
    },

    // --- Negotiation queries (the engine calls these; returning true claims the gesture) ---
    onStartShouldSetResponder: query('onStartShouldSetPanResponder'),
    onStartShouldSetResponderCapture: query(
      'onStartShouldSetPanResponderCapture',
    ),
    onMoveShouldSetResponder: query('onMoveShouldSetPanResponder'),
    onMoveShouldSetResponderCapture: query(
      'onMoveShouldSetPanResponderCapture',
    ),
    onResponderTerminationRequest:
      typeof config.onPanResponderTerminationRequest === 'function'
        ? e => config.onPanResponderTerminationRequest(e, gestureState) === true
        : undefined, // absent → the engine's default: yield (RN's default too)

    // --- Responder lifecycle (native events; fire only on the node that owns the gesture) ---
    onResponderGrant(e) {
      if (granted) {
        // A second finger's touch slot granted the same node: one gesture, not two. RN calls this a
        // responder "start" for the extra touch.
        fire('onPanResponderStart', e);
        return;
      }
      granted = true;
      baseDx = e.dx;
      baseDy = e.dy;
      gestureState.x0 = e.x;
      gestureState.y0 = e.y;
      gestureState.dx = 0;
      gestureState.dy = 0;
      lastMoveTime = now();
      fire('onPanResponderGrant', e);
    },
    onResponderMove(e) {
      if (!granted) return;
      track(e);
      fire('onPanResponderMove', e);
    },
    onResponderRelease(e) {
      if (!granted) return;
      // Another finger still down: its touch slot keeps the responder, so the gesture continues (the
      // raw onTouchEnd above already reported the End).
      if (gestureState.numberActiveTouches > 0) return;
      finish(e, 'onPanResponderRelease');
    },
    onResponderTerminate(e) {
      if (!granted) return;
      gestureState.numberActiveTouches = 0;
      finish(e, 'onPanResponderTerminate');
    },
    onResponderReject(e) {
      fire('onPanResponderReject', e);
    },
  };

  // Absent queries must not appear as props at all (an undefined on* prop is skipped by the host
  // config anyway, but a clean bag is easier to reason about and to spread).
  for (const k of Object.keys(panHandlers)) {
    if (panHandlers[k] === undefined) delete panHandlers[k];
  }

  return {panHandlers};
}

export const PanResponder = {create};
