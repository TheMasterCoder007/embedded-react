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

import { describe, it, expect } from 'vitest';
import { svgToVector } from '../bake-svg.mjs';

const SHAPE = 0;
const MOVE = 1;
const LINE = 2;
const QUAD = 3;
const CUBIC = 4;
const ARC = 5;
const CLOSE = 6;
const RED = 0xffff0000;
const GREEN = 0xff008000;

// Extract just the opcodes from the flat op-tape (opcodes + coords are interleaved, so a raw
// `toContain` can't tell an opcode from a coordinate value that happens to equal it).
function opcodes(ops) {
  const args = { [SHAPE]: 1, [MOVE]: 2, [LINE]: 2, [QUAD]: 4, [CUBIC]: 6, [ARC]: 6, [CLOSE]: 0 };
  const codes = [];
  let i = 0;
  while (i < ops.length) {
    const op = ops[i++];
    codes.push(op);
    i += args[op] ?? 0;
  }
  return codes;
}

describe('bake-svg: svgToVector', () => {
  it('compiles a path to the op-tape with its solid fill', async () => {
    const { ops, paints, width, height } = await svgToVector(
      '<svg viewBox="0 0 10 10"><path d="M0 0 L10 0" fill="red"/></svg>'
    );
    // [SHAPE, paintIdx, MOVE, 0,0, LINE, 10,0]
    expect(ops.slice(0, 8)).toEqual([SHAPE, 0, MOVE, 0, 0, LINE, 10, 0]);
    expect(paints[0]).toBe(RED); // fill
    expect(paints[1]).toBe(0); // stroke = none -> transparent
    expect(width).toBe(10);
    expect(height).toBe(10);
  });

  it('bakes a transform into absolute coordinates', async () => {
    const { ops } = await svgToVector(
      '<svg viewBox="0 0 100 100"><path d="M0 0" transform="translate(10,20)"/></svg>'
    );
    // MOVE point shifted by (10,20)
    expect(ops.slice(0, 5)).toEqual([SHAPE, 0, MOVE, 10, 20]);
  });

  it('bakes the viewBox -> width/height scale', async () => {
    const { ops, width } = await svgToVector('<svg width="20" height="20" viewBox="0 0 10 10"><path d="M5 5"/></svg>');
    // sx = 20/10 = 2 -> (5,5) becomes (10,10)
    expect(ops.slice(0, 5)).toEqual([SHAPE, 0, MOVE, 10, 10]);
    expect(width).toBe(20);
  });

  it('emits primitives as cubic paths (no native arc op) so transforms bake cleanly', async () => {
    const { ops, paints } = await svgToVector('<svg viewBox="0 0 10 10"><circle cx="5" cy="5" r="3" fill="green"/></svg>');
    const codes = opcodes(ops);
    expect(codes).toContain(CUBIC);
    expect(codes).not.toContain(ARC); // a baked arc would not survive an arbitrary matrix
    expect(paints[0]).toBe(GREEN);
  });

  it('inherits paint from a parent <g>', async () => {
    const { paints } = await svgToVector('<svg viewBox="0 0 10 10"><g fill="red"><path d="M0 0 L1 1"/></g></svg>');
    expect(paints[0]).toBe(RED);
  });

  it('rotation actually rotates the geometry (full transform fidelity, no engine support)', async () => {
    const { ops } = await svgToVector(
      '<svg viewBox="0 0 100 100"><path d="M10 0" transform="rotate(90)"/></svg>'
    );
    // rotate(90) maps (10,0) -> (0,10)
    const x = ops[3];
    const y = ops[4];
    expect(Math.round(x)).toBe(0);
    expect(Math.round(y)).toBe(10);
  });

  it('an undefined url() fill (no matching gradient) bakes to nothing', async () => {
    const { paints, gradients } = await svgToVector(
      '<svg viewBox="0 0 10 10"><path d="M0 0 L1 1" fill="url(#missing)"/></svg>'
    );
    expect(paints[0]).toBe(0); // no solid fill
    expect(paints[7]).toBe(0); // no gradient assigned (fill_grad = 0)
    expect(gradients).toHaveLength(0);
  });

  it('bakes a <linearGradient> fill into the gradient table with a 1-based fill_grad', async () => {
    const a = await svgToVector(
      '<svg viewBox="0 0 10 10">' +
        '<defs><linearGradient id="g"><stop offset="0" stop-color="#ff0000"/><stop offset="1" stop-color="#0000ff"/></linearGradient></defs>' +
        '<rect x="0" y="0" width="10" height="10" fill="url(#g)"/>' +
        '</svg>'
    );
    expect(a.gradients).toHaveLength(1);
    const g = a.gradients[0];
    expect(g.type).toBe(1); // GRAD_LINEAR
    expect(g.stops.map((s) => s.color >>> 0)).toEqual([0xffff0000, 0xff0000ff]);
    // objectBoundingBox default: axis (0,0)->(1,0) maps onto the rect's bbox (0,0,10,10).
    expect([Math.round(g.ax), Math.round(g.ay), Math.round(g.bx), Math.round(g.by)]).toEqual([0, 0, 10, 0]);
    // The rect's paint: solid fill 0, fill_grad = 1 (1-based; the 8th paint field).
    expect(a.paints[0]).toBe(0);
    expect(a.paints[7]).toBe(1);
  });

  it('bakes a <radialGradient> fill (centre + radius)', async () => {
    const a = await svgToVector(
      '<svg viewBox="0 0 100 100">' +
        '<defs><radialGradient id="r"><stop offset="0" stop-color="#ffffff"/><stop offset="1" stop-color="#000000"/></radialGradient></defs>' +
        '<rect x="0" y="0" width="100" height="100" fill="url(#r)"/>' +
        '</svg>'
    );
    expect(a.gradients).toHaveLength(1);
    expect(a.gradients[0].type).toBe(2); // GRAD_RADIAL
    expect([Math.round(a.gradients[0].ax), Math.round(a.gradients[0].ay)]).toEqual([50, 50]); // bbox centre
    expect(a.gradients[0].r).toBeGreaterThan(0);
    expect(a.paints[7]).toBe(1);
  });

  it('bakes a STROKE gradient into stroke_grad while the fill stays solid', async () => {
    const a = await svgToVector(
      '<svg viewBox="0 0 10 10">' +
        '<defs><linearGradient id="s"><stop offset="0" stop-color="#ff0000"/><stop offset="1" stop-color="#0000ff"/></linearGradient></defs>' +
        '<rect x="0" y="0" width="10" height="10" fill="#101010" stroke="url(#s)" stroke-width="2"/>' +
        '</svg>'
    );
    expect(a.gradients).toHaveLength(1);
    expect(a.gradients[0].type).toBe(1); // GRAD_LINEAR
    expect(a.paints[0] & 0xffffff).toBe(0x101010); // solid fill kept
    expect(a.paints[7]).toBe(0); // fill_grad = none
    expect(a.paints[8]).toBe(1); // stroke_grad = 1 (the 9th paint field)
  });

  it('bakes opacity / fill-opacity / stroke-opacity into the alpha channel', async () => {
    const a = await svgToVector('<svg viewBox="0 0 10 10"><path d="M0 0 L1 1" fill="#ff0000" opacity="0.5"/></svg>');
    expect(a.paints[0] >>> 24).toBe(0x80); // 0xff * 0.5 -> 128
    expect(a.paints[0] & 0xffffff).toBe(0xff0000); // color preserved

    const b = await svgToVector('<svg viewBox="0 0 10 10"><path d="M0 0 L1 1" stroke="#00ff00" stroke-opacity="0.25"/></svg>');
    expect(b.paints[1] >>> 24).toBe(0x40); // 0xff * 0.25 -> 64, stroke only
  });
});
