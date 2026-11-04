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

// Build-time font baker: TrueType/OpenType → the engine's BitmapFont glyph data, at the exact pixel
// sizes the app uses. Pure JS (opentype.js + the local rasterizer) — no native deps, no Python.
//
// The output is consumed zero-copy by er_font_register() (the BitmapFont is a flash-resident const),
// so there is no runtime rasterizer and no font pool. Per-pixel coverage is packed to the requested
// bits-per-pixel exactly as engine/text/text_renderer.c decodes it:
//   bpp 1 → 1 bit/px, MSB = leftmost, set when coverage >= 128
//   bpp 2 → 4 px/byte, value = round(cov*3/255)   (decoded as value*85)
//   bpp 4 → 2 px/byte, high nibble = left, value = round(cov*15/255)   (decoded as value*17)
//   bpp 8 → 1 byte/px, raw coverage
import fs from 'node:fs';
import opentype from 'opentype.js';
import {rasterize} from './rasterize.mjs';

/** Default dense glyph range: printable ASCII (0x20..0x7E). */
export const ASCII_FIRST = 0x20;
export const ASCII_LAST = 0x7e;

const SPACE_CODEPOINT = 0x20;
const MAX_CODEPOINT = 0x10ffff;
const SUPPORTED_BPP = [1, 2, 4, 8];
const DEFAULT_BPP = 4;
const BITS_PER_BYTE = 8;
const UINT8_MAX = 255;
const COVERAGE_MAX = 255;
const COVERAGE_THRESHOLD_1BPP = 128;
const SUPERSAMPLE = 4;
const SUPERSAMPLE_8BPP = 8;
const MIN_CURVE_STEPS = 6;
const PIXELS_PER_CURVE_STEP = 3;

// Named extra-symbol sets selectable from assets.config.js (glyphs: 'common' | 'minimal' | ...).
const SYMBOLS_MINIMAL = [
  0x00b0, 0x00b1, 0x00b5, 0x00d7, 0x00f7, 0x2013, 0x2014, 0x2022, 0x2026,
  0x2190, 0x2191, 0x2192, 0x2193, 0x2713, 0x2717,
];
const SYMBOLS_COMMON = [
  0x00a2, 0x00a3, 0x00a5, 0x00a7, 0x00a9, 0x00ae, 0x00b0, 0x00b1, 0x00b5,
  0x00d7, 0x00f7, 0x2013, 0x2014, 0x2018, 0x2019, 0x201c, 0x201d, 0x2020,
  0x2021, 0x2022, 0x2026, 0x2030, 0x20ac, 0x2122, 0x2190, 0x2191, 0x2192,
  0x2193, 0x2194, 0x21b5, 0x2202, 0x2206, 0x221a, 0x221e, 0x2211, 0x2212,
  0x2248, 0x2260, 0x2264, 0x2265, 0x25a0, 0x25cf, 0x25c6, 0x2605, 0x2606,
  0x2713, 0x2717,
];
const SYMBOLS_GREEK = [];
for (let codepoint = 0x0391; codepoint <= 0x03a9; codepoint++)
  if (codepoint !== 0x03a2) SYMBOLS_GREEK.push(codepoint); // uppercase (skip gap)
for (let codepoint = 0x03b1; codepoint <= 0x03c9; codepoint++)
  SYMBOLS_GREEK.push(codepoint); // lowercase
const SYMBOL_SETS = {
  none: [],
  minimal: SYMBOLS_MINIMAL,
  common: SYMBOLS_COMMON,
  greek: SYMBOLS_GREEK,
  'common-greek': [...new Set([...SYMBOLS_COMMON, ...SYMBOLS_GREEK])].sort(
    (left, right) => left - right,
  ),
};

// The engine's built-in font (engine/font/font_data.c) covers printable ASCII plus these. It is its
// own list, not an alias of a named set: build-builtin-font.mjs bakes from it and the coverage check
// reports against it, so the two can never drift — while the named sets stay free to evolve.
// Changing it changes the engine's fallback font; re-run `npm run build:builtin-font` if you do.
export const BUILTIN_EXTRAS = Object.freeze([
  0x00a2, 0x00a3, 0x00a5, 0x00a7, 0x00a9, 0x00ae, 0x00b0, 0x00b1, 0x00b5,
  0x00d7, 0x00f7, 0x2013, 0x2014, 0x2018, 0x2019, 0x201c, 0x201d, 0x2020,
  0x2021, 0x2022, 0x2026, 0x2030, 0x20ac, 0x2122, 0x2190, 0x2191, 0x2192,
  0x2193, 0x2194, 0x21b5, 0x2202, 0x2206, 0x2211, 0x2212, 0x221a, 0x221e,
  0x2248, 0x2260, 0x2264, 0x2265, 0x25a0, 0x25c6, 0x25cf, 0x2605, 0x2606,
  0x2713, 0x2717,
]);

/**
 * Pixel sizes the engine's built-in font is baked at (engine/font/font_data.c). Like BUILTIN_EXTRAS
 * this is the single source of truth: build-builtin-font.mjs bakes from it and the build-time size
 * check reports against it, so an app rendering at some other size is told what it will snap to.
 * Changing it changes the engine's fallback font; re-run `npm run build:builtin-font` if you do.
 */
export const BUILTIN_SIZES = Object.freeze([10, 12, 16, 20, 24, 32, 48]);

/**
 * Normalizes one codepoint list: numbers are codepoints, a string contributes every character in
 * it — so `'°±'` and `[0x00b0, 0x00b1]` mean the same thing.
 *
 * @param {number|string|Array<number|string>|undefined} spec
 * @param {string} field  Config field name, for error messages.
 * @returns {number[]} Codepoints, unsorted and possibly duplicated.
 */
function toCodepoints(spec, field) {
  if (spec === undefined || spec === null) return [];
  const codepoints = [];
  for (const item of Array.isArray(spec) ? spec : [spec]) {
    if (typeof item === 'string') {
      for (const character of item) codepoints.push(character.codePointAt(0));
    } else if (
      typeof item === 'number' &&
      Number.isInteger(item) &&
      item >= 0 &&
      item <= MAX_CODEPOINT
    ) {
      codepoints.push(item);
    } else {
      throw new Error(
        `${field}: expected a codepoint number or a string of characters, got ${JSON.stringify(item)}`,
      );
    }
  }
  return codepoints;
}

/**
 * Resolves a glyph-coverage spec into a sorted list of extra (sparse) codepoints.
 *
 * `glyphs` picks the baseline: 'ascii' (none), a named set, or an explicit list. `extraGlyphs` adds
 * the per-app characters no named set can cover, as codepoints or as the characters themselves.
 *
 * @param {string|Array<number|string>|undefined} glyphs      'ascii', a named set, or explicit codepoints.
 * @param {number|string|Array<number|string>} [extraGlyphs]  Per-app additions, on top of `glyphs`.
 * @returns {number[]} Sorted extra codepoints outside the dense ASCII range.
 */
export function resolveExtras(glyphs, extraGlyphs) {
  let codepoints;
  if (!glyphs || glyphs === 'ascii') codepoints = [];
  else if (typeof glyphs === 'string') {
    if (!SYMBOL_SETS[glyphs])
      throw new Error(
        `unknown glyph set "${glyphs}" (use 'ascii', one of [${Object.keys(SYMBOL_SETS).join(', ')}], or a list of codepoints/characters)`,
      );
    codepoints = SYMBOL_SETS[glyphs].slice();
  } else codepoints = toCodepoints(glyphs, 'glyphs');

  // A bare set name here is almost certainly meant for `glyphs` — as characters it would be a
  // silent no-op (all ASCII), so say so instead of baking nothing.
  if (typeof extraGlyphs === 'string' && SYMBOL_SETS[extraGlyphs])
    throw new Error(
      `extraGlyphs: "${extraGlyphs}" is a named glyph set — put it in \`glyphs\` (extraGlyphs takes the characters themselves)`,
    );
  codepoints.push(...toCodepoints(extraGlyphs, 'extraGlyphs'));

  return [...new Set(codepoints)]
    .filter(codepoint => codepoint < ASCII_FIRST || codepoint > ASCII_LAST)
    .sort((left, right) => left - right);
}

/**
 * Opens a font file and returns a predicate for "does this file have a glyph for this codepoint?".
 * The coverage check uses it to tell a missing bake ("add it to extraGlyphs") apart from a font that
 * simply cannot draw the character ("pick a different font").
 *
 * @param {string} path  Path to the .ttf/.otf file.
 * @returns {(codepoint:number)=>boolean} Always true if the file can't be read, so a probe failure never
 *          turns into a misleading claim about the font.
 */
export function loadGlyphProbe(path) {
  try {
    const font = opentype.parse(fs.readFileSync(path).buffer);
    return codepoint =>
      font.charToGlyphIndex(String.fromCodePoint(codepoint)) !== 0;
  } catch {
    return () => true;
  }
}

/**
 * Packs an 8-bit coverage bitmap (one byte per pixel) into the engine's per-row bpp layout.
 *
 * @param {Uint8Array} coverage  Row-major coverage, width*height bytes.
 * @param {number} width         Glyph width in pixels.
 * @param {number} height        Glyph height in pixels.
 * @param {number} bpp           Bits per pixel (1, 2, 4, or 8).
 * @returns {number[]} Packed bytes (row-major, each row padded to a whole byte).
 */
function packCoverage(coverage, width, height, bpp) {
  const packed = [];
  if (bpp === BITS_PER_BYTE) {
    for (let pixel = 0; pixel < width * height; pixel++)
      packed.push(coverage[pixel]);
    return packed;
  }
  const maxLevel = (1 << bpp) - 1;
  const pixelsPerByte = BITS_PER_BYTE / bpp;
  const rowBytes = Math.ceil(width / pixelsPerByte);
  for (let row = 0; row < height; row++) {
    for (let byteIndex = 0; byteIndex < rowBytes; byteIndex++) {
      let byte = 0;
      for (let slot = 0; slot < pixelsPerByte; slot++) {
        const column = byteIndex * pixelsPerByte + slot;
        if (column >= width) continue;
        const sample = coverage[row * width + column];
        const level =
          bpp === 1
            ? sample >= COVERAGE_THRESHOLD_1BPP
              ? 1
              : 0
            : Math.round((sample * maxLevel) / COVERAGE_MAX);
        byte |= level << (BITS_PER_BYTE - bpp - slot * bpp); // first pixel in the high bits
      }
      packed.push(byte);
    }
  }
  return packed;
}

/**
 * Rasterizes a single codepoint into a glyph record + appended bitmap bytes.
 *
 * @returns {{glyph:object, bytes:number[]}|null} null when the font has no glyph for the codepoint.
 */
function bakeGlyph(font, codepoint, pixelSize, baseline, scale, bpp) {
  if (
    font.charToGlyphIndex(String.fromCodePoint(codepoint)) === 0 &&
    codepoint !== SPACE_CODEPOINT
  )
    return null;
  const glyph = font.charToGlyph(String.fromCodePoint(codepoint));
  const advance = Math.round(glyph.advanceWidth * scale);
  const supersample = bpp === BITS_PER_BYTE ? SUPERSAMPLE_8BPP : SUPERSAMPLE;
  const steps = Math.max(
    MIN_CURVE_STEPS,
    Math.ceil(pixelSize / PIXELS_PER_CURVE_STEP),
  );
  const raster = rasterize(glyph.getPath(0, baseline, pixelSize), {
    ss: supersample,
    steps,
  });

  if (raster.width > UINT8_MAX || raster.height > UINT8_MAX) {
    throw new Error(
      `glyph U+${codepoint.toString(16)} is ${raster.width}x${raster.height}px — exceeds the uint8 glyph limit`,
    );
  }
  const glyphRecord = {
    bitmapOffset: 0, // filled in by the caller once the running offset is known
    width: raster.width,
    height: raster.height,
    xOffset: raster.xOffset,
    yOffset: raster.yOffset,
    advance: Math.min(advance, UINT8_MAX),
  };
  const bytes =
    raster.width > 0 && raster.height > 0
      ? packCoverage(raster.coverage, raster.width, raster.height, bpp)
      : [];
  return {glyph: glyphRecord, bytes};
}

/**
 * Bakes one font into per-size BitmapFont data.
 *
 * @param {object} opts
 * @param {string} opts.path    Path to the .ttf/.otf file.
 * @param {string} opts.family  Family name to register under (used by fontFamily lookups).
 * @param {number[]} opts.sizes Pixel sizes to bake.
 * @param {number} [opts.bpp]   Bits per pixel (default 4).
 * @param {string|Array<number|string>} [opts.glyphs] Extra glyph coverage beyond ASCII (default 'ascii').
 * @param {number|string|Array<number|string>} [opts.extraGlyphs] Per-app additions on top of `glyphs`.
 * @returns {{family:string, path:string, sizes:Array<object>}} Baked data ready for the C emitter.
 *          Extras the font file has no glyph for are silently absent from each size's `extras`.
 */
export function bakeFont({
  path,
  family,
  sizes,
  bpp = DEFAULT_BPP,
  glyphs = 'ascii',
  extraGlyphs,
}) {
  if (!SUPPORTED_BPP.includes(bpp))
    throw new Error(`bpp must be 1, 2, 4, or 8 (got ${bpp})`);
  const font = opentype.parse(fs.readFileSync(path).buffer);
  const extraCodepoints = resolveExtras(glyphs, extraGlyphs);

  const bakedSizes = [];
  for (const pixelSize of sizes) {
    const scale = pixelSize / font.unitsPerEm;
    const baseline = Math.round(font.ascender * scale);
    const lineHeight = baseline + Math.round(-font.descender * scale);

    const bitmap = [];
    const dense = [];
    for (let codepoint = ASCII_FIRST; codepoint <= ASCII_LAST; codepoint++) {
      const bakedGlyph = bakeGlyph(
        font,
        codepoint,
        pixelSize,
        baseline,
        scale,
        bpp,
      ) || {
        glyph: {
          bitmapOffset: 0,
          width: 0,
          height: 0,
          xOffset: 0,
          yOffset: 0,
          advance: Math.round(pixelSize / 2),
        },
        bytes: [],
      };
      bakedGlyph.glyph.bitmapOffset =
        bakedGlyph.bytes.length > 0 ? bitmap.length : 0;
      bitmap.push(...bakedGlyph.bytes);
      dense.push(bakedGlyph.glyph);
    }

    const extras = [];
    for (const codepoint of extraCodepoints) {
      const bakedGlyph = bakeGlyph(
        font,
        codepoint,
        pixelSize,
        baseline,
        scale,
        bpp,
      );
      if (!bakedGlyph) continue; // font has no such glyph
      bakedGlyph.glyph.bitmapOffset =
        bakedGlyph.bytes.length > 0 ? bitmap.length : 0;
      bitmap.push(...bakedGlyph.bytes);
      extras.push({codepoint, info: bakedGlyph.glyph});
    }

    bakedSizes.push({
      pixelSize,
      lineHeight: Math.min(lineHeight, UINT8_MAX),
      baseline: Math.min(baseline, UINT8_MAX),
      first: ASCII_FIRST,
      last: ASCII_LAST,
      bpp,
      dense,
      extras,
      bitmap,
    });
  }
  return {family, path, sizes: bakedSizes};
}
