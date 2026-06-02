// Animated — the React Native analog, backed by the engine's native-driver value system
// (er_anim_value_*). An Animated.Value is a handle to an engine-side float; binding it to a node
// prop (via Animated.View) lets the engine advance the animation each frame with NO per-frame JS.
import { createElement } from 'react';
import { NativeUI } from '../native-ui.js';
import { splitAnimatedStyle } from './split-style.js';

/** A standalone animatable value bound to an engine-side float. */
export class AnimatedValue {
  constructor(initial = 0) {
    this.__animated = true;
    this._handle = NativeUI.animValueCreate(initial);
    this._value = initial;
  }

  setValue(v) {
    this._value = v;
    NativeUI.animValueSet(this._handle, v);
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

/** A value derived from a parent Animated.Value through a piecewise-linear interpolation. */
export class AnimatedInterpolation {
  constructor(parent, config) {
    this.__animated = true;
    this._parent = parent;
    this._config = config;
  }

  __bind(node, prop) {
    NativeUI.animValueBindInterpolated(this._parent._handle, node, prop, this._config);
  }

  interpolate(config) {
    // Chained interpolation isn't supported by the engine; re-map against the same parent.
    return new AnimatedInterpolation(this._parent, config);
  }
}

/**
 * Wraps a NativeUI animation call in an object with start()/stop() (RN's animation handle shape).
 * The completion callback passed to start() is not wired through the engine yet (fire-and-forget).
 */
function makeAnimation(value, toValue, config) {
  let handle = 0;
  return {
    start(_onComplete) {
      handle = NativeUI.animValueAnimate(value._handle, toValue, config);
    },
    stop() {
      if (handle) NativeUI.animStop(handle);
    },
  };
}

export function timing(value, config = {}) {
  const { toValue = 0, useNativeDriver, ...rest } = config;
  return makeAnimation(value, toValue, { type: 'timing', ...rest });
}

export function spring(value, config = {}) {
  const { toValue = 0, useNativeDriver, ...rest } = config;
  return makeAnimation(value, toValue, { type: 'spring', ...rest });
}

export function decay(value, config = {}) {
  const { useNativeDriver, ...rest } = config;
  // Decay coasts from its velocity; the engine ignores the target, so pass the current value.
  return makeAnimation(value, value.__getValue(), { type: 'decay', ...rest });
}

/**
 * Wraps a host component so animated values in its `style` are bound to the node (native driver).
 */
export function createAnimatedComponent(Component) {
  return function AnimatedComponent(props) {
    const { style, ...rest } = props;
    const { staticStyle, bindings } = splitAnimatedStyle(style);
    const ref = (node) => {
      if (node == null) return; // unmount
      for (const b of bindings) b.value.__bind(node, b.prop);
    };
    return createElement(Component, { ...rest, style: staticStyle, ref });
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
  createAnimatedComponent,
};
