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

// Unit tests for the build-time asset bakers (images + fonts) and the C emitter. These lock the
// output format the engine consumes (premultiplied ARGB8888 images, BitmapFont glyph data) and the
// er_register_assets() entry point. The font fixture is the repo's Inter-Regular.ttf.
import {describe, it, expect} from 'vitest';
import {PNG} from 'pngjs';
import os from 'node:os';
import fs from 'node:fs';
import {resolve, dirname, join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {rasterize} from '../rasterize.mjs';
import {bakeImage} from '../bake-image.mjs';
import {
  bakeFont,
  resolveExtras,
  ASCII_FIRST,
  ASCII_LAST,
} from '../bake-font.mjs';
import {emitAssetsC} from '../emit-c.mjs';
import {emitAssetPack} from '../emit-pack.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const INTER = resolve(here, '../../../../../assets/fonts/Inter-Regular.ttf');
const ASCII_COUNT = ASCII_LAST - ASCII_FIRST + 1;

describe('rasterize', () => {
  it('fills an axis-aligned square to full coverage with the right bbox', () => {
    const square = {
      commands: [
        {type: 'M', x: 0, y: 0},
        {type: 'L', x: 10, y: 0},
        {type: 'L', x: 10, y: 10},
        {type: 'L', x: 0, y: 10},
        {type: 'Z'},
      ],
    };
    const r = rasterize(square, {ss: 4, steps: 4});
    expect(r.width).toBe(10);
    expect(r.height).toBe(10);
    expect(r.xOffset).toBe(0);
    expect(r.yOffset).toBe(0);
    expect([...r.coverage].every(c => c === 255)).toBe(true);
  });

  it('returns an empty result for a path with no contours', () => {
    const r = rasterize({commands: []});
    expect(r.width).toBe(0);
    expect(r.height).toBe(0);
    expect(r.coverage.length).toBe(0);
  });
});

describe('bakeImage', () => {
  it('decodes PNG to premultiplied ARGB8888', () => {
    const png = new PNG({width: 2, height: 1});
    png.data = Buffer.from([
      255, 0, 0, 255, /* opaque red */ 0, 0, 255, 128 /* half-alpha blue */,
    ]);
    const tmp = join(os.tmpdir(), `er-baketest-${process.pid}.png`);
    fs.writeFileSync(tmp, PNG.sync.write(png));
    try {
      const out = bakeImage({path: tmp, name: 'tmp'});
      expect(out.width).toBe(2);
      expect(out.height).toBe(1);
      expect(out.pixels[0] >>> 0).toBe(0xffff0000); // opaque red, unchanged by premultiply
      // half-alpha blue: a=128, b = round(255*128/255) = 128
      expect(out.pixels[1] >>> 0).toBe(0x80000080);
    } finally {
      fs.rmSync(tmp, {force: true});
    }
  });

  it('bakes an opaque PNG to RGB565 when format is rgb565', () => {
    const png = new PNG({width: 4, height: 1});
    png.data = Buffer.from([
      255, 0, 0, 255 /* red */, 0, 255, 0, 255 /* green */, 0, 0, 255,
      255 /* blue */, 255, 255, 255, 255 /* white */,
    ]);
    const tmp = join(os.tmpdir(), `er-bake565-${process.pid}.png`);
    fs.writeFileSync(tmp, PNG.sync.write(png));
    try {
      const out = bakeImage({path: tmp, name: 'bg', format: 'rgb565'});
      expect(out.format).toBe('rgb565');
      expect(out.pixels).toBeInstanceOf(Uint16Array);
      expect(out.pixels[0]).toBe(0xf800); // red
      expect(out.pixels[1]).toBe(0x07e0); // green
      expect(out.pixels[2]).toBe(0x001f); // blue
      expect(out.pixels[3]).toBe(0xffff); // white
    } finally {
      fs.rmSync(tmp, {force: true});
    }
  });

  it('rejects a non-opaque source for rgb565', () => {
    const png = new PNG({width: 2, height: 1});
    png.data = Buffer.from([255, 0, 0, 255, 0, 0, 255, 128]);
    const tmp = join(os.tmpdir(), `er-bake565a-${process.pid}.png`);
    fs.writeFileSync(tmp, PNG.sync.write(png));
    try {
      expect(() => bakeImage({path: tmp, name: 'bg', format: 'rgb565'})).toThrow(
        /alpha 128/,
      );
    } finally {
      fs.rmSync(tmp, {force: true});
    }
  });

  it('rejects an unknown format', () => {
    expect(() => bakeImage({path: 'x.png', name: 'x', format: 'rgb888'})).toThrow(
      /unknown format/,
    );
  });
});

describe('resolveExtras', () => {
  it("treats 'ascii' as no extras", () => {
    expect(resolveExtras('ascii')).toEqual([]);
    expect(resolveExtras(undefined)).toEqual([]);
  });

  it('drops codepoints already in the dense ASCII range and sorts the rest', () => {
    expect(
      resolveExtras([
        0x2022, 0x41 /* 'A', in ASCII */, 0x20ac, 0x2022 /* dup */,
      ]),
    ).toEqual([0x2022, 0x20ac]);
  });

  it('resolves a named symbol set', () => {
    const common = resolveExtras('common');
    expect(common.length).toBeGreaterThan(0);
    expect(common.every(c => c < ASCII_FIRST || c > ASCII_LAST)).toBe(true);
  });
});

describe('bakeFont', () => {
  it('bakes the dense ASCII range with self-consistent metrics', () => {
    const f = bakeFont({path: INTER, family: 'Inter', sizes: [20], bpp: 4});
    expect(f.family).toBe('Inter');
    expect(f.sizes).toHaveLength(1);
    const s = f.sizes[0];
    expect(s.pixelSize).toBe(20);
    expect(s.bpp).toBe(4);
    expect(s.baseline).toBeGreaterThan(0);
    expect(s.lineHeight).toBeGreaterThan(s.baseline);
    expect(s.dense).toHaveLength(ASCII_COUNT);
    expect(s.extras).toHaveLength(0);

    const space = s.dense[0x20 - ASCII_FIRST];
    expect(space.width).toBe(0);
    expect(space.height).toBe(0);
    expect(space.advance).toBeGreaterThan(0);

    const I = s.dense['I'.charCodeAt(0) - ASCII_FIRST];
    expect(I.width).toBeGreaterThan(0);
    expect(I.height).toBeGreaterThan(0);
    expect(I.advance).toBeGreaterThan(0);

    // Every inked glyph's bitmap slice must lie within the atlas.
    expect(s.bitmap.length).toBeGreaterThan(0);
    for (const g of s.dense) {
      if (g.width > 0) expect(g.bitmapOffset).toBeLessThan(s.bitmap.length);
    }
  });

  it('includes extra glyphs when a symbol set is requested', () => {
    const f = bakeFont({
      path: INTER,
      family: 'Inter',
      sizes: [16],
      bpp: 4,
      glyphs: 'common',
    });
    expect(f.sizes[0].extras.length).toBeGreaterThan(0);
  });

  it('rejects an unsupported bpp', () => {
    expect(() =>
      bakeFont({path: INTER, family: 'Inter', sizes: [16], bpp: 3}),
    ).toThrow();
  });
});

/** Parses an ERPK pack back into a structured form (mirrors tools/simulator/asset_pack.c). */
function readPack(buf) {
  let o = 0;
  const u8 = () => buf.readUInt8(o++);
  const i8 = () => buf.readInt8(o++);
  const u16 = () => {
    const v = buf.readUInt16LE(o);
    o += 2;
    return v;
  };
  const u32 = () => {
    const v = buf.readUInt32LE(o);
    o += 4;
    return v;
  };
  const str = () => {
    const len = u16();
    const s = buf.toString('utf8', o, o + len);
    o += len;
    return s;
  };
  const magic = buf.toString('ascii', 0, 4);
  o = 4;
  const version = u32();
  const nImages = u32();
  const nFonts = u32();
  const images = [];
  for (let i = 0; i < nImages; i++) {
    const name = str();
    const w = u32();
    const h = u32();
    let format = 0; // v1: implicitly ARGB8888
    if (version >= 2) {
      format = u8();
      o += (4 - (o % 4)) % 4; // skip the 4-byte alignment pad before the pixels
    }
    const pixels = [];
    for (let p = 0; p < w * h; p++) pixels.push(format === 1 ? u16() : u32());
    images.push({name, w, h, format, pixels});
  }
  const fonts = [];
  for (let i = 0; i < nFonts; i++) {
    const family = str();
    const f = {
      family,
      pixel_size: u8(),
      line_height: u8(),
      baseline: u8(),
      format: u8(),
    };
    f.first = u16();
    f.last = u16();
    f.gc = u16();
    f.ec = u16();
    f.blen = u32();
    for (let g = 0; g < f.gc; g++) {
      u32();
      u8();
      u8();
      i8();
      i8();
      u8();
    } // glyph
    for (let e = 0; e < f.ec; e++) {
      u32();
      u32();
      u8();
      u8();
      i8();
      i8();
      u8();
    } // extra
    o += f.blen; // bitmap
    fonts.push(f);
  }
  return {
    magic,
    version,
    nImages,
    nFonts,
    images,
    fonts,
    consumed: o,
    total: buf.length,
  };
}

describe('emitAssetPack', () => {
  it('serializes images + fonts into a self-consistent ERPK pack', () => {
    const image = {
      name: 'logo',
      width: 2,
      height: 1,
      pixels: new Uint32Array([0xffff0000, 0x80000080]),
    };
    const font = bakeFont({path: INTER, family: 'Inter', sizes: [16], bpp: 4});
    const p = readPack(emitAssetPack({images: [image], fonts: [font]}));

    expect(p.magic).toBe('ERPK');
    expect(p.version).toBe(1);
    expect(p.nImages).toBe(1);
    expect(p.nFonts).toBe(1); // one font *size*

    expect(p.images[0]).toMatchObject({name: 'logo', w: 2, h: 1});
    expect(p.images[0].pixels[0] >>> 0).toBe(0xffff0000);
    expect(p.images[0].pixels[1] >>> 0).toBe(0x80000080);

    expect(p.fonts[0].family).toBe('Inter');
    expect(p.fonts[0].pixel_size).toBe(16);
    expect(p.fonts[0].format).toBe(4);
    expect(p.fonts[0].gc).toBe(ASCII_COUNT);

    // Every byte is accounted for — the reader consumes exactly what the writer produced.
    expect(p.consumed).toBe(p.total);
  });

  it('stays version 1 when no image is RGB565', () => {
    const image = {
      name: 'logo',
      width: 1,
      height: 1,
      format: 'argb8888',
      pixels: new Uint32Array([0xffffffff]),
    };
    const p = readPack(emitAssetPack({images: [image], fonts: []}));
    expect(p.version).toBe(1);
  });

  it('writes version 2 with per-image format + aligned pixels when RGB565 is used', () => {
    const bg = {
      name: 'bg',
      width: 3,
      height: 1,
      format: 'rgb565',
      pixels: new Uint16Array([0xf800, 0x07e0, 0x001f]),
    };
    // An odd-length name would leave the pixels misaligned without the v2 pad.
    const icon = {
      name: 'ico',
      width: 1,
      height: 1,
      pixels: new Uint32Array([0x80000080]),
    };
    const p = readPack(emitAssetPack({images: [bg, icon], fonts: []}));

    expect(p.version).toBe(2);
    expect(p.images[0]).toMatchObject({name: 'bg', w: 3, h: 1, format: 1});
    expect(p.images[0].pixels).toEqual([0xf800, 0x07e0, 0x001f]);
    expect(p.images[1]).toMatchObject({name: 'ico', w: 1, h: 1, format: 0});
    expect(p.images[1].pixels[0] >>> 0).toBe(0x80000080);
    expect(p.consumed).toBe(p.total);
  });
});

describe('emitAssetsC', () => {
  it('emits images, fonts, and the register entry point', () => {
    const font = bakeFont({path: INTER, family: 'Inter', sizes: [16], bpp: 4});
    const image = {
      name: 'logo',
      width: 1,
      height: 1,
      pixels: new Uint32Array([0xffffffff]),
    };
    const {c, h} = emitAssetsC({
      headerName: 'assets.generated.h',
      images: [image],
      fonts: [font],
    });

    expect(h).toContain('void er_register_assets(void);');
    expect(c).toContain('#include "er_scene.h"');
    expect(c).toContain('static const uint32_t logo_px[]');
    expect(c).toContain('static const BitmapFont g_font_Inter_16');
    expect(c).toContain('static const GlyphInfo s_Inter_16_glyphs');
    expect(c).toContain('.format       = 4,');
    expect(c).toContain('void er_register_assets(void)');
    expect(c).toContain('er_image_load("logo", logo_px, 1, 1);');
    expect(c).toContain('er_font_register("Inter", &g_font_Inter_16);');
  });

  it('emits uint16_t arrays and er_image_load_rgb565 for RGB565 images', () => {
    const bg = {
      name: 'bg',
      width: 2,
      height: 1,
      format: 'rgb565',
      pixels: new Uint16Array([0xf800, 0xffff]),
    };
    const {c} = emitAssetsC({
      headerName: 'assets.generated.h',
      images: [bg],
      fonts: [],
    });

    expect(c).toContain('static const uint16_t bg_px[]');
    expect(c).toContain('0xF800u');
    expect(c).toContain('er_image_load_rgb565("bg", bg_px, 2, 1);');
    expect(c).not.toContain('er_image_load("bg"');
  });
});
