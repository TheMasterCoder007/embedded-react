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
import { rasterize } from './rasterize.mjs';

/** Default dense glyph range: printable ASCII (0x20..0x7E). */
export const ASCII_FIRST = 0x20;
export const ASCII_LAST = 0x7e;

// Named extra-symbol sets selectable from assets.config.js (glyphs: 'common' | 'minimal' | ...).
const SYMBOLS_MINIMAL = [
  0x00b0, 0x00b1, 0x00b5, 0x00d7, 0x00f7, 0x2013, 0x2014, 0x2022, 0x2026, 0x2190, 0x2191, 0x2192,
  0x2193, 0x2713, 0x2717,
];
const SYMBOLS_COMMON = [
  0x00a2, 0x00a3, 0x00a5, 0x00a7, 0x00a9, 0x00ae, 0x00b0, 0x00b1, 0x00b5, 0x00d7, 0x00f7, 0x2013,
  0x2014, 0x2018, 0x2019, 0x201c, 0x201d, 0x2020, 0x2021, 0x2022, 0x2026, 0x2030, 0x20ac, 0x2122,
  0x2190, 0x2191, 0x2192, 0x2193, 0x2194, 0x21b5, 0x2202, 0x2206, 0x221a, 0x221e, 0x2211, 0x2212,
  0x2248, 0x2260, 0x2264, 0x2265, 0x25a0, 0x25cf, 0x25c6, 0x2605, 0x2606, 0x2713, 0x2717,
];
const SYMBOLS_GREEK = [];
for (let c = 0x0391; c <= 0x03a9; c++) if (c !== 0x03a2) SYMBOLS_GREEK.push(c); // uppercase (skip gap)
for (let c = 0x03b1; c <= 0x03c9; c++) SYMBOLS_GREEK.push(c); // lowercase
const SYMBOL_SETS = {
  none: [],
  minimal: SYMBOLS_MINIMAL,
  common: SYMBOLS_COMMON,
  greek: SYMBOLS_GREEK,
  'common-greek': [...new Set([...SYMBOLS_COMMON, ...SYMBOLS_GREEK])].sort((a, b) => a - b),
};

/**
 * Resolves a glyph-coverage spec into a sorted list of extra (sparse) codepoints.
 *
 * @param {string|Array<number>|undefined} glyphs  'ascii' (none), a named set, or explicit codepoints.
 * @returns {number[]} Sorted extra codepoints outside the dense ASCII range.
 */
export function resolveExtras(glyphs) {
  if (!glyphs || glyphs === 'ascii') return [];
  let cps;
  if (Array.isArray(glyphs)) cps = glyphs.slice();
  else if (SYMBOL_SETS[glyphs]) cps = SYMBOL_SETS[glyphs].slice();
  else throw new Error(`unknown glyph set "${glyphs}" (use 'ascii', a named set, or a codepoint array)`);
  return [...new Set(cps)].filter((c) => c < ASCII_FIRST || c > ASCII_LAST).sort((a, b) => a - b);
}

/**
 * Packs an 8-bit coverage bitmap (one byte per pixel) into the engine's per-row bpp layout.
 *
 * @param {Uint8Array} cov  Row-major coverage, width*height bytes.
 * @param {number} w        Glyph width in pixels.
 * @param {number} h        Glyph height in pixels.
 * @param {number} bpp      Bits per pixel (1, 2, 4, or 8).
 * @returns {number[]} Packed bytes (row-major, each row padded to a whole byte).
 */
function packCoverage(cov, w, h, bpp) {
  const out = [];
  if (bpp === 8) {
    for (let i = 0; i < w * h; i++) out.push(cov[i]);
    return out;
  }
  const maxVal = (1 << bpp) - 1;
  const perByte = 8 / bpp;
  const rowBytes = Math.ceil(w / perByte);
  for (let row = 0; row < h; row++) {
    for (let b = 0; b < rowBytes; b++) {
      let byte = 0;
      for (let s = 0; s < perByte; s++) {
        const col = b * perByte + s;
        if (col >= w) continue;
        const c = cov[row * w + col];
        const v = bpp === 1 ? (c >= 128 ? 1 : 0) : Math.round((c * maxVal) / 255);
        byte |= v << (8 - bpp - s * bpp); // first pixel in the high bits
      }
      out.push(byte);
    }
  }
  return out;
}

/**
 * Rasterizes a single codepoint into a glyph record + appended bitmap bytes.
 *
 * @returns {{glyph:object, bytes:number[]}|null} null when the font has no glyph for the codepoint.
 */
function bakeGlyph(font, cp, pixelSize, baseline, scale, bpp) {
  if (font.charToGlyphIndex(String.fromCodePoint(cp)) === 0 && cp !== 0x20) return null;
  const glyph = font.charToGlyph(String.fromCodePoint(cp));
  const advance = Math.round(glyph.advanceWidth * scale);
  const ss = bpp === 8 ? 8 : 4;
  const steps = Math.max(6, Math.ceil(pixelSize / 3));
  const r = rasterize(glyph.getPath(0, baseline, pixelSize), { ss, steps });

  if (r.width > 255 || r.height > 255) {
    throw new Error(`glyph U+${cp.toString(16)} is ${r.width}x${r.height}px — exceeds the uint8 glyph limit`);
  }
  const glyphRec = {
    bitmapOffset: 0, // filled in by the caller once the running offset is known
    width: r.width,
    height: r.height,
    xOffset: r.xOffset,
    yOffset: r.yOffset,
    advance: Math.min(advance, 255),
  };
  const bytes = r.width > 0 && r.height > 0 ? packCoverage(r.coverage, r.width, r.height, bpp) : [];
  return { glyph: glyphRec, bytes };
}

/**
 * Bakes one font into per-size BitmapFont data.
 *
 * @param {object} opts
 * @param {string} opts.path    Path to the .ttf/.otf file.
 * @param {string} opts.family  Family name to register under (used by fontFamily lookups).
 * @param {number[]} opts.sizes Pixel sizes to bake.
 * @param {number} [opts.bpp]   Bits per pixel (default 4).
 * @param {string|Array<number>} [opts.glyphs] Extra glyph coverage beyond ASCII (default 'ascii').
 * @returns {{family:string, sizes:Array<object>}} Baked data ready for the C emitter.
 */
export function bakeFont({ path, family, sizes, bpp = 4, glyphs = 'ascii' }) {
  if (![1, 2, 4, 8].includes(bpp)) throw new Error(`bpp must be 1, 2, 4, or 8 (got ${bpp})`);
  const font = opentype.parse(fs.readFileSync(path).buffer);
  const extraCps = resolveExtras(glyphs);

  const baked = [];
  for (const pixelSize of sizes) {
    const scale = pixelSize / font.unitsPerEm;
    const baseline = Math.round(font.ascender * scale);
    const lineHeight = baseline + Math.round(-font.descender * scale);

    const bitmap = [];
    const dense = [];
    for (let cp = ASCII_FIRST; cp <= ASCII_LAST; cp++) {
      const baked1 = bakeGlyph(font, cp, pixelSize, baseline, scale, bpp) || {
        glyph: { bitmapOffset: 0, width: 0, height: 0, xOffset: 0, yOffset: 0, advance: Math.round(pixelSize / 2) },
        bytes: [],
      };
      baked1.glyph.bitmapOffset = baked1.bytes.length > 0 ? bitmap.length : 0;
      bitmap.push(...baked1.bytes);
      dense.push(baked1.glyph);
    }

    const extras = [];
    for (const cp of extraCps) {
      const b = bakeGlyph(font, cp, pixelSize, baseline, scale, bpp);
      if (!b) continue; // font has no such glyph
      b.glyph.bitmapOffset = b.bytes.length > 0 ? bitmap.length : 0;
      bitmap.push(...b.bytes);
      extras.push({ codepoint: cp, info: b.glyph });
    }

    baked.push({
      pixelSize,
      lineHeight: Math.min(lineHeight, 255),
      baseline: Math.min(baseline, 255),
      first: ASCII_FIRST,
      last: ASCII_LAST,
      bpp,
      dense,
      extras,
      bitmap,
    });
  }
  return { family, sizes: baked };
}
