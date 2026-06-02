import { describe, it, expect } from 'vitest';
import { splitAnimatedStyle } from '../split-style.js';

// Stand-in for an Animated.Value / interpolation (anything with the __animated marker).
const anim = (id) => ({ __animated: true, id, __bind() {} });

describe('splitAnimatedStyle', () => {
  it('keeps a fully static style static, with no bindings', () => {
    const { staticStyle, bindings } = splitAnimatedStyle({ width: 10, opacity: 1 });
    expect(staticStyle).toEqual({ width: 10, opacity: 1 });
    expect(bindings).toEqual([]);
  });

  it('extracts an animated top-level prop into a binding', () => {
    const o = anim('o');
    const { staticStyle, bindings } = splitAnimatedStyle({ width: 10, opacity: o });
    expect(staticStyle).toEqual({ width: 10 });
    expect(bindings).toEqual([{ prop: 'opacity', value: o }]);
  });

  it('extracts an animated transform axis, keeping static transform entries', () => {
    const tx = anim('tx');
    const { staticStyle, bindings } = splitAnimatedStyle({ transform: [{ translateX: tx }, { rotate: '10deg' }] });
    expect(staticStyle).toEqual({ transform: [{ rotate: '10deg' }] });
    expect(bindings).toEqual([{ prop: 'translateX', value: tx }]);
  });

  it('animated scale binds both axes', () => {
    const s = anim('s');
    const { bindings } = splitAnimatedStyle({ transform: [{ scale: s }] });
    expect(bindings).toEqual([
      { prop: 'scaleX', value: s },
      { prop: 'scaleY', value: s },
    ]);
  });

  it('flattens nested style arrays before splitting', () => {
    const o = anim('o');
    const { staticStyle, bindings } = splitAnimatedStyle([{ width: 10 }, { opacity: o }]);
    expect(staticStyle).toEqual({ width: 10 });
    expect(bindings).toEqual([{ prop: 'opacity', value: o }]);
  });
});
