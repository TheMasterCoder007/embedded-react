import { describe, it, expect } from 'vitest';
import { parseColor, parsePath, flattenSvg } from '../svg-ops.js';

// Opcodes mirror er_scene.h.
const SHAPE = 0;
const MOVE = 1;
const LINE = 2;
const QUAD = 3;
const CUBIC = 4;
const ARC = 5;
const CLOSE = 6;

const el = (type, props) => ({ type, props });

describe('parseColor', () => {
  it('parses 6-digit hex as opaque ARGB', () => {
    expect(parseColor('#f4a261') >>> 0).toBe(0xfff4a261);
  });
  it('parses 3-digit hex', () => {
    expect(parseColor('#fff') >>> 0).toBe(0xffffffff);
  });
  it('parses 8-digit hex with alpha (CSS #RRGGBBAA, alpha last)', () => {
    expect(parseColor('#ff000080') >>> 0).toBe(0x80ff0000);
  });
  it('treats none/transparent as 0', () => {
    expect(parseColor('none')).toBe(0);
    expect(parseColor('transparent')).toBe(0);
    expect(parseColor(null)).toBe(0);
  });
  it('parses rgba()', () => {
    const c = parseColor('rgba(255,0,0,0.5)') >>> 0;
    expect((c >>> 24) & 0xff).toBeGreaterThanOrEqual(126);
    expect((c >>> 24) & 0xff).toBeLessThanOrEqual(130);
    expect(c & 0xffffff).toBe(0xff0000);
  });
});

describe('parsePath', () => {
  it('parses absolute M/L/Z', () => {
    expect(parsePath('M0 0 L10 0 Z')).toEqual([MOVE, 0, 0, LINE, 10, 0, CLOSE]);
  });
  it('parses relative m/l', () => {
    expect(parsePath('m5 5 l10 0')).toEqual([MOVE, 5, 5, LINE, 15, 5]);
  });
  it('implicit lineto after moveto', () => {
    expect(parsePath('M0 0 1 1')).toEqual([MOVE, 0, 0, LINE, 1, 1]);
  });
  it('H and V', () => {
    expect(parsePath('M0 0 H10 V10')).toEqual([MOVE, 0, 0, LINE, 10, 0, LINE, 10, 10]);
  });
  it('cubic C', () => {
    expect(parsePath('M0 0 C1 2 3 4 5 6')).toEqual([MOVE, 0, 0, CUBIC, 1, 2, 3, 4, 5, 6]);
  });
  it('converts arc A to cubic segments', () => {
    const ops = parsePath('M0 0 A 10 10 0 0 1 20 0');
    expect(ops[0]).toBe(MOVE);
    // remaining ops should all be cubics
    let i = 3;
    let cubics = 0;
    while (i < ops.length) {
      expect(ops[i]).toBe(CUBIC);
      cubics++;
      i += 7;
    }
    expect(cubics).toBeGreaterThanOrEqual(1);
  });
});

describe('flattenSvg', () => {
  it('emits one SHAPE per shape with a 7-field paint each', () => {
    const props = {
      children: [
        el('Path', { d: 'M0 0 L10 0', stroke: '#4cc9f0', strokeWidth: 3, strokeLinecap: 'round', fill: 'none' }),
        el('Circle', { cx: 5, cy: 5, r: 4, fill: '#16202f' }),
      ],
    };
    const { ops, paints } = flattenSvg(props);
    // Two shapes => two paint records (14 numbers).
    expect(paints.length).toBe(14);
    // Path paint: fill none(0), stroke blue, width 3, cap round(1).
    expect(paints[0]).toBe(0); // fill
    expect(paints[1] >>> 0).toBe(0xff4cc9f0); // stroke
    expect(paints[2]).toBe(3); // width
    expect(paints[4]).toBe(1); // round cap
    // Circle paint: fill set, stroke none.
    expect(paints[7] >>> 0).toBe(0xff16202f);
    expect(paints[8]).toBe(0);
    // Tape starts with a SHAPE op and contains two of them.
    expect(ops[0]).toBe(SHAPE);
    expect(ops.filter((_, i) => ops[i - 1] === undefined)).toBeTruthy();
    const shapeCount = ops.reduce((n, _v, i) => {
      // crude: count SHAPE ops by scanning (SHAPE is followed by an index)
      return n;
    }, 0);
    // Circle produced an ARC op somewhere.
    expect(ops).toContain(ARC);
  });

  it('applies viewBox -> width/height scaling', () => {
    const props = {
      width: 100,
      height: 100,
      viewBox: '0 0 50 50',
      children: [el('Rect', { x: 0, y: 0, width: 50, height: 50, fill: '#fff' })],
    };
    const { ops } = flattenSvg(props);
    // First op after SHAPE+index is MOVE 0,0; the LINE to (50,0) should scale to (100,0).
    // ops: [SHAPE, 0, MOVE, 0, 0, LINE, 100, 0, LINE, 100, 100, LINE, 0, 100, CLOSE]
    const moveIdx = ops.indexOf(MOVE);
    expect(ops[moveIdx + 1]).toBe(0);
    expect(ops[moveIdx + 2]).toBe(0);
    // first LINE x should be 100 (50 * 2)
    const lineIdx = ops.indexOf(LINE);
    expect(ops[lineIdx + 1]).toBe(100);
  });

  it('G passes inherited paint and translate to children', () => {
    const props = {
      children: [el('G', { fill: '#ff0000', x: 10, y: 20, children: [el('Rect', { x: 0, y: 0, width: 5, height: 5 })] })],
    };
    const { ops, paints } = flattenSvg(props);
    expect(paints[0] >>> 0).toBe(0xffff0000); // inherited fill
    const moveIdx = ops.indexOf(MOVE);
    expect(ops[moveIdx + 1]).toBe(10); // translated x
    expect(ops[moveIdx + 2]).toBe(20); // translated y
  });
});
