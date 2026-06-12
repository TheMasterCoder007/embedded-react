import { describe, it, expect } from 'vitest';
import { buildProps, flattenStyle, isEventProp, PASSTHROUGH, resolveImageSource } from '../props.js';

describe('flattenStyle', () => {
  it('merges a plain style object', () => {
    const out = {};
    flattenStyle({ width: 10, height: 20 }, out);
    expect(out).toEqual({ width: 10, height: 20 });
  });

  it('flattens nested arrays, later entries winning', () => {
    const out = {};
    flattenStyle([{ a: 1 }, [{ b: 2 }, { a: 3 }]], out);
    expect(out).toEqual({ a: 3, b: 2 });
  });

  it('ignores null / undefined / false', () => {
    const out = {};
    flattenStyle(null, out);
    flattenStyle(undefined, out);
    flattenStyle(false, out);
    expect(out).toEqual({});
  });
});

describe('buildProps', () => {
  it('flattens style into the flat bag', () => {
    expect(buildProps('View', { style: { width: 10, height: 20 } })).toEqual({ width: 10, height: 20 });
  });

  it('copies passthrough props alongside style', () => {
    expect(buildProps('Image', { style: { width: 5 }, resizeMode: 'cover' })).toEqual({
      width: 5,
      resizeMode: 'cover',
    });
  });

  it('does not copy unknown top-level props', () => {
    expect(buildProps('View', { style: {}, foo: 'bar', onPress: () => {} })).toEqual({});
  });

  it('honors <Svg width/height> as direct props (react-native-svg convention, Flow A↔B parity)', () => {
    expect(buildProps('Svg', { width: 180, height: 180 })).toEqual({ width: 180, height: 180 });
  });

  it('does not treat width/height as direct props on a non-Svg element', () => {
    expect(buildProps('View', { width: 180, height: 180 })).toEqual({});
  });

  it('an explicit style width/height wins over the <Svg> direct prop', () => {
    expect(buildProps('Svg', { width: 180, height: 180, style: { width: 200 } })).toEqual({
      width: 200,
      height: 180,
    });
  });

  it('resolves an <Image source> string to imageName', () => {
    expect(buildProps('Image', { style: {}, source: 'wx_sun' })).toEqual({ imageName: 'wx_sun' });
  });

  it('resolves an <Image source={{uri}}> to imageName', () => {
    expect(buildProps('Image', { style: {}, source: { uri: 'logo' } })).toEqual({ imageName: 'logo' });
  });

  it('an explicit imageName wins over source', () => {
    expect(buildProps('Image', { style: {}, imageName: 'a', source: 'b' })).toEqual({ imageName: 'a' });
  });

  it('ignores an unresolvable source (e.g. a numeric require id)', () => {
    expect(buildProps('Image', { style: {}, source: 7 })).toEqual({});
  });

  it('uses a string child as Text content', () => {
    expect(buildProps('Text', { children: 'hi' })).toEqual({ text: 'hi' });
  });

  it('stringifies a numeric Text child', () => {
    expect(buildProps('Text', { children: 42 })).toEqual({ text: '42' });
  });

  it('does not set text for a non-Text node', () => {
    expect(buildProps('View', { children: 'hi' })).toEqual({});
  });

  it('concatenates multi-child Text into the node text (interpolation)', () => {
    expect(buildProps('Text', { children: ['a', 'b'] })).toEqual({ text: 'ab' });
  });

  it('PASSTHROUGH is a non-empty list of strings', () => {
    expect(PASSTHROUGH.length).toBeGreaterThan(0);
    expect(PASSTHROUGH.every((k) => typeof k === 'string')).toBe(true);
  });
});

describe('resolveImageSource', () => {
  it('passes through a bare name string (also what the image import resolves to)', () => {
    expect(resolveImageSource('wx_sun')).toBe('wx_sun');
  });
  it('reads { uri } objects', () => {
    expect(resolveImageSource({ uri: 'logo' })).toBe('logo');
  });
  it('returns null for unresolvable values', () => {
    expect(resolveImageSource(null)).toBe(null);
    expect(resolveImageSource(undefined)).toBe(null);
    expect(resolveImageSource(42)).toBe(null);
    expect(resolveImageSource({})).toBe(null);
  });
});

describe('isEventProp', () => {
  it('detects onPress with a function', () => {
    expect(isEventProp('onPress', () => {})).toBe(true);
  });

  it('rejects on* with a non-function value', () => {
    expect(isEventProp('onPress', 5)).toBe(false);
  });

  it('rejects keys whose third char is lowercase (e.g. "only")', () => {
    expect(isEventProp('only', () => {})).toBe(false);
  });

  it('rejects short keys', () => {
    expect(isEventProp('on', () => {})).toBe(false);
  });
});
