// Separates a style object into the static props (passed straight to the node) and the animated
// bindings (Animated.Value / interpolation, bound to the node's prop so the engine drives them).
// Pure (no NativeUI), so it's unit-testable. An animated entry is anything with an `__animated`
// marker — `transform: [{ translateX: value }]` entries are handled too (scale binds both axes).
import { flattenStyle } from '../props.js';

export function splitAnimatedStyle(style) {
  const staticStyle = {};
  const bindings = [];

  const flat = {};
  flattenStyle(style, flat);

  for (const key in flat) {
    const val = flat[key];

    if (key === 'transform' && Array.isArray(val)) {
      const staticTransform = [];
      for (const entry of val) {
        const axis = Object.keys(entry)[0];
        const v = entry[axis];
        if (v && v.__animated) {
          if (axis === 'scale') {
            bindings.push({ prop: 'scaleX', value: v });
            bindings.push({ prop: 'scaleY', value: v });
          } else {
            bindings.push({ prop: axis, value: v });
          }
        } else {
          staticTransform.push(entry);
        }
      }
      if (staticTransform.length > 0) staticStyle.transform = staticTransform;
    } else if (val && val.__animated) {
      bindings.push({ prop: key, value: val });
    } else {
      staticStyle[key] = val;
    }
  }

  return { staticStyle, bindings };
}
