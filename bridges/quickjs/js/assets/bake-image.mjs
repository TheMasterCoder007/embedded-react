// Build-time image baker: PNG → premultiplied ARGB8888, the format the engine's image registry
// references by pointer (er_image_load). Pure JS (pngjs) — no native deps, no Python. The engine
// scales at render time, so bake at whatever source resolution you want to ship.
import fs from 'node:fs';
import { PNG } from 'pngjs';

/**
 * Decodes an image and returns its premultiplied ARGB8888 pixels (row-major, 0xAARRGGBB).
 *
 * @param {object} opts
 * @param {string} opts.path  Path to the source image.
 * @param {string} opts.name  Asset name an <Image source>/imageName looks up.
 * @returns {{name:string, width:number, height:number, pixels:Uint32Array}}
 */
export function bakeImage({ path, name }) {
  if (!/\.png$/i.test(path)) {
    throw new Error(`image "${path}": only PNG is supported by the baker (convert to PNG, or extend bake-image.mjs)`);
  }
  const png = PNG.sync.read(fs.readFileSync(path));
  const { width, height, data } = png; // data = RGBA, 8-bit, row-major
  const pixels = new Uint32Array(width * height);
  for (let i = 0; i < width * height; i++) {
    const r = data[i * 4];
    const g = data[i * 4 + 1];
    const b = data[i * 4 + 2];
    const a = data[i * 4 + 3];
    // Premultiply the color channels by alpha (round-to-nearest) — the engine's image format.
    const rp = Math.floor((r * a + 127) / 255);
    const gp = Math.floor((g * a + 127) / 255);
    const bp = Math.floor((b * a + 127) / 255);
    pixels[i] = ((a << 24) | (rp << 16) | (gp << 8) | bp) >>> 0;
  }
  return { name, width, height, pixels };
}
