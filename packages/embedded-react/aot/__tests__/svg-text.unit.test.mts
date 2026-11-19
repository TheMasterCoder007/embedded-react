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

import {afterEach, describe, expect, it, vi} from 'vitest';
import {dirname, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';
import {compilePackage} from '../compile.mts';
import {bakeSvgTextElements, bakeSvgTextPaths} from '../svg-text-elements.mts';
import {flattenSvg} from '../../src/embedded-react/svg-ops.mts';

// Holds Inter-Regular.ttf and Inter-Bold.ttf.
const FONTS = resolve(
  dirname(fileURLToPath(import.meta.url)),
  '../../../../assets/fonts',
);

const IMPORTS = `import {Svg, Circle, Path, SvgText} from 'embedded-react';
import Inter from './Inter-Regular.ttf';
`;

const app = (children: string, before = ''): string => `${IMPORTS}${before}
export function App() {
  return (
    <Svg width={100} height={100}>
      ${children}
    </Svg>
  );
}`;

/** The `d` of the first baked element. */
const pathOf = (source: string): string => /d="([^"]*)"/.exec(source)![1];

describe('<SvgText>', () => {
  it('bakes to an element with a path and its paint, on the same lines', () => {
    const source = app(
      `<SvgText
        x={50}
        y={20}
        fontFamily={Inter}
        fontSize={14}
        textAnchor="middle"
        fill="#fff">
        N
      </SvgText>`,
    );
    const baked = bakeSvgTextElements(source, FONTS);
    expect(baked).toMatch(/<SvgText d="M[^"]+" fill="#fff"\s*\/>/);
    expect(baked).not.toContain('fontSize');
    expect(baked.split('\n')).toHaveLength(source.split('\n').length);
  });

  it('drops the import of a font it alone used, and keeps one a Text still uses', () => {
    const text = `<SvgText fontFamily={Inter}>N</SvgText>`;
    expect(bakeSvgTextElements(app(text), FONTS)).not.toContain('.ttf');
    const shared = app(text, 'const label = {fontFamily: Inter};');
    expect(bakeSvgTextElements(shared, FONTS)).toContain('.ttf');
  });

  it('finds a family by its file name, and a weight by the file for it', () => {
    const regular = bakeSvgTextElements(
      app(`<SvgText fontFamily="Inter-Regular">N</SvgText>`),
      FONTS,
    );
    const bold = bakeSvgTextElements(
      app(`<SvgText fontFamily="Inter" fontWeight="bold">N</SvgText>`),
      FONTS,
    );
    expect(pathOf(bold)).not.toBe(pathOf(regular));
  });

  it('takes module-level constants, and bakes a transform into the path', () => {
    const plain = bakeSvgTextElements(
      app(
        `<SvgText x={CENTER} y={CENTER} fontFamily={Inter}>{LABEL}</SvgText>`,
        `const CENTER = 50;\nconst LABEL = 'N';`,
      ),
      FONTS,
    );
    const literal = bakeSvgTextElements(
      app(`<SvgText x={50} y={50} fontFamily={Inter}>N</SvgText>`),
      FONTS,
    );
    expect(pathOf(plain)).toBe(pathOf(literal));
    const turned = bakeSvgTextElements(
      app(
        `<SvgText x={50} y={50} fontFamily={Inter} transform="rotate(90 50 50)">N</SvgText>`,
      ),
      FONTS,
    );
    expect(pathOf(turned)).not.toBe(pathOf(literal));
  });

  it('refuses text or placement that is not a constant', () => {
    expect(() =>
      bakeSvgTextElements(
        app(`<SvgText fontFamily={Inter}>{label}</SvgText>`),
        FONTS,
      ),
    ).toThrow(/AOT: <SvgText> text must be a constant/);
    expect(() =>
      bakeSvgTextElements(
        app(`<SvgText fontFamily={Inter} x={offset}>N</SvgText>`),
        FONTS,
      ),
    ).toThrow(/AOT: <SvgText> "x" must be a constant/);
  });

  it('fails with the family when its font is missing', () => {
    expect(() =>
      bakeSvgTextElements(app(`<SvgText fontFamily="Nope">N</SvgText>`), FONTS),
    ).toThrow(/AOT: <SvgText>: no font for font-family "Nope"/);
  });

  it('draws in Flow A as the path it was baked to', () => {
    const baked = bakeSvgTextElements(
      app(`<SvgText x={10} y={20} fontFamily={Inter}>N</SvgText>`),
      FONTS,
    );
    const d = pathOf(baked);
    const element = (type: string) => ({
      $$typeof: Symbol.for('react.transitional.element'),
      type,
      props: {d},
    });
    const asText = flattenSvg({children: element('SvgText')} as never);
    const asPath = flattenSvg({children: element('Path')} as never);
    expect(asText.ops.length).toBeGreaterThan(2);
    expect(asText.ops).toEqual(asPath.ops);
  });

  it('compiles in Flow B as the same package a Path makes', () => {
    const text = app(
      `<SvgText x={10} y={20} fontFamily={Inter} fill="red">N</SvgText>`,
    );
    const baked = bakeSvgTextPaths(text, FONTS);
    const path = app(`<Path d="${pathOf(baked)}" fill="red" />`).replace(
      /import Inter.*\n/,
      '\n',
    );
    const options = {screen: {width: 240, height: 320}};
    const fromText = compilePackage(baked, 'text', options);
    expect(fromText.fonts).toEqual([]);
    expect(Buffer.from(fromText.program)).toEqual(
      Buffer.from(compilePackage(path, 'text', options).program),
    );
  });
});

describe("an <Svg>'s tape size", () => {
  afterEach(() => vi.unstubAllEnvs());

  const screen = {width: 240, height: 320};
  // Three subpaths: a shape header and nine moves or lines, 29 values.
  const drawing = `import {Svg, Path} from 'embedded-react';
export function App() {
  return (
    <Svg width={100} height={100}>
      <Path d="M0 0L9 9L0 9M20 0L29 9L20 9M40 0L49 9L40 9" />
    </Svg>
  );
}`;

  it('refuses a drawing longer than the engine holds, naming both counts', () => {
    expect(() =>
      compilePackage(drawing, 'tape', {screen, maxVectorTape: 28}),
    ).toThrow(
      /AOT: an <Svg> draws 29 tape values but the engine holds at most 28/,
    );
    expect(() =>
      compilePackage(drawing, 'tape', {screen, maxVectorTape: 29}),
    ).not.toThrow();
  });

  it('takes the limit from ER_AOT_VECTOR_TAPE_MAX, and 1024 without it', () => {
    vi.stubEnv('ER_AOT_VECTOR_TAPE_MAX', '28');
    expect(() => compilePackage(drawing, 'tape', {screen})).toThrow(
      /holds at most 28/,
    );
    vi.stubEnv('ER_AOT_VECTOR_TAPE_MAX', undefined);
    const sentence = bakeSvgTextPaths(
      app(`<SvgText fontFamily={Inter}>A sentence of outlined text</SvgText>`),
      FONTS,
    );
    expect(() => compilePackage(sentence, 'tape', {screen})).toThrow(
      /holds at most 1024/,
    );
  });
});
