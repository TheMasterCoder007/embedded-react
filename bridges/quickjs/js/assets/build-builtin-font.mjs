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

// Regenerates the engine's built-in font (engine/font/font_data.c) from assets/fonts/Inter-Regular.ttf
// using the same JS bakers as app assets — this is the single source of truth for the built-in font
// (there is no Python step). font_registry.c falls back to g_inter_sizes[] for any text without a
// matching custom family.
//
//   cd bridges/quickjs/js && npm run build:builtin-font
//
// Regenerating changes glyph metrics slightly vs a prior rasterizer, so re-run the engine text tests
// (test_text, yoga_parity) and re-flash to eyeball on-device after changing it.
import {resolve, dirname} from 'node:path';
import {fileURLToPath} from 'node:url';
import {writeFileSync} from 'node:fs';
import {bakeFont, BUILTIN_EXTRAS, BUILTIN_SIZES} from './bake-font.mjs';
import {emitBuiltinFont} from './emit-c.mjs';

const here = dirname(fileURLToPath(import.meta.url)); // bridges/quickjs/js/assets
const repoRoot = resolve(here, '../../../..');
const FONT = resolve(repoRoot, 'assets/fonts/Inter-Regular.ttf');
const OUT = resolve(repoRoot, 'engine/font/font_data.c');

// The built-in covers printable ASCII plus a fixed set of common symbols (degrees, arrows, math,
// punctuation, etc.) the UI components rely on — kept stable across regenerations. The set lives in
// bake-font.mjs as BUILTIN_EXTRAS — with BUILTIN_SIZES alongside it — so the build-time coverage
// checks report against what is actually baked here.
const SIZES = BUILTIN_SIZES;

const font = bakeFont({
  path: FONT,
  family: 'Inter',
  sizes: SIZES,
  bpp: 4,
  glyphs: BUILTIN_EXTRAS,
});
writeFileSync(
  OUT,
  emitBuiltinFont({font, symbol: 'inter', sourceName: 'Inter-Regular.ttf'}),
);

const bytes = font.sizes.reduce((n, s) => n + s.bitmap.length, 0);
console.log(
  `Regenerated ${OUT}\n  ${SIZES.length} sizes [${SIZES.join(',')}], bpp 4, ${font.sizes[0].extras.length} extra glyphs, ${bytes} bitmap bytes`,
);
