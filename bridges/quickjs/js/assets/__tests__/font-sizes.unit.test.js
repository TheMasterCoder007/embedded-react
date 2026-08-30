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

// Unit tests for build-time font-size discovery: which fontSize values fold to a bakeable size
// (a literal, a token scale, a responsive ternary, a scale handed down as a prop), which are
// honestly reported as unreadable, and what the coverage check says about the sizes that are.
import {describe, it, expect, vi} from 'vitest';
import {BUILTIN_SIZES} from '../bake-font.mjs';
import {BUILTIN_FAMILY} from '../glyph-coverage.mjs';
import {
  analyzeFontSizes,
  findSizeGaps,
  formatFontSizeReport,
  pickBakedSize,
  warnFontSizes,
} from '../font-sizes.mjs';

const sizes = source => analyzeFontSizes(source).sizes;
const dynamic = source => analyzeFontSizes(source).dynamic.map(d => d.expr);

describe('analyzeFontSizes — literals', () => {
  it('finds literal sizes and rounds fractional ones', () => {
    expect(sizes(`const s = {fontSize: 14};`)).toEqual([14]);
    expect(sizes(`const s = {fontSize: 14.6};`)).toEqual([15]);
    expect(sizes(`const s = {'fontSize': 18};`)).toEqual([18]);
  });

  it('sorts and de-duplicates', () => {
    expect(
      sizes(
        `const a = {fontSize: 24}, b = {fontSize: 10}, c = {fontSize: 24};`,
      ),
    ).toEqual([10, 24]);
  });

  it('reads a shorthand property', () => {
    expect(sizes(`const fontSize = 21; const s = {fontSize};`)).toEqual([21]);
  });

  it('ignores a fontSize in a comment or a string', () => {
    expect(sizes(`// fontSize: 13\nconst s = {fontSize: 20};`)).toEqual([20]);
    expect(
      sizes(`const doc = 'fontSize: 13'; const s = {fontSize: 20};`),
    ).toEqual([20]);
  });

  it('ignores a value that resolves but cannot be baked', () => {
    // Zero/negative are authoring slips and the engine's pixel_size is a uint8 — neither is a size,
    // and neither is a discovery failure worth reporting.
    const r = analyzeFontSizes(
      `const a = {fontSize: 0}, b = {fontSize: -4}, c = {fontSize: 900};`,
    );
    expect(r.sizes).toEqual([]);
    expect(r.dynamic).toEqual([]);
  });
});

describe('analyzeFontSizes — token scales', () => {
  it('folds a scale object, however it is spelled', () => {
    expect(
      sizes(`const T = {body: 14, title: 22}; const s = {fontSize: T.title};`),
    ).toEqual([22]);
    expect(
      sizes(`const T = {'x-large': 33}; const s = {fontSize: T['x-large']};`),
    ).toEqual([33]);
    expect(
      sizes(`const T = {text: {lg: 27}}; const s = {fontSize: T.text.lg};`),
    ).toEqual([27]);
    expect(
      sizes(`const T = [11, 13, 17]; const s = {fontSize: T[2]};`),
    ).toEqual([17]);
  });

  it('folds a spread onto a base scale', () => {
    const src = `const BASE = {sm: 11}; const T = {...BASE, lg: 29};
                 const a = {fontSize: T.sm}; const b = {fontSize: T.lg};`;
    expect(sizes(src)).toEqual([11, 29]);
  });

  it('folds a ratio scale through arithmetic and Math', () => {
    expect(sizes(`const B = 12; const s = {fontSize: B * 2};`)).toEqual([24]);
    expect(
      sizes(`const B = 12; const s = {fontSize: Math.round(B * 1.5)};`),
    ).toEqual([18]);
    expect(
      sizes(`const B = 12; const s = {fontSize: Math.max(B, 20)};`),
    ).toEqual([20]);
  });

  it('resolves a constant declared after the scale that reads it', () => {
    expect(
      sizes(
        `const T = {lg: BASE * 2}; const BASE = 13; const s = {fontSize: T.lg};`,
      ),
    ).toEqual([26]);
  });

  it('bakes both arms of a responsive ternary', () => {
    // Which arm a `screen.width`-driven scale takes is a runtime fact, and one bundle runs on any
    // panel — so both sizes have to exist.
    const src = `const W = screen.width; const T = W > 400 ? {t: 30} : {t: 18};
                 const s = {fontSize: T.t};`;
    expect(sizes(src)).toEqual([18, 30]);
  });

  it('bakes the whole scale when the key is picked at runtime', () => {
    expect(
      sizes(`const T = {a: 13, b: 19}; const s = {fontSize: T[variant]};`),
    ).toEqual([13, 19]);
  });
});

describe('analyzeFontSizes — scales handed to a component', () => {
  const scale = `const SZ = {big: 44, small: 13};`;
  const reader = `const Dial = ({sz}) => ({fontSize: sz.big});`;

  it('follows a scale passed as a JSX prop', () => {
    expect(
      sizes(`${scale} ${reader} const A = () => <Dial sz={SZ}/>;`),
    ).toEqual([44]);
  });

  it('follows it after a bundler has lowered the JSX to a props object', () => {
    expect(sizes(`${scale} ${reader} jsx(Dial, {sz: SZ});`)).toEqual([44]);
  });

  it('unions the scales passed under one prop name', () => {
    const src = `const A = {t: 15}; const B = {t: 31};
                 jsx(X, {s: A}); jsx(Y, {s: B}); const f = () => ({fontSize: s.t});`;
    expect(sizes(src)).toEqual([15, 31]);
  });

  it('does not bind an inline literal — that scale is local to its own component', () => {
    const src = `jsx(Dial, {sz: {big: 44}}); ${reader}`;
    expect(sizes(src)).toEqual([]);
    expect(dynamic(src)).toEqual(['sz.big']);
  });

  it('lets a declared name win over anything passed under the same name', () => {
    const src = `const sz = {big: 13}; const SZ = {big: 44};
                 jsx(D, {sz: SZ}); const f = () => ({fontSize: sz.big});`;
    expect(sizes(src)).toEqual([13]);
  });

  it('survives a prop that is passed itself', () => {
    expect(
      sizes(`jsx(D, {sz: sz}); const f = () => ({fontSize: sz.big});`),
    ).toEqual([]);
  });
});

describe('analyzeFontSizes — what it cannot see', () => {
  it('reports a size that only exists at runtime, with its occurrence count', () => {
    const src = `const f = p => [{fontSize: p.size}, {fontSize: p.size}, {fontSize: n + 1}];`;
    expect(analyzeFontSizes(src).dynamic).toEqual([
      {expr: 'p.size', count: 2},
      {expr: 'n + 1', count: 1},
    ]);
  });

  it('leaves a name bound twice unresolved rather than picking one', () => {
    const src = `const n = 10; function f() { const n = 40; return {fontSize: n}; }`;
    expect(sizes(src)).toEqual([]);
    expect(dynamic(src)).toEqual(['n']);
  });

  it('reads JSX and TypeScript sources', () => {
    expect(
      sizes(
        `const T = {h1: 26}; export const A = () => <Text style={{fontSize: T.h1}}/>;`,
      ),
    ).toEqual([26]);
    expect(
      sizes(`const T: Record<string, number> = {h1: 26};
             export const A = () => <Text style={{fontSize: T.h1 as number}}/>;`),
    ).toEqual([26]);
  });

  it('falls back to a literal scan when the source will not parse', () => {
    expect(sizes(`const s = {fontSize: 14}; @@@ not js @@@`)).toEqual([14]);
  });

  it('handles an empty source', () => {
    expect(analyzeFontSizes('')).toEqual({sizes: [], dynamic: []});
  });
});

describe('pickBakedSize', () => {
  it('picks the nearest baked size, ties going to the larger', () => {
    expect(pickBakedSize(BUILTIN_SIZES, 22)).toBe(24); // equidistant from 20 and 24
    expect(pickBakedSize(BUILTIN_SIZES, 14)).toBe(16);
    expect(pickBakedSize(BUILTIN_SIZES, 36)).toBe(32);
  });

  it('applies the clamp the text renderer applies before resolving a font', () => {
    expect(pickBakedSize(BUILTIN_SIZES, 4)).toBe(10); // clamped to 8, then nearest
    expect(pickBakedSize(BUILTIN_SIZES, 200)).toBe(48); // clamped to 96
  });
});

describe('findSizeGaps', () => {
  it('checks against the built-in font when the app imports none', () => {
    const {gaps} = findSizeGaps({sizes: [14, 16]});
    expect(gaps).toEqual([
      {
        size: 14,
        family: BUILTIN_FAMILY,
        snapsTo: 16,
        baked: [...BUILTIN_SIZES],
      },
    ]);
  });

  it('checks against each imported font, taking jobs or baked results', () => {
    const {gaps} = findSizeGaps({
      sizes: [12, 30],
      fonts: [
        {family: 'Inter', sizes: [12, 24]}, // a bakeFont() job
        {family: 'Mono', sizes: [{pixelSize: 30}]}, // a bakeFont() result
      ],
    });
    expect(gaps).toEqual([
      {size: 12, family: 'Mono', snapsTo: 30, baked: [30]},
      {size: 30, family: 'Inter', snapsTo: 24, baked: [12, 24]},
    ]);
  });

  it('reports sizes past the engine per-family limit as never registering', () => {
    const baked = Array.from({length: 18}, (_, i) => 8 + i);
    const {gaps, dropped} = findSizeGaps({
      sizes: [25],
      fonts: [{family: 'Inter', sizes: baked}],
    });
    expect(dropped).toEqual([
      {family: 'Inter', registered: baked.slice(0, 16), ignored: [24, 25]},
    ]);
    // 25 is baked, but past the limit — so it still snaps, to the last size that did register.
    expect(gaps).toEqual([
      {size: 25, family: 'Inter', snapsTo: 23, baked: baked.slice(0, 16)},
    ]);
  });

  it('says nothing when every used size is baked', () => {
    expect(
      findSizeGaps({
        sizes: [12, 24],
        fonts: [{family: 'Inter', sizes: [12, 24]}],
      }),
    ).toEqual({
      gaps: [],
      dropped: [],
    });
  });
});

describe('formatFontSizeReport', () => {
  it('is empty when there is nothing to report', () => {
    expect(formatFontSizeReport({})).toBe('');
  });

  it('names the size, what it becomes, and the config line that fixes it', () => {
    const fonts = [{family: 'Inter-Regular', sizes: [12, 16]}];
    const text = formatFontSizeReport({
      ...findSizeGaps({sizes: [29], fonts}),
      fonts,
    });
    expect(text).toContain('29px → renders at 16px in Inter-Regular');
    // A family is a file basename, so the suggested key has to survive being one.
    expect(text).toContain(`fonts: {"Inter-Regular": {sizes: [12, 16, 29]}}`);
  });

  it('points an app with no imported font at the sizes the built-in has', () => {
    const text = formatFontSizeReport(findSizeGaps({sizes: [14]}));
    expect(text).toContain(
      `the built-in font is baked at [${BUILTIN_SIZES.join(', ')}]`,
    );
  });

  it('lists unreadable sizes with their counts', () => {
    const text = formatFontSizeReport({dynamic: [{expr: 'p.size', count: 3}]});
    expect(text).toContain(
      '3 fontSize value(s) could not be read at build time',
    );
    expect(text).toContain('fontSize: p.size  (×3)');
  });
});

describe('warnFontSizes', () => {
  const used = {sizes: [14], dynamic: []};

  it('warns once and stays quiet while the warning is unchanged', () => {
    const log = vi.fn();
    warnFontSizes({used: {sizes: [30], dynamic: []}, log}); // seed: any prior state is not ours
    log.mockClear();
    warnFontSizes({used, log});
    warnFontSizes({used, log});
    expect(log).toHaveBeenCalledTimes(1);
    expect(log.mock.calls[0][0]).toContain('14px → renders at 16px');
  });

  it('says nothing when coverage is complete, and reports what it found', () => {
    const log = vi.fn();
    const found = warnFontSizes({
      used: {sizes: [...BUILTIN_SIZES], dynamic: []},
      log,
    });
    expect(log).not.toHaveBeenCalled();
    expect(found).toEqual({gaps: [], dropped: []});
  });
});
