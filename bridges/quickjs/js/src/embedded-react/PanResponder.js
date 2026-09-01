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
// Scope (issue #115): the commonly used subset — should-set, grant, move, release, terminate. This is
// pure JS over the per-node `onTouch*` events, NOT the engine's C responder system, so there is no
// negotiation between two PanResponders: whichever node's handlers see the touch decides for itself.
// The `*Capture` variants, `onPanResponderReject`, `onPanResponderTerminationRequest` and
// `onShouldBlockNativeResponder` are the negotiation API and are not implemented; passing one warns
// rather than pretending to honour it.
//
// Three engine facts shape the rest:
//   • Touches BUBBLE. A handler on a container sees its children's touches, so one PanResponder on a
//     wrapper covers the whole subtree — no handlers on the children.
//   • `onTouchCancel` → `onPanResponderTerminate` is the hook for undoing what `onPanResponderGrant`
//     began. The engine raises it when the host reports a cancelled touch (a panel driver dropping the
//     sequence, the browser's touchcancel), and when a fresh touch-down arrives on a finger whose
//     previous sequence never ended — which is what keeps a lost touch-up from wedging the gesture.
//   • A ScrollView ancestor still auto-scrolls. Raw touch events bubble whatever the C responder is
//     doing, so a pan inside a scroller drives BOTH. Put the responder outside the ScrollView, or give
//     the scrolling axis to the scroller and use only the other one here.
//
// Flow A only. The AOT compiler (Flow B) only spreads compile-time-constant objects, so
// `{...pan.panHandlers}` fails the build with "AOT: a spread {...} on <View> is not supported"; write
// the `onTouch*` handlers out by hand there.

/** Per-instance gesture id, so two responders on screen at once are told apart. */
let s_nextStateID = 1;

/** True once the unsupported-config warning has been printed (once per process is enough). */
let s_warnedConfig = false;

/** Config keys this module acts on. Everything else is either RN negotiation or a typo. */
const SUPPORTED = [
  'onStartShouldSetPanResponder',
  'onMoveShouldSetPanResponder',
  'onPanResponderGrant',
  'onPanResponderMove',
  'onPanResponderRelease',
  'onPanResponderTerminate',
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

/** Warns ONCE about config keys that are RN's responder negotiation (or simply misspelt). */
function warnUnsupportedConfig(config) {
  if (s_warnedConfig) return;
  const names = Object.keys(config).filter(k => !SUPPORTED.includes(k));
  if (names.length === 0) return;
  s_warnedConfig = true;
  console.warn(
    `embedded-react: PanResponder ignores ${names.join(', ')} — this is the gesture subset built on ` +
      `onTouch* events, not the responder negotiation system (no capture phase, no reject/terminate ` +
      `request, no blocking the native responder). Supported: ${SUPPORTED.join(', ')}.`,
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
    /** Fingers currently on the panel. */
    numberActiveTouches: 0,
  };

  let granted = false;
  let lastMoveTime = 0;

  /** Runs a should-set callback; an absent one means "no", as in RN. */
  const ask = (name, e) =>
    typeof config[name] === 'function' &&
    config[name](e, gestureState) === true;

  const fire = (name, e) => {
    if (typeof config[name] === 'function') config[name](e, gestureState);
  };

  /**
   * Re-anchors the gesture at the current point and hands it over. RN does the same: a responder that
   * needed 10 px of slop to claim the pan starts its own dx/dy at zero from where it took over, so the
   * slop never shows up as a jump in the first move.
   */
  const grant = e => {
    granted = true;
    gestureState.x0 = e.x;
    gestureState.y0 = e.y;
    gestureState.dx = 0;
    gestureState.dy = 0;
    fire('onPanResponderGrant', e);
  };

  /** Folds one touch point into the travel fields. */
  const advance = e => {
    gestureState.moveX = e.x;
    gestureState.moveY = e.y;
    gestureState.dx = e.x - gestureState.x0;
    gestureState.dy = e.y - gestureState.y0;
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

  /** Ends the gesture: reports it if this responder owned it, then clears the state for the next one. */
  const finish = (e, callback) => {
    const wasGranted = granted;
    granted = false;
    // Fold the release point into the travel so `dx` is the gesture's true total even when the finger
    // lifted without a final move — but leave vx/vy at the last MOVE's reading. A fling ends with the
    // finger resting for a frame or two, and re-measuring across that would report zero and throw the
    // throw away.
    advance(e);
    if (wasGranted) fire(callback, e);
    resetGestureState(gestureState);
  };

  const panHandlers = {
    onTouchStart(e) {
      gestureState.numberActiveTouches += 1;
      // A second finger joins the gesture in flight rather than starting its own: the JS touch event
      // carries no finger id, so there is no way to track two independently. The first finger owns the
      // gesture and the last one to lift ends it.
      if (gestureState.numberActiveTouches > 1) return;

      resetGestureState(gestureState);
      gestureState.x0 = e.x;
      gestureState.y0 = e.y;
      gestureState.moveX = e.x;
      gestureState.moveY = e.y;
      lastMoveTime = now();

      if (ask('onStartShouldSetPanResponder', e)) grant(e);
    },

    onTouchMove(e) {
      if (gestureState.numberActiveTouches === 0) return;
      track(e);
      // The move that wins the gesture only grants it; the next one is the first onPanResponderMove
      // (as in RN, where grant and move are separate responder events).
      if (granted) fire('onPanResponderMove', e);
      else if (ask('onMoveShouldSetPanResponder', e)) grant(e);
    },

    onTouchEnd(e) {
      if (gestureState.numberActiveTouches === 0) return;
      gestureState.numberActiveTouches -= 1;
      if (gestureState.numberActiveTouches > 0) return; // another finger is still down
      finish(e, 'onPanResponderRelease');
    },

    onTouchCancel(e) {
      gestureState.numberActiveTouches = 0;
      finish(e, 'onPanResponderTerminate');
    },
  };

  return {panHandlers};
}

export const PanResponder = {create};
