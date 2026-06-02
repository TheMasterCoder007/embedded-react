// LayoutAnimation — the React Native analog. Calling configureNext(...) before a state update that
// changes layout makes the engine tween every node whose computed rect moved (instead of snapping)
// on the next commit. Backed by the engine's er_layout_anim_configure_next; the tween advances in C
// each frame (er_layout_anim_tick), so there's no per-frame JS.
import { NativeUI } from '../native-ui.js';
import { Types, Properties, Presets, toEngineConfig, create } from './layout-anim-config.js';

/**
 * Arms a layout animation for the next commit. Call right before the setState that changes layout.
 * onAnimationDidEnd is approximated with a timer (the engine config carries no completion hook).
 */
export function configureNext(config, onAnimationDidEnd) {
  NativeUI.configureNextLayoutAnimation(toEngineConfig(config));
  if (typeof onAnimationDidEnd === 'function') {
    const ms = config && typeof config.duration === 'number' ? config.duration : 300;
    setTimeout(onAnimationDidEnd, ms);
  }
}

export const LayoutAnimation = {
  configureNext,
  create,
  Types,
  Properties,
  Presets,
  easeInEaseOut: (onDone) => configureNext(Presets.easeInEaseOut, onDone),
  linear: (onDone) => configureNext(Presets.linear, onDone),
  spring: (onDone) => configureNext(Presets.spring, onDone),
};
