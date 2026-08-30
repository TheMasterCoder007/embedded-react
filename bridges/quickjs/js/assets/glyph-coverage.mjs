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

// Build-time glyph-coverage check: a character the app renders but the bake has no glyph for is
// silently substituted with '?' by the engine (font_glyph() in engine/font/font_bitmap.c), so the
// only way to find out used to be looking at the device. Every bake path (build, pack, sim, hot
// reload, AOT) runs this over the app source and warns with the `assets.config.js` line that fixes it.
//
// The check is source-driven: it reads string and template literals (plus JSX element text, for the
// unbundled `.jsx` the AOT flow compiles). Non-ASCII from a runtime source — a fetched string,
// String.fromCharCode — can't be seen here. Comments are skipped: an em dash in prose is not text.
import {
  ASCII_FIRST,
  ASCII_LAST,
  BUILTIN_EXTRAS,
  loadGlyphProbe,
} from './bake-font.mjs';

/** Name used for the engine's fallback font in warnings (it has no family of its own). */
export const BUILTIN_FAMILY = 'the built-in font';

// Comments first so an apostrophe inside one can't open a bogus string; then the three literal
// forms. Bundled output is machine-generated, so the simple form is enough (a regex literal
// containing a lone quote is the only realistic misread, and it costs at most a stray sample).
const LITERALS =
  /\/\/[^\n]*|\/\*[\s\S]*?\*\/|'(?:[^'\\\n]|\\[\s\S])*'|"(?:[^"\\\n]|\\[\s\S])*"|`(?:[^`\\]|\\[\s\S])*`/g;

const SIMPLE_ESCAPES = {n: '\n', t: '\t', r: '\r', b: '\b', f: '\f', v: '\v'};

/** Resolves the \u{...}, \uXXXX, \xNN and simple escapes a bundler emits back to characters. */
function decodeEscapes(body) {
  return body.replace(
    /\\u\{([0-9a-fA-F]{1,6})\}|\\u([0-9a-fA-F]{4})|\\x([0-9a-fA-F]{2})|\\([\s\S])/g,
    (_m, braced, u, x, other) => {
      if (braced !== undefined) {
        const cp = parseInt(braced, 16);
        return cp <= 0x10ffff ? String.fromCodePoint(cp) : '';
      }
      if (u !== undefined) return String.fromCharCode(parseInt(u, 16));
      if (x !== undefined) return String.fromCharCode(parseInt(x, 16));
      return SIMPLE_ESCAPES[other] ?? other; // \\ \' \" \` and friends: the character itself
    },
  );
}

/** Invisible/formatting characters that are never baked and never render — not worth warning about. */
function ignorable(cp) {
  return (
    (cp >= 0x7f && cp <= 0x9f) || // DEL + C1 controls
    (cp >= 0x200b && cp <= 0x200f) || // zero-width + bidi marks
    (cp >= 0x2028 && cp <= 0x202e) || // line/paragraph separators + bidi overrides
    (cp >= 0xfe00 && cp <= 0xfe0f) || // variation selectors
    cp === 0xfeff // BOM
  );
}

// Text between two tags in unbundled JSX: `<Text>24° out</Text>`. Braces are excluded so an
// expression child stays out of it — its own literals are already covered by LITERALS.
const JSX_TEXT = />([^<>{}]+)</g;

/**
 * Collects every non-ASCII character that appears in the app's rendered text.
 *
 * @param {string} source  JS source (a bundle, a chunk, or a single component file).
 * @param {object} [opts]
 * @param {boolean} [opts.jsx]  Also read JSX element text. Set it for unbundled `.jsx` — a bundler
 *        has already turned element text into string literals, and on bundled output the extra
 *        pass would only misread `a > b` comparisons.
 * @returns {Map<number, string>} Codepoint → a short excerpt of the text it was found in.
 */
export function scanTextCodepoints(source, {jsx = false} = {}) {
  const found = new Map();
  const collect = text => {
    let i = 0;
    for (const ch of text) {
      const cp = ch.codePointAt(0);
      if (cp > ASCII_LAST && !ignorable(cp) && !found.has(cp))
        found.set(cp, excerpt(text, i, ch.length));
      i += ch.length;
    }
  };

  // Blank each token out as it is read, so the JSX pass below sees neither strings (already
  // collected) nor comments (never rendered) — only real element text.
  const bare = source.replace(LITERALS, tok => {
    if (tok[0] !== '/') collect(decodeEscapes(tok.slice(1, -1)));
    return ' '.repeat(tok.length);
  });
  if (jsx) for (const m of bare.matchAll(JSX_TEXT)) collect(m[1]);
  return found;
}

/**
 * Compact signature of the non-ASCII characters a source renders. Watch loops fold this into their
 * "did the asset inputs change?" key so new text is re-checked, not just new fonts.
 *
 * @param {string} source  JS source to scan.
 * @param {object} [opts]   Passed through to scanTextCodepoints (e.g. `{jsx: true}`).
 * @returns {string} Sorted codepoints, comma-joined.
 */
export function textSignature(source, opts) {
  return [...scanTextCodepoints(source, opts).keys()]
    .sort((a, b) => a - b)
    .join(',');
}

/** A one-line, printable window of `text` around the character at `at` (a UTF-16 index). */
function excerpt(text, at, len) {
  const start = Math.max(0, at - 12);
  const end = Math.min(text.length, at + len + 12);
  const body = text
    .slice(start, end)
    .replace(/[\n\r\t]/g, ' ')
    .replace(/^[\udc00-\udfff]|[\ud800-\udbff]$/g, '') // a pair split by the window
    .trim();
  return `${start > 0 ? '…' : ''}${body}${end < text.length ? '…' : ''}`;
}

/** The set of codepoints a baked font can render: the dense ASCII range plus its baked extras. */
function coverageOf(baked) {
  const size = baked.sizes?.[0];
  const covered = new Set();
  const first = size ? size.first : ASCII_FIRST;
  const last = size ? size.last : ASCII_LAST;
  for (let cp = first; cp <= last; cp++) covered.add(cp);
  for (const e of size?.extras ?? []) covered.add(e.codepoint);
  return covered;
}

/** The engine's fallback font, shaped like a bakeFont() result so both go through one code path. */
function builtinFont() {
  return {
    family: BUILTIN_FAMILY,
    sizes: [
      {
        first: ASCII_FIRST,
        last: ASCII_LAST,
        extras: BUILTIN_EXTRAS.map(codepoint => ({codepoint})),
      },
    ],
  };
}

/**
 * Finds the characters the app renders that some font in the bake has no glyph for.
 *
 * Which fonts are checked follows what the app can actually draw with: with imported fonts, each
 * baked font is checked (text using that family is the whole reason it was imported, and a
 * character the app's own font lacks is exactly the bug worth reporting); with none, the engine's
 * built-in fallback is checked instead.
 *
 * @param {object} opts
 * @param {string} opts.source            App/bundle JS to scan for rendered text.
 * @param {Array<object>} [opts.fonts]    bakeFont() results; empty means built-in font only.
 * @param {boolean} [opts.jsx]            Source is unbundled JSX — read element text too.
 * @returns {Array<{codepoint:number, char:string, sample:string, missingFrom:string[],
 *          noGlyphIn:string[]}>} Sorted by codepoint. `noGlyphIn` narrows `missingFrom` to the
 *          fonts whose file has no glyph for the character at all — baking it there can't help.
 */
export function findMissingGlyphs({source, fonts = [], jsx = false}) {
  const checked = fonts.length ? fonts : [builtinFont()];
  const coverage = checked.map(f => ({
    family: f.family,
    path: f.path,
    covered: coverageOf(f),
  }));
  // Opened lazily and only for a font with a gap: most builds have none and pay nothing.
  const probes = new Map();
  const hasGlyph = (font, cp) => {
    if (!font.path) return true; // the built-in font's coverage is its bake, nothing more to ask
    if (!probes.has(font.path))
      probes.set(font.path, loadGlyphProbe(font.path));
    return probes.get(font.path)(cp);
  };

  const missing = [];
  for (const [cp, sample] of scanTextCodepoints(source, {jsx})) {
    const gaps = coverage.filter(c => !c.covered.has(cp));
    if (!gaps.length) continue;
    missing.push({
      codepoint: cp,
      char: String.fromCodePoint(cp),
      sample,
      missingFrom: gaps.map(g => g.family),
      noGlyphIn: gaps.filter(g => !hasGlyph(g, cp)).map(g => g.family),
    });
  }
  return missing.sort((a, b) => a.codepoint - b.codepoint);
}

const MAX_LISTED = 12;

/**
 * Renders findMissingGlyphs() output as a developer-facing warning, ending with the config change
 * that fixes it.
 *
 * @param {Array<object>} missing   Result of findMissingGlyphs().
 * @param {Array<object>} [fonts]   The same bakeFont() results, for the suggested config line.
 * @returns {string} The warning text (empty when nothing is missing).
 */
export function formatMissingGlyphs(missing, fonts = []) {
  if (!missing.length) return '';
  const lines = missing.slice(0, MAX_LISTED).map(m => {
    const hex = m.codepoint.toString(16).toUpperCase().padStart(4, '0');
    const note = m.noGlyphIn.length
      ? ` (the font file itself has no glyph for it — pick a font that covers it)`
      : '';
    return `  U+${hex} '${m.char}'  missing from ${m.missingFrom.join(', ')}${note}   in "${m.sample}"`;
  });
  if (missing.length > MAX_LISTED)
    lines.push(`  … and ${missing.length - MAX_LISTED} more`);

  // One config entry per font, listing only what baking it there would actually fix.
  const perFamily = fonts
    .map(f => {
      const chars = missing
        .filter(
          m =>
            m.missingFrom.includes(f.family) && !m.noGlyphIn.includes(f.family),
        )
        .map(m => m.char)
        .join('');
      // A family is a file basename, so it often needs quoting to be a valid key ('Inter-Regular').
      const key = /^[A-Za-z_$][\w$]*$/.test(f.family)
        ? f.family
        : JSON.stringify(f.family);
      return chars ? `${key}: {extraGlyphs: ${JSON.stringify(chars)}}` : null;
    })
    .filter(Boolean);

  const fix = perFamily.length
    ? `  fix: assets.config.js → fonts: {${perFamily.join(', ')}}`
    : fonts.length
      ? `  fix: bake these from a font that has them (assets.config.js → fonts: {<Family>: {extraGlyphs: ...}})`
      : `  fix: the built-in font covers ASCII + common symbols only — import a .ttf/.otf and bake the\n` +
        `       rest with assets.config.js → fonts: {<Family>: {extraGlyphs: ${JSON.stringify(missing.map(m => m.char).join(''))}}}`;

  return (
    `embedded-react: ${missing.length} character(s) in the app's text have no baked glyph and will ` +
    `render as '?':\n${lines.join('\n')}\n${fix}`
  );
}

let lastWarning = null; // dev loops re-bake on every save — don't reprint an unchanged warning

/**
 * Checks glyph coverage and warns. Repeats are suppressed while the warning is unchanged, so a
 * watch/hot-reload loop reports each gap once rather than on every save.
 *
 * @param {object} opts
 * @param {string} opts.source          App/bundle JS to scan for rendered text.
 * @param {Array<object>} [opts.fonts]  bakeFont() results; empty means built-in font only.
 * @param {boolean} [opts.jsx]          Source is unbundled JSX — read element text too.
 * @param {Function} [opts.log]         Sink for the warning (default console.warn).
 * @returns {Array<object>} The missing-glyph records (empty when coverage is complete).
 */
export function warnMissingGlyphs({
  source,
  fonts = [],
  jsx = false,
  log = console.warn,
}) {
  const missing = findMissingGlyphs({source, fonts, jsx});
  const text = formatMissingGlyphs(missing, fonts);
  if (text && text !== lastWarning) log(text);
  lastWarning = text || null;
  return missing;
}
