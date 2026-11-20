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
import {Table} from '../compiler/package/format.mts';

const app = (
  children: string,
  hooks = '',
  svg = '<Svg width={100} height={100}>',
): string => `import {useState} from 'react';
import {Svg, Circle, Rect, Line, G, Animated, useAnimatedValue, screen} from 'embedded-react';
export function App() {
  ${hooks}
  return (
    ${svg}
      ${children}
    </Svg>
  );
}`;

const compile = (children: string, hooks = '', svg?: string) =>
  compilePackage(app(children, hooks, svg), 'App.tsx', {
    screens: [
      {width: 240, height: 320},
      {width: 320, height: 240},
    ],
    release: true,
  });

/** Whether a program holds these floats in a row, as a tape does. */
const holds = (program: Uint8Array, floats: number[]): boolean =>
  Buffer.from(program).includes(Buffer.from(new Float32Array(floats).buffer));

const DASH = 8;
const DASH_OFFSET = 9;
const ANIMATED = 'const offset = useAnimatedValue(0);';

describe('dashed strokes in Flow B', () => {
  it('bakes a pattern and its offset into a static tape', () => {
    const {program} = compile(
      '<Circle cx={50} cy={50} r={40} fill="none" stroke="#fff" strokeDasharray="6 3" strokeDashoffset={2} />',
    );
    expect(holds(program, [0, 0, DASH, 6, 3, DASH_OFFSET, 2, 0])).toBe(true);
  });

  it('binds an animated offset to the node, and leaves the tape static', () => {
    const plain = compile(
      '<Circle cx={50} cy={50} r={40} fill="none" stroke="#fff" strokeDasharray={[250, 250]} />',
      ANIMATED,
    );
    const bound = compile(
      '<Circle cx={50} cy={50} r={40} fill="none" stroke="#fff" strokeDasharray={[250, 250]} strokeDashoffset={offset} />',
      ANIMATED,
    );
    expect(holds(bound.program, [DASH, 250, 250, DASH_OFFSET, 0, 1])).toBe(
      true,
    );
    const size = (result: typeof plain, table: number): number =>
      result.tables
        .find(entry => entry.id === table)
        ?.bytes.reduce((total, bytes) => total + bytes, 0) ?? 0;
    expect(size(bound, Table.bindings)).toBeGreaterThan(
      size(plain, Table.bindings),
    );
    expect(size(bound, Table.functions)).toBe(size(plain, Table.functions));
  });

  it('binds one through an interpolation, and from the <Svg> itself', () => {
    expect(() =>
      compile(
        `<Circle cx={50} cy={50} r={40} fill="none" stroke="#fff" strokeDasharray={251}
          strokeDashoffset={offset.interpolate({inputRange: [0, 1], outputRange: [251, 0]})} />`,
        ANIMATED,
      ),
    ).not.toThrow();
    expect(() =>
      compile(
        '<Rect width={50} height={50} /><Line x1={0} y1={0} x2={9} y2={9} />',
        ANIMATED,
        '<Svg width={100} height={100} stroke="#fff" strokeDasharray="4" strokeDashoffset={offset}>',
      ),
    ).not.toThrow();
  });

  it('refuses a second animated offset in one <Svg>, and a colour one', () => {
    expect(() =>
      compile(
        `<Line x1={0} y1={0} x2={9} y2={9} stroke="#fff" strokeDasharray="4" strokeDashoffset={offset} />
        <Line x1={0} y1={5} x2={9} y2={5} stroke="#fff" strokeDasharray="4" strokeDashoffset={other} />`,
        `${ANIMATED} const other = useAnimatedValue(0);`,
      ),
    ).toThrow(/an <Svg> takes one animated strokeDashoffset/);
    expect(() =>
      compile(
        `<Line x1={0} y1={0} x2={9} y2={9} stroke="#fff" strokeDasharray="4"
          strokeDashoffset={offset.interpolate({inputRange: [0, 1], outputRange: ['#000', '#fff']})} />`,
        ANIMATED,
      ),
    ).toThrow(/an animated strokeDashoffset takes an outputRange of numbers/);
  });

  it('scales the pattern with a viewBox sized at boot', () => {
    expect(() =>
      compile(
        '<Circle cx={50} cy={50} r={40} fill="none" stroke="#fff" strokeDasharray="6 3" strokeDashoffset={offset} />',
        ANIMATED,
        '<Svg viewBox="0 0 100 100" width={screen.width} height={screen.width}>',
      ),
    ).not.toThrow();
  });

  it('takes a state-driven offset, and an animated one, in a state-driven <Svg>', () => {
    const {program} = compile(
      `<Circle cx={50} cy={50} r={40} fill="none" stroke="#fff" strokeDasharray="6 3" strokeDashoffset={n} />
      <Line x1={0} y1={0} x2={n} y2={9} stroke="#fff" strokeDasharray="5" strokeDashoffset={offset} />
      <Line x1={0} y1={0} x2={n} y2={9} stroke="#fff" strokeDasharray="5" strokeDashoffset={1.5} />`,
      `const [n, setN] = useState(5); ${ANIMATED}`,
    );
    expect(holds(program, [DASH, 6, 3, DASH_OFFSET])).toBe(true);
    expect(holds(program, [DASH, 5, 5, DASH_OFFSET, 0, 1])).toBe(true);
    expect(holds(program, [DASH, 5, 5, DASH_OFFSET, 1.5, 0])).toBe(true);
  });

  it('refuses a state-driven pattern, and one longer than the engine keeps', () => {
    expect(() =>
      compile(
        '<Line x1={0} y1={0} x2={9} y2={9} stroke="#fff" strokeDasharray={[n, 2]} />',
        'const [n, setN] = useState(5);',
      ),
    ).toThrow(/a state-driven strokeDasharray is not supported/);
    const long = `{[${new Array(18).fill(1).join(', ')}]}`;
    expect(() =>
      compile(
        `<Line x1={0} y1={0} x2={9} y2={9} stroke="#fff" strokeDasharray=${long} />`,
      ),
    ).toThrow(/a strokeDasharray of 9 dashes is more than the engine keeps/);
    expect(() =>
      compile(
        `<Line x1={0} y1={0} x2={n} y2={9} stroke="#fff" strokeDasharray=${long} />`,
        'const [n, setN] = useState(5);',
      ),
    ).toThrow(/more than the engine keeps/);
  });
});
