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

import {describe, it, expect} from 'vitest';
import {dirname, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';
import {PNG} from 'pngjs';
import {svgToRaster, svgToVector} from '../bake-svg.mts';

// Inter-Regular.ttf and Inter-Bold.ttf.
const FONTS = {
  imports: new Map(),
  directories: [
    resolve(
      dirname(fileURLToPath(import.meta.url)),
      '../../../../assets/fonts',
    ),
  ],
};

const SHAPE = 0;
const MOVE = 1;
const LINE = 2;
const QUAD = 3;
const CUBIC = 4;
const ARC = 5;
const CLOSE = 6;
const DASH = 8;
const DASH_OFFSET = 9;
const RED = 0xffff0000;
const GREEN = 0xff008000;

// Extract just the opcodes from the flat op-tape (opcodes + coords are interleaved, so a raw
// `toContain` can't tell an opcode from a coordinate value that happens to equal it).
function opcodes(ops) {
  const args = {
    [SHAPE]: 1,
    [MOVE]: 2,
    [LINE]: 2,
    [QUAD]: 4,
    [CUBIC]: 6,
    [ARC]: 6,
    [CLOSE]: 0,
    [DASH]: 2,
    [DASH_OFFSET]: 2,
  };
  const codes = [];
  let cursor = 0;
  while (cursor < ops.length) {
    const opcode = ops[cursor++];
    codes.push(opcode);
    cursor += args[opcode] ?? 0;
  }
  return codes;
}

describe('bake-svg: svgToVector', () => {
  it('compiles a path to the op-tape with its solid fill', async () => {
    const {ops, paints, width, height} = await svgToVector(
      '<svg viewBox="0 0 10 10"><path d="M0 0 L10 0" fill="red"/></svg>',
    );
    // [SHAPE, paintIdx, MOVE, 0,0, LINE, 10,0]
    expect(ops.slice(0, 8)).toEqual([SHAPE, 0, MOVE, 0, 0, LINE, 10, 0]);
    expect(paints[0]).toBe(RED); // fill
    expect(paints[1]).toBe(0); // stroke = none -> transparent
    expect(width).toBe(10);
    expect(height).toBe(10);
  });

  it('bakes a transform into absolute coordinates', async () => {
    const {ops} = await svgToVector(
      '<svg viewBox="0 0 100 100"><path d="M0 0" transform="translate(10,20)"/></svg>',
    );
    // MOVE point shifted by (10,20)
    expect(ops.slice(0, 5)).toEqual([SHAPE, 0, MOVE, 10, 20]);
  });

  it('bakes the viewBox -> width/height scale', async () => {
    const {ops, width} = await svgToVector(
      '<svg width="20" height="20" viewBox="0 0 10 10"><path d="M5 5"/></svg>',
    );
    // sx = 20/10 = 2 -> (5,5) becomes (10,10)
    expect(ops.slice(0, 5)).toEqual([SHAPE, 0, MOVE, 10, 10]);
    expect(width).toBe(20);
  });

  it('emits primitives as cubic paths (no native arc op) so transforms bake cleanly', async () => {
    const {ops, paints} = await svgToVector(
      '<svg viewBox="0 0 10 10"><circle cx="5" cy="5" r="3" fill="green"/></svg>',
    );
    const codes = opcodes(ops);
    expect(codes).toContain(CUBIC);
    expect(codes).not.toContain(ARC); // a baked arc would not survive an arbitrary matrix
    expect(paints[0]).toBe(GREEN);
  });

  it('inherits paint from a parent <g>', async () => {
    const {paints} = await svgToVector(
      '<svg viewBox="0 0 10 10"><g fill="red"><path d="M0 0 L1 1"/></g></svg>',
    );
    expect(paints[0]).toBe(RED);
  });

  it('rotation actually rotates the geometry (full transform fidelity, no engine support)', async () => {
    const {ops} = await svgToVector(
      '<svg viewBox="0 0 100 100"><path d="M10 0" transform="rotate(90)"/></svg>',
    );
    // rotate(90) maps (10,0) -> (0,10)
    const x = ops[3];
    const y = ops[4];
    expect(Math.round(x)).toBe(0);
    expect(Math.round(y)).toBe(10);
  });

  it('an undefined url() fill (no matching gradient) bakes to nothing', async () => {
    const {paints, gradients} = await svgToVector(
      '<svg viewBox="0 0 10 10"><path d="M0 0 L1 1" fill="url(#missing)"/></svg>',
    );
    expect(paints[0]).toBe(0); // no solid fill
    expect(paints[7]).toBe(0); // no gradient assigned (fill_gradient = 0)
    expect(gradients).toHaveLength(0);
  });

  it('bakes a <linearGradient> fill into the gradient table with a 1-based fill_gradient', async () => {
    const artifact = await svgToVector(
      '<svg viewBox="0 0 10 10">' +
        '<defs><linearGradient id="g"><stop offset="0" stop-color="#ff0000"/><stop offset="1" stop-color="#0000ff"/></linearGradient></defs>' +
        '<rect x="0" y="0" width="10" height="10" fill="url(#g)"/>' +
        '</svg>',
    );
    expect(artifact.gradients).toHaveLength(1);
    const gradient = artifact.gradients[0];
    expect(gradient.type).toBe(1); // GRAD_LINEAR
    expect(gradient.stops.map(stop => stop.color >>> 0)).toEqual([
      0xffff0000, 0xff0000ff,
    ]);
    // objectBoundingBox default: axis (0,0)->(1,0) maps onto the rect's bbox (0,0,10,10).
    expect([
      Math.round(gradient.ax),
      Math.round(gradient.ay),
      Math.round(gradient.bx),
      Math.round(gradient.by),
    ]).toEqual([0, 0, 10, 0]);
    // The rect's paint: solid fill 0, fill_gradient = 1 (1-based; the 8th paint field).
    expect(artifact.paints[0]).toBe(0);
    expect(artifact.paints[7]).toBe(1);
  });

  it('bakes a <radialGradient> fill (centre + radius)', async () => {
    const artifact = await svgToVector(
      '<svg viewBox="0 0 100 100">' +
        '<defs><radialGradient id="r"><stop offset="0" stop-color="#ffffff"/><stop offset="1" stop-color="#000000"/></radialGradient></defs>' +
        '<rect x="0" y="0" width="100" height="100" fill="url(#r)"/>' +
        '</svg>',
    );
    expect(artifact.gradients).toHaveLength(1);
    expect(artifact.gradients[0].type).toBe(2); // GRAD_RADIAL
    expect([
      Math.round(artifact.gradients[0].ax),
      Math.round(artifact.gradients[0].ay),
    ]).toEqual([50, 50]); // bbox centre
    expect(artifact.gradients[0].r).toBeGreaterThan(0);
    expect(artifact.paints[7]).toBe(1);
  });

  it('bakes a <conicGradient> fill (centre + start angle in radians)', async () => {
    const artifact = await svgToVector(
      '<svg viewBox="0 0 100 100">' +
        '<defs><conicGradient id="c" from="90"><stop offset="0" stop-color="#ff0000"/><stop offset="1" stop-color="#0000ff"/></conicGradient></defs>' +
        '<rect x="0" y="0" width="100" height="100" fill="url(#c)"/>' +
        '</svg>',
    );
    expect(artifact.gradients).toHaveLength(1);
    expect(artifact.gradients[0].type).toBe(3); // GRAD_CONIC
    expect([
      Math.round(artifact.gradients[0].ax),
      Math.round(artifact.gradients[0].ay),
    ]).toEqual([50, 50]); // bbox centre
    expect(artifact.gradients[0].r).toBeCloseTo(Math.PI / 2, 3); // from="90deg" -> π/2 rad start angle
    expect(artifact.paints[7]).toBe(1);
  });

  it('bakes a STROKE gradient into stroke_gradient while the fill stays solid', async () => {
    const artifact = await svgToVector(
      '<svg viewBox="0 0 10 10">' +
        '<defs><linearGradient id="s"><stop offset="0" stop-color="#ff0000"/><stop offset="1" stop-color="#0000ff"/></linearGradient></defs>' +
        '<rect x="0" y="0" width="10" height="10" fill="#101010" stroke="url(#s)" stroke-width="2"/>' +
        '</svg>',
    );
    expect(artifact.gradients).toHaveLength(1);
    expect(artifact.gradients[0].type).toBe(1); // GRAD_LINEAR
    expect(artifact.paints[0] & 0xffffff).toBe(0x101010); // solid fill kept
    expect(artifact.paints[7]).toBe(0); // fill_gradient = none
    expect(artifact.paints[8]).toBe(1); // stroke_gradient = 1 (the 9th paint field)
  });

  it('falls back to default coordinates when <linearGradient> attributes evaluate to NaN', async () => {
    const artifact = await svgToVector(
      '<svg viewBox="0 0 10 10">' +
        '<defs><linearGradient id="g" x1="abc" y1="invalid" x2="bad" y2="none"><stop offset="0" stop-color="#ff0000"/><stop offset="1" stop-color="#0000ff"/></linearGradient></defs>' +
        '<rect x="0" y="0" width="10" height="10" fill="url(#g)"/>' +
        '</svg>',
    );
    expect(artifact.gradients).toHaveLength(1);
    const gradient = artifact.gradients[0];
    // Defaults: x1=0, y1=0, x2=1, y2=0 mapped onto bbox (0,0,10,10) -> (0,0) to (10,0)
    expect([
      Math.round(gradient.ax),
      Math.round(gradient.ay),
      Math.round(gradient.bx),
      Math.round(gradient.by),
    ]).toEqual([0, 0, 10, 0]);
  });

  it('falls back to default coordinates when <radialGradient> and <conicGradient> attributes evaluate to NaN', async () => {
    const radial = await svgToVector(
      '<svg viewBox="0 0 100 100">' +
        '<defs><radialGradient id="r" cx="bad" cy="abc" r="xyz"><stop offset="0" stop-color="#fff"/><stop offset="1" stop-color="#000"/></radialGradient></defs>' +
        '<rect x="0" y="0" width="100" height="100" fill="url(#r)"/>' +
        '</svg>',
    );
    expect(radial.gradients).toHaveLength(1);
    expect([
      Math.round(radial.gradients[0].ax),
      Math.round(radial.gradients[0].ay),
    ]).toEqual([50, 50]); // default cx=0.5, cy=0.5 -> center (50, 50)
    expect(radial.gradients[0].r).toBeCloseTo(50, 1); // default r=0.5 * 100 = 50

    const conic = await svgToVector(
      '<svg viewBox="0 0 100 100">' +
        '<defs><conicGradient id="c" cx="invalid" cy="bad"><stop offset="0" stop-color="#f00"/><stop offset="1" stop-color="#00f"/></conicGradient></defs>' +
        '<rect x="0" y="0" width="100" height="100" fill="url(#c)"/>' +
        '</svg>',
    );
    expect(conic.gradients).toHaveLength(1);
    expect([
      Math.round(conic.gradients[0].ax),
      Math.round(conic.gradients[0].ay),
    ]).toEqual([50, 50]); // default cx=0.5, cy=0.5 -> center (50, 50)
  });

  it('falls back to default coordinates when gradient attributes are infinite', async () => {
    const artifact = await svgToVector(
      '<svg viewBox="0 0 10 10">' +
        '<defs><linearGradient id="g" x1="-Infinity" y1="1e999" x2="Infinity" y2="1e999%"><stop offset="0" stop-color="#ff0000"/><stop offset="1" stop-color="#0000ff"/></linearGradient></defs>' +
        '<rect x="0" y="0" width="10" height="10" fill="url(#g)"/>' +
        '</svg>',
    );
    expect(artifact.gradients).toHaveLength(1);
    const gradient = artifact.gradients[0];
    expect([
      Math.round(gradient.ax),
      Math.round(gradient.ay),
      Math.round(gradient.bx),
      Math.round(gradient.by),
    ]).toEqual([0, 0, 10, 0]);
  });

  it('bakes opacity / fill-opacity / stroke-opacity into the alpha channel', async () => {
    const artifact = await svgToVector(
      '<svg viewBox="0 0 10 10"><path d="M0 0 L1 1" fill="#ff0000" opacity="0.5"/></svg>',
    );
    expect(artifact.paints[0] >>> 24).toBe(0x80); // 0xff * 0.5 -> 128
    expect(artifact.paints[0] & 0xffffff).toBe(0xff0000); // color preserved

    const second = await svgToVector(
      '<svg viewBox="0 0 10 10"><path d="M0 0 L1 1" stroke="#00ff00" stroke-opacity="0.25"/></svg>',
    );
    expect(second.paints[1] >>> 24).toBe(0x40); // 0xff * 0.25 -> 64, stroke only
  });
});

describe('svgToVector — dropped-feature detection (raster-fallback trigger)', () => {
  it('reports no dropped features for a fully vector SVG', async () => {
    const artifact = await svgToVector(
      '<svg viewBox="0 0 10 10"><rect width="10" height="10"/><circle cx="5" cy="5" r="3"/></svg>',
    );
    expect(artifact.dropped).toEqual([]);
  });

  it('flags unsupported visual elements but not metadata/defs/gradients', async () => {
    const artifact = await svgToVector(
      '<svg viewBox="0 0 10 10"><title>t</title><defs></defs><rect width="10" height="10"/><foreignObject/></svg>',
    );
    expect(artifact.dropped).toEqual(['foreignObject']); // title/defs ignored; rect baked; foreignObject recorded
  });

  it('flags a filter/mask/clip-path applied to an otherwise-bakeable shape', async () => {
    const artifact = await svgToVector(
      '<svg viewBox="0 0 10 10"><rect width="10" height="10" filter="url(#f)"/></svg>',
    );
    expect(artifact.dropped).toContain('filter');
  });

  it('bakes a dashed stroke into the tape, scaled like the stroke', async () => {
    const artifact = await svgToVector(
      '<svg viewBox="0 0 10 10" width="20" height="20"><line x1="0" y1="5" x2="10" y2="5" stroke="black" ' +
        'stroke-dasharray="2 1" stroke-dashoffset="1.5"/></svg>',
    );
    expect(artifact.dropped).toEqual([]);
    expect(artifact.ops.slice(0, 8)).toEqual([
      SHAPE,
      0,
      DASH,
      4,
      2,
      DASH_OFFSET,
      3,
      0,
    ]);
    // inherited from a <g>, an odd count repeated to make pairs
    const group = await svgToVector(
      '<svg viewBox="0 0 10 10"><g stroke="red" stroke-dasharray="3,1,2"><line x1="0" y1="0" x2="9" y2="9"/></g></svg>',
    );
    expect(group.dropped).toEqual([]);
    expect(group.ops.slice(2, 11)).toEqual([
      DASH,
      3,
      1,
      DASH,
      2,
      3,
      DASH,
      1,
      2,
    ]);
    // from a style attribute
    const styled = await svgToVector(
      '<svg viewBox="0 0 10 10"><line x1="0" y1="0" x2="9" y2="0" style="stroke:#000;stroke-dasharray:4" /></svg>',
    );
    expect(styled.ops.slice(2, 5)).toEqual([DASH, 4, 4]);
  });

  it('flags a dash pattern the tape cannot hold', async () => {
    const percent = await svgToVector(
      '<svg viewBox="0 0 10 10"><line x1="0" y1="5" x2="10" y2="5" stroke="black" stroke-dasharray="10%"/></svg>',
    );
    expect(percent.dropped).toContain('stroke-dasharray');
    const long = await svgToVector(
      '<svg viewBox="0 0 10 10"><line x1="0" y1="5" x2="10" y2="5" stroke="black" ' +
        `stroke-dasharray="${Array.from({length: 18}, () => 1).join(' ')}"/></svg>`,
    );
    expect(long.dropped).toContain('stroke-dasharray');
  });

  it('writes no dash when there is no visible stroke or no positive dash', async () => {
    // no stroke → dash is inert
    const noStroke = await svgToVector(
      '<svg viewBox="0 0 10 10"><rect width="10" height="10" fill="blue" stroke-dasharray="2 1"/></svg>',
    );
    expect(noStroke.dropped).toEqual([]);
    // dasharray="none" and an all-zero pattern are both solid
    const none = await svgToVector(
      '<svg viewBox="0 0 10 10"><line x1="0" y1="0" x2="9" y2="9" stroke="black" stroke-dasharray="none"/></svg>',
    );
    expect(none.dropped).toEqual([]);
    const zero = await svgToVector(
      '<svg viewBox="0 0 10 10"><line x1="0" y1="0" x2="9" y2="9" stroke="black" stroke-dasharray="0 0"/></svg>',
    );
    expect(zero.dropped).toEqual([]);
    for (const artifact of [noStroke, none, zero])
      expect(opcodes(artifact.ops)).not.toContain(DASH);
  });
});

describe('svgToVector — <rect rx/ry>', () => {
  it('bakes rounded corners as cubics instead of squaring them off', async () => {
    const artifact = await svgToVector(
      '<svg viewBox="0 0 40 20"><rect width="40" height="20" rx="5" fill="#fff"/></svg>',
    );
    // Four edges + four corner cubics. Walk the tape — every opcode is also a plausible coordinate.
    const seen = [];
    const argc = {
      [MOVE]: 2,
      [LINE]: 2,
      [QUAD]: 4,
      [CUBIC]: 6,
      [ARC]: 6,
      [CLOSE]: 0,
    };
    for (let cursor = 0; cursor < artifact.ops.length;) {
      const opcode = artifact.ops[cursor++];
      if (opcode === SHAPE) {
        cursor++;
        continue;
      }
      seen.push(opcode);
      cursor += argc[opcode];
    }
    expect(seen.filter(opcode => opcode === CUBIC).length).toBe(4);
    expect(seen.filter(opcode => opcode === LINE).length).toBe(4);
    // Opens on the top edge one radius in from the left, and stays inside the box.
    expect(artifact.ops.slice(0, 5)).toEqual([SHAPE, 0, MOVE, 5, 0]);
    expect(artifact.dropped).toEqual([]);
  });

  it('leaves a rect with no rx/ry square-cornered', async () => {
    const artifact = await svgToVector(
      '<svg viewBox="0 0 10 10"><rect width="10" height="10" fill="#fff"/></svg>',
    );
    expect(artifact.ops).toEqual([
      SHAPE,
      0,
      MOVE,
      0,
      0,
      LINE,
      10,
      0,
      LINE,
      10,
      10,
      LINE,
      0,
      10,
      CLOSE,
    ]);
  });
});

describe('svgToVector — text', () => {
  const text = (attributes, content = 'N') =>
    svgToVector(
      `<svg viewBox="0 0 100 100"><text font-family="Inter" font-size="20" ${attributes}>${content}</text></svg>`,
      FONTS,
    );

  // The x and y extent of a tape's coordinates.
  function bounds(ops) {
    const args = {
      [SHAPE]: 1,
      [MOVE]: 2,
      [LINE]: 2,
      [QUAD]: 4,
      [CUBIC]: 6,
      [CLOSE]: 0,
    };
    const xs = [];
    const ys = [];
    let index = 0;
    while (index < ops.length) {
      const opcode = ops[index++];
      if (opcode !== SHAPE) {
        for (let offset = 0; offset < args[opcode]; offset += 2) {
          xs.push(ops[index + offset]);
          ys.push(ops[index + offset + 1]);
        }
      }
      index += args[opcode];
    }
    return {
      left: Math.min(...xs),
      right: Math.max(...xs),
      top: Math.min(...ys),
      bottom: Math.max(...ys),
    };
  }

  it('bakes text to one filled shape of glyph outlines, with nothing dropped', async () => {
    const artifact = await text('x="10" y="50" fill="red"');
    expect(artifact.dropped).toEqual([]);
    expect(
      opcodes(artifact.ops).filter(opcode => opcode === SHAPE),
    ).toHaveLength(1);
    expect(artifact.paints[0]).toBe(RED);
    const box = bounds(artifact.ops);
    expect(box.left).toBeGreaterThanOrEqual(10);
    expect(box.bottom).toBeCloseTo(50, 0); // an N sits on the baseline
    expect(box.top).toBeGreaterThan(30); // and is under one font-size tall
  });

  it('honours text-anchor', async () => {
    const start = bounds((await text('x="50" y="50"')).ops);
    const middle = bounds(
      (await text('x="50" y="50" text-anchor="middle"')).ops,
    );
    const end = bounds((await text('x="50" y="50" text-anchor="end"')).ops);
    expect(start.left).toBeGreaterThanOrEqual(50);
    expect((middle.left + middle.right) / 2).toBeCloseTo(50, 0);
    expect(end.right).toBeLessThanOrEqual(50);
  });

  it('honours font-size and letter-spacing', async () => {
    const small = bounds((await text('x="0" y="50"', 'NN')).ops);
    const large = bounds((await text('x="0" y="50" font-size="40"', 'NN')).ops);
    const spaced = bounds(
      (await text('x="0" y="50" letter-spacing="10"', 'NN')).ops,
    );
    expect(large.bottom - large.top).toBeCloseTo(
      2 * (small.bottom - small.top),
      1,
    );
    expect(spaced.right - small.right).toBeCloseTo(10, 1);
  });

  it("honours font-weight with the family's file for that weight", async () => {
    const regular = await text('x="0" y="50"');
    const bold = await text('x="0" y="50" font-weight="bold"');
    expect(bold.ops).not.toEqual(regular.ops);
    await expect(text('x="0" y="50" font-weight="300"')).rejects.toThrow(
      /no font for font-family "Inter" at weight 300: looked for Inter-Light, Inter/,
    );
  });

  it('honours a transform on the element', async () => {
    const turned = bounds(
      (await text('x="50" y="50" transform="rotate(90 50 50)"')).ops,
    );
    // The baseline now runs down the page, with the glyph to its right.
    expect(turned.left).toBeCloseTo(50, 0);
    expect(turned.top).toBeGreaterThanOrEqual(50);
  });

  it('lays a tspan out after the text before it, as its own shape when its paint differs', async () => {
    const artifact = await text('x="0" y="50"', 'N<tspan fill="red">N</tspan>');
    expect(
      opcodes(artifact.ops).filter(opcode => opcode === SHAPE),
    ).toHaveLength(2);
    expect(artifact.paints[9]).toBe(RED);
    const same = await text('x="0" y="50"', 'N<tspan>N</tspan>');
    expect(opcodes(same.ops).filter(opcode => opcode === SHAPE)).toHaveLength(
      1,
    );
    expect(bounds(same.ops).right).toBeCloseTo(bounds(artifact.ops).right, 3);
  });

  it('collapses whitespace around and inside the text', async () => {
    const plain = await text('x="0" y="50"', 'N N');
    const padded = await text('x="0" y="50"', '\n  N \n  N  ');
    expect(padded.ops).toEqual(plain.ops);
  });

  it('fails with the family when no font file matches', async () => {
    await expect(
      svgToVector(
        '<svg><text font-family="Nope, sans-serif">N</text></svg>',
        FONTS,
      ),
    ).rejects.toThrow(
      /no font for font-family "Nope": looked for Nope, Nope-Regular/,
    );
    await expect(
      svgToVector('<svg><text>N</text></svg>', FONTS),
    ).rejects.toThrow(/needs a font-family/);
  });

  it('fails when the font lacks a character', async () => {
    await expect(text('x="0" y="50"', '日')).rejects.toThrow(
      /Inter\.ttf has no glyph for "日"|has no glyph for "日"/,
    );
  });

  it('leaves text it cannot lay out to the raster fallback', async () => {
    expect((await text('x="0" y="50" rotate="20"')).dropped).toEqual([
      'text rotate',
    ]);
    expect((await text('x="0 12" y="50"', 'NN')).dropped).toEqual([
      'text x list',
    ]);
    expect(
      (await text('', '<textPath href="#p">N</textPath>')).dropped,
    ).toEqual(['textPath']);
  });

  it('checks the font and glyphs of text it leaves to the raster fallback', async () => {
    await expect(
      svgToVector(
        '<svg><text font-family="Nope" rotate="20">N</text></svg>',
        FONTS,
      ),
    ).rejects.toThrow(/no font for font-family "Nope"/);
    await expect(text('x="0" y="50" rotate="20"', '日')).rejects.toThrow(
      /has no glyph for "日"/,
    );
  });

  it("draws the raster fallback with the text's own font files, not the machine's", async () => {
    const svg =
      '<svg xmlns="http://www.w3.org/2000/svg" width="40" height="40">' +
      '<text x="4" y="30" font-family="Inter" font-size="30" rotate="20">N</text></svg>';
    const artifact = await svgToVector(svg, FONTS);
    expect(artifact.dropped).toEqual(['text rotate']);
    expect(artifact.fonts.files).toEqual([
      resolve(FONTS.directories[0], 'Inter-Regular.ttf'),
    ]);

    // How many pixels a render inked.
    const inked = async fonts => {
      const {data} = PNG.sync.read((await svgToRaster(svg, fonts)).png);
      let count = 0;
      for (let index = 3; index < data.length; index += 4) {
        if (data[index] > 0) count++;
      }
      return count;
    };
    expect(await inked(artifact.fonts)).toBeGreaterThan(50);
    expect(await inked(undefined)).toBe(0);
  });
});

describe('svgToVector — clip-path and mask', () => {
  const MASK = 7;
  const CLIP = 1;
  const LUMINANCE = 2;
  const ALPHA = 3;
  const PAINT_STRIDE = 9;
  const svg = (body: string): string =>
    `<svg viewBox="0 0 100 100" width="100" height="100">${body}</svg>`;
  /** The mask ops of a tape, as [kind, shapes] pairs in order. */
  const maskOps = (ops: number[]): number[][] => {
    const operands = [1, 2, 2, 4, 6, 6, 0, 2];
    const found: number[][] = [];
    for (let index = 0; index < ops.length; index += 1 + operands[ops[index]])
      if (ops[index] === MASK) found.push([ops[index + 1], ops[index + 2]]);
    return found;
  };

  it('bakes a clip-path ahead of the shape it clips, and stays a vector', async () => {
    const artifact = await svgToVector(
      svg(
        '<clipPath id="c"><rect x="10" y="10" width="20" height="20"/></clipPath>' +
          '<rect width="100" height="100" fill="red" clip-path="url(#c)"/>',
      ),
    );
    expect(artifact.dropped).toEqual([]);
    // MASK clip 1, the clip's rect on the shared clip paint, then the drawn rect.
    expect(artifact.ops.slice(0, 8)).toEqual([
      MASK,
      CLIP,
      1,
      SHAPE,
      0,
      MOVE,
      10,
      10,
    ]);
    expect(maskOps(artifact.ops)).toEqual([[CLIP, 1]]);
    expect(artifact.paints.length / PAINT_STRIDE).toBe(2);
    expect(artifact.paints[PAINT_STRIDE]).toBe(0xffff0000);
  });

  it('clips every shape of a group once, and lifts the clip after it', async () => {
    const artifact = await svgToVector(
      svg(
        '<clipPath id="c"><circle cx="50" cy="50" r="30"/></clipPath>' +
          '<g clip-path="url(#c)"><rect width="100" height="50"/><rect y="50" width="100" height="50"/></g>' +
          '<rect width="5" height="5"/>',
      ),
    );
    expect(artifact.dropped).toEqual([]);
    expect(maskOps(artifact.ops)).toEqual([
      [CLIP, 1],
      [0, 0],
    ]);
  });

  it('reads clip-rule, a transform on the clipPath, and a clip-path given in a style', async () => {
    const artifact = await svgToVector(
      svg(
        '<clipPath id="c" transform="translate(5 0)"><path d="M0 0L10 0L10 10Z" clip-rule="evenodd"/></clipPath>' +
          '<rect width="100" height="100" style="clip-path:url(#c)"/>',
      ),
    );
    expect(artifact.ops.slice(0, 8)).toEqual([
      MASK,
      CLIP,
      1,
      SHAPE,
      0,
      MOVE,
      5,
      0,
    ]);
    expect(artifact.paints[6]).toBe(1); // the clip paint's fill rule
  });

  it('places objectBoundingBox clip units across the element, in its own space', async () => {
    const artifact = await svgToVector(
      svg(
        '<clipPath id="c" clipPathUnits="objectBoundingBox"><rect width="0.5" height="1"/></clipPath>' +
          '<rect x="20" y="40" width="40" height="10" transform="translate(100 0)" clip-path="url(#c)"/>',
      ),
    );
    // Half the box's width, moved with the rect.
    expect(artifact.ops.slice(3, 14)).toEqual([
      SHAPE,
      0,
      MOVE,
      120,
      40,
      LINE,
      140,
      40,
      LINE,
      140,
      50,
    ]);
  });

  it('bakes a mask with its shapes painted, by luminance or by alpha', async () => {
    const body = (type: string): string =>
      svg(
        '<linearGradient id="g"><stop offset="0" stop-color="white"/><stop offset="1" stop-color="black"/></linearGradient>' +
          `<mask id="m"${type}><rect width="100" height="100" fill="url(#g)"/><circle cx="50" cy="50" r="10" fill="white" opacity="0.5"/></mask>` +
          '<rect width="100" height="100" fill="blue" mask="url(#m)"/>',
      );
    const luminance = await svgToVector(body(''));
    expect(luminance.dropped).toEqual([]);
    expect(maskOps(luminance.ops)).toEqual([[LUMINANCE, 2]]);
    expect(luminance.gradients).toHaveLength(1);
    expect(luminance.paints[7]).toBe(1); // the mask rect's gradient
    expect(luminance.paints[PAINT_STRIDE]).toBe(0x80ffffff);
    const alpha = await svgToVector(body(' mask-type="alpha"'));
    expect(maskOps(alpha.ops)).toEqual([[ALPHA, 2]]);
  });

  it("adds a mask's box as a clip only when the mask paints past a box smaller than its element", async () => {
    const mask = (attributes: string): string =>
      svg(
        `<mask id="m" ${attributes}><rect width="100" height="100" fill="white"/></mask>` +
          '<rect x="10" y="10" width="20" height="20" mask="url(#m)"/>',
      );
    // The default box is a tenth past the element on every side.
    expect(maskOps((await svgToVector(mask(''))).ops)).toEqual([
      [LUMINANCE, 1],
    ]);
    const narrowed = await svgToVector(
      mask('x="0" y="0" width="0.5" height="1"'),
    );
    expect(maskOps(narrowed.ops)).toEqual([
      [CLIP, 1],
      [LUMINANCE, 1],
    ]);
    expect(narrowed.ops.slice(3, 11)).toEqual([
      SHAPE,
      1, // the box's paint, after the mask rect's
      MOVE,
      10,
      10,
      LINE,
      20,
      10,
    ]);
    const userSpace = await svgToVector(
      mask('maskUnits="userSpaceOnUse" x="0" y="0" width="15" height="100"'),
    );
    expect(userSpace.ops.slice(5, 11)).toEqual([MOVE, 0, 0, LINE, 15, 0]);
  });

  it("narrows a clip by the clipPath's own clip-path, and nests a clipped shape in a clipped group", async () => {
    const artifact = await svgToVector(
      svg(
        '<clipPath id="a"><rect width="50" height="100"/></clipPath>' +
          '<clipPath id="b" clip-path="url(#a)"><rect width="100" height="50"/></clipPath>' +
          '<clipPath id="c"><circle cx="50" cy="50" r="40"/></clipPath>' +
          '<g clip-path="url(#c)"><rect width="100" height="100" clip-path="url(#b)"/><rect width="9" height="9"/></g>',
      ),
    );
    expect(artifact.dropped).toEqual([]);
    expect(maskOps(artifact.ops)).toEqual([
      [CLIP, 1], // c
      [CLIP, 1], // a
      [CLIP, 1], // b
      [0, 0],
      [CLIP, 1], // c again, for the group's second shape
    ]);
  });

  it('leaves what it cannot apply to the raster fallback', async () => {
    const unknown = await svgToVector(
      svg('<rect width="9" height="9" clip-path="url(#nope)"/>'),
    );
    expect(unknown.dropped).toEqual(['clip-path']);
    const shape = await svgToVector(
      svg('<rect width="9" height="9" clip-path="circle(40%)"/>'),
    );
    expect(shape.dropped).toEqual(['clip-path']);
    const inside = await svgToVector(
      svg(
        '<clipPath id="a"><rect width="5" height="5"/></clipPath>' +
          '<clipPath id="b"><rect width="9" height="9" clip-path="url(#a)"/></clipPath>' +
          '<rect width="9" height="9" clip-path="url(#b)"/>',
      ),
    );
    expect(inside.dropped).toEqual(['clipPath child with a clip-path or mask']);
    const looped = await svgToVector(
      svg(
        '<clipPath id="a" clip-path="url(#a)"><rect width="5" height="5"/></clipPath>' +
          '<rect width="9" height="9" clip-path="url(#a)"/>',
      ),
    );
    expect(looped.dropped).toEqual(['clipPath that applies itself']);
    const clips = Array.from(
      {length: 5},
      (_, index) =>
        `<clipPath id="c${index}"><rect width="50" height="50"/></clipPath>`,
    ).join('');
    const deep = await svgToVector(
      svg(
        `${clips}<g clip-path="url(#c0)"><g clip-path="url(#c1)"><g clip-path="url(#c2)"><g clip-path="url(#c3)">` +
          '<rect width="9" height="9" clip-path="url(#c4)"/></g></g></g></g>',
      ),
    );
    expect(deep.dropped).toEqual(['clip-path and mask nesting']);
  });

  it('matches the rasterized reference where a clip and a mask apply', async () => {
    // The clip's and the mask's own shapes never show: only the masked square's pixels are drawn.
    const artifact = await svgToVector(
      svg(
        '<clipPath id="c"><rect x="0" y="0" width="60" height="100" fill="red"/></clipPath>' +
          '<mask id="m"><rect width="100" height="100" fill="#808080"/></mask>' +
          '<rect x="20" y="20" width="60" height="60" fill="blue" clip-path="url(#c)" mask="url(#m)"/>',
      ),
    );
    expect(artifact.dropped).toEqual([]);
    expect(maskOps(artifact.ops)).toEqual([
      [CLIP, 1],
      [LUMINANCE, 1],
    ]);
    // The grey mask rect, then the drawn square: no red paint is in the table.
    expect(
      Array.from(artifact.paints).filter(value => value === 0xffff0000),
    ).toEqual([]);
  });
});
