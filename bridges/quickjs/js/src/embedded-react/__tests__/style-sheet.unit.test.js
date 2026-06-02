import { describe, it, expect } from 'vitest';
import { StyleSheet } from '../StyleSheet.js';
import { Platform } from '../Platform.js';

describe('StyleSheet', () => {
  it('create returns the styles unchanged (identity)', () => {
    const styles = { box: { width: 10 }, label: { color: 'white' } };
    expect(StyleSheet.create(styles)).toBe(styles);
  });

  it('flatten collapses nested arrays, later entries winning', () => {
    expect(StyleSheet.flatten([{ a: 1 }, [{ b: 2 }, { a: 3 }]])).toEqual({ a: 3, b: 2 });
  });

  it('flatten of null/undefined is an empty object', () => {
    expect(StyleSheet.flatten(null)).toEqual({});
    expect(StyleSheet.flatten(undefined)).toEqual({});
  });

  it('flatten passes a plain object through', () => {
    expect(StyleSheet.flatten({ width: 5 })).toEqual({ width: 5 });
  });
});

describe('Platform', () => {
  it('OS is "embedded"', () => {
    expect(Platform.OS).toBe('embedded');
  });

  it('select picks the embedded entry', () => {
    expect(Platform.select({ embedded: 'e', ios: 'i', default: 'd' })).toBe('e');
  });

  it('select falls back to default when no embedded entry', () => {
    expect(Platform.select({ ios: 'i', default: 'd' })).toBe('d');
  });
});
