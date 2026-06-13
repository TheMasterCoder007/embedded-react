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

// Asset baking orchestrator: turns the images and fonts an app imports into a single generated C
// translation unit (assets.generated.c + .h) exposing er_register_assets(). Invoked by build.mjs
// after bundling; the example firmware compiles the .c and calls er_register_assets() at boot.
import fs from 'node:fs';
import path from 'node:path';
import { bakeImage } from './bake-image.mjs';
import { bakeFont } from './bake-font.mjs';
import { emitAssetsC } from './emit-c.mjs';
import { emitAssetPack } from './emit-pack.mjs';

/**
 * Bakes the given assets and writes assets.generated.{c,h} into outDir.
 *
 * @param {object} opts
 * @param {Array<{path:string,name:string}>} [opts.images]  Discovered image imports.
 * @param {Array<{path:string,family:string,sizes:number[],bpp:number,glyphs:any}>} [opts.fonts]
 *        Discovered font imports with their resolved size/bpp/glyph config.
 * @param {string} opts.outDir   Directory to write the generated files into.
 * @returns {{cPath:string, hPath:string, images:number, fonts:number}}
 */
export function bakeAssets({ images = [], fonts = [], outDir }) {
  const bakedImages = images.map((i) => bakeImage(i));
  const bakedFonts = fonts.map((f) => bakeFont(f));

  const headerName = 'assets.generated.h';
  const { c, h } = emitAssetsC({ headerName, images: bakedImages, fonts: bakedFonts });

  fs.mkdirSync(outDir, { recursive: true });
  const cPath = path.join(outDir, 'assets.generated.c');
  const hPath = path.join(outDir, headerName);
  fs.writeFileSync(cPath, c);
  fs.writeFileSync(hPath, h);

  const fontSizes = bakedFonts.reduce((n, f) => n + f.sizes.length, 0);
  return { cPath, hPath, images: bakedImages.length, fonts: fontSizes };
}

/**
 * Bakes the given assets into a binary ERPK pack the simulator loads at runtime (hot-reloadable),
 * using the same bakers as bakeAssets so the result is pixel-identical.
 *
 * @param {object} opts
 * @param {Array<{path:string,name:string}>} [opts.images]
 * @param {Array<{path:string,family:string,sizes:number[],bpp:number,glyphs:any}>} [opts.fonts]
 * @param {string} opts.outPath  Path to write the .pack file.
 * @returns {{path:string, bytes:number, images:number, fonts:number}}
 */
export function bakeAssetPack({ images = [], fonts = [], outPath }) {
  const bakedImages = images.map((i) => bakeImage(i));
  const bakedFonts = fonts.map((f) => bakeFont(f));
  const pack = emitAssetPack({ images: bakedImages, fonts: bakedFonts });
  fs.mkdirSync(path.dirname(outPath), { recursive: true });
  fs.writeFileSync(outPath, pack);
  const fontSizes = bakedFonts.reduce((n, f) => n + f.sizes.length, 0);
  return { path: outPath, bytes: pack.length, images: bakedImages.length, fonts: fontSizes };
}
