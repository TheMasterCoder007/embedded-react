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

import {describe, expect, it, vi} from 'vitest';
import {dirname, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';
import {compilePackage} from '../compile.mts';
import {bakePackageAssets} from '../package-assets.mts';

const here = dirname(fileURLToPath(import.meta.url));
const FONTS = resolve(here, '../../../../assets/fonts');

const IMPORTS = `import {useState} from 'react';
import {View, Text, TextInput, StyleSheet} from 'embedded-react';
import Bold from './Inter-Bold.ttf';
import Regular from './Inter-Regular.ttf';
`;

/** An app whose root holds `children`. */
const app = (children: string, before = ''): string => `${IMPORTS}${before}
export function App() {
  const [on, setOn] = useState(false);
  return <View>${children}</View>;
}`;

const compile = (children: string, before = '') =>
  compilePackage(app(children, before), 'fonts', {
    screen: {width: 240, height: 320},
  });

/** Whether a program holds a string. */
const holds = (program: Uint8Array, text: string): boolean =>
  Buffer.from(program).includes(Buffer.from(text, 'utf8'));

describe('fontFamily', () => {
  it('sets the family of a Text and lists the sizes it is drawn at', () => {
    const result = compile(
      `<Text style={{fontFamily: Bold, fontSize: 64}}>21.5</Text>
       <Text style={{fontFamily: Bold, fontSize: 72}}>22</Text>
       <Text style={{fontSize: 20}}>plain</Text>`,
    );
    expect(holds(result.program, 'Inter-Bold')).toBe(true);
    expect(result.fonts).toEqual([
      {
        family: 'Inter-Bold',
        importPath: './Inter-Bold.ttf',
        sizes: [64, 72],
        openSizes: false,
      },
      {family: '', importPath: null, sizes: [20], openSizes: false},
    ]);
  });

  it('leaves out an imported font no text is drawn in', () => {
    const {fonts, program} = compile(
      `<Text style={{fontSize: 20}}>plain</Text>`,
    );
    expect(fonts.map(font => font.family)).toEqual(['']);
    expect(holds(program, 'Inter-Bold')).toBe(false);
  });

  it('takes a family from a StyleSheet, with a later style winning', () => {
    const result = compile(
      `<Text style={[styles.figure, {fontFamily: Regular}]}>21.5</Text>`,
      `const styles = StyleSheet.create({figure: {fontFamily: Bold, fontSize: 48}});`,
    );
    expect(result.fonts).toEqual([
      {
        family: 'Inter-Regular',
        importPath: './Inter-Regular.ttf',
        sizes: [48],
        openSizes: false,
      },
    ]);
  });

  it('adds the sizes of spans and the size of a TextInput', () => {
    const result = compile(
      `<Text style={{fontFamily: Bold, fontSize: 24}}>a<Text style={{fontSize: 32}}>b</Text></Text>
       <TextInput style={{fontFamily: Bold}} />`,
    );
    expect(result.fonts[0].sizes).toEqual([16, 24, 32]);
  });

  it('bakes every imported font for a family state picks', () => {
    const result = compile(
      `<Text style={{fontFamily: on ? Bold : Regular, fontSize: 24}}>a</Text>`,
    );
    expect(
      result.fonts.map(font => [font.family, font.sizes, font.openSizes]),
    ).toEqual([
      ['Inter-Bold', [24], false],
      ['Inter-Regular', [24], false],
    ]);
  });

  it('leaves the sizes open when state drives the size', () => {
    const result = compile(
      `<Text style={{fontFamily: Bold, fontSize: on ? 24 : 32}}>a</Text>`,
    );
    expect(result.fonts).toEqual([
      {
        family: 'Inter-Bold',
        importPath: './Inter-Bold.ttf',
        sizes: [],
        openSizes: true,
      },
    ]);
  });

  it('keeps a family the app names without importing', () => {
    const result = compile(
      `<Text style={{fontFamily: 'Firmware', fontSize: 24}}>a</Text>`,
    );
    expect(result.fonts).toEqual([
      {family: 'Firmware', importPath: null, sizes: [24], openSizes: false},
    ]);
  });

  it('is refused on a View', () => {
    expect(() => compile(`<View style={{fontFamily: Bold}} />`)).toThrow(
      /style "fontFamily" has no effect on <View>/,
    );
  });

  it('is refused when it is no string, or longer than the engine holds', () => {
    expect(() => compile(`<Text style={{fontFamily: 12}}>a</Text>`)).toThrow(
      /"fontFamily" must be a font's family name/,
    );
    expect(() =>
      compile(`<Text style={{fontFamily: '${'x'.repeat(32)}'}}>a</Text>`),
    ).toThrow(/is 32 bytes, but the engine holds 31/);
  });

  it('refuses two font files of one name', () => {
    expect(() =>
      compilePackage(
        `${IMPORTS}import Other from './other/Inter-Bold.ttf';\nexport function App() { return <View />; }`,
      ),
    ).toThrow(/two fonts are named "Inter-Bold"/);
  });
});

describe('bakePackageAssets', () => {
  /** A pack's image and font counts, from its header. */
  const counts = (pack: Uint8Array): {images: number; fonts: number} => {
    const view = new DataView(pack.buffer, pack.byteOffset, pack.byteLength);
    return {images: view.getUint32(8, true), fonts: view.getUint32(12, true)};
  };

  it('bakes each imported font at the sizes its text is drawn at', () => {
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => {});
    const source = app(
      `<Text style={{fontFamily: Bold, fontSize: 64}}>21.5</Text>
       <Text style={{fontFamily: Regular, fontSize: 20}}>plain</Text>
       <Text style={{fontSize: 20}}>built in</Text>`,
    );
    const result = compilePackage(source, 'fonts');
    const {assetPack, fontJobs} = bakePackageAssets(result, source, FONTS);
    expect(fontJobs.map(job => [job.family, job.sizes]).sort()).toEqual([
      ['Inter-Bold', [64]],
      ['Inter-Regular', [20]],
    ]);
    expect(counts(assetPack!)).toEqual({images: 0, fonts: 2});
    expect(warn).not.toHaveBeenCalled();
    warn.mockRestore();
  });

  it('takes the sizes assets.config.js pins, and warns of a size they leave out', () => {
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => {});
    const source = app(
      `<Text style={{fontFamily: Bold, fontSize: 64}}>21.5</Text>`,
    );
    const {fontJobs} = bakePackageAssets(
      compilePackage(source, 'fonts'),
      source,
      FONTS,
      {fonts: {'Inter-Bold': {sizes: [48]}}},
    );
    expect(fontJobs[0].sizes).toEqual([48]);
    expect(warn.mock.calls.flat().join('\n')).toContain(
      '64px → renders at 48px in Inter-Bold',
    );
    warn.mockRestore();
  });

  it('checks a built-in size against the built-in font only', () => {
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => {});
    const source = app(
      `<Text style={{fontFamily: Bold, fontSize: 64}}>21.5</Text>
       <Text style={{fontSize: 14}}>built in</Text>`,
    );
    bakePackageAssets(compilePackage(source, 'fonts'), source, FONTS);
    const text = warn.mock.calls.flat().join('\n');
    expect(text).toContain('14px → renders at 16px in the built-in font');
    expect(text).not.toContain('in Inter-Bold');
    warn.mockRestore();
  });

  it('bakes a family whose size state drives at every size the source names', () => {
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => {});
    const source = app(
      `<Text style={{fontFamily: Bold, fontSize: on ? 24 : 32}}>a</Text>`,
    );
    const {fontJobs} = bakePackageAssets(
      compilePackage(source, 'fonts'),
      source,
      FONTS,
    );
    expect(fontJobs[0].sizes).toEqual([24, 32]);
    warn.mockRestore();
  });

  it('stops on a font file that is not there', () => {
    const source = app(`<Text style={{fontFamily: Bold}}>a</Text>`);
    expect(() =>
      bakePackageAssets(compilePackage(source, 'fonts'), source, here),
    ).toThrow(/font "Inter-Bold" not found at/);
  });

  it('makes no pack for an app with no assets', () => {
    const source = app(`<Text>a</Text>`);
    expect(
      bakePackageAssets(compilePackage(source, 'fonts'), source, FONTS)
        .assetPack,
    ).toBeUndefined();
  });
});
