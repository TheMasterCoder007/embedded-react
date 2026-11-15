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
import {lowerStyle} from '../style-map.mts';

const IMPORTS = `import {useState} from 'react';
import {View, Text, ScrollView, Pressable} from 'embedded-react';
`;

/** The package an app compiles to. */
const programOf = (body: string): Uint8Array =>
  compilePackage(`${IMPORTS}${body}`).program;

/** An app that is one element with a style. */
const styled = (style: string, tag = 'View'): string => `
export function App() {
  const [on, setOn] = useState(false);
  return <${tag} style={${style}} />;
}`;

describe('backgroundImage', () => {
  it('lowers a string to the gradient fields', () => {
    expect(
      lowerStyle({
        backgroundImage: 'linear-gradient(to right, #102030, red 75%)',
      }),
    ).toEqual([
      {field: 'gradient_type', value: 'ER_GRADIENT_LINEAR'},
      {field: 'gradient_corner', value: 0},
      {field: 'gradient_angle', value: 90},
      {field: 'gradient_stop_count', value: 2},
      {field: 'gradient_stops[0].color', value: 0xff102030},
      {field: 'gradient_stops[0].position', value: 0},
      {field: 'gradient_stops[1].color', value: 0xffff0000},
      {field: 'gradient_stops[1].position', value: 0.75},
      {field: 'gradient_stops[2].color', value: 0},
      {field: 'gradient_stops[2].position', value: 0},
      {field: 'gradient_stops[3].color', value: 0},
      {field: 'gradient_stops[3].position', value: 0},
    ]);
  });

  it('lowers a corner and a radial circle', () => {
    expect(
      lowerStyle({
        backgroundImage: 'linear-gradient(to top left, red, blue)',
      }).slice(0, 2),
    ).toEqual([
      {field: 'gradient_type', value: 'ER_GRADIENT_LINEAR'},
      {field: 'gradient_corner', value: 4},
    ]);
    expect(
      lowerStyle({backgroundImage: 'radial-gradient(circle, red, blue)'})[0],
    ).toEqual({field: 'gradient_type', value: 'ER_GRADIENT_RADIAL'});
  });

  it('compiles the object form to the same package as its string', () => {
    expect(
      programOf(
        styled(`{backgroundImage: [{
          type: 'linear-gradient',
          direction: 'to bottom right',
          colorStops: [{color: '#0f172a'}, {color: '#38bdf8', positions: ['70%']}],
        }]}`),
      ),
    ).toEqual(
      programOf(
        styled(
          "{backgroundImage: 'linear-gradient(to bottom right, #0f172a, #38bdf8 70%)'}",
        ),
      ),
    );
  });

  it('replaces an earlier backgroundImage whole', () => {
    expect(
      programOf(
        styled(
          "[{backgroundImage: 'linear-gradient(45deg, red, green, blue)'}, {backgroundImage: 'linear-gradient(red, blue)'}]",
        ),
      ),
    ).toEqual(
      programOf(styled("{backgroundImage: 'linear-gradient(red, blue)'}")),
    );
  });

  it('compiles on a View, a ScrollView and a Pressable', () => {
    for (const tag of ['View', 'ScrollView', 'Pressable']) {
      expect(() =>
        programOf(
          styled("{backgroundImage: 'linear-gradient(red, blue)'}", tag),
        ),
      ).not.toThrow();
    }
  });

  it('refuses it on a Text', () => {
    expect(() =>
      programOf(
        styled("{backgroundImage: 'linear-gradient(red, blue)'}", 'Text'),
      ),
    ).toThrow(/backgroundImage/);
  });

  it('refuses a state-driven gradient', () => {
    expect(() =>
      programOf(
        styled(
          "{backgroundImage: on ? 'linear-gradient(red, blue)' : 'linear-gradient(blue, red)'}",
        ),
      ),
    ).toThrow(
      /state-driven value for style "backgroundImage" is not supported/,
    );
  });

  it('refuses a gradient the engine does not draw', () => {
    expect(() =>
      programOf(styled("{backgroundImage: 'radial-gradient(red, blue)'}")),
    ).toThrow(
      /unsupported value for style "backgroundImage": a radial gradient must be `circle`/,
    );
    expect(() =>
      programOf(
        styled(
          "{backgroundImage: 'linear-gradient(red, lime, blue, white, black)'}",
        ),
      ),
    ).toThrow(/2 to 4 stops, got 5/);
  });
});
