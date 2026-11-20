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
import {PATTERN_TILES_MAX, svgToVector} from '../bake-svg.mts';

const SHAPE = 0;
const MOVE = 1;
const MASK = 7;
const IMAGE = 10;
const CLIP = 1;
const NONE = 0;
const PAINT_STRIDE = 9;
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
  ops(tape).map(op => op[0]);
const count = (tape: readonly number[], opcode: number): number =>
  opcodes(tape).filter(code => code === opcode).length;

/** A PNG data URL of @p width x @p height, every pixel @p rgba. */
function pngUrl(
  width: number,
  height: number,
  rgba: [number, number, number, number],
): string {
  const png = new PNG({width, height});
  for (let pixel = 0; pixel < width * height; pixel++)
    png.data.set(rgba, pixel * 4);
  return `data:image/png;base64,${PNG.sync.write(png).toString('base64')}`;
}

// A 16x8 JPEG: red over blue.
const JPEG =
  'data:image/jpeg;base64,/9j/4AAQSkZJRgABAQAASABIAAD/4QBMRXhpZgAATU0AKgAAAAgAAYdpAAQAAAABAAAAGgAAAAAAA6ABAAMAAAABAAEAAKACAAQAAAABAAAAEKADAAQAAAABAAAACAAAAAD/7QA4UGhvdG9zaG9wIDMuMAA4QklNBAQAAAAAAAA4QklNBCUAAAAAABDUHYzZjwCyBOmACZjs+EJ+/8AAEQgACAAQAwEiAAIRAQMRAf/EAB8AAAEFAQEBAQEBAAAAAAAAAAABAgMEBQYHCAkKC//EALUQAAIBAwMCBAMFBQQEAAABfQECAwAEEQUSITFBBhNRYQcicRQygZGhCCNCscEVUtHwJDNicoIJChYXGBkaJSYnKCkqNDU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6g4SFhoeIiYqSk5SVlpeYmZqio6Slpqeoqaqys7S1tre4ubrCw8TFxsfIycrS09TV1tfY2drh4uPk5ebn6Onq8fLz9PX29/j5+v/EAB8BAAMBAQEBAQEBAQEAAAAAAAABAgMEBQYHCAkKC//EALURAAIBAgQEAwQHBQQEAAECdwABAgMRBAUhMQYSQVEHYXETIjKBCBRCkaGxwQkjM1LwFWJy0QoWJDThJfEXGBkaJicoKSo1Njc4OTpDREVGR0hJSlNUVVZXWFlaY2RlZmdoaWpzdHV2d3h5eoKDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uLj5OXm5+jp6vLz9PX29/j5+v/bAEMAExMTExMTIBMTIC0gICAtPS0tLS09TT09PT09TV1NTU1NTU1dXV1dXV1dXXBwcHBwcIODg4ODk5OTk5OTk5OTk//bAEMBFxgYJSMlQCMjQJloVWiZmZmZmZmZmZmZmZmZmZmZmZmZmZmZmZmZmZmZmZmZmZmZmZmZmZmZmZmZmZmZmZmZmf/dAAQAAf/aAAwDAQACEQMRAD8A5/7T/s/rR9p/2f1qtRXu/wBm4f8Al/F/5m39oV/5vwR//9k=';
// A 1x1 GIF.
const GIF =
  'data:image/gif;base64,R0lGODlhAQABAIAAAP///wAAACH5BAEAAAAALAAAAAABAAEAAAICRAEAOw==';

describe('svgToVector — use and symbol', () => {
  it('draws what a use references in its place, with the use’s paint', async () => {
    const used = await svgToVector(
      svg(
        '<defs><path id="p" d="M0 0 L10 0 L10 10Z"/></defs>' +
          '<use href="#p" x="20" y="30" fill="red"/>',
      ),
    );
    const placed = await svgToVector(
      svg('<path d="M20 30 L30 30 L30 40Z" fill="red"/>'),
    );
    expect(used.dropped).toEqual([]);
    expect(used.ops).toEqual(placed.ops);
    expect(used.paints).toEqual(placed.paints);
  });

  it('reads xlink:href, and keeps the referenced element’s own transform and paint', async () => {
    const used = await svgToVector(
      svg(
        '<rect id="r" width="5" height="5" fill="blue" transform="translate(1 2)"/>' +
          '<use xlink:href="#r" x="10" fill="red"/>',
      ),
    );
    expect(count(used.ops, SHAPE)).toBe(2);
    expect(used.ops.slice(0, 5)).toEqual([SHAPE, 0, MOVE, 1, 2]);
    expect(ops(used.ops)[7]).toEqual([MOVE, 11, 2]);
    expect(used.paints[PAINT_STRIDE]).toBe(used.paints[0]);
  });

  it('fits a symbol’s viewBox into the size the use gives it, and never draws a symbol by itself', async () => {
    const used = await svgToVector(
      svg(
        '<symbol id="s" viewBox="0 0 10 10"><rect width="10" height="10"/></symbol>' +
          '<use href="#s" x="10" y="10" width="40" height="20"/>',
      ),
    );
    expect(used.dropped).toEqual([]);
    // Meet: scaled by 2 and centred across the width.
    expect(ops(used.ops).filter(op => op[0] !== SHAPE)[0]).toEqual([
      MOVE,
      20,
      10,
    ]);
    expect(count(used.ops, SHAPE)).toBe(1);
  });

  it('clips a symbol’s content to its viewport only when it reaches past it', async () => {
    const inside = await svgToVector(
      svg(
        '<symbol id="s" viewBox="0 0 10 10"><rect x="1" y="1" width="8" height="8"/></symbol>' +
          '<use href="#s" width="20" height="20"/>',
      ),
    );
    expect(count(inside.ops, MASK)).toBe(0);
    const past = await svgToVector(
      svg(
        '<symbol id="s" viewBox="0 0 10 10"><rect x="5" width="10" height="10"/></symbol>' +
          '<use href="#s" width="20" height="20"/>',
      ),
    );
    expect(ops(past.ops)[0]).toEqual([MASK, CLIP, 1]);
    const visible = await svgToVector(
      svg(
        '<symbol id="s" viewBox="0 0 10 10" overflow="visible"><rect x="5" width="10" height="10"/></symbol>' +
          '<use href="#s" width="20" height="20"/>',
      ),
    );
    expect(count(visible.ops, MASK)).toBe(0);
  });

  it('draws nothing for a use of nothing, and leaves a use that contains itself to the raster fallback', async () => {
    const nothing = await svgToVector(svg('<use href="#nope"/>'));
    expect(nothing.ops).toEqual([]);
    expect(nothing.dropped).toEqual([]);
    const looped = await svgToVector(
      svg('<g id="g"><rect width="5" height="5"/><use href="#g" x="5"/></g>'),
    );
    expect(looped.dropped).toEqual(['use that contains itself']);
  });

  it('applies a clip-path on a use', async () => {
    const clipped = await svgToVector(
      svg(
        '<clipPath id="c"><rect width="5" height="5"/></clipPath>' +
          '<rect id="r" width="10" height="10"/>' +
          '<use href="#r" x="50" clip-path="url(#c)"/>',
      ),
    );
    expect(clipped.dropped).toEqual([]);
    expect(count(clipped.ops, MASK)).toBe(1);
  });
});

describe('svgToVector — pattern', () => {
  const dotPattern =
    '<pattern id="p" width="10" height="10" patternUnits="userSpaceOnUse">' +
    '<circle cx="5" cy="5" r="3" fill="red"/></pattern>';

  it('tiles a pattern across the shape it fills, under a clip of the shape', async () => {
    const artifact = await svgToVector(
      svg(`${dotPattern}<rect width="40" height="20" fill="url(#p)"/>`),
    );
    expect(artifact.dropped).toEqual([]);
    expect(opcodes(artifact.ops).slice(0, 3)).toEqual([MASK, SHAPE, MOVE]);
    expect(ops(artifact.ops)[0]).toEqual([MASK, CLIP, 1]);
    // The clip's shape, then one shape holding a dot for each of the 4 x 2 tiles.
    expect(count(artifact.ops, SHAPE)).toBe(2);
    expect(count(artifact.ops, MOVE)).toBe(1 + 8);
    const dotsPaint = ops(artifact.ops).filter(op => op[0] === SHAPE)[1][1];
    expect(artifact.paints[dotsPaint * PAINT_STRIDE]).toBe(0xffff0000);
    const dotStarts = ops(artifact.ops)
      .filter(op => op[0] === MOVE)
      .slice(1)
      .map(op => op.slice(1));
    expect(dotStarts).toContainEqual([2, 5]);
    expect(dotStarts).toContainEqual([32, 15]);
  });

  it('places tiles across the shape’s box in objectBoundingBox units, and moves them by patternTransform', async () => {
    const box = await svgToVector(
      svg(
        '<pattern id="p" width="0.5" height="0.5"><rect width="2" height="2"/></pattern>' +
          '<rect x="20" y="20" width="40" height="40" fill="url(#p)"/>',
      ),
    );
    const starts = ops(box.ops)
      .filter(op => op[0] === MOVE)
      .slice(1)
      .map(op => op.slice(1));
    expect(starts).toEqual([
      [20, 20],
      [40, 20],
      [20, 40],
      [40, 40],
    ]);
    const moved = await svgToVector(
      svg(
        '<pattern id="p" width="10" height="10" patternUnits="userSpaceOnUse" patternTransform="translate(3 0)">' +
          '<rect width="2" height="2"/></pattern><rect width="20" height="10" fill="url(#p)"/>',
      ),
    );
    const firsts = ops(moved.ops)
      .filter(op => op[0] === MOVE)
      .slice(1)
      .map(op => op[1]);
    expect(firsts).toEqual([-7, 3, 13]);
  });

  it('clips each tile when its shapes reach past it', async () => {
    const artifact = await svgToVector(
      svg(
        '<pattern id="p" width="10" height="10" patternUnits="userSpaceOnUse">' +
          '<path d="M0 0 L10 10" stroke="black"/></pattern>' +
          '<rect width="20" height="10" fill="url(#p)"/>',
      ),
    );
    expect(artifact.dropped).toEqual([]);
    const masks = ops(artifact.ops)
      .filter(op => op[0] === MASK)
      .map(op => op.slice(1));
    expect(masks).toEqual([
      [CLIP, 1],
      [CLIP, 1],
      [NONE, 0],
      [CLIP, 1],
      [CLIP, 1],
    ]);
    const visible = await svgToVector(
      svg(
        '<pattern id="p" width="10" height="10" patternUnits="userSpaceOnUse" overflow="visible">' +
          '<path d="M0 0 L10 10" stroke="black"/></pattern>' +
          '<rect width="20" height="10" fill="url(#p)"/>',
      ),
    );
    expect(count(visible.ops, MASK)).toBe(1);
  });

  it('follows a pattern’s href for its tile and its shapes, and fits a viewBox to the tile', async () => {
    const artifact = await svgToVector(
      svg(
        '<pattern id="base" viewBox="0 0 2 2"><rect width="1" height="1" fill="red"/></pattern>' +
          '<pattern id="p" href="#base" width="10" height="10" patternUnits="userSpaceOnUse"/>' +
          '<rect width="10" height="10" fill="url(#p)"/>',
      ),
    );
    expect(artifact.dropped).toEqual([]);
    expect(ops(artifact.ops).slice(-4, -1)).toEqual([
      [2, 5, 0],
      [2, 5, 5],
      [2, 0, 5],
    ]);
  });

  it('fades the tiles by the fill’s opacity, and strokes the shape over them', async () => {
    const artifact = await svgToVector(
      svg(
        `${dotPattern}<rect width="10" height="10" fill="url(#p)" fill-opacity="0.5" stroke="blue"/>`,
      ),
    );
    const [, dots, outline] = ops(artifact.ops)
      .filter(op => op[0] === SHAPE)
      .map(op => op[1]);
    expect(artifact.paints[dots * PAINT_STRIDE] >>> 24).toBe(128);
    // The clip, the dots, then the outline, unclipped.
    expect(
      ops(artifact.ops)
        .filter(op => op[0] === MASK)
        .map(op => op.slice(1)),
    ).toEqual([
      [CLIP, 1],
      [NONE, 0],
    ]);
    expect(artifact.paints[outline * PAINT_STRIDE]).toBe(0);
    expect(artifact.paints[outline * PAINT_STRIDE + 1]).toBe(0xff0000ff);
  });

  it('leaves what it cannot tile to the raster fallback', async () => {
    const stroked = await svgToVector(
      svg(
        `${dotPattern}<rect width="10" height="10" fill="none" stroke="url(#p)"/>`,
      ),
    );
    expect(stroked.dropped).toEqual(['pattern stroke']);
    const gradient = await svgToVector(
      svg(
        '<linearGradient id="g"><stop offset="0"/><stop offset="1" stop-color="red"/></linearGradient>' +
          '<pattern id="p" width="10" height="10" patternUnits="userSpaceOnUse">' +
          '<rect width="5" height="5" fill="url(#g)"/></pattern><rect width="10" height="10" fill="url(#p)"/>',
      ),
    );
    expect(gradient.dropped).toEqual(['gradient inside a pattern']);
    const many = await svgToVector(
      svg(
        '<pattern id="p" width="1" height="1" patternUnits="userSpaceOnUse"><rect width="0.5" height="0.5"/></pattern>' +
          '<rect width="100" height="100" fill="url(#p)"/>',
      ),
    );
    expect(many.dropped).toEqual([
      `pattern of more than ${PATTERN_TILES_MAX} tiles`,
    ]);
    const nested = await svgToVector(
      svg(
        `${dotPattern}<pattern id="q" width="20" height="20" patternUnits="userSpaceOnUse">` +
          '<rect width="10" height="10" fill="url(#p)"/></pattern><rect width="10" height="10" fill="url(#q)"/>',
      ),
    );
    expect(nested.dropped).toEqual(['pattern inside a pattern']);
  });
});

describe('svgToVector — embedded images', () => {
  const imageOp = (tape: readonly number[]): number[] =>
    ops(tape).find(op => op[0] === IMAGE)!;

  it('registers a PNG and draws it in place, its pixels mapped to its box', async () => {
    const artifact = await svgToVector(
      svg(
        '<rect width="100" height="100" fill="blue"/>' +
          `<image href="${pngUrl(2, 2, [255, 0, 0, 255])}" x="10" y="20" width="40" height="40"/>` +
          '<rect width="5" height="5"/>',
      ),
    );
    expect(artifact.dropped).toEqual([]);
    expect(
      opcodes(artifact.ops).filter(code => code === SHAPE || code === IMAGE),
    ).toEqual([SHAPE, IMAGE, SHAPE]);
    expect(artifact.images).toHaveLength(1);
    const [{name, png}] = artifact.images;
    expect(name).toMatch(/^svg-image-[0-9a-f]{6}$/);
    expect(PNG.sync.read(readFileSync(png)).width).toBe(2);
    const [, id, alpha, ...matrix] = imageOp(artifact.ops);
    expect(id).toBe(parseInt(name.slice(-6), 16));
    expect(alpha).toBe(1);
    expect(matrix).toEqual([20, 0, 0, 20, 10, 20]);
  });

  it('fits the image to its box by preserveAspectRatio, and clips a slice to the box', async () => {
    const url = pngUrl(4, 2, [0, 0, 0, 255]);
    const meet = await svgToVector(
      svg(`<image href="${url}" width="40" height="40"/>`),
    );
    expect(imageOp(meet.ops).slice(3)).toEqual([10, 0, 0, 10, 0, 10]);
    expect(count(meet.ops, MASK)).toBe(0);
    const none = await svgToVector(
      svg(
        `<image href="${url}" width="40" height="40" preserveAspectRatio="none"/>`,
      ),
    );
    expect(imageOp(none.ops).slice(3)).toEqual([10, 0, 0, 20, 0, 0]);
    const slice = await svgToVector(
      svg(
        `<image href="${url}" width="40" height="40" preserveAspectRatio="xMinYMin slice" opacity="0.5"/>`,
      ),
    );
    expect(imageOp(slice.ops).slice(2)).toEqual([0.5, 20, 0, 0, 20, 0, 0]);
    expect(ops(slice.ops)[0]).toEqual([MASK, CLIP, 1]);
  });

  it('takes its size from the image when the element gives none, and keeps its aspect with one side', async () => {
    const url = pngUrl(4, 2, [0, 0, 0, 255]);
    const natural = await svgToVector(svg(`<image href="${url}"/>`));
    expect(imageOp(natural.ops).slice(3)).toEqual([1, 0, 0, 1, 0, 0]);
    const wide = await svgToVector(
      svg(`<image href="${url}" width="8" transform="translate(5 5)"/>`),
    );
    expect(imageOp(wide.ops).slice(3)).toEqual([2, 0, 0, 2, 5, 5]);
  });

  it('decodes a JPEG and a GIF to PNGs of their own size, and shares one image used twice', async () => {
    const artifact = await svgToVector(
      svg(
        `<image href="${JPEG}"/><image href="${GIF}" x="10"/><image href="${JPEG}" x="20"/>`,
      ),
    );
    expect(artifact.dropped).toEqual([]);
    expect(artifact.images).toHaveLength(2);
    const sizes = artifact.images.map(({png}) => {
      const {width, height} = PNG.sync.read(readFileSync(png));
      return [width, height];
    });
    expect(sizes).toEqual([
      [16, 8],
      [1, 1],
    ]);
    const jpeg = PNG.sync.read(readFileSync(artifact.images[0].png)).data;
    expect(jpeg[0]).toBeGreaterThan(200);
    expect(jpeg[(16 * 7 + 15) * 4 + 2]).toBeGreaterThan(200);
    expect(count(artifact.ops, IMAGE)).toBe(3);
  });

  it('repeats an image in a pattern’s tiles', async () => {
    const artifact = await svgToVector(
      svg(
        '<pattern id="p" width="10" height="10" patternUnits="userSpaceOnUse">' +
          `<image href="${pngUrl(1, 1, [0, 255, 0, 255])}" width="10" height="10"/></pattern>` +
          '<rect width="20" height="10" fill="url(#p)"/>',
      ),
    );
    expect(artifact.dropped).toEqual([]);
    expect(
      ops(artifact.ops)
        .filter(op => op[0] === IMAGE)
        .map(op => op.slice(7)),
    ).toEqual([
      [0, 0],
      [10, 0],
    ]);
  });

  it('leaves an image out of a clip, and what it cannot draw to the raster fallback', async () => {
    const url = pngUrl(1, 1, [0, 0, 0, 255]);
    const clipped = await svgToVector(
      svg(
        `<clipPath id="c"><image href="${url}" width="9" height="9"/><rect width="5" height="5"/></clipPath>` +
          '<rect width="9" height="9" clip-path="url(#c)"/>',
      ),
    );
    expect(clipped.dropped).toEqual([]);
    expect(clipped.images).toEqual([]);
    const masked = await svgToVector(
      svg(
        `<mask id="m"><image href="${url}" width="9" height="9"/></mask>` +
          '<rect width="9" height="9" mask="url(#m)"/>',
      ),
    );
    expect(masked.dropped).toEqual(['image inside a mask']);
    const vector = await svgToVector(
      svg(
        '<image href="data:image/svg+xml;utf8,%3Csvg/%3E" width="9" height="9"/>',
      ),
    );
    expect(vector.dropped).toEqual(['image of type image/svg+xml']);
    const broken = await svgToVector(
      svg('<image href="data:image/png;base64,AAAA" width="9" height="9"/>'),
    );
    expect(broken.dropped).toEqual(['image that is not a readable PNG']);
  });
});

describe('svgToVector — references outside the file', () => {
  it('fails the build with the reference', async () => {
    await expect(
      svgToVector(svg('<image href="photo.png" width="9" height="9"/>')),
    ).rejects.toThrow('"photo.png"');
    await expect(
      svgToVector(svg('<use xlink:href="icons.svg#star"/>')),
    ).rejects.toThrow('"icons.svg#star"');
    await expect(
      svgToVector(svg('<rect width="9" height="9" fill="url(other.svg#g)"/>')),
    ).rejects.toThrow('"other.svg#g"');
    await expect(
      svgToVector(
        svg(
          '<rect width="9" height="9" style="clip-path: url(\'https://example.com/a.svg#c\')"/>',
        ),
      ),
    ).rejects.toThrow('"https://example.com/a.svg#c"');
  });

  it('lets a link name anything', async () => {
    const linked = await svgToVector(
      svg('<a href="https://example.com"><rect width="9" height="9"/></a>'),
    );
    expect(linked.dropped).toEqual(['a']);
  });
});
