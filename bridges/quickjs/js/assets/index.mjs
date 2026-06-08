// Asset baking orchestrator: turns the images and fonts an app imports into a single generated C
// translation unit (assets.generated.c + .h) exposing er_register_assets(). Invoked by build.mjs
// after bundling; the example firmware compiles the .c and calls er_register_assets() at boot.
import fs from 'node:fs';
import path from 'node:path';
import { bakeImage } from './bake-image.mjs';
import { bakeFont } from './bake-font.mjs';
import { emitAssetsC } from './emit-c.mjs';

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
