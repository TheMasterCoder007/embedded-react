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

/*
 * Bakes a compiled app's assets into its package's asset pack: the images it shows, and each imported font at
 * the sizes its text is drawn at. Checks the app's text against what was baked.
 */

import {existsSync} from 'node:fs';
import {resolve} from 'node:path';
import {bakeFont} from '../assets/bake-font.mts';
import {bakeImage} from '../assets/bake-image.mts';
import {emitAssetPack} from '../assets/emit-pack.mts';
import {resolveFontJobs} from '../assets/font-config.mts';
import {
  analyzeFontSizes,
  MAX_BAKED_SIZES,
  warnFontSizes,
} from '../assets/font-sizes.mts';
import {BUILTIN_FAMILY, warnMissingGlyphs} from '../assets/glyph-coverage.mts';
import type {FontJob} from '../assets/bake-font.mts';
import type {ImageJob} from '../assets/bake-image.mts';
import type {AssetsConfig} from '../assets/font-config.mts';
import type {PackageResult} from './compiler/package/assemble.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** A package's baked assets. */
export interface PackageAssets {
  /** The asset pack, or nothing when the app has no assets. */
  assetPack: Uint8Array | undefined;
  imageJobs: ImageJob[];
  fontJobs: FontJob[];
}

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** The font families the engine's registry holds (FONT_REGISTRY_MAX). */
const MAX_FAMILIES = 8;

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Bakes the images and fonts a compiled app reaches. Throws when a file is missing, or the fonts are more than
 * the engine's font registry holds, which the runtime would refuse the package for.
 *
 * @param result  The compiled app.
 * @param source  The app's source, which the glyph check reads.
 * @param appDirectory  The directory the app's imports are relative to.
 * @param assetsConfig  The app's assets.config.js.
 *
 * @returns The pack, and what went into it.
 */
export function bakePackageAssets(
  result: PackageResult,
  source: string,
  appDirectory: string,
  assetsConfig: AssetsConfig = {},
): PackageAssets {
  const imageJobs: ImageJob[] = result.images.map(image => ({
    name: image.name,
    path: resolve(appDirectory, image.importPath),
    format: assetsConfig.images?.[image.name]?.format,
  }));
  for (const job of imageJobs) {
    if (!existsSync(job.path)) {
      throw new Error(
        `AOT: <Image> asset "${job.name}" not found at ${job.path}`,
      );
    }
  }

  // A size state drives isn't known to the compiler, so such a family takes every size the source names too.
  const used = analyzeFontSizes(source);
  const usedBy = new Map<string, number[]>();
  const fontJobs: FontJob[] = [];
  for (const font of result.fonts) {
    const sizes = font.openSizes
      ? [...new Set([...font.sizes, ...used.sizes])].sort(
          (left, right) => left - right,
        )
      : font.sizes;
    if (font.family === '') {
      usedBy.set(BUILTIN_FAMILY, sizes);
    }
    if (font.importPath === null) continue;

    const path = resolve(appDirectory, font.importPath);
    if (!existsSync(path)) {
      throw new Error(`AOT: font "${font.family}" not found at ${path}`);
    }

    const [job] = resolveFontJobs(
      new Map([[font.family, path]]),
      assetsConfig.fonts,
      sizes,
    );
    if (job.sizes.length > MAX_BAKED_SIZES) {
      throw new Error(
        `AOT: font "${font.family}" is drawn at ${job.sizes.length} sizes (${job.sizes.join(', ')}), ` +
          `and the engine holds ${MAX_BAKED_SIZES} of one family. Use fewer sizes, or pin them: ` +
          `assets.config.js → fonts: {${JSON.stringify(font.family)}: {sizes: [...]}}`,
      );
    }
    usedBy.set(font.family, sizes);
    fontJobs.push(job);
  }
  if (fontJobs.length > MAX_FAMILIES) {
    throw new Error(
      `AOT: the app draws text in ${fontJobs.length} imported fonts, and the engine holds ${MAX_FAMILIES} families`,
    );
  }

  const bakedFonts = fontJobs.map(job => bakeFont(job));
  warnMissingGlyphs({
    source,
    jsx: true,
    fonts: bakedFonts,
    withBuiltin: usedBy.has(BUILTIN_FAMILY),
  });
  warnFontSizes({
    used: {sizes: [], dynamic: used.dynamic},
    fonts: bakedFonts,
    usedBy,
  });

  return {
    assetPack:
      imageJobs.length || bakedFonts.length
        ? emitAssetPack({
            images: imageJobs.map(job => bakeImage(job)),
            fonts: bakedFonts,
          })
        : undefined,
    imageJobs,
    fontJobs,
  };
}
