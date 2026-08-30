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

// Unit tests for the build-time glyph-coverage check: what the scanner counts as rendered text
// (string literals, escapes and all — not comments), and which characters get reported against a
// bake. The font fixture is the repo's Inter-Regular.ttf.
import {describe, it, expect} from 'vitest';
import {readFileSync} from 'node:fs';
import {resolve, dirname} from 'node:path';
import {fileURLToPath} from 'node:url';
import {bakeFont, BUILTIN_EXTRAS} from '../bake-font.mjs';
import {
  BUILTIN_FAMILY,
  findMissingGlyphs,
  formatMissingGlyphs,
  scanTextCodepoints,
  textSignature,
  warnMissingGlyphs,
} from '../glyph-coverage.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const INTER = resolve(here, '../../../../../assets/fonts/Inter-Regular.ttf');
const FONT_DATA = resolve(here, '../../../../../engine/font/font_data.c');

/** Bakes Inter at one size — the smallest bake that still exercises the extras path. */
const inter = extraGlyphs =>
  bakeFont({path: INTER, family: 'Inter', sizes: [16], extraGlyphs});

const cps = (source, opts) =>
  [...scanTextCodepoints(source, opts).keys()].sort((a, b) => a - b);

describe('scanTextCodepoints', () => {
  it('finds non-ASCII in every literal form, raw or escaped', () => {
    expect(cps(`const a = "24\\xB0C";`)).toEqual([0x00b0]);
    expect(cps(`const a = 'ok \\u2713';`)).toEqual([0x2713]);
    expect(cps('const a = `\\u{1F600}`;')).toEqual([0x1f600]);
    expect(cps(`const a = 'µ';`)).toEqual([0x00b5]); // bundler left it raw
  });

  it('ignores comments — an em dash in prose is not rendered text', () => {
    expect(cps(`// a comment — with a dash\nconst a = 'ok';`)).toEqual([]);
    expect(cps(`/* block — comment */ const a = 'ok';`)).toEqual([]);
  });

  it("does not let an apostrophe in a comment swallow the code's strings", () => {
    expect(cps(`// don't do this\nconst a = '±';`)).toEqual([0x00b1]);
  });

  it('ignores zero-width and formatting characters that never render', () => {
    expect(cps(`const a = '\\uFEFF\\u200B\\u2060';`)).toEqual([0x2060]);
  });

  it('reports each character once, with an excerpt of where it was found', () => {
    const found = scanTextCodepoints(
      `const a = 'set to 21\\xB0 now'; const b = '\\xB0';`,
    );
    expect([...found.keys()]).toEqual([0x00b0]);
    expect(found.get(0x00b0)).toContain('21°');
  });

  it('reads JSX element text only when asked (unbundled .jsx)', () => {
    const jsx = `<Text style={s}>LEVEL \u03C0</Text>`; // a bundler would have made this a literal
    expect(cps(jsx)).toEqual([]);
    expect(cps(jsx, {jsx: true})).toEqual([0x03c0]);
  });

  it('keeps comments and strings out of the JSX pass', () => {
    const src = `{/* prose — with a dash */}\n<Text>{'±'}</Text>`;
    expect(cps(src, {jsx: true})).toEqual([0x00b1]);
  });

  it('does not read a comparison as element text', () => {
    expect(cps(`const ok = a > b && c < d;`, {jsx: true})).toEqual([]);
  });

  it('summarises a source as a stable signature', () => {
    expect(textSignature(`const a = '°±';`)).toBe('176,177');
    expect(textSignature(`const a = 'plain ascii';`)).toBe('');
  });
});

describe('BUILTIN_EXTRAS', () => {
  it('matches what the engine actually ships in font_data.c', () => {
    // The check reports gaps against this list, so a drift here would silently under-warn. If this
    // fails, re-run `npm run build:builtin-font` (and the engine text tests).
    const shipped = [
      ...new Set(
        [
          ...readFileSync(FONT_DATA, 'utf8').matchAll(
            /\.codepoint = (0x[0-9A-Fa-f]+)/g,
          ),
        ].map(m => Number(m[1])),
      ),
    ].sort((a, b) => a - b);
    expect(shipped).toEqual([...BUILTIN_EXTRAS]);
  });
});

describe('findMissingGlyphs', () => {
  it('checks the built-in font when the app imports none', () => {
    const missing = findMissingGlyphs({source: `const t = '24\\xB0 \\u03C0';`});
    expect(missing.map(m => m.codepoint)).toEqual([0x03c0]); // ° is in the built-in set, π is not
    expect(missing[0].missingFrom).toEqual([BUILTIN_FAMILY]);
    expect(missing[0].sample).toContain('π');
  });

  it('reports nothing for text the bake fully covers', () => {
    expect(findMissingGlyphs({source: `const t = 'plain ASCII';`})).toEqual([]);
    expect(
      findMissingGlyphs({source: `const t = '\\u2318';`, fonts: [inter('⌘')]}),
    ).toEqual([]);
  });

  it('catches a character the imported font was not asked to bake', () => {
    const font = inter('°');
    const missing = findMissingGlyphs({
      source: `const t = '24\\xB0 \\u2318S';`,
      fonts: [font],
    });
    expect(missing.map(m => m.char)).toEqual(['⌘']);
    expect(missing[0].missingFrom).toEqual(['Inter']);
    expect(missing[0].noGlyphIn).toEqual([]); // Inter has ⌘ — baking it would fix this
  });

  it('marks a character the font file itself cannot draw', () => {
    const missing = findMissingGlyphs({
      source: `const t = '\\u6F22';`,
      fonts: [inter()],
    });
    expect(missing[0].noGlyphIn).toEqual(['Inter']); // Inter has no CJK
  });

  it('reports a gap in any one font when several are baked', () => {
    const missing = findMissingGlyphs({
      source: `const t = '\\u2318';`,
      fonts: [inter('⌘'), {...inter(), family: 'Other'}],
    });
    expect(missing.map(m => m.missingFrom)).toEqual([['Other']]);
  });
});

describe('formatMissingGlyphs', () => {
  it('is empty when nothing is missing', () => {
    expect(formatMissingGlyphs([])).toBe('');
  });

  it('suggests the extraGlyphs line that fixes it, per font', () => {
    const font = inter();
    const text = formatMissingGlyphs(
      findMissingGlyphs({source: `const t = '\\xB0\\u2318';`, fonts: [font]}),
      [font],
    );
    expect(text).toContain("U+00B0 '°'");
    expect(text).toContain(`Inter: {extraGlyphs: "°⌘"}`);
  });

  it('leaves characters the font cannot draw out of the suggestion', () => {
    const font = inter();
    const text = formatMissingGlyphs(
      findMissingGlyphs({source: `const t = '\\xB0\\u6F22';`, fonts: [font]}),
      [font],
    );
    expect(text).toContain('the font file itself has no glyph');
    expect(text).toContain(`Inter: {extraGlyphs: "°"}`); // 漢 dropped — baking it can't help
  });

  it('points at importing a font when only the built-in is in play', () => {
    const text = formatMissingGlyphs(
      findMissingGlyphs({source: `const t = '\\u03C0';`}),
      [],
    );
    expect(text).toContain('built-in font covers ASCII + common symbols');
  });
});

describe('warnMissingGlyphs', () => {
  it('warns once and stays quiet while the warning is unchanged', () => {
    const seen = [];
    const log = m => seen.push(m);
    const source = `const t = '\\u03C0';`;
    warnMissingGlyphs({source, log});
    warnMissingGlyphs({source, log});
    expect(seen).toHaveLength(1);

    warnMissingGlyphs({source: `const t = '\\u03C0\\u2211\\u03B4';`, log});
    expect(seen).toHaveLength(2); // δ is new — the developer needs to see it

    warnMissingGlyphs({source: `const t = 'ascii only';`, log});
    warnMissingGlyphs({source, log});
    expect(seen).toHaveLength(3); // cleared, then regressed — warn again
  });
});
