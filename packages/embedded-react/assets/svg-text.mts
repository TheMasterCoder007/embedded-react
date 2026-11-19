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

// Build-time SVG text: finds the font file a family names and turns a run of text into path ops.
// Shared by the .svg baker and the inline <SvgText> rewrite. The device never sees a font here.
import {existsSync, readFileSync} from 'node:fs';
import {basename, dirname, join, resolve} from 'node:path';
import opentype from 'opentype.js';
import {
  VOP_MOVE,
  VOP_LINE,
  VOP_QUAD,
  VOP_CUBIC,
  VOP_CLOSE,
} from '../src/embedded-react/svg-ops.mts';

/** Where a family's font file may come from. */
export interface FontSearch {
  /** Fonts the app imports: family (the file's basename) → path. */
  imports: Map<string, string>;
  /** Folders searched for `<family>.ttf` or `<family>.otf`. */
  directories: string[];
}

/** One run of text in a single font. */
export interface TextRun {
  text: string;
  font: opentype.Font;
  fontSize: number;
  /** Extra advance after each glyph, in px. */
  letterSpacing: number;
}

export type TextAnchor = 'start' | 'middle' | 'end';

export const DEFAULT_FONT_SIZE = 16;
export const WEIGHT_NORMAL = 400;
const WEIGHT_BOLD = 700;
const WEIGHT_LIGHT = 300;
const WEIGHT_STEP = 100;
const WEIGHT_MIN = 100;
const WEIGHT_MAX = 900;
const FONT_EXTENSIONS = ['.ttf', '.otf'];
const PATH_DECIMALS = 2;

/** The style name a font file carries for each weight, as in `Inter-Bold.ttf`. */
const WEIGHT_NAMES: Record<number, string> = {
  100: 'Thin',
  200: 'ExtraLight',
  300: 'Light',
  400: 'Regular',
  500: 'Medium',
  600: 'SemiBold',
  700: 'Bold',
  800: 'ExtraBold',
  900: 'Black',
};

/** CSS generic families name no file. */
const GENERIC_FAMILIES = new Set([
  'serif',
  'sans-serif',
  'monospace',
  'cursive',
  'fantasy',
  'system-ui',
]);

const fontCache = new Map<string, opentype.Font>();
const fontFiles = new Map<opentype.Font, string>();

function loadFont(path: string): opentype.Font {
  let font = fontCache.get(path);
  if (!font) {
    const bytes = readFileSync(path);
    font = opentype.parse(
      bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength),
    );
    fontCache.set(path, font);
    fontFiles.set(font, basename(path));
  }
  return font;
}

/** A `font-family` list → its family names, generic ones dropped. */
export function parseFontFamilies(value: unknown): string[] {
  if (value == null) return [];
  return String(value)
    .split(',')
    .map(family => family.trim().replace(/^['"]|['"]$/g, ''))
    .filter(family => family && !GENERIC_FAMILIES.has(family.toLowerCase()));
}

/** A `font-weight` → 100..900. */
export function parseFontWeight(value: unknown): number {
  if (value == null || value === '' || value === 'normal') return WEIGHT_NORMAL;
  if (value === 'bold' || value === 'bolder') return WEIGHT_BOLD;
  if (value === 'lighter') return WEIGHT_LIGHT;
  const weight = Number(value);
  if (!Number.isFinite(weight)) return WEIGHT_NORMAL;
  return Math.min(
    WEIGHT_MAX,
    Math.max(WEIGHT_MIN, Math.round(weight / WEIGHT_STEP) * WEIGHT_STEP),
  );
}

/** A `letter-spacing` → px. `em` scales with the font size. */
export function parseLetterSpacing(value: unknown, fontSize: number): number {
  if (value == null || value === 'normal') return 0;
  const spacing = parseFloat(String(value));
  if (!Number.isFinite(spacing)) return 0;
  return /em\s*$/.test(String(value)) ? spacing * fontSize : spacing;
}

export function parseTextAnchor(value: unknown): TextAnchor {
  return value === 'middle' || value === 'end' ? value : 'start';
}

function findFontFile(name: string, search: FontSearch): string | undefined {
  const imported = search.imports.get(name);
  if (imported) return imported;
  for (const directory of search.directories) {
    for (const extension of FONT_EXTENSIONS) {
      const candidate = join(directory, name + extension);
      if (existsSync(candidate)) return candidate;
    }
  }
  return undefined;
}

/**
 * Finds the font for a family list and weight. A weight other than normal wants the family's file for that
 * weight (`Inter-Bold`), or the family's own file when that is its weight.
 *
 * @throws When no file matches, naming the families it looked for.
 */
export function resolveFont(
  families: string[],
  weight: number,
  search: FontSearch,
): opentype.Font {
  // Text that names no family takes the app's only imported font.
  if (families.length === 0 && search.imports.size === 1) {
    return loadFont([...search.imports.values()][0]);
  }
  if (families.length === 0) {
    throw new Error(
      'SVG text needs a font-family that names a font file, such as font-family="Inter" for Inter.ttf',
    );
  }

  const tried: string[] = [];
  for (const family of families) {
    // "Open Sans" may be saved as OpenSans.ttf.
    const spellings = [...new Set([family, family.replace(/\s+/g, '')])];
    for (const spelling of spellings) {
      const styled = `${spelling}-${WEIGHT_NAMES[weight]}`;
      const names =
        weight === WEIGHT_NORMAL ? [spelling, styled] : [styled, spelling];
      for (const name of names) {
        tried.push(name);
        const path = findFontFile(name, search);
        if (!path) continue;
        const font = loadFont(path);
        const fileWeight = font.tables.os2?.usWeightClass;
        if (
          name === styled ||
          weight === WEIGHT_NORMAL ||
          fileWeight === weight
        )
          return font;
      }
    }
  }

  const weightNote = weight === WEIGHT_NORMAL ? '' : ` at weight ${weight}`;
  const places = search.directories.length
    ? ` or as a .ttf or .otf file in ${search.directories.join(', ')}`
    : '';
  throw new Error(
    `no font for font-family "${families.join(', ')}"${weightNote}: looked for ${tried.join(', ')} ` +
      `among the imported fonts${places}`,
  );
}

function checkGlyphs(run: TextRun): void {
  for (const character of run.text) {
    if (character !== ' ' && run.font.charToGlyphIndex(character) === 0) {
      const file = fontFiles.get(run.font) ?? 'the font';
      throw new Error(
        `${file} has no glyph for ${JSON.stringify(character)} in SVG text ${JSON.stringify(run.text)}`,
      );
    }
  }
}

/** Walks a run's glyphs with kerning and letter spacing, returning its advance in px. */
function layoutRun(
  run: TextRun,
  place?: (glyph: opentype.Glyph, x: number) => void,
): number {
  const scale = run.fontSize / run.font.unitsPerEm;
  // One glyph per character, as the bitmap fonts are baked: no ligatures.
  const glyphs = [...run.text].map(character =>
    run.font.charToGlyph(character),
  );
  let x = 0;
  glyphs.forEach((glyph, index) => {
    place?.(glyph, x);
    x += (glyph.advanceWidth ?? 0) * scale + run.letterSpacing;
    if (index + 1 < glyphs.length)
      x += run.font.getKerningValue(glyph, glyphs[index + 1]) * scale;
  });
  return x;
}

/** A run's advance width in px. */
export function runAdvance(run: TextRun): number {
  return layoutRun(run);
}

/**
 * A run's glyph outlines as path ops (no SHAPE header), its baseline starting at (x, y).
 *
 * @throws When the font lacks one of the run's characters.
 */
export function runOps(run: TextRun, x: number, y: number): number[] {
  checkGlyphs(run);
  const ops: number[] = [];
  layoutRun(run, (glyph, offset) => {
    for (const command of glyph.getPath(x + offset, y, run.fontSize).commands) {
      if (command.type === 'M') ops.push(VOP_MOVE, command.x, command.y);
      else if (command.type === 'L') ops.push(VOP_LINE, command.x, command.y);
      else if (command.type === 'Q')
        ops.push(VOP_QUAD, command.x1, command.y1, command.x, command.y);
      else if (command.type === 'C')
        ops.push(
          VOP_CUBIC,
          command.x1,
          command.y1,
          command.x2,
          command.y2,
          command.x,
          command.y,
        );
      else ops.push(VOP_CLOSE);
    }
  });
  return ops;
}

/** How far the baseline drops for a `dominant-baseline`, in px. */
export function baselineShift(
  font: opentype.Font,
  fontSize: number,
  dominantBaseline: unknown,
): number {
  const scale = fontSize / font.unitsPerEm;
  if (dominantBaseline === 'middle') {
    const xHeight = font.tables.os2?.sxHeight || font.ascender / 2;
    return (xHeight / 2) * scale;
  }
  if (dominantBaseline === 'central')
    return ((font.ascender + font.descender) / 2) * scale;
  if (dominantBaseline === 'hanging' || dominantBaseline === 'text-before-edge')
    return font.ascender * scale;
  return 0;
}

/** How far a chunk of the given width shifts for its anchor. */
export function anchorOffset(anchor: TextAnchor, width: number): number {
  return anchor === 'middle' ? -width / 2 : anchor === 'end' ? -width : 0;
}

/** Path ops (no SHAPE header) → an SVG `d` string. */
export function opsToPathData(ops: readonly number[]): string {
  const format = (value: number) =>
    String(Number(value.toFixed(PATH_DECIMALS)));
  const LETTERS: Record<number, [string, number]> = {
    [VOP_MOVE]: ['M', 2],
    [VOP_LINE]: ['L', 2],
    [VOP_QUAD]: ['Q', 4],
    [VOP_CUBIC]: ['C', 6],
    [VOP_CLOSE]: ['Z', 0],
  };
  let d = '';
  let index = 0;
  while (index < ops.length) {
    const [letter, count] = LETTERS[ops[index++]];
    d +=
      letter +
      ops
        .slice(index, index + count)
        .map(format)
        .join(' ');
    index += count;
  }
  return d;
}

/**
 * The fonts a source file imports: family (the file's basename) → path.
 *
 * @param source  The importing file's text.
 * @param directory  The folder its import paths are relative to.
 */
export function importedFonts(
  source: string,
  directory: string,
): Map<string, string> {
  const fonts = new Map<string, string>();
  const pattern = /\bfrom\s*['"]([^'"]+\.(?:ttf|otf))['"]/gi;
  let match;
  while ((match = pattern.exec(source)) !== null) {
    const family = basename(match[1]).replace(/\.[^.]+$/, '');
    fonts.set(family, resolve(directory, match[1]));
  }
  return fonts;
}

/** Where an .svg file's fonts are found: beside it, in a `fonts` folder next to it, or imported. */
export function svgFontSearch(
  svgPath: string,
  imports: Map<string, string>,
): FontSearch {
  const directory = dirname(svgPath);
  return {imports, directories: [directory, join(directory, 'fonts')]};
}
