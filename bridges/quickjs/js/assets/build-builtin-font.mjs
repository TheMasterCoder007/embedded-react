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
import {bakeFont} from './bake-font.mjs';
import {emitBuiltinFont} from './emit-c.mjs';

const here = dirname(fileURLToPath(import.meta.url)); // bridges/quickjs/js/assets
const repoRoot = resolve(here, '../../../..');
const FONT = resolve(repoRoot, 'assets/fonts/Inter-Regular.ttf');
const OUT = resolve(repoRoot, 'engine/font/font_data.c');

// The built-in covers printable ASCII plus a fixed set of common symbols (degrees, arrows, math,
// punctuation, etc.) the UI components rely on — kept stable across regenerations.
const SIZES = [10, 12, 16, 20, 24, 32, 48];
const EXTRAS = [
  0x00a2, 0x00a3, 0x00a5, 0x00a7, 0x00a9, 0x00ae, 0x00b0, 0x00b1, 0x00b5,
  0x00d7, 0x00f7, 0x2013, 0x2014, 0x2018, 0x2019, 0x201c, 0x201d, 0x2020,
  0x2021, 0x2022, 0x2026, 0x2030, 0x20ac, 0x2122, 0x2190, 0x2191, 0x2192,
  0x2193, 0x2194, 0x21b5, 0x2202, 0x2206, 0x2211, 0x2212, 0x221a, 0x221e,
  0x2248, 0x2260, 0x2264, 0x2265, 0x25a0, 0x25c6, 0x25cf, 0x2605, 0x2606,
  0x2713, 0x2717,
];

const font = bakeFont({
  path: FONT,
  family: 'Inter',
  sizes: SIZES,
  bpp: 4,
  glyphs: EXTRAS,
});
writeFileSync(
  OUT,
  emitBuiltinFont({font, symbol: 'inter', sourceName: 'Inter-Regular.ttf'}),
);

const bytes = font.sizes.reduce((n, s) => n + s.bitmap.length, 0);
console.log(
  `Regenerated ${OUT}\n  ${SIZES.length} sizes [${SIZES.join(',')}], bpp 4, ${font.sizes[0].extras.length} extra glyphs, ${bytes} bitmap bytes`,
);
