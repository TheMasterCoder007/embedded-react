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
import {View, Text, TextInput, Modal, Pressable} from 'embedded-react';
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

describe('shadow styles', () => {
  it('lowers each key to its ERProps field', () => {
    expect(
      lowerStyle({
        shadowColor: '#102030',
        shadowOffset: {width: 2, height: 3.5},
        shadowOpacity: 0.4,
        shadowRadius: 6,
        elevation: 4,
      }),
    ).toEqual([
      {field: 'shadow_color', value: 0xff102030},
      {field: 'shadow_offset_x', value: 2},
      {field: 'shadow_offset_y', value: 3.5},
      {field: 'shadow_opacity', value: 0.4},
      {field: 'shadow_radius', value: 6},
      {field: 'elevation', value: 4},
    ]);
  });

  it('reads a side left out of shadowOffset as 0', () => {
    expect(lowerStyle({shadowOffset: {height: 5}})).toEqual([
      {field: 'shadow_offset_x', value: 0},
      {field: 'shadow_offset_y', value: 5},
    ]);
  });

  it('clamps a radius and an elevation to a byte', () => {
    expect(lowerStyle({shadowRadius: 300, elevation: -2})).toEqual([
      {field: 'shadow_radius', value: 255},
      {field: 'elevation', value: 0},
    ]);
  });

  it('replaces an earlier shadowOffset whole', () => {
    expect(
      programOf(
        styled(
          '[{shadowOffset: {width: 9, height: 9}}, {shadowOffset: {height: 2}}]',
        ),
      ),
    ).toEqual(programOf(styled('{shadowOffset: {width: 0, height: 2}}')));
  });

  it('compiles a shadow on a View, a Pressable and a Modal', () => {
    const shadow =
      "{shadowColor: 'black', shadowOffset: {width: 1, height: 2}, shadowOpacity: 0.5, shadowRadius: 4, elevation: 2}";
    for (const tag of ['View', 'Pressable', 'Modal visible={on}']) {
      expect(() => programOf(styled(shadow, tag))).not.toThrow();
    }
  });

  it('changes the package', () => {
    expect(programOf(styled('{shadowOpacity: 0.5}'))).not.toEqual(
      programOf(styled('{}')),
    );
  });

  it('compiles a state-driven color, opacity, radius and elevation', () => {
    expect(() =>
      programOf(
        styled(
          "{shadowColor: on ? 'red' : 'black', shadowOpacity: on ? 0.8 : 0.2, shadowRadius: on ? 8 : 2, elevation: on ? 4 : 0}",
        ),
      ),
    ).not.toThrow();
  });

  it('refuses a state-driven shadowOffset', () => {
    expect(() =>
      programOf(styled('{shadowOffset: {width: 0, height: on ? 4 : 1}}')),
    ).toThrow(/state-driven value for style "shadowOffset" is not supported/);
  });

  it('refuses a shadowOffset that is not an object', () => {
    expect(() => programOf(styled('{shadowOffset: 4}'))).toThrow(
      /unsupported value for style "shadowOffset": expected \{width, height\}/,
    );
  });

  it('refuses a shadow where the engine draws none', () => {
    expect(() => programOf(styled('{shadowOpacity: 0.5}', 'Text'))).toThrow(
      /style "shadowOpacity" has no effect on <Text>/,
    );
    expect(() => programOf(styled('{elevation: 2}', 'TextInput'))).toThrow(
      /style "elevation" has no effect on <TextInput>/,
    );
  });
});
