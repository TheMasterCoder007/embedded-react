// StyleSheet — the React Native analog. `create` is an identity pass-through (styles are plain
// objects the host config flattens at setProps time); `flatten` collapses nested arrays/objects.
export const StyleSheet = {
  create(styles) {
    return styles;
  },

  flatten(style) {
    if (!style) return {};
    if (Array.isArray(style)) {
      const out = {};
      for (const s of style) Object.assign(out, StyleSheet.flatten(s));
      return out;
    }
    return style;
  },

  hairlineWidth: 1,
  absoluteFill: { position: 'absolute', top: 0, left: 0, right: 0, bottom: 0 },
};
