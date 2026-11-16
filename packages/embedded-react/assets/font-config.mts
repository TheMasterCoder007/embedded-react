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

// Turns the `fonts` block of a project's assets.config.js into bakeFont() jobs. Six entry points
// bake fonts (build, pack, sim, sim-server, hot-reload app/split chunks); they share this so a
// config field means the same thing in every one of them.

import type {FontJob} from './bake-font.mts';
import type {ImageFormat} from './bake-image.mts';

/** One family's entry in the `fonts` block of assets.config.js. */
export interface FontConfig {
  sizes?: number[];
  bpp?: number;
  glyphs?: FontJob['glyphs'];
  extraGlyphs?: FontJob['extraGlyphs'];
}

/** A project's assets.config.js. */
export interface AssetsConfig {
  fonts?: Record<string, FontConfig>;
  images?: Record<string, {format?: ImageFormat}>;
}

/** Fallback pixel size when no fontSize could be discovered and the config pins none. */
const DEFAULT_SIZE = 16;

/** Bits per pixel when the config pins none. */
const DEFAULT_BPP = 4;

/**
 * Resolves discovered font imports against their per-project config.
 *
 * @param fonts            Discovered imports: family → .ttf/.otf path.
 * @param fontConfig       assets.config.js `fonts` block, keyed by family.
 * @param discoveredSizes  Sizes the bundle uses (font-sizes.mts analyzeFontSizes).
 * @returns One bakeFont() job per imported font.
 */
export function resolveFontJobs(
  fonts: Map<string, string>,
  fontConfig: Record<string, FontConfig> = {},
  discoveredSizes: number[] = [],
): FontJob[] {
  return [...fonts.entries()].map(([family, path]) => {
    const familyConfig = fontConfig[family] || {};
    return {
      path,
      family,
      sizes: familyConfig.sizes?.length
        ? familyConfig.sizes
        : discoveredSizes.length
          ? discoveredSizes
          : [DEFAULT_SIZE],
      bpp: familyConfig.bpp ?? DEFAULT_BPP,
      glyphs: familyConfig.glyphs ?? 'ascii',
      extraGlyphs: familyConfig.extraGlyphs,
    };
  });
}
