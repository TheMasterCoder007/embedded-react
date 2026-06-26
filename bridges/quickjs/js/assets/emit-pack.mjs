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
//   magic "ERPK" (4 bytes), version u32=1, n_images u32, n_font_sizes u32
//   images[n_images]:    name(u16 len + bytes), width u32, height u32, pixels (w*h * u32 ARGB)
//   fonts[n_font_sizes]: family(u16 len + bytes), pixel_size/line_height/baseline/format (4*u8),
//                        first u16, last u16, glyph_count u16, extras_count u16, bitmap_len u32,
//                        glyphs[glyph_count] (9B: off u32, w/h u8, xo/yo i8, adv u8),
//                        extras[extras_count] (13B: codepoint u32 + the 9B glyph),
//                        bitmap (bitmap_len bytes)

const VERSION = 1;

/** Accumulates little-endian binary chunks. */
class Writer {
  constructor() {
    this.chunks = [];
  }
  u8(v) {
    const b = Buffer.alloc(1);
    b.writeUInt8(v & 0xff, 0);
    this.chunks.push(b);
  }
  i8(v) {
    const b = Buffer.alloc(1);
    b.writeInt8(Math.max(-128, Math.min(127, v | 0)), 0);
    this.chunks.push(b);
  }
  u16(v) {
    const b = Buffer.alloc(2);
    b.writeUInt16LE(v & 0xffff, 0);
    this.chunks.push(b);
  }
  u32(v) {
    const b = Buffer.alloc(4);
    b.writeUInt32LE(v >>> 0, 0);
    this.chunks.push(b);
  }
  str(s) {
    const b = Buffer.from(s, 'utf8');
    this.u16(b.length);
    this.chunks.push(b);
  }
  bytes(buf) {
    this.chunks.push(Buffer.from(buf));
  }
  done() {
    return Buffer.concat(this.chunks);
  }
}

/** Writes one glyph record (9 bytes). */
function writeGlyph(w, g) {
  w.u32(g.bitmapOffset);
  w.u8(g.width);
  w.u8(g.height);
  w.i8(g.xOffset);
  w.i8(g.yOffset);
  w.u8(g.advance);
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
  const w = new Writer();
  w.bytes(Buffer.from('ERPK', 'ascii'));
  w.u32(VERSION);
  w.u32(images.length);
  const fontSizes = fonts.reduce((n, f) => n + f.sizes.length, 0);
  w.u32(fontSizes);

  for (const img of images) {
    w.str(img.name);
    w.u32(img.width);
    w.u32(img.height);
    // pixels is a Uint32Array (premultiplied ARGB words); copy its LE bytes verbatim.
    w.bytes(
      Buffer.from(
        img.pixels.buffer,
        img.pixels.byteOffset,
        img.pixels.byteLength,
      ),
    );
  }

  for (const font of fonts) {
    for (const sz of font.sizes) {
      w.str(font.family);
      w.u8(sz.pixelSize);
      w.u8(sz.lineHeight);
      w.u8(sz.baseline);
      w.u8(sz.bpp);
      w.u16(sz.first);
      w.u16(sz.last);
      w.u16(sz.dense.length);
      w.u16(sz.extras.length);
      w.u32(sz.bitmap.length);
      for (const g of sz.dense) writeGlyph(w, g);
      for (const e of sz.extras) {
        w.u32(e.codepoint);
        writeGlyph(w, e.info);
      }
      w.bytes(Buffer.from(sz.bitmap));
    }
  }

  return w.done();
}
