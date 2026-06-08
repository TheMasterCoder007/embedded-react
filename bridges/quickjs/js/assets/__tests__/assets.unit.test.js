// Unit tests for the build-time asset bakers (images + fonts) and the C emitter. These lock the
// output format the engine consumes (premultiplied ARGB8888 images, BitmapFont glyph data) and the
// er_register_assets() entry point. The font fixture is the repo's Inter-Regular.ttf.
import { describe, it, expect } from 'vitest';
import { PNG } from 'pngjs';
import os from 'node:os';
import fs from 'node:fs';
import { resolve, dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { rasterize } from '../rasterize.mjs';
import { bakeImage } from '../bake-image.mjs';
import { bakeFont, resolveExtras, ASCII_FIRST, ASCII_LAST } from '../bake-font.mjs';
import { emitAssetsC } from '../emit-c.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const INTER = resolve(here, '../../../../../assets/fonts/Inter-Regular.ttf');
const ASCII_COUNT = ASCII_LAST - ASCII_FIRST + 1;

describe('rasterize', () => {
  it('fills an axis-aligned square to full coverage with the right bbox', () => {
    const square = {
      commands: [
        { type: 'M', x: 0, y: 0 },
        { type: 'L', x: 10, y: 0 },
        { type: 'L', x: 10, y: 10 },
        { type: 'L', x: 0, y: 10 },
        { type: 'Z' },
      ],
    };
    const r = rasterize(square, { ss: 4, steps: 4 });
    expect(r.width).toBe(10);
    expect(r.height).toBe(10);
    expect(r.xOffset).toBe(0);
    expect(r.yOffset).toBe(0);
    expect([...r.coverage].every((c) => c === 255)).toBe(true);
  });

  it('returns an empty result for a path with no contours', () => {
    const r = rasterize({ commands: [] });
    expect(r.width).toBe(0);
    expect(r.height).toBe(0);
    expect(r.coverage.length).toBe(0);
  });
});

describe('bakeImage', () => {
  it('decodes PNG to premultiplied ARGB8888', () => {
    const png = new PNG({ width: 2, height: 1 });
    png.data = Buffer.from([255, 0, 0, 255, /* opaque red */ 0, 0, 255, 128 /* half-alpha blue */]);
    const tmp = join(os.tmpdir(), `er-baketest-${process.pid}.png`);
    fs.writeFileSync(tmp, PNG.sync.write(png));
    try {
      const out = bakeImage({ path: tmp, name: 'tmp' });
      expect(out.width).toBe(2);
      expect(out.height).toBe(1);
      expect(out.pixels[0] >>> 0).toBe(0xffff0000); // opaque red, unchanged by premultiply
      // half-alpha blue: a=128, b = round(255*128/255) = 128
      expect(out.pixels[1] >>> 0).toBe(0x80000080);
    } finally {
      fs.rmSync(tmp, { force: true });
    }
  });
});

describe('resolveExtras', () => {
  it("treats 'ascii' as no extras", () => {
    expect(resolveExtras('ascii')).toEqual([]);
    expect(resolveExtras(undefined)).toEqual([]);
  });

  it('drops codepoints already in the dense ASCII range and sorts the rest', () => {
    expect(resolveExtras([0x2022, 0x41 /* 'A', in ASCII */, 0x20ac, 0x2022 /* dup */])).toEqual([0x2022, 0x20ac]);
  });

  it('resolves a named symbol set', () => {
    const common = resolveExtras('common');
    expect(common.length).toBeGreaterThan(0);
    expect(common.every((c) => c < ASCII_FIRST || c > ASCII_LAST)).toBe(true);
  });
});

describe('bakeFont', () => {
  it('bakes the dense ASCII range with self-consistent metrics', () => {
    const f = bakeFont({ path: INTER, family: 'Inter', sizes: [20], bpp: 4 });
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
    const f = bakeFont({ path: INTER, family: 'Inter', sizes: [16], bpp: 4, glyphs: 'common' });
    expect(f.sizes[0].extras.length).toBeGreaterThan(0);
  });

  it('rejects an unsupported bpp', () => {
    expect(() => bakeFont({ path: INTER, family: 'Inter', sizes: [16], bpp: 3 })).toThrow();
  });
});

describe('emitAssetsC', () => {
  it('emits images, fonts, and the register entry point', () => {
    const font = bakeFont({ path: INTER, family: 'Inter', sizes: [16], bpp: 4 });
    const image = { name: 'logo', width: 1, height: 1, pixels: new Uint32Array([0xffffffff]) };
    const { c, h } = emitAssetsC({ headerName: 'assets.generated.h', images: [image], fonts: [font] });

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
});
