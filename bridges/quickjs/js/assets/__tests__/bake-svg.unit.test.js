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

  it('url() paints (gradients) bake to nothing in v1', async () => {
    const { paints } = await svgToVector(
      '<svg viewBox="0 0 10 10"><path d="M0 0 L1 1" fill="url(#grad)"/></svg>'
    );
    expect(paints[0]).toBe(0); // no fill yet — Track B/C handle gradients
  });
});
