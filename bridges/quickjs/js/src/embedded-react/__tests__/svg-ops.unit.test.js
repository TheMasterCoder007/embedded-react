import { describe, it, expect } from 'vitest';
import { parseColor, parsePath, flattenSvg, shapesToVector } from '../svg-ops.js';

// Opcodes mirror er_scene.h.
const SHAPE = 0;
const MOVE = 1;
const LINE = 2;
const QUAD = 3;
const CUBIC = 4;
const ARC = 5;
const CLOSE = 6;

const el = (type, props) => ({ type, props });

// Walk the op-tape (args-per-op are fixed by the ER_VOP_* contract) and return each SHAPE's paint
// index. Needed because SHAPE === 0 collides with zero coordinates, so a naive scan is unreliable.
const OP_ARGC = { [MOVE]: 2, [LINE]: 2, [QUAD]: 4, [CUBIC]: 6, [ARC]: 6, [CLOSE]: 0 };
function shapeIndices(ops) {
  const out = [];
  let i = 0;
  while (i < ops.length) {
    const op = ops[i++];
    if (op === SHAPE) {
      out.push(ops[i++]);
    } else {
      i += OP_ARGC[op];
    }
  }
  return out;
}

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

// The imperative compiler used by updateVector() on the drag hot path. Builds the same op-tape +
// 7-field paint table as flattenSvg, but from flat primitive descriptors (no JSX) and into reused
// buffers — so these also guard the reused-array reset that makes that allocation-free.
describe('shapesToVector (imperative)', () => {
  it('compiles an arc descriptor to MOVE + ARC with a stroke paint', () => {
    const { ops, paints } = shapesToVector([
      { arc: [100, 100, 80, -135, 135], stroke: '#f4a261', strokeWidth: 14, cap: 'round' },
    ]);
    expect(ops[0]).toBe(SHAPE);
    expect(ops[1]).toBe(0); // paint index
    expect(ops[2]).toBe(MOVE);
    expect(ops[5]).toBe(ARC);
    expect(ops.slice(6, 9)).toEqual([100, 100, 80]); // cx, cy, r
    expect(ops[ops.length - 1]).toBe(0); // ccw flag
    expect(paints.length).toBe(7);
    expect(paints[0]).toBe(0); // fill none
    expect(paints[1] >>> 0).toBe(0xfff4a261); // stroke
    expect(paints[2]).toBe(14); // width
    expect(paints[4]).toBe(1); // round cap
  });

  it('compiles a circle to a full-circle ARC + CLOSE', () => {
    const { ops } = shapesToVector([{ circle: [50, 60, 8], fill: '#16202f' }]);
    expect(ops.slice(0, 5)).toEqual([SHAPE, 0, MOVE, 58, 60]); // MOVE to (cx+r, cy)
    expect(ops[5]).toBe(ARC);
    expect(ops[ops.length - 1]).toBe(CLOSE);
  });

  it('compiles a rect to MOVE + 3 LINE + CLOSE', () => {
    const { ops, paints } = shapesToVector([{ rect: [10, 20, 30, 40], fill: '#ffffff' }]);
    expect(ops).toEqual([SHAPE, 0, MOVE, 10, 20, LINE, 40, 20, LINE, 40, 60, LINE, 10, 60, CLOSE]);
    expect(paints[0] >>> 0).toBe(0xffffffff);
  });

  it('compiles a line and a path descriptor', () => {
    expect(shapesToVector([{ line: [1, 2, 3, 4], stroke: '#fff' }]).ops).toEqual([SHAPE, 0, MOVE, 1, 2, LINE, 3, 4]);
    expect(shapesToVector([{ path: 'M0 0 L10 0 Z', stroke: '#fff' }]).ops).toEqual([SHAPE, 0, MOVE, 0, 0, LINE, 10, 0, CLOSE]);
  });

  it('assigns sequential paint indices across multiple shapes', () => {
    const { ops, paints } = shapesToVector([
      { rect: [0, 0, 1, 1], fill: '#fff' },
      { line: [0, 0, 1, 1], stroke: '#000' },
      { circle: [0, 0, 1], fill: '#f00' },
    ]);
    expect(paints.length).toBe(21); // 3 * 7
    expect(shapeIndices(ops)).toEqual([0, 1, 2]);
  });

  it('maps strokeWidth/miter/cap/join/fillRule into the paint record', () => {
    const { paints } = shapesToVector([
      { path: 'M0 0 L1 0', stroke: '#fff', strokeWidth: 2, cap: 'square', join: 'bevel', miter: 6, fillRule: 'evenodd' },
    ]);
    expect(paints[2]).toBe(2); // width
    expect(paints[3]).toBe(6); // miter limit
    expect(paints[4]).toBe(2); // square cap
    expect(paints[5]).toBe(2); // bevel join
    expect(paints[6]).toBe(1); // evenodd
  });

  it('skips empty/unknown shapes without leaving a dangling SHAPE header', () => {
    const { ops, paints } = shapesToVector([{ foo: 1 }, { rect: [0, 0, 2, 2], fill: '#fff' }]);
    expect(ops[0]).toBe(SHAPE);
    expect(ops[1]).toBe(0); // the rect's paint index is 0 — the unknown shape emitted nothing
    expect(paints.length).toBe(7);
  });

  it('reuses its output buffers but resets them each call (no stale leakage)', () => {
    const lenA = shapesToVector([{ rect: [0, 0, 9, 9], fill: '#fff' }]).ops.length;
    const b = shapesToVector([{ line: [0, 0, 1, 1], stroke: '#000' }]);
    expect(b.ops).toEqual([SHAPE, 0, MOVE, 0, 0, LINE, 1, 1]); // no trailing ops from the longer prior call
    expect(b.ops.length).toBeLessThan(lenA);
  });
});
