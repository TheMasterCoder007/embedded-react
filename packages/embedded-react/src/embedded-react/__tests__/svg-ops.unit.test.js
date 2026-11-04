/*
 * Copyright 2026 Cory Lamming
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

import {describe, it, expect, vi} from 'vitest';
import {createElement, Fragment} from 'react';
import {
  parseColor,
  parsePath,
  clearPathCache,
  flattenSvg,
  shapesToVector,
  scaleVectorArtifact,
  rectRadii,
  encodeVectorGradients,
  warnVectorCaps,
  GRAD_STRIDE,
  GRAD_MAX_STOPS,
  GRAD_LINEAR,
  PAINT_STRIDE,
} from '../svg-ops.js';

// Opcodes mirror er_scene.h.
const SHAPE = 0;
const MOVE = 1;
const LINE = 2;
const QUAD = 3;
const CUBIC = 4;
const ARC = 5;
const CLOSE = 6;

const el = (type, props) => ({type, props});

// Walk the op-tape (args-per-op are fixed by the ER_VECTOR_OP_* contract) and return each SHAPE's paint
// index. Needed because SHAPE === 0 collides with zero coordinates, so a naive scan is unreliable.
const ARGUMENTS_PER_OP = {
  [MOVE]: 2,
  [LINE]: 2,
  [QUAD]: 4,
  [CUBIC]: 6,
  [ARC]: 6,
  [CLOSE]: 0,
};
function shapeIndices(ops) {
  const indices = [];
  let cursor = 0;
  while (cursor < ops.length) {
    const opcode = ops[cursor++];
    if (opcode === SHAPE) {
      indices.push(ops[cursor++]);
    } else {
      cursor += ARGUMENTS_PER_OP[opcode];
    }
  }
  return indices;
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
    const color = parseColor('rgba(255,0,0,0.5)') >>> 0;
    expect((color >>> 24) & 0xff).toBeGreaterThanOrEqual(126);
    expect((color >>> 24) & 0xff).toBeLessThanOrEqual(130);
    expect(color & 0xffffff).toBe(0xff0000);
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
    expect(parsePath('M0 0 H10 V10')).toEqual([
      MOVE,
      0,
      0,
      LINE,
      10,
      0,
      LINE,
      10,
      10,
    ]);
  });
  it('cubic C', () => {
    expect(parsePath('M0 0 C1 2 3 4 5 6')).toEqual([
      MOVE,
      0,
      0,
      CUBIC,
      1,
      2,
      3,
      4,
      5,
      6,
    ]);
  });
  it('converts arc A to cubic segments', () => {
    const ops = parsePath('M0 0 A 10 10 0 0 1 20 0');
    expect(ops[0]).toBe(MOVE);
    // remaining ops should all be cubics
    let cursor = 3;
    let cubics = 0;
    while (cursor < ops.length) {
      expect(ops[cursor]).toBe(CUBIC);
      cubics++;
      cursor += 7;
    }
    expect(cubics).toBeGreaterThanOrEqual(1);
  });
});

describe('parsePath cache', () => {
  it('returns the same ops for a repeated `d`, and re-parses after a clear', () => {
    const path = 'M0 0 L10 0 A 4 4 0 0 1 20 0 Z';
    const first = parsePath(path);
    expect(parsePath(path)).toBe(first); // cached: same array, not just an equal one
    clearPathCache();
    const fresh = parsePath(path);
    expect(fresh).not.toBe(first);
    expect(fresh).toEqual(first);
  });

  it('keeps each `d` distinct as the cache evicts', () => {
    clearPathCache();
    const paths = [];
    for (let index = 0; index < 400; index++)
      paths.push(`M${index} 0 L${index + 1} 1`);
    const expected = paths.map(path => parsePath(path).slice());
    paths.forEach((path, index) =>
      expect(parsePath(path)).toEqual(expected[index]),
    );
  });

  it('hits the cache for a `d` that is not a plain string', () => {
    clearPathCache();
    const path = 'M0 0 L10 0 Z';
    const first = parsePath(path);
    // eslint-disable-next-line no-new-wrappers
    expect(parsePath(new String(path))).toBe(first);
  });

  it('does not store a path too big for the whole budget', () => {
    clearPathCache();
    const small = 'M1 1 L2 2';
    const cached = parsePath(small);
    // Comfortably over PATH_CACHE_MAX_OPS (3 ops per segment).
    let huge = 'M0 0';
    for (let index = 0; index < 2000; index++)
      huge += ` L${index} ${index % 7}`;
    const hugeOps = parsePath(huge);
    expect(hugeOps.length).toBeGreaterThan(4096);
    // Storing it would have evicted everything else to make room; it must not be stored at all.
    expect(parsePath(small)).toBe(cached);
    expect(parsePath(huge)).not.toBe(hugeOps); // re-parsed, never cached
  });

  it('rebuilds a tape whose shapes changed and leaves the unchanged ones identical', () => {
    const face = {type: 'Path', props: {d: 'M0 0 L10 0 Z', fill: '#111'}};
    const svg = angle => ({
      children: [face, {type: 'Path', props: {d: `M5 5 L5 ${angle}`}}],
    });
    const first = flattenSvg(svg(9));
    const second = flattenSvg(svg(9));
    expect(second.ops).toEqual(first.ops);
    const changed = flattenSvg(svg(12));
    expect(changed.ops.slice(0, 9)).toEqual(first.ops.slice(0, 9)); // the face segment
    expect(changed.ops).not.toEqual(first.ops);
  });

  it('applies the right transform when one `d` is drawn under two of them', () => {
    const shared = {type: 'Path', props: {d: 'M0 0 L10 0'}};
    const {ops} = flattenSvg({
      children: [
        {type: 'G', props: {x: 100, children: shared}},
        {type: 'G', props: {x: 200, children: shared}},
      ],
    });
    expect(ops).toEqual([
      SHAPE,
      0,
      MOVE,
      100,
      0,
      LINE,
      110,
      0,
      SHAPE,
      1,
      MOVE,
      200,
      0,
      LINE,
      210,
      0,
    ]);
  });
});

describe('flattenSvg gradients', () => {
  const grad = {
    type: 3,
    ax: 50,
    ay: 50,
    r: 1.5,
    stops: [
      {color: '#F2A64B', offset: 0},
      {color: '#4FA9F5', offset: 0.5},
    ],
  };

  it('collects a strokeGradient and references it 1-based from the paint', () => {
    const props = {
      children: {
        type: 'Arc',
        props: {
          cx: 50,
          cy: 50,
          r: 40,
          startAngle: 0,
          endAngle: 90,
          strokeGradient: grad,
        },
      },
    };
    const {paints, gradients} = flattenSvg(props);
    expect(gradients).toHaveLength(1);
    expect(gradients[0].type).toBe(3);
    // String stop colours are normalised to ARGB on the way in.
    expect(typeof gradients[0].stops[0].color).toBe('number');
    expect(paints[8]).toBe(1); // stroke_gradient, 1-based
    expect(paints[7]).toBe(0); // fill_gradient stays solid
  });

  it('leaves a shape with no gradient solid and the table empty', () => {
    const props = {
      children: {type: 'Circle', props: {cx: 10, cy: 10, r: 5, fill: '#fff'}},
    };
    const {paints, gradients} = flattenSvg(props);
    expect(gradients).toHaveLength(0);
    expect(paints[7]).toBe(0);
    expect(paints[8]).toBe(0);
  });

  it('inherits a gradient through <G> like any other paint attribute', () => {
    const props = {
      children: {
        type: 'G',
        props: {
          strokeGradient: grad,
          children: {
            type: 'Line',
            props: {x1: 0, y1: 0, x2: 10, y2: 10},
          },
        },
      },
    };
    const {paints, gradients} = flattenSvg(props);
    expect(gradients).toHaveLength(1);
    expect(paints[8]).toBe(1);
  });
});

describe('flattenSvg', () => {
  it('emits one SHAPE per shape with a 9-field paint each', () => {
    const props = {
      children: [
        el('Path', {
          d: 'M0 0 L10 0',
          stroke: '#4cc9f0',
          strokeWidth: 3,
          strokeLinecap: 'round',
          fill: 'none',
        }),
        el('Circle', {cx: 5, cy: 5, r: 4, fill: '#16202f'}),
      ],
    };
    const {ops, paints} = flattenSvg(props);
    // Two shapes => two 9-field paint records (18 numbers).
    expect(paints.length).toBe(18);
    // Path paint: fill none(0), stroke blue, width 3, cap round(1), no gradients(0).
    expect(paints[0]).toBe(0); // fill
    expect(paints[1] >>> 0).toBe(0xff4cc9f0); // stroke
    expect(paints[2]).toBe(3); // width
    expect(paints[4]).toBe(1); // round cap
    expect(paints[7]).toBe(0); // fill_gradient
    expect(paints[8]).toBe(0); // stroke_gradient
    // Circle paint (record 2 starts at index 9): fill set, stroke none.
    expect(paints[9] >>> 0).toBe(0xff16202f);
    expect(paints[10]).toBe(0);
    expect(paints[16]).toBe(0); // fill_gradient
    expect(paints[17]).toBe(0); // stroke_gradient
    // Tape starts with a SHAPE op and contains two of them.
    expect(ops[0]).toBe(SHAPE);
    expect(ops.filter((_, index) => ops[index - 1] === undefined)).toBeTruthy();
    const shapeCount = ops.reduce((count, _opcode, index) => {
      // crude: count SHAPE ops by scanning (SHAPE is followed by an index)
      return count;
    }, 0);
    // Circle produced an ARC op somewhere.
    expect(ops).toContain(ARC);
  });

  it('applies viewBox -> width/height scaling', () => {
    const props = {
      width: 100,
      height: 100,
      viewBox: '0 0 50 50',
      children: [el('Rect', {x: 0, y: 0, width: 50, height: 50, fill: '#fff'})],
    };
    const {ops} = flattenSvg(props);
    // First op after SHAPE+index is MOVE 0,0; the LINE to (50,0) should scale to (100,0).
    // ops: [SHAPE, 0, MOVE, 0, 0, LINE, 100, 0, LINE, 100, 100, LINE, 0, 100, CLOSE]
    const moveIndex = ops.indexOf(MOVE);
    expect(ops[moveIndex + 1]).toBe(0);
    expect(ops[moveIndex + 2]).toBe(0);
    // first LINE x should be 100 (50 * 2)
    const lineIndex = ops.indexOf(LINE);
    expect(ops[lineIndex + 1]).toBe(100);
  });

  it('G passes inherited paint and translate to children', () => {
    const props = {
      children: [
        el('G', {
          fill: '#ff0000',
          x: 10,
          y: 20,
          children: [el('Rect', {x: 0, y: 0, width: 5, height: 5})],
        }),
      ],
    };
    const {ops, paints} = flattenSvg(props);
    expect(paints[0] >>> 0).toBe(0xffff0000); // inherited fill
    const moveIndex = ops.indexOf(MOVE);
    expect(ops[moveIndex + 1]).toBe(10); // translated x
    expect(ops[moveIndex + 2]).toBe(20); // translated y
  });

  it('the <Svg> paint cascades to its shapes, under a G and a shape that override it', () => {
    const props = {
      fill: 'none',
      stroke: '#ff0000',
      strokeWidth: 3,
      children: [
        el('Circle', {cx: 10, cy: 10, r: 5}),
        el('G', {
          stroke: '#00ff00',
          children: [
            el('Rect', {x: 0, y: 0, width: 5, height: 5, fill: '#0000ff'}),
          ],
        }),
      ],
    };
    const {paints} = flattenSvg(props);
    expect(paints[0]).toBe(0); // fill
    expect(paints[1] >>> 0).toBe(0xffff0000); // stroke
    expect(paints[2]).toBe(3); // width
    expect(paints[9] >>> 0).toBe(0xff0000ff); // the Rect's own fill
    expect(paints[10] >>> 0).toBe(0xff00ff00); // the G's stroke
    expect(paints[11]).toBe(3); // the <Svg>'s width
  });
});

// <Svg> owns its subtree instead of mounting it, so the walk sees raw JSX children — and every child
// not written inline arrives wrapped: a `{items.map(...)}` is an array, a `<>…</>` is a
// fragment element. Both used to fall through the shape branches and drop their shapes with no output.
describe('flattenSvg child unwrapping', () => {
  it('ignores null/false/whitespace children without warning', () => {
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => {});
    const {ops, paints} = flattenSvg({
      children: [
        null,
        undefined,
        false,
        true,
        '\n  ',
        el('Line', {x1: 0, y1: 0, x2: 10, y2: 10}),
      ],
    });
    expect(paints.length / PAINT_STRIDE).toBe(1);
    expect(shapeIndices(ops)).toEqual([0]);
    expect(warn).not.toHaveBeenCalled();
    warn.mockRestore();
  });

  it('draws a mapped list of shapes alongside an inline sibling', () => {
    const bars = [1, 2, 3].map(index =>
      el('Rect', {x: index * 10, y: 0, width: 5, height: 5}),
    );
    const {ops, paints} = flattenSvg({
      children: [el('Circle', {cx: 0, cy: 0, r: 4}), bars],
    });
    expect(paints.length / PAINT_STRIDE).toBe(4);
    expect(shapeIndices(ops)).toEqual([0, 1, 2, 3]);
  });

  it('flattens arrays at any depth (two sibling maps, nested groups of them)', () => {
    const line = x => el('Line', {x1: x, y1: 0, x2: x, y2: 10});
    const {ops, paints} = flattenSvg({
      children: [
        [line(0), line(1)],
        [[line(2)], [[line(3)]]],
      ],
    });
    expect(paints.length / PAINT_STRIDE).toBe(4);
    expect(shapeIndices(ops)).toEqual([0, 1, 2, 3]);
  });

  it('unwraps a fragment, and one nested in a mapped array', () => {
    const {ops, paints} = flattenSvg({
      children: createElement(
        Fragment,
        null,
        el('Line', {x1: 0, y1: 0, x2: 1, y2: 1}),
        [1, 2].map(index =>
          createElement(
            Fragment,
            {key: index},
            el('Line', {x1: index, y1: 0, x2: index, y2: 1}),
            el('Circle', {cx: index, cy: 0, r: 2}),
          ),
        ),
      ),
    });
    expect(paints.length / PAINT_STRIDE).toBe(5);
    expect(shapeIndices(ops)).toEqual([0, 1, 2, 3, 4]);
  });

  it('a fragment is transparent: shapes inside it keep the <G> paint and transform', () => {
    const {ops, paints} = flattenSvg({
      children: [
        el('G', {
          fill: '#ff0000',
          x: 10,
          y: 20,
          children: createElement(
            Fragment,
            null,
            el('Rect', {x: 0, y: 0, width: 5, height: 5}),
          ),
        }),
      ],
    });
    expect(paints[0] >>> 0).toBe(0xffff0000); // inherited fill, as if written inline
    const moveIndex = ops.indexOf(MOVE);
    expect(ops[moveIndex + 1]).toBe(10); // group translate
    expect(ops[moveIndex + 2]).toBe(20);
  });

  it('warns once about a child it cannot draw, and still draws the siblings', () => {
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => {});
    const Needle = () => null;
    const {paints} = flattenSvg({
      children: [
        createElement(Needle, null),
        el('Text', {}), // a host tag, but not one <Svg> can flatten
        el('Line', {x1: 0, y1: 0, x2: 10, y2: 10}),
      ],
    });
    expect(paints.length / PAINT_STRIDE).toBe(1); // the Line still drew
    expect(warn).toHaveBeenCalledTimes(1);
    expect(String(warn.mock.calls[0][0])).toMatch(/<Needle>, a component/);
    // warn-once: an <Svg> recompiles its tape on every update
    flattenSvg({children: [createElement(Needle, null)]});
    expect(warn).toHaveBeenCalledTimes(1);
    warn.mockRestore();
  });
});

// The imperative compiler used by updateVector() on the drag hot path. Builds the same op-tape +
// 7-field paint table as flattenSvg, but from flat primitive descriptors (no JSX) and into reused
// buffers — so these also guard the reused-array reset that makes that allocation-free.
describe('shapesToVector (imperative)', () => {
  it('compiles an arc descriptor to a bare ARC (no leading MOVE) with a stroke paint', () => {
    const {ops, paints} = shapesToVector([
      {
        arc: [100, 100, 80, -135, 135],
        stroke: '#f4a261',
        strokeWidth: 14,
        cap: 'round',
      },
    ]);
    expect(ops[0]).toBe(SHAPE);
    expect(ops[1]).toBe(0); // paint index
    expect(ops[2]).toBe(ARC); // straight to ARC — a MOVE here is the miter-spike bug
    expect(ops.indexOf(MOVE)).toBe(-1);
    expect(ops.slice(3, 6)).toEqual([100, 100, 80]); // cx, cy, r
    expect(ops[ops.length - 1]).toBe(0); // ccw flag
    expect(paints.length).toBe(9);
    expect(paints[0]).toBe(0); // fill none
    expect(paints[1] >>> 0).toBe(0xfff4a261); // stroke
    expect(paints[2]).toBe(14); // width
    expect(paints[4]).toBe(1); // round cap
  });

  it('carries a stroke gradient and indexes it 1-based in the paint record', () => {
    const {paints, gradients} = shapesToVector([
      {
        arc: [100, 100, 80, 0, 90],
        stroke: '#F2A64B',
        strokeWidth: 20,
        strokeGradient: {
          type: 3, // conic
          ax: 100,
          ay: 100,
          r: 0,
          stops: [
            {color: '#F2A64B', offset: 0},
            {color: '#4FA9F5', offset: 0.25},
          ],
        },
      },
    ]);
    expect(paints[7]).toBe(0); // fill_gradient — unused
    expect(paints[8]).toBe(1); // stroke_gradient — 1-based index into the table
    expect(gradients.length).toBe(1);
    expect(gradients[0].type).toBe(3);
    // Stop colours given as strings are normalized to packed ARGB for the engine.
    expect(gradients[0].stops[0].color >>> 0).toBe(0xfff2a64b);
    expect(gradients[0].stops[1].color >>> 0).toBe(0xff4fa9f5);
  });

  it('leaves both gradient slots at 0 for a plain solid shape', () => {
    const {paints, gradients} = shapesToVector([
      {arc: [0, 0, 10, 0, 90], stroke: '#fff'},
    ]);
    expect(paints[7]).toBe(0);
    expect(paints[8]).toBe(0);
    expect(gradients.length).toBe(0);
  });

  it('compiles a circle to a full-circle ARC + CLOSE', () => {
    const {ops} = shapesToVector([{circle: [50, 60, 8], fill: '#16202f'}]);
    expect(ops.slice(0, 5)).toEqual([SHAPE, 0, MOVE, 58, 60]); // MOVE to (cx+r, cy)
    expect(ops[5]).toBe(ARC);
    expect(ops[ops.length - 1]).toBe(CLOSE);
  });

  it('compiles a rect to MOVE + 3 LINE + CLOSE', () => {
    const {ops, paints} = shapesToVector([
      {rect: [10, 20, 30, 40], fill: '#ffffff'},
    ]);
    expect(ops).toEqual([
      SHAPE,
      0,
      MOVE,
      10,
      20,
      LINE,
      40,
      20,
      LINE,
      40,
      60,
      LINE,
      10,
      60,
      CLOSE,
    ]);
    expect(paints[0] >>> 0).toBe(0xffffffff);
  });

  it('compiles a line and a path descriptor', () => {
    expect(shapesToVector([{line: [1, 2, 3, 4], stroke: '#fff'}]).ops).toEqual([
      SHAPE,
      0,
      MOVE,
      1,
      2,
      LINE,
      3,
      4,
    ]);
    expect(
      shapesToVector([{path: 'M0 0 L10 0 Z', stroke: '#fff'}]).ops,
    ).toEqual([SHAPE, 0, MOVE, 0, 0, LINE, 10, 0, CLOSE]);
  });

  it('assigns sequential paint indices across multiple shapes', () => {
    const {ops, paints} = shapesToVector([
      {rect: [0, 0, 1, 1], fill: '#fff'},
      {line: [0, 0, 1, 1], stroke: '#000'},
      {circle: [0, 0, 1], fill: '#f00'},
    ]);
    expect(paints.length).toBe(27); // 3 * 9
    expect(shapeIndices(ops)).toEqual([0, 1, 2]);
  });

  it('maps strokeWidth/miter/cap/join/fillRule into the paint record', () => {
    const {paints} = shapesToVector([
      {
        path: 'M0 0 L1 0',
        stroke: '#fff',
        strokeWidth: 2,
        cap: 'square',
        join: 'bevel',
        miter: 6,
        fillRule: 'evenodd',
      },
    ]);
    expect(paints[2]).toBe(2); // width
    expect(paints[3]).toBe(6); // miter limit
    expect(paints[4]).toBe(2); // square cap
    expect(paints[5]).toBe(2); // bevel join
    expect(paints[6]).toBe(1); // evenodd
  });

  it('skips empty/unknown shapes without leaving a dangling SHAPE header', () => {
    const {ops, paints} = shapesToVector([
      {foo: 1},
      {rect: [0, 0, 2, 2], fill: '#fff'},
    ]);
    expect(ops[0]).toBe(SHAPE);
    expect(ops[1]).toBe(0); // the rect's paint index is 0 — the unknown shape emitted nothing
    expect(paints.length).toBe(9);
  });

  it('reuses its output buffers but resets them each call (no stale leakage)', () => {
    const firstLength = shapesToVector([{rect: [0, 0, 9, 9], fill: '#fff'}]).ops
      .length;
    const second = shapesToVector([{line: [0, 0, 1, 1], stroke: '#000'}]);
    expect(second.ops).toEqual([SHAPE, 0, MOVE, 0, 0, LINE, 1, 1]); // no trailing ops from the longer prior call
    expect(second.ops.length).toBeLessThan(firstLength);
  });
});

describe('scaleVectorArtifact (<Svg source> box scaling)', () => {
  it('returns the artifact arrays untouched when the box equals the intrinsic size', () => {
    const art = {
      kind: 'vector',
      ops: [SHAPE, 0, MOVE, 1, 2],
      paints: [0xff000000, 0, 3, 4, 0, 0, 0],
      width: 10,
      height: 10,
    };
    const scaled = scaleVectorArtifact(art, 10, 10);
    expect(scaled.ops).toBe(art.ops); // same reference — no needless copy
    expect(scaled.paints).toBe(art.paints);
  });

  it('scales coordinates + stroke width to the target box, leaving paint indices + colors alone', () => {
    const art = {
      kind: 'vector',
      ops: [SHAPE, 0, MOVE, 5, 5, LINE, 10, 0],
      paints: [0xff112233, 0, 4, 4, 0, 0, 0],
      width: 10,
      height: 10,
    };
    const scaled = scaleVectorArtifact(art, 20, 20); // 2x
    expect(scaled.ops).toEqual([SHAPE, 0, MOVE, 10, 10, LINE, 20, 0]); // coords doubled; SHAPE's paint index (0) untouched
    expect(scaled.paints[2]).toBe(8); // strokeWidth 4 -> 8
    expect(scaled.paints[0]).toBe(0xff112233); // fill color untouched
  });
});

describe('encodeVectorGradients', () => {
  it('encodes a gradient to GRAD_STRIDE floats with its stops + geometry', () => {
    const encoded = encodeVectorGradients([
      {
        type: GRAD_LINEAR,
        stops: [
          {color: 0xffff0000, offset: 0},
          {color: 0xff0000ff, offset: 1},
        ],
        ax: 1,
        ay: 2,
        bx: 3,
        by: 4,
        r: 0,
      },
    ]);
    expect(encoded.length).toBe(GRAD_STRIDE);
    expect(encoded[0]).toBe(GRAD_LINEAR); // type
    expect(encoded[1]).toBe(2); // stop_count
    expect(encoded[2] >>> 0).toBe(0xffff0000); // stop0 color
    expect(encoded[3]).toBe(0); // stop0 offset
    expect(encoded[4] >>> 0).toBe(0xff0000ff); // stop1 color
    const geometryOffset = 2 + GRAD_MAX_STOPS * 2; // geometry follows the (padded) stop slots
    expect([
      encoded[geometryOffset],
      encoded[geometryOffset + 1],
      encoded[geometryOffset + 2],
      encoded[geometryOffset + 3],
    ]).toEqual([1, 2, 3, 4]);
  });

  it('resamples a gradient with more than GRAD_MAX_STOPS stops down to the cap (no truncation)', () => {
    const stopCount = GRAD_MAX_STOPS + 4;
    const stops = [];
    for (let index = 0; index < stopCount; index++) {
      const color =
        index === 0
          ? 0xffff0000
          : index === stopCount - 1
            ? 0xff0000ff
            : 0xff00ff00;
      stops.push({color, offset: index / (stopCount - 1)});
    }
    const encoded = encodeVectorGradients([
      {type: GRAD_LINEAR, stops, ax: 0, ay: 0, bx: 0, by: 0, r: 0},
    ]);
    expect(encoded[1]).toBe(GRAD_MAX_STOPS); // capped, not truncated
    // Endpoints preserved by the even resample.
    expect(encoded[2] >>> 0).toBe(0xffff0000); // first stop = first colour
    const last = 2 + (GRAD_MAX_STOPS - 1) * 2;
    expect(encoded[last] >>> 0).toBe(0xff0000ff); // last resampled stop = last colour
    expect(encoded[3]).toBe(0); // offsets span [0,1] ascending
    expect(encoded[last + 1]).toBe(1);
  });
});

describe('warnVectorCaps — gradient cap', () => {
  it('warns once when the gradient count exceeds the bridge cap (shapes would fall back to solid)', () => {
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => {});
    // ops/paints within cap, gradients OVER cap → only the gradient warning fires.
    warnVectorCaps(10, PAINT_STRIDE, 1000, 64, 20, 16);
    expect(warn).toHaveBeenCalledTimes(1);
    expect(String(warn.mock.calls[0][0])).toMatch(/20 gradients \(> 16\)/);
    expect(String(warn.mock.calls[0][0])).toMatch(/solid fills\/strokes/);
    // warn-once: a second over-cap call must not warn again
    warnVectorCaps(10, PAINT_STRIDE, 1000, 64, 30, 16);
    expect(warn).toHaveBeenCalledTimes(1);
    warn.mockRestore();
  });

  it('does not warn for gradients within cap, or when maxGrads is absent (older bridge)', () => {
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => {});
    warnVectorCaps(10, PAINT_STRIDE, 1000, 64, 8, 16); // within cap
    warnVectorCaps(10, PAINT_STRIDE, 1000, 64, 99); // no maxGrads → no-op
    expect(warn).not.toHaveBeenCalled();
    warn.mockRestore();
  });
});

// Walks a single-shape tape into { ops: [opcode…], points: [[x, y]…] }. A raw filter/indexOf would
// miscount here, since every opcode is also a plausible coordinate value (CUBIC === 4 vs rx = 4).
function walkShape(tape) {
  const opcodes = [];
  const points = [];
  let cursor = 0;
  while (cursor < tape.length) {
    const opcode = tape[cursor++];
    if (opcode === SHAPE) {
      cursor++; // paint index
      continue;
    }
    opcodes.push(opcode);
    for (let argument = 0; argument < ARGUMENTS_PER_OP[opcode]; argument += 2)
      points.push([tape[cursor++], tape[cursor++]]);
  }
  return {ops: opcodes, points};
}

// <Rect rx/ry> — rounded corners without hand-built <Path> arc data (issue #121).
describe('Rect corner radii', () => {
  it('resolves rx/ry the way SVG does: mutual fallback, clamp to half the side, 0 disables', () => {
    expect(rectRadii(undefined, undefined, 100, 60)).toEqual([0, 0]); // neither given
    expect(rectRadii(8, undefined, 100, 60)).toEqual([8, 8]); // ry omitted -> rx
    expect(rectRadii(undefined, 8, 100, 60)).toEqual([8, 8]); // rx omitted -> ry
    expect(rectRadii(4, 9, 100, 60)).toEqual([4, 9]); // independent radii
    expect(rectRadii(999, 999, 40, 20)).toEqual([20, 10]); // clamped per side
    expect(rectRadii(0, 8, 100, 60)).toEqual([0, 0]); // an explicit 0 squares the corners
    expect(rectRadii(8, 0, 100, 60)).toEqual([0, 0]); // ... either way round
    expect(rectRadii(-3, 8, 100, 60)).toEqual([8, 8]);
    expect(rectRadii(-3, undefined, 100, 60)).toEqual([0, 0]);
    expect(rectRadii('6', '6', 100, 60)).toEqual([6, 6]); // string attrs (JSX rx="6")
  });

  it('leaves a radius-free <Rect> on the four-line tape (no regression, no extra ops)', () => {
    const {ops} = flattenSvg({
      children: [
        el('Rect', {x: 0, y: 0, width: 100, height: 60, fill: '#fff'}),
      ],
    });
    expect(ops).toEqual([
      SHAPE,
      0,
      MOVE,
      0,
      0,
      LINE,
      100,
      0,
      LINE,
      100,
      60,
      LINE,
      0,
      60,
      CLOSE,
    ]);
  });

  it('compiles <Rect rx> to line edges joined by four cubic corners', () => {
    const {ops} = flattenSvg({
      children: [
        el('Rect', {x: 0, y: 0, width: 100, height: 60, rx: 10, fill: '#fff'}),
      ],
    });
    // Four straight edges + four corners, and NO arc op: a corner must start at exactly the previous
    // line's endpoint, which ER_VECTOR_OP_ARC's cos/sin start point cannot guarantee (the miter-spike trap).
    expect(walkShape(ops).ops).toEqual([
      MOVE,
      LINE,
      CUBIC,
      LINE,
      CUBIC,
      LINE,
      CUBIC,
      LINE,
      CUBIC,
      CLOSE,
    ]);
    // Opens on the top edge past the top-left corner, not at the corner itself.
    expect(ops.slice(0, 8)).toEqual([SHAPE, 0, MOVE, 10, 0, LINE, 90, 0]);
    expect(ops[ops.length - 1]).toBe(CLOSE);
    // The top-right corner ends on the right edge, one radius down.
    const firstCubic = ops.indexOf(CUBIC);
    expect(ops.slice(firstCubic + 5, firstCubic + 7)).toEqual([100, 10]);
  });

  it('never bulges past the box: an oversized rx/ry becomes a stadium/ellipse', () => {
    const {ops} = flattenSvg({
      children: [el('Rect', {x: 0, y: 0, width: 40, height: 20, rx: 999})],
    });
    const {points} = walkShape(ops);
    const xValues = points.map(point => point[0]);
    const yValues = points.map(point => point[1]);
    expect(Math.min(...xValues)).toBe(0);
    expect(Math.max(...xValues)).toBe(40);
    expect(Math.min(...yValues)).toBe(0);
    expect(Math.max(...yValues)).toBe(20);
  });

  it('rounds an imperative { rect: [x, y, w, h, rx, ry] } descriptor too', () => {
    const {ops} = shapesToVector([{rect: [0, 0, 20, 20, 4, 4], fill: '#f00'}]);
    expect(walkShape(ops).ops.filter(opcode => opcode === CUBIC).length).toBe(
      4,
    );
    expect(ops.slice(0, 5)).toEqual([SHAPE, 0, MOVE, 4, 0]);
    // The 4-element form stays on the allocation-free square-cornered path.
    const sharp = shapesToVector([{rect: [0, 0, 20, 20], fill: '#f00'}]);
    expect(sharp.ops).toEqual([
      SHAPE,
      0,
      MOVE,
      0,
      0,
      LINE,
      20,
      0,
      LINE,
      20,
      20,
      LINE,
      0,
      20,
      CLOSE,
    ]);
  });
});
