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
import {readFileSync} from 'node:fs';
import {PNG} from 'pngjs';
import {svgToVector} from '../bake-svg.mts';

const SHAPE = 0;
const MASK = 7;
const IMAGE = 10;
const OPERANDS = [1, 2, 2, 4, 6, 6, 0, 2, 2, 2, 8];

const svg = (body: string): string =>
  `<svg viewBox="0 0 100 100" width="100" height="100">${body}</svg>`;

/** Each op of a tape, opcode first. */
function ops(tape: readonly number[]): number[][] {
  const found: number[][] = [];
  for (let index = 0; index < tape.length; index += 1 + OPERANDS[tape[index]])
    found.push(tape.slice(index, index + 1 + OPERANDS[tape[index]]));
  return found;
}
const opcodes = (tape: readonly number[]): number[] =>
  ops(tape)
    .map(op => op[0])
    .filter(code => code === SHAPE || code === MASK || code === IMAGE);

/** The pixels of an image the tape draws, straight RGBA. */
function pixels(png: string): PNG {
  return PNG.sync.read(readFileSync(png));
}

const BLUR = '<filter id="f"><feGaussianBlur stdDeviation="2"/></filter>';
const GLOW =
  '<filter id="f" x="-50%" y="-50%" width="200%" height="200%">' +
  '<feGaussianBlur in="SourceGraphic" stdDeviation="2" result="blur"/>' +
  '<feMerge><feMergeNode in="blur"/><feMergeNode in="SourceGraphic"/></feMerge></filter>';
const DROP_SHADOW =
  '<filter id="f"><feDropShadow dx="3" dy="3" stdDeviation="1" flood-color="#ff0000"/></filter>';
// As a design tool exports a drop shadow: the element blended over the shadow last.
const BLENDED_SHADOW =
  '<filter id="f" x="0" y="0" width="100" height="100" filterUnits="userSpaceOnUse">' +
  '<feFlood flood-opacity="0" result="BackgroundImageFix"/>' +
  '<feOffset in="SourceAlpha" dy="3"/><feGaussianBlur stdDeviation="2"/>' +
  '<feColorMatrix type="matrix" values="0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0.5 0"/>' +
  '<feBlend mode="normal" in2="BackgroundImageFix" result="shadow"/>' +
  '<feBlend mode="normal" in="SourceGraphic" in2="shadow" result="shape"/></filter>';

describe('svgToVector — filters', () => {
  it('draws a blurred shape as an image and keeps the rest a vector', async () => {
    const baked = await svgToVector(
      svg(
        `<defs>${BLUR}</defs>` +
          '<rect x="0" y="0" width="10" height="10" fill="blue"/>' +
          '<circle cx="50" cy="50" r="10" fill="red" filter="url(#f)"/>',
      ),
    );
    expect(baked.dropped).toEqual([]);
    expect(opcodes(baked.ops)).toEqual([SHAPE, IMAGE]);
    expect(baked.images).toHaveLength(1);

    // Cut to what the blur draws: the circle's box and about three deviations past it.
    const image = ops(baked.ops).find(op => op[0] === IMAGE)!;
    const [, , alpha, a, b, c, d, x, y] = image;
    expect([alpha, a, b, c, d]).toEqual([1, 1, 0, 0, 1]);
    const drawn = pixels(baked.images[0].png);
    expect(x).toBeGreaterThan(32);
    expect(x).toBeLessThan(40);
    expect(x + drawn.width).toBeGreaterThan(60);
    expect(x + drawn.width).toBeLessThan(68);
    expect(y).toBe(x);
  });

  it('draws only what a glow puts under its group, and the group as a vector over it', async () => {
    const baked = await svgToVector(
      svg(
        `<defs>${GLOW}</defs>` +
          '<g fill="#38bdf8" filter="url(#f)">' +
          '<circle cx="30" cy="30" r="8"/><rect x="50" y="20" width="20" height="5"/></g>',
      ),
    );
    expect(baked.dropped).toEqual([]);
    expect(opcodes(baked.ops)).toEqual([IMAGE, SHAPE, SHAPE]);
  });

  it('draws a drop shadow without its shape, which stays a vector', async () => {
    for (const filter of [DROP_SHADOW, BLENDED_SHADOW]) {
      const baked = await svgToVector(
        svg(
          `<defs>${filter}</defs>` +
            '<rect x="20" y="20" width="40" height="30" fill="#ffffff" filter="url(#f)"/>',
        ),
      );
      expect(baked.dropped).toEqual([]);
      expect(opcodes(baked.ops)).toEqual([IMAGE, SHAPE]);
      // The shadow is red; none of the white shape is in it.
      const drawn = pixels(baked.images[0].png);
      for (let pixel = 0; pixel < drawn.width * drawn.height; pixel++) {
        if (!drawn.data[pixel * 4 + 3]) continue;
        expect(drawn.data[pixel * 4 + 1]).toBe(0);
      }
    }
  });

  it('keeps the whole element in the image when the filter changes it', async () => {
    const baked = await svgToVector(
      svg(
        '<defs><filter id="f"><feGaussianBlur stdDeviation="2" result="b"/>' +
          '<feBlend mode="multiply" in="SourceGraphic" in2="b"/></filter></defs>' +
          '<rect x="20" y="20" width="40" height="30" fill="red" filter="url(#f)"/>',
      ),
    );
    expect(opcodes(baked.ops)).toEqual([IMAGE]);
  });

  it('puts the element’s own opacity in the image and fades it by its parents’', async () => {
    const baked = await svgToVector(
      svg(
        `<defs>${BLUR}</defs>` +
          '<g opacity="0.5"><rect x="20" y="20" width="40" height="30" fill="red" ' +
          'opacity="0.5" filter="url(#f)"/></g>',
      ),
    );
    const image = ops(baked.ops).find(op => op[0] === IMAGE)!;
    expect(image[2]).toBe(0.5);
    const drawn = pixels(baked.images[0].png);
    const middle =
      (Math.floor(drawn.height / 2) * drawn.width +
        Math.floor(drawn.width / 2)) *
      4;
    expect(drawn.data[middle + 3]).toBeGreaterThan(120);
    expect(drawn.data[middle + 3]).toBeLessThan(135);
  });

  it('clips the image by a clip around the element, and draws the element’s own clip into it', async () => {
    const clip =
      '<clipPath id="c"><rect x="0" y="0" width="40" height="100"/></clipPath>';
    const around = await svgToVector(
      svg(
        `<defs>${BLUR}${clip}</defs>` +
          '<g clip-path="url(#c)"><circle cx="40" cy="50" r="10" fill="red" filter="url(#f)"/></g>',
      ),
    );
    expect(opcodes(around.ops)).toEqual([MASK, SHAPE, IMAGE]);

    const own = await svgToVector(
      svg(
        `<defs>${BLUR}${clip}</defs>` +
          '<circle cx="40" cy="50" r="10" fill="red" filter="url(#f)" clip-path="url(#c)"/>',
      ),
    );
    expect(opcodes(own.ops)).toEqual([IMAGE]);
    const image = ops(own.ops).find(op => op[0] === IMAGE)!;
    expect(image[7] + pixels(own.images[0].png).width).toBe(40);
  });

  it('draws the element where its transforms and the viewBox put it', async () => {
    const baked = await svgToVector(
      `<svg viewBox="0 0 50 50" width="100" height="100"><defs>${BLUR}</defs>` +
        '<g transform="translate(10 0)"><rect x="5" y="20" width="10" height="10" ' +
        'transform="translate(0 5)" fill="red" filter="url(#f)"/></g></svg>',
    );
    const image = ops(baked.ops).find(op => op[0] === IMAGE)!;
    const drawn = pixels(baked.images[0].png);
    // The rect spans x 30..50 and y 50..70 in pixels, blurred about 6 px past that.
    expect(image[7] + drawn.width / 2).toBeCloseTo(40, 0);
    expect(image[8] + drawn.height / 2).toBeCloseTo(60, 0);
  });

  it('draws nothing for a filter that draws nothing', async () => {
    const baked = await svgToVector(
      svg(
        '<defs><filter id="f"><feFlood flood-opacity="0"/></filter></defs>' +
          '<rect width="10" height="10" filter="url(#f)"/>',
      ),
    );
    expect(baked.dropped).toEqual([]);
    expect(baked.ops).toEqual([]);
  });

  it('ignores a filter on a shape in a clipPath and falls back for one in a mask', async () => {
    const clipped = await svgToVector(
      svg(
        `<defs>${BLUR}<clipPath id="c"><rect width="40" height="40" filter="url(#f)"/></clipPath></defs>` +
          '<rect width="100" height="100" fill="red" clip-path="url(#c)"/>',
      ),
    );
    expect(clipped.dropped).toEqual([]);
    expect(clipped.images).toEqual([]);

    const masked = await svgToVector(
      svg(
        `<defs>${BLUR}<mask id="m"><rect width="40" height="40" fill="white" filter="url(#f)"/></mask></defs>` +
          '<rect width="100" height="100" fill="red" mask="url(#m)"/>',
      ),
    );
    expect(masked.dropped).toEqual(['filter inside a mask']);
  });

  it('draws a filtered use, references and inherited paint included', async () => {
    const baked = await svgToVector(
      svg(
        `<defs>${BLUR}<linearGradient id="g"><stop offset="0" stop-color="red"/>` +
          '<stop offset="1" stop-color="blue"/></linearGradient>' +
          '<path id="p" d="M0 0 H20 V20 H0 Z"/></defs>' +
          '<g fill="url(#g)" data-note="a &amp; b"><use href="#p" x="40" y="40" filter="url(#f)"/></g>',
      ),
    );
    expect(baked.dropped).toEqual([]);
    expect(opcodes(baked.ops)).toEqual([IMAGE]);
    // Red on the left, blue on the right.
    const drawn = pixels(baked.images[0].png);
    const row = Math.floor(drawn.height / 2) * drawn.width * 4;
    const left = row + Math.floor(drawn.width * 0.3) * 4;
    const right = row + Math.floor(drawn.width * 0.7) * 4;
    expect(drawn.data[left]).toBeGreaterThan(drawn.data[left + 2]);
    expect(drawn.data[right + 2]).toBeGreaterThan(drawn.data[right]);
  });
});
