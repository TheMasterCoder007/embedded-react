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

import {describe, expect, it} from 'vitest';
import {compilePackage} from '../compile.mts';

const app = (
  children: string,
  hooks = '',
): string => `import {useState} from 'react';
import {Svg, Circle, Rect, G, ClipPath, Mask, Pressable} from 'embedded-react';
export function App() {
  ${hooks}
  return (
    <Svg width={100} height={100}>
      ${children}
    </Svg>
  );
}`;

const compile = (children: string, hooks = ''): unknown =>
  compilePackage(app(children, hooks), 'App.tsx', {
    screen: {width: 240, height: 320},
    release: true,
  });

const CLIP = '<ClipPath id="c"><Circle cx={50} cy={50} r={30} /></ClipPath>';

describe('<ClipPath> and <Mask> in Flow B', () => {
  it('compiles a clipped and a masked shape', () => {
    expect(() =>
      compile(
        `${CLIP}<Mask id="m"><Rect width={100} height={50} fill="#fff" /></Mask>
        <G clipPath="url(#c)"><Rect width={100} height={100} mask="url(#m)" /></G>`,
      ),
    ).not.toThrow();
  });

  it('compiles an empty clip, which hides what it clips', () => {
    expect(() =>
      compile(
        '<ClipPath id="none"></ClipPath><Rect width={9} height={9} clipPath="url(#none)" />',
      ),
    ).not.toThrow();
  });

  it('compiles one scaled by a viewBox to the screen', () => {
    const source = app(
      `${CLIP}<Rect width={100} height={100} clipPath="url(#c)" />`,
    ).replace(
      '<Svg width={100} height={100}>',
      '<Svg viewBox="0 0 100 100" width={screen.width} height={screen.width}>',
    );
    expect(() =>
      compilePackage(
        source.replace(
          'export function App',
          "import {screen} from 'embedded-react';\nexport function App",
        ),
        'App.tsx',
        {
          screens: [
            {width: 240, height: 320},
            {width: 320, height: 240},
          ],
          release: true,
        },
      ),
    ).not.toThrow();
  });

  it('refuses a clipPath or mask that names nothing, or the wrong kind', () => {
    expect(() =>
      compile('<Rect width={9} height={9} clipPath="url(#nope)" />'),
    ).toThrow(/clipPath="url\(#nope\)" names no <ClipPath> in its <Svg>/);
    expect(() =>
      compile(`${CLIP}<Rect width={9} height={9} mask="url(#c)" />`),
    ).toThrow(/mask="url\(#c\)" names no <Mask>/);
    expect(() =>
      compile(`${CLIP}<Rect width={9} height={9} clipPath="c" />`),
    ).toThrow(/names no <ClipPath>[\s\S]*url\(#id\)/);
  });

  it('refuses a clip on a shape inside a clip, and more clips at once than the engine keeps', () => {
    expect(() =>
      compile(
        `${CLIP}<ClipPath id="d"><Rect width={9} height={9} clipPath="url(#c)" /></ClipPath>
        <Rect width={9} height={9} clipPath="url(#d)" />`,
      ),
    ).toThrow(/inside a <ClipPath> or <Mask> can't take a clipPath of its own/);
    expect(() =>
      compile(
        `${CLIP}<G clipPath="url(#c)"><G clipPath="url(#c)"><G clipPath="url(#c)"><G clipPath="url(#c)">
          <Rect width={9} height={9} clipPath="url(#c)" />
        </G></G></G></G>`,
      ),
    ).toThrow(/more than 4 clips and masks apply to one shape/);
  });

  it('refuses one in a state-driven <Svg>, saying what to do', () => {
    expect(() =>
      compile(
        `<Circle cx={50} cy={50} r={n} clipPath="url(#c)" />`,
        'const [n, setN] = useState(5);',
      ),
    ).toThrow(
      /a clipPath or mask in a state-driven <Svg> is not yet supported/,
    );
  });
});
