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
import {lowerStyle, RADIANS_TO_DEGREES} from '../style-map.mts';

const IMPORTS = `import {useState} from 'react';
import {View, Text, Image, StyleSheet, useAnimatedValue} from 'embedded-react';
`;

/** The package an app compiles to. */
const programOf = (body: string): Uint8Array =>
  compilePackage(`${IMPORTS}${body}`).program;

/** An app that is one element with a style, a state and an animated value in reach. */
const styled = (style: string, tag = 'View'): string => `
const TILT = '30deg';
const styles = StyleSheet.create({turned: {transform: [{rotate: '45deg'}, {scale: 2}]}});
export function App() {
  const [on, setOn] = useState(false);
  const [turn, setTurn] = useState(0);
  const spin = useAnimatedValue(0);
  return <${tag} style={${style}} />;
}`;

describe('transform styles', () => {
  it('lowers each entry to its ERProps fields', () => {
    expect(
      lowerStyle({
        transform: [
          {translateX: 4},
          {translateY: -2.5},
          {scale: 2},
          {scaleY: 3},
          {rotate: '45deg'},
          {rotateX: 10},
          {rotateY: '20deg'},
          {perspective: 300},
        ],
      }),
    ).toEqual([
      {field: 'transform_translate_x', value: 4},
      {field: 'transform_translate_y', value: -2.5},
      {field: 'transform_scale_x', value: 2},
      {field: 'transform_scale_y', value: 2},
      {field: 'transform_scale_y', value: 3},
      {field: 'transform_rotate_z', value: 45},
      {field: 'transform_rotate_x', value: 10},
      {field: 'transform_rotate_y', value: 20},
      {field: 'transform_perspective', value: 300},
    ]);
  });

  it('reads an angle in radians as degrees', () => {
    expect(lowerStyle({transform: [{rotateZ: '2rad'}]})).toEqual([
      {field: 'transform_rotate_z', value: 2 * RADIANS_TO_DEGREES},
    ]);
  });

  it('lowers transformOrigin to its two fractions', () => {
    expect(lowerStyle({transformOrigin: [0, 1]})).toEqual([
      {field: 'transform_origin_x', value: 0},
      {field: 'transform_origin_y', value: 1},
    ]);
  });

  it('changes the package', () => {
    expect(programOf(styled("{transform: [{rotate: '45deg'}]}"))).not.toEqual(
      programOf(styled('{}')),
    );
  });

  it('compiles a static transform from a StyleSheet or a const', () => {
    expect(programOf(styled('styles.turned'))).toEqual(
      programOf(styled("{transform: [{rotate: '45deg'}, {scale: 2}]}")),
    );
    expect(programOf(styled('{transform: [{rotate: TILT}]}'))).toEqual(
      programOf(styled("{transform: [{rotate: '30deg'}]}")),
    );
  });

  it('replaces an earlier transform whole', () => {
    expect(
      programOf(
        styled(
          '[styles.turned, {transform: [{scale: spin}]}, {transform: [{translateX: 4}]}]',
        ),
      ),
    ).toEqual(programOf(styled('{transform: [{translateX: 4}]}')));
  });

  it('compiles a state-driven entry', () => {
    for (const entry of [
      '{rotate: `${turn}deg`}',
      '{rotate: `${turn}rad`}',
      "{rotate: turn + 'deg'}",
      '{rotate: turn}',
      "{rotate: on ? '90deg' : '0deg'}",
      '{rotate: on ? TILT : `${turn}deg`}',
      '{scale: on ? 1.5 : 1}',
      '{translateX: turn * 2}',
    ]) {
      expect(programOf(styled(`{transform: [${entry}]}`))).not.toEqual(
        programOf(styled('{}')),
      );
    }
  });

  it('reads a state-driven angle the same however it is written', () => {
    expect(programOf(styled("{transform: [{rotate: turn + 'deg'}]}"))).toEqual(
      programOf(styled('{transform: [{rotate: `${turn}deg`}]}')),
    );
  });

  it('compiles static, state-driven and animated entries in one array', () => {
    for (const tag of ['View', 'Text']) {
      expect(() =>
        programOf(
          styled(
            '{transform: [{translateX: 10}, {rotate: `${turn}deg`}, {scale: spin}], transformOrigin: [0.5, 1]}',
            tag,
          ),
        ),
      ).not.toThrow();
    }
  });

  it('compiles a state-driven transform on an <Svg>, and no other state-driven style', () => {
    const drawing = (style: string): string =>
      styled(style, 'Svg width={40} height={40}').replace(
        ' />;',
        '><Rect width={40} height={40} fill="#fff" /></Svg>;',
      );
    expect(() =>
      programOf(drawing('{transform: [{rotate: `${turn}deg`}]}')),
    ).not.toThrow();
    expect(() => programOf(drawing('{width: on ? 40 : 20}'))).toThrow(
      /state-driven style on an <Svg> is not supported/,
    );
  });

  it('refuses a transform it has no field for', () => {
    expect(() => programOf(styled("{transform: [{skewX: '10deg'}]}"))).toThrow(
      /transform "skewX" is not supported/,
    );
    expect(() => programOf(styled('{transform: [{rotateX: spin}]}'))).toThrow(
      /transform "rotateX" cannot be animated in Flow B/,
    );
  });

  it('refuses a value that is no number or angle', () => {
    expect(() => programOf(styled("{transform: [{rotate: 'left'}]}"))).toThrow(
      /unsupported value for transform "rotate": expected an angle/,
    );
    expect(() =>
      programOf(styled('{transform: [{scale: on ? "a" : "b"}]}')),
    ).toThrow(/state-driven transform "scale" needs a number/);
  });

  it('refuses a state-driven transform that is not written in place', () => {
    expect(() =>
      programOf(styled('{transform: on ? [{scale: 2}] : []}')),
    ).toThrow(/state-driven "transform" must be an array written in place/);
  });

  it('refuses a state-driven transformOrigin', () => {
    expect(() =>
      programOf(styled('{transformOrigin: [on ? 0 : 1, 0.5]}')),
    ).toThrow(
      /state-driven value for style "transformOrigin" is not supported/,
    );
  });
});
