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

// Animated — the React Native analog, backed by the engine's native-driver value system
// (er_anim_value_*). An Animated.Value is a handle to an engine-side float; binding it to a node
// prop (via Animated.View) lets the engine advance the animation each frame with NO per-frame JS.
import {createElement, useRef, useEffect} from 'react';
import {NativeUI} from '../native-ui.js';
import {splitAnimatedStyle} from './split-style.js';

/** A standalone animatable value bound to an engine-side float. */
export class AnimatedValue {
  constructor(initial = 0) {
    this.__animated = true;
    this._handle = NativeUI.animValueCreate(initial);
    this._value = initial;
    this._destroyed = false;
  }

  setValue(v) {
    this._value = v;
    NativeUI.animValueSet(this._handle, v);
  }

  /**
   * Releases the engine-side value slot. The engine's value pool is fixed-size (unlike RN, where the
   * JS value is simply garbage-collected), so a value tied to a mounting/unmounting component must be
   * freed explicitly — see useAnimatedValue. Idempotent; the value must not be used after destroy().
   */
  destroy() {
    if (this._destroyed) return;
    this._destroyed = true;
    NativeUI.animValueDestroy(this._handle);
  }

  __getValue() {
    return NativeUI.animValueGet(this._handle);
  }

  __bind(node, prop) {
    // The engine applies the current value on bind and ignores duplicate (node, prop) pairs, so
    // this is safe to call on every render.
    NativeUI.animValueBind(this._handle, node, prop);
  }

  interpolate(config) {
    return new AnimatedInterpolation(this, config);
  }
}

/**
 * A value derived from a parent Animated.Value through a piecewise-linear interpolation.
 * config = { inputRange, outputRange, extrapolate?, extrapolateLeft?, extrapolateRight? }; the
 * extrapolate tokens ('extend' | 'clamp' | 'identity') control behavior outside the input range.
 */
export class AnimatedInterpolation {
  constructor(parent, config) {
    this.__animated = true;
    this._parent = parent;
    this._config = config;
  }

  __bind(node, prop) {
    NativeUI.animValueBindInterpolated(
      this._parent._handle,
      node,
      prop,
      this._config,
    );
  }

  interpolate(config) {
    // Chained interpolation isn't supported by the engine; re-map against the same parent.
    return new AnimatedInterpolation(this._parent, config);
  }
}

/** Wraps fn so it runs at most once, regardless of how many times it is invoked. */
function once(fn) {
  let called = false;
  return arg => {
    if (called) return;
    called = true;
    if (fn) fn(arg);
  };
}

/**
 * Wraps a NativeUI animation call in an object with start()/stop() (RN's animation handle shape).
 *
 * start(onComplete) wires the engine's completion callback through the bridge: onComplete is called
 * with `{ finished }` when the animation ends naturally (finished: true) or is interrupted by a new
 * animation / stop() (finished: false). `_value` is exposed so Animated.loop can reset it between
 * iterations.
 */
function makeAnimation(value, toValue, config) {
  let handle = 0;
  return {
    _value: value,
    start(onComplete) {
      // Always pass a wrapper so we can null out the (possibly recycled) handle on completion.
      const cb = finished => {
        handle = 0;
        if (onComplete) onComplete({finished: !!finished});
      };
      handle = NativeUI.animValueAnimate(value._handle, toValue, config, cb);
    },
    stop() {
      if (handle) NativeUI.animStop(handle);
    },
  };
}

export function timing(value, config = {}) {
  const {toValue = 0, useNativeDriver, ...rest} = config;
  return makeAnimation(value, toValue, {type: 'timing', ...rest});
}

export function spring(value, config = {}) {
  const {toValue = 0, useNativeDriver, ...rest} = config;
  return makeAnimation(value, toValue, {type: 'spring', ...rest});
}

export function decay(value, config = {}) {
  const {useNativeDriver, ...rest} = config;
  // Decay coasts from its velocity; the engine ignores the target, so pass the current value.
  return makeAnimation(value, value.__getValue(), {type: 'decay', ...rest});
}

// --- Composition -------------------------------------------------------------------------------
// These are pure JS over each child animation's start()/stop() + the §2 timer globals. The engine
// deactivates a finished animation before firing its completion callback, so chaining the next
// animation from inside a completion handler is safe (no per-frame JS — each child still runs in C).

/** Runs animations one after another; each starts when the previous finishes. */
export function sequence(animations) {
  let current = 0;
  let stopped = false;
  return {
    start(onComplete) {
      // Reset the per-run state so the sequence is restartable — e.g., each iteration of Animated.loop calls
      // start() again. Without this, `current` stays at animations.length from the previous run, so the next
      // start() completes instantly and the loop re-enters synchronously → stack overflow.
      current = 0;
      stopped = false;
      const done = once(onComplete);
      const next = result => {
        if (stopped || !result || result.finished === false) {
          done({finished: false});
          return;
        }
        if (current >= animations.length) {
          done({finished: true});
          return;
        }
        animations[current++].start(next);
      };
      next({finished: true}); // kick off the first entry
    },
    stop() {
      stopped = true;
      if (current > 0 && current <= animations.length)
        animations[current - 1].stop();
    },
  };
}

/** Runs animations simultaneously; completes when all finish. stopTogether (default true). */
export function parallel(animations, config) {
  const stopTogether = !config || config.stopTogether !== false;
  let stopped = false;
  return {
    start(onComplete) {
      const done = once(onComplete);
      const total = animations.length;
      if (total === 0) {
        done({finished: true});
        return;
      }
      let doneCount = 0;
      let anyUnfinished = false;
      const onChild = result => {
        doneCount++;
        if (!result || result.finished === false) {
          anyUnfinished = true;
          if (stopTogether && !stopped) {
            stopped = true;
            for (const a of animations) a.stop();
          }
        }
        if (doneCount >= total) done({finished: !anyUnfinished});
      };
      for (const a of animations) a.start(onChild);
    },
    stop() {
      stopped = true;
      for (const a of animations) a.stop();
    },
  };
}

/** Like parallel, but each animation starts `delay` ms after the previous one (uses setTimeout). */
export function stagger(delay, animations) {
  let stopped = false;
  const timers = [];
  return {
    start(onComplete) {
      const done = once(onComplete);
      const total = animations.length;
      if (total === 0) {
        done({finished: true});
        return;
      }
      let doneCount = 0;
      let anyUnfinished = false;
      const onChild = result => {
        doneCount++;
        if (!result || result.finished === false) anyUnfinished = true;
        if (doneCount >= total) done({finished: !anyUnfinished});
      };
      animations.forEach((a, i) => {
        if (i === 0) {
          a.start(onChild);
        } else {
          timers.push(
            setTimeout(() => {
              if (!stopped) a.start(onChild);
            }, delay * i),
          );
        }
      });
    },
    stop() {
      stopped = true;
      for (const t of timers) clearTimeout(t);
      for (const a of animations) a.stop();
    },
  };
}

/** An animation that simply waits `time` ms — useful as a spacer inside sequence (uses setTimeout). */
export function delay(time) {
  let timer = 0;
  return {
    start(onComplete) {
      const done = once(onComplete);
      timer = setTimeout(() => {
        timer = 0;
        done({finished: true});
      }, time);
    },
    stop() {
      if (timer) {
        clearTimeout(timer);
        timer = 0;
      }
    },
  };
}

/**
 * Repeats an animation. iterations: -1 (default) loops forever; otherwise that many times.
 * By default the underlying value is reset to its loop-start position before each iteration
 * (resetBeforeIteration), matching RN — so a timing 0→1 repeats 0→1 rather than ping-ponging.
 */
export function loop(animation, config) {
  const iterations =
    config && Number.isInteger(config.iterations) ? config.iterations : -1;
  const resetBeforeIteration = !config || config.resetBeforeIteration !== false;
  let stopped = false;
  return {
    _value: animation._value,
    start(onComplete) {
      const done = once(onComplete);
      const startValue =
        resetBeforeIteration && animation._value
          ? animation._value.__getValue()
          : null;
      let count = 0;
      const startIteration = () => {
        if (stopped) {
          done({finished: false});
          return;
        }
        if (iterations >= 0 && count >= iterations) {
          done({finished: true});
          return;
        }
        count++;
        if (
          resetBeforeIteration &&
          animation._value &&
          startValue != null &&
          count > 1
        ) {
          animation._value.setValue(startValue);
        }
        animation.start(onIterationDone);
      };
      const onIterationDone = result => {
        if (stopped || !result || result.finished === false) {
          done({finished: false});
          return;
        }
        // Defer the next iteration to a fresh task instead of starting it inline. A child animation can
        // complete *synchronously* (a long-duration timing finishing inside one large catch-up frame in
        // the simulator), and starting the next iteration from within, that completion callback would
        // recurse — loop → child → completion → loop → … — until the stack overflows. setTimeout breaks
        // the chain: the host pump runs it on the next frame, by which point real time has advanced so
        // the animation runs its full duration again. (Negligible cost in the normal async case.)
        setTimeout(startIteration, 0);
      };
      startIteration();
    },
    stop() {
      stopped = true;
      animation.stop();
    },
  };
}

/**
 * Returns a stable Animated.Value that persists across renders (the analog of
 * `useRef(new Animated.Value(initial)).current`), and frees the engine-side value slot when the
 * component unmounts. The unmount cleanup matters here in a way it doesn't in React Native: the
 * engine's value pool is fixed-size, so a value owned by a mounting/unmounting component (e.g., a list
 * row or a tab screen) would otherwise leak a slot on every unmount.
 */
export function useAnimatedValue(initial = 0) {
  const ref = useRef(null);
  if (ref.current == null) {
    ref.current = new AnimatedValue(initial);
  }
  // Empty deps → the cleanup runs only on unmount. `initial` is read once on the first render (matching
  // useRef semantics); later changes don't recreate the value, as in React Native.
  useEffect(() => {
    const value = ref.current;
    return () => value.destroy();
  }, []);
  return ref.current;
}

/**
 * Wraps a host component so animated values in its `style` are bound to the node (native driver).
 */
export function createAnimatedComponent(Component) {
  return function AnimatedComponent(props) {
    const {style, ...rest} = props;
    const {staticStyle, bindings} = splitAnimatedStyle(style);
    const ref = node => {
      if (node == null) return; // unmount
      for (const b of bindings) b.value.__bind(node, b.prop);
    };
    return createElement(Component, {...rest, style: staticStyle, ref});
  };
}

export const Animated = {
  Value: AnimatedValue,
  View: createAnimatedComponent('View'),
  Text: createAnimatedComponent('Text'),
  Image: createAnimatedComponent('Image'),
  timing,
  spring,
  decay,
  sequence,
  parallel,
  stagger,
  delay,
  loop,
  createAnimatedComponent,
};
