import { describe, it, expect } from 'vitest';
import { isTextContent, flattenTextChildren, buildTextSpans, buildProps, flattenStyleObj } from '../props.js';

// A minimal stand-in for a React <Text> element (React.createElement output shape: { type, props }).
// children is a single value when there's one child, else an array — matching React.
const Text = (style, ...children) => ({
  type: 'Text',
  props: { style, children: children.length === 1 ? children[0] : children },
});
const View = (style, ...children) => ({
  type: 'View',
  props: { style, children: children.length === 1 ? children[0] : children },
});

describe('isTextContent', () => {
  it('accepts primitives, null/false, and arrays of them', () => {
    expect(isTextContent('hi')).toBe(true);
    expect(isTextContent(5)).toBe(true);
    expect(isTextContent(null)).toBe(true);
    expect(isTextContent(false)).toBe(true);
    expect(isTextContent(['Hi ', 'name', '!'])).toBe(true);
  });

  it('accepts nested <Text> elements', () => {
    expect(isTextContent(Text(null, 'bold'))).toBe(true);
    expect(isTextContent(['Hello ', Text({ fontWeight: 'bold' }, 'world')])).toBe(true);
  });

  it('rejects non-Text elements (fall back to mounted instances)', () => {
    expect(isTextContent(View(null, 'x'))).toBe(false);
    expect(isTextContent(['ok ', View(null, 'x')])).toBe(false);
  });
});

describe('flattenTextChildren', () => {
  it('coalesces plain interpolation into a single base-style segment', () => {
    const base = flattenStyleObj({ color: 'white' });
    const segs = flattenTextChildren(['Hi ', 'Ada', '!'], base);
    expect(segs).toHaveLength(1);
    expect(segs[0].text).toBe('Hi Ada!');
    expect(segs[0].style).toBe(base); // same reference → no spans needed
  });

  it('stringifies numbers and drops null/false', () => {
    const base = flattenStyleObj({});
    const segs = flattenTextChildren(['n=', 42, null, false], base);
    expect(segs).toHaveLength(1);
    expect(segs[0].text).toBe('n=42');
  });

  it('produces distinct segments for nested styled <Text>', () => {
    const base = flattenStyleObj({ color: 'white' });
    const segs = flattenTextChildren(['Hello ', Text({ fontWeight: 'bold' }, 'world'), '!'], base);
    expect(segs.map((s) => s.text)).toEqual(['Hello ', 'world', '!']);
    expect(segs[0].style).toBe(base);
    expect(segs[2].style).toBe(base);
    expect(segs[1].style).not.toBe(base);
    expect(segs[1].style.fontWeight).toBe('bold');
    expect(segs[1].style.color).toBe('white'); // inherited via merge
  });

  it('keeps the base reference for an unstyled nested <Text> (still coalesces)', () => {
    const base = flattenStyleObj({ color: 'white' });
    const segs = flattenTextChildren(['a', Text(null, 'b'), 'c'], base);
    expect(segs).toHaveLength(1);
    expect(segs[0].text).toBe('abc');
  });
});

describe('buildTextSpans', () => {
  it('returns [] for uniformly-styled text (plain-text path)', () => {
    expect(buildTextSpans({ style: { color: 'white' }, children: ['Hi ', 'Ada'] })).toEqual([]);
    expect(buildTextSpans({ children: 'just text' })).toEqual([]);
  });

  it('returns one span per styled run, with only span-relevant keys', () => {
    const spans = buildTextSpans({
      style: { color: 'white' },
      children: ['Hello ', Text({ fontWeight: 'bold', color: 'red' }, 'world'), '!'],
    });
    expect(spans).toHaveLength(3);
    expect(spans[0]).toEqual({ text: 'Hello ', color: 'white' });
    expect(spans[1]).toEqual({ text: 'world', color: 'red', fontWeight: 'bold' });
    expect(spans[2]).toEqual({ text: '!', color: 'white' });
  });

  it('carries fontSize / fontStyle / textDecorationLine / letterSpacing through', () => {
    const spans = buildTextSpans({
      children: Text({ fontSize: 20, fontStyle: 'italic', textDecorationLine: 'underline', letterSpacing: 2 }, 'x'),
    });
    expect(spans).toEqual([
      { text: 'x', fontSize: 20, fontStyle: 'italic', textDecorationLine: 'underline', letterSpacing: 2 },
    ]);
  });

  it('returns [] for non-flattenable children', () => {
    expect(buildTextSpans({ children: View(null, 'x') })).toEqual([]);
  });
});

describe('buildProps Text content', () => {
  it('flattens interpolation into the full text string', () => {
    expect(buildProps('Text', { children: ['uptime: ', 7, 's'] }).text).toBe('uptime: 7s');
  });

  it('concatenates nested <Text> into the plain-text fallback', () => {
    const flat = buildProps('Text', {
      style: { color: 'white' },
      children: ['Hello ', Text({ fontWeight: 'bold' }, 'world'), '!'],
    });
    expect(flat.text).toBe('Hello world!');
    expect(flat.color).toBe('white');
  });

  it('leaves text unset for non-flattenable children', () => {
    expect(buildProps('Text', { children: View(null, 'x') }).text).toBeUndefined();
  });
});
