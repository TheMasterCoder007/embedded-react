// Easing tokens. The engine implements a fixed set of easing curves (ERAnimEasing), so unlike RN's
// composable Easing functions these are string tokens the bridge maps to the engine enum. `bezier`
// returns a control-point object the bridge reads as a custom cubic-bezier curve.
export const Easing = {
  linear: 'linear',
  ease: 'ease',
  easeIn: 'easeIn',
  easeOut: 'easeOut',
  easeInOut: 'easeInOut',
  quadIn: 'quadIn',
  quadOut: 'quadOut',
  quadInOut: 'quadInOut',
  cubicIn: 'cubicIn',
  cubicOut: 'cubicOut',
  cubicInOut: 'cubicInOut',
  bounceOut: 'bounceOut',
  elasticOut: 'elasticOut',

  /** Custom cubic-bezier curve via control points. */
  bezier(x1, y1, x2, y2) {
    return { x1, y1, x2, y2 };
  },
};
