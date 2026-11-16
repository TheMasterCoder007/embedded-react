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
// translation unit (assets.generated.c + .h) exposing er_register_assets(). Invoked by build.mts
// after bundling; the example firmware compiles the .c and calls er_register_assets() at boot.
import fs from 'node:fs';
import path from 'node:path';
import {bakeImage} from './bake-image.mts';
import {bakeFont} from './bake-font.mts';
import {emitAssetsC} from './emit-c.mts';
import {emitAssetPack} from './emit-pack.mts';
import {warnMissingGlyphs} from './glyph-coverage.mts';
import {warnFontSizes} from './font-sizes.mts';
import type {ImageJob} from './bake-image.mts';
import type {FontJob} from './bake-font.mts';
import type {FontSizeUsage} from './font-sizes.mts';

/** What to bake. */
export interface BakeJobs {
  /** Discovered image imports. */
  images?: ImageJob[];
  /** Discovered font imports with their resolved size/bpp/glyph config. */
  fonts?: FontJob[];
}

/** What to bake, and what the bake is checked against. */
export interface CheckedBakeJobs extends BakeJobs {
  /** Bundled app JS, scanned for text the bake has no glyph for. */
  source?: string;
  /** analyzeFontSizes() result, checked against what was baked so an unbaked size is reported. */
  usedSizes?: FontSizeUsage;
}

/**
 * Bakes the given assets and writes assets.generated.{c,h} into outDir, the directory to write into.
 */
export function bakeAssets({
  images = [],
  fonts = [],
  outDir,
  source,
  usedSizes,
}: CheckedBakeJobs & {outDir: string}): {
  cPath: string;
  hPath: string;
  images: number;
  fonts: number;
} {
  const bakedImages = images.map(image => bakeImage(image));
  const bakedFonts = fonts.map(font => bakeFont(font));
  if (source) warnMissingGlyphs({source, fonts: bakedFonts});
  if (usedSizes) warnFontSizes({used: usedSizes, fonts: bakedFonts});

  const headerName = 'assets.generated.h';
  const {c: cSource, h: header} = emitAssetsC({
    headerName,
    images: bakedImages,
    fonts: bakedFonts,
  });

  fs.mkdirSync(outDir, {recursive: true});
  const cPath = path.join(outDir, 'assets.generated.c');
  const hPath = path.join(outDir, headerName);
  fs.writeFileSync(cPath, cSource);
  fs.writeFileSync(hPath, header);

  const fontSizes = bakedFonts.reduce(
    (total, font) => total + font.sizes.length,
    0,
  );
  return {cPath, hPath, images: bakedImages.length, fonts: fontSizes};
}

/**
 * Bakes the given assets into ERPK pack bytes, with the same bakers as bakeAssets, without writing a file.
 *
 * @returns The pack.
 */
export function bakeAssetPackBytes({
  images = [],
  fonts = [],
}: BakeJobs): Buffer {
  return emitAssetPack({
    images: images.map(image => bakeImage(image)),
    fonts: fonts.map(font => bakeFont(font)),
  });
}

/**
 * Bakes the given assets into a binary ERPK pack the simulator loads at runtime (hot-reloadable),
 * using the same bakers as bakeAssets so the result is pixel-identical. outPath is the .pack file to write.
 */
export function bakeAssetPack({
  images = [],
  fonts = [],
  outPath,
  source,
  usedSizes,
}: CheckedBakeJobs & {outPath: string}): {
  path: string;
  bytes: number;
  images: number;
  fonts: number;
} {
  const bakedImages = images.map(image => bakeImage(image));
  const bakedFonts = fonts.map(font => bakeFont(font));
  if (source) warnMissingGlyphs({source, fonts: bakedFonts});
  if (usedSizes) warnFontSizes({used: usedSizes, fonts: bakedFonts});
  const pack = emitAssetPack({images: bakedImages, fonts: bakedFonts});
  fs.mkdirSync(path.dirname(outPath), {recursive: true});
  fs.writeFileSync(outPath, pack);
  const fontSizes = bakedFonts.reduce(
    (total, font) => total + font.sizes.length,
    0,
  );
  return {
    path: outPath,
    bytes: pack.length,
    images: bakedImages.length,
    fonts: fontSizes,
  };
}
