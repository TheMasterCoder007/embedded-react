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

// Build-time image baker: PNG → premultiplied ARGB8888 (the default) or RGB565, the two formats
// the engine's image registry references by pointer (er_image_load / er_image_load_rgb565).
// Pure JS (pngjs) — no native deps, no Python. The engine scales at render time, so bake at
// whatever source resolution you want to ship.
import fs from 'node:fs';
import {PNG} from 'pngjs';

/**
 * Decodes an image and returns its baked pixels.
 *
 * The default format is premultiplied ARGB8888 (row-major, 0xAARRGGBB). Pass format 'rgb565' to bake
 * 16-bit RGB565 instead — half the flash and half the source-read bandwidth on every repaint,
 * meant for opaque full-screen backgrounds. RGB565 has no alpha channel, so a source image with
 * any non-opaque pixel is rejected with an error rather than silently dropping its transparency.
 *
 * @param {object} opts
 * @param {string} opts.path      Path to the source image.
 * @param {string} opts.name      Asset name an <Image source>/imageName looks up.
 * @param {string} [opts.format]  'argb8888' (default) or 'rgb565'.
 * @returns {{name:string, width:number, height:number, format:'argb8888'|'rgb565',
 *            pixels:Uint32Array|Uint16Array}}
 */
export function bakeImage({path, name, format = 'argb8888'}) {
  if (!/\.png$/i.test(path)) {
    throw new Error(
      `image "${path}": only PNG is supported by the baker (convert to PNG, or extend bake-image.mjs)`,
    );
  }
  if (format !== 'argb8888' && format !== 'rgb565') {
    throw new Error(
      `image "${name}": unknown format "${format}" (expected "argb8888" or "rgb565")`,
    );
  }
  const png = PNG.sync.read(fs.readFileSync(path));
  const {width, height, data} = png; // data = RGBA, 8-bit, row-major

  if (format === 'rgb565') {
    const pixels = new Uint16Array(width * height);
    for (let i = 0; i < width * height; i++) {
      const r = data[i * 4];
      const g = data[i * 4 + 1];
      const b = data[i * 4 + 2];
      const a = data[i * 4 + 3];
      if (a !== 255) {
        const x = i % width;
        const y = (i - x) / width;
        throw new Error(
          `image "${name}": rgb565 requires a fully opaque source, but pixel (${x},${y}) has alpha ${a} — ` +
            `flatten the art onto its background, or keep the default argb8888 format`,
        );
      }
      // Round-to-nearest 8→5/6/5 bit reduction (the engine bit-replicates back on expand).
      const r5 = Math.round((r * 31) / 255);
      const g6 = Math.round((g * 63) / 255);
      const b5 = Math.round((b * 31) / 255);
      pixels[i] = ((r5 << 11) | (g6 << 5) | b5) & 0xffff;
    }
    return {name, width, height, format, pixels};
  }

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
  return {name, width, height, format, pixels};
}
