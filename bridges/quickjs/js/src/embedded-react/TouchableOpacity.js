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

// TouchableOpacity — RN's press-to-dim wrapper, as a plain component over <Pressable>. No engine node
// of its own:
//
//   <TouchableOpacity style={styles.tile} onPress={open}>…</TouchableOpacity>
//     renders the same tree as
//   <Pressable style={[styles.tile, {opacity: dim}]} onPressIn={…} onPressOut={…}>…</Pressable>
//
// The fade is a native-driver animation, not a re-render: the two press handlers hand the engine a
// target opacity and a duration, and it runs the ramp in C (er_anim_value_*). Nothing re-enters JS per
// frame, and the subtree React just built is not touched at all.
//
// Worth knowing before wrapping a whole screen in one: an opacity below 1 makes the Pressable an
// OPACITY GROUP, so the engine composites its entire subtree through an off-screen strip for as long
// as the fade lasts. That is exactly what makes the label dim with its button — but it costs in
// proportion to the dimmed area, so dim the box that reads as the button, not the page around it.
import {
  createElement,
  forwardRef,
  useCallback,
  useEffect,
  useMemo,
  useRef,
} from 'react';
import {Pressable} from './components.js';
import {timing, useAnimatedValue} from './Animated.js';
import {Easing} from './Easing.js';
import {flattenStyleObj} from '../props.js';

// RN's own numbers (Libraries/Components/Touchable/TouchableOpacity.js): the dim lands on touch-down
// with no ramp at all, and fades back over a quarter second once the finger lifts.
const DEFAULT_ACTIVE_OPACITY = 0.2;
const DIM_MS = 0;
const RESTORE_MS = 250;

let warnedAnimatedOpacity = false;

/** The opacity the touchable rests at — whatever its style asks for, fully opaque if it asks nothing. */
function restingOpacity(style) {
  const o = flattenStyleObj(style).opacity;
  if (o && o.__animated) {
    if (!warnedAnimatedOpacity) {
      warnedAnimatedOpacity = true;
      console.warn(
        'embedded-react: <TouchableOpacity> ignores an animated `opacity` in its style — the press ' +
          'feedback owns that property. To animate opacity yourself, use <Pressable>: the dim is just ' +
          'onPressIn/onPressOut driving an Animated.Value.',
      );
    }
    return 1;
  }
  return typeof o === 'number' ? o : 1;
}

/** Runs `value` to `to` over `ms` on the native driver (a 0 ms ramp snaps, without taking a slot). */
function fadeTo(value, to, ms) {
  timing(value, {toValue: to, duration: ms, easing: Easing.quadInOut}).start();
}

/**
 * A press target that dims while it is held.
 *
 * Every <Pressable> prop works here unchanged — this IS a Pressable, plus the feedback — so the two are
 * interchangeable and the choice is only whether you want the dim.
 *
 * @param {object} props Pressable props, plus the two below.
 * @param {number} [props.activeOpacity] Opacity to dim to while held. Defaults to RN's 0.2.
 * @param {boolean} [props.disabled] Stops it responding to presses, and stops it dimming with them.
 * @returns {*} A <Pressable> whose opacity is bound to the press animation.
 */
export const TouchableOpacity = forwardRef(
  function TouchableOpacity(props, ref) {
    const {
      activeOpacity = DEFAULT_ACTIVE_OPACITY,
      disabled,
      style,
      onPress,
      onLongPress,
      onPressIn,
      onPressOut,
      ...rest
    } = props;

    // Both of these walk the style, so they are keyed to its identity rather than redone every render —
    // a StyleSheet entry (or any hoisted object) makes that a pointer compare. Memoizing the composed
    // array also keeps the reconciler's style diff a pointer compare instead of a deep walk.
    const resting = useMemo(() => restingOpacity(style), [style]);
    const opacity = useAnimatedValue(resting);
    const composedStyle = useMemo(() => [style, {opacity}], [style, opacity]);

    // Only read by the resting-opacity sync below: a press whose node unmounts mid-touch never sees its
    // press-out, but the animated value goes with the node, so nothing has to unwind.
    const pressed = useRef(false);

    const handlePressIn = useCallback(
      event => {
        pressed.current = true;
        fadeTo(opacity, activeOpacity, DIM_MS);
        if (onPressIn) onPressIn(event);
      },
      [opacity, activeOpacity, onPressIn],
    );

    const handlePressOut = useCallback(
      event => {
        pressed.current = false;
        fadeTo(opacity, resting, RESTORE_MS);
        if (onPressOut) onPressOut(event);
      },
      [opacity, resting, onPressOut],
    );

    // The binding owns the node's opacity from the first commit — buildProps no longer carries it — so a
    // style that CHANGES its opacity would otherwise never reach the node. Skipped while the finger is
    // down: the dim owns the value until the press-out, which already restores to the latest resting one.
    const synced = useRef(resting);
    useEffect(() => {
      if (synced.current === resting) return;
      synced.current = resting;
      if (!pressed.current) opacity.setValue(resting);
    }, [opacity, resting]);

    return createElement(Pressable, {
      ...rest,
      ref,
      style: composedStyle,
      disabled,
      // `disabled` here is RN's: no press, and no feedback with it. Withholding the handlers is what
      // actually does that — the engine has no such node flag, so a Pressable left holding onPress still
      // fires (the same reason <Button> withholds its own).
      onPress: disabled ? undefined : onPress,
      onLongPress: disabled ? undefined : onLongPress,
      onPressIn: disabled ? undefined : handlePressIn,
      onPressOut: disabled ? undefined : handlePressOut,
    });
  },
);
