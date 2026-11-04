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

// ERPK — a binary asset pack for the simulator's runtime loader (see tools/simulator/README.md).
//
// Produced from the SAME JS bakers (bake-image.mjs / bake-font.mjs) as the device's baked C, so the
// simulator renders pixel-identical images and fonts. The sim host (tools/simulator/asset_pack.c)
// loads this at runtime and calls er_image_load / er_font_register, so assets hot-reload without a
// sim rebuild. Little-endian (the simulator is x86). Format:
//
//   magic "ERPK" (4 bytes), version u32 (1 or 2), n_images u32, n_font_sizes u32
//   images[n_images]:    name(u16 len + bytes), width u32, height u32,
//                        [v2 only: format u8 (0=ARGB8888, 1=RGB565), zero-pad to 4-byte alignment],
//                        pixels (w*h * u32 ARGB, or w*h * u16 RGB565 when format=1)
//   fonts[n_font_sizes]: family(u16 len + bytes), pixel_size/line_height/baseline/format (4*u8),
//                        first u16, last u16, glyph_count u16, extras_count u16, bitmap_len u32,
//                        glyphs[glyph_count] (9B: off u32, w/h u8, xo/yo i8, adv u8),
//                        extras[extras_count] (13B: codepoint u32 + the 9B glyph),
//                        bitmap (bitmap_len bytes)
//
// Version 2 is now emitted ALWAYS. It was introduced for the 16-bit image bake, but its real value
// is the per-image alignment pad: the engine reads pixels in place, and a DMA2D-class backend
// refuses a word-misaligned ARGB8888 source (configuration error — silently absent image). v1 had
// no alignment machinery, so whether an image's pixels landed word-aligned in flash was an accident
// of the name strings — it varied build to build. Parsers still accept v1 packs from older tooling.

const INT8_MIN = -128;
const INT8_MAX = 127;
const PIXEL_ALIGNMENT = 4;
const FORMAT_ARGB8888 = 0;
const FORMAT_RGB565 = 1;

/** Accumulates little-endian binary chunks. */
class Writer {
  constructor() {
    this.chunks = [];
  }
  u8(value) {
    const buffer = Buffer.alloc(1);
    buffer.writeUInt8(value & 0xff, 0);
    this.chunks.push(buffer);
  }
  i8(value) {
    const buffer = Buffer.alloc(1);
    buffer.writeInt8(Math.max(INT8_MIN, Math.min(INT8_MAX, value | 0)), 0);
    this.chunks.push(buffer);
  }
  u16(value) {
    const buffer = Buffer.alloc(2);
    buffer.writeUInt16LE(value & 0xffff, 0);
    this.chunks.push(buffer);
  }
  u32(value) {
    const buffer = Buffer.alloc(4);
    buffer.writeUInt32LE(value >>> 0, 0);
    this.chunks.push(buffer);
  }
  str(text) {
    const buffer = Buffer.from(text, 'utf8');
    this.u16(buffer.length);
    this.chunks.push(buffer);
  }
  bytes(data) {
    this.chunks.push(Buffer.from(data));
  }
  /** Zero-pads so the next write lands on an n-byte boundary from the pack start. */
  align(alignment) {
    const length = this.chunks.reduce(
      (total, chunk) => total + chunk.length,
      0,
    );
    const padding = (alignment - (length % alignment)) % alignment;
    if (padding) this.chunks.push(Buffer.alloc(padding));
  }
  done() {
    return Buffer.concat(this.chunks);
  }
}

/** Writes one glyph record (9 bytes). */
function writeGlyph(writer, glyph) {
  writer.u32(glyph.bitmapOffset);
  writer.u8(glyph.width);
  writer.u8(glyph.height);
  writer.i8(glyph.xOffset);
  writer.i8(glyph.yOffset);
  writer.u8(glyph.advance);
}

/**
 * Serializes baked images + fonts into an ERPK pack.
 *
 * @param {object} opts
 * @param {Array<object>} [opts.images]  Results of bakeImage().
 * @param {Array<object>} [opts.fonts]   Results of bakeFont().
 * @returns {Buffer} The pack bytes.
 */
export function emitAssetPack({images = [], fonts = []}) {
  // Emit in a locale-independent sorted order: the discovery maps upstream are filled by esbuild
  // onLoad callbacks in whatever order the bundler visits files, which varies run to run — and an
  // unstable pack means unstable in-flash addresses and non-reproducible builds.
  const byName = key => (left, right) =>
    left[key] < right[key] ? -1 : left[key] > right[key] ? 1 : 0;
  images = [...images].sort(byName('name'));
  fonts = [...fonts].sort(byName('family'));

  const version = 2;
  const writer = new Writer();
  writer.bytes(Buffer.from('ERPK', 'ascii'));
  writer.u32(version);
  writer.u32(images.length);
  const fontSizes = fonts.reduce((total, font) => total + font.sizes.length, 0);
  writer.u32(fontSizes);

  for (const image of images) {
    writer.str(image.name);
    writer.u32(image.width);
    writer.u32(image.height);
    if (version >= 2) {
      writer.u8(image.format === 'rgb565' ? FORMAT_RGB565 : FORMAT_ARGB8888);
      writer.align(PIXEL_ALIGNMENT); // the engine reads the pixels in place; keep the pointer 4-byte aligned
    }
    // pixels is a Uint32Array (premultiplied ARGB words) or Uint16Array (RGB565);
    // copy its LE bytes verbatim.
    writer.bytes(
      Buffer.from(
        image.pixels.buffer,
        image.pixels.byteOffset,
        image.pixels.byteLength,
      ),
    );
  }

  for (const font of fonts) {
    for (const size of font.sizes) {
      writer.str(font.family);
      writer.u8(size.pixelSize);
      writer.u8(size.lineHeight);
      writer.u8(size.baseline);
      writer.u8(size.bpp);
      writer.u16(size.first);
      writer.u16(size.last);
      writer.u16(size.dense.length);
      writer.u16(size.extras.length);
      writer.u32(size.bitmap.length);
      for (const glyph of size.dense) writeGlyph(writer, glyph);
      for (const extra of size.extras) {
        writer.u32(extra.codepoint);
        writeGlyph(writer, extra.info);
      }
      writer.bytes(Buffer.from(size.bitmap));
    }
  }

  return writer.done();
}
