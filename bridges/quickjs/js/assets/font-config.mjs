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

/** Fallback pixel size when no fontSize could be discovered and the config pins none. */
const DEFAULT_SIZE = 16;

/**
 * Resolves discovered font imports against their per-project config.
 *
 * @param {Map<string,string>} fonts        Discovered imports: family → .ttf/.otf path.
 * @param {object} [fontConfig]             assets.config.js `fonts` block, keyed by family.
 * @param {number[]} [discoveredSizes]      Sizes the bundle uses (font-sizes.mjs analyzeFontSizes).
 * @returns {Array<{path:string, family:string, sizes:number[], bpp:number, glyphs:any,
 *          extraGlyphs:any}>} One bakeFont() job per imported font.
 */
export function resolveFontJobs(fonts, fontConfig = {}, discoveredSizes = []) {
  return [...fonts.entries()].map(([family, path]) => {
    const fc = fontConfig[family] || {};
    return {
      path,
      family,
      sizes: fc.sizes?.length
        ? fc.sizes
        : discoveredSizes.length
          ? discoveredSizes
          : [DEFAULT_SIZE],
      bpp: fc.bpp ?? 4,
      glyphs: fc.glyphs ?? 'ascii',
      extraGlyphs: fc.extraGlyphs,
    };
  });
}
