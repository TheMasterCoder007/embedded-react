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
import {Svg, Rect, Circle, Polygon, Polyline, Defs, LinearGradient, RadialGradient, Stop} from 'embedded-react';
export function App() {
  ${hooks}
  return (
    <Svg width={100} height={100}>
      ${children}
    </Svg>
  );
}`;

const compile = (children: string, hooks = '') =>
  compilePackage(app(children, hooks), 'App.tsx', {
    screen: {width: 240, height: 320},
    release: true,
  });

/** Whether a program holds these floats in a row. */
const holds = (program: Uint8Array, floats: number[]): boolean =>
  Buffer.from(program).includes(Buffer.from(new Float32Array(floats).buffer));

const STATE = 'const [on, setOn] = useState(false);';
const STOPS =
  '<Stop offset={0} stopColor="#ff0000" /><Stop offset="100%" stopColor="#0000ff" />';
const LINEAR = `<Defs><LinearGradient id="g">${STOPS}</LinearGradient></Defs>`;

describe('the new <Svg> elements in Flow B', () => {
  it('draws polygons and polylines, static and state-driven', () => {
    const tape = [1, 0, 0, 2, 9, 0, 2, 5, 8, 6];
    expect(
      holds(compile('<Polygon points="0,0 9,0 5,8" />').program, tape),
    ).toBe(true);
    expect(
      holds(
        compile(
          '<Polygon points={[0, 0, 9, 0, 5, 8]} fill={on ? "#fff" : "#000"} />',
          STATE,
        ).program,
        tape,
      ),
    ).toBe(true);
    expect(() =>
      compile(
        '<Polyline points={on ? "0,0 9,9" : "0,0 9,0"} fill="none" stroke="#fff" />',
        STATE,
      ),
    ).toThrow(/a state-driven <Polyline points=…> is not supported/);
  });

  it('places a gradient by reference across the shape, static and state-driven', () => {
    const rect =
      '<Rect x={10} y={20} width={40} height={10} fill="url(#g)" stroke={on ? "#fff" : "#000"} />';
    const placed = [10, 20, 50, 20];
    expect(
      holds(
        compile(LINEAR + rect.replace(/ stroke=\{.*\}/, '')).program,
        placed,
      ),
    ).toBe(true);
    expect(holds(compile(LINEAR + rect, STATE).program, placed)).toBe(true);
    expect(() =>
      compile(
        `<RadialGradient id="r" gradientUnits="userSpaceOnUse" cx={50} cy={50} r={40}>${STOPS}</RadialGradient>
        <Circle cx={50} cy={50} r={on ? 40 : 20} fill="url(#r)" fillOpacity={0.5} />`,
        STATE,
      ),
    ).not.toThrow();
  });

  it('refuses a gradient across a shape state moves', () => {
    expect(() =>
      compile(
        `${LINEAR}<Circle cx={50} cy={50} r={on ? 40 : 20} fill="url(#g)" />`,
        STATE,
      ),
    ).toThrow(/a gradient across a state-driven shape's box is not supported/);
  });

  it('refuses a fill naming no gradient, and stops out of place', () => {
    expect(() =>
      compile('<Rect width={9} height={9} fill="url(#g)" />'),
    ).toThrow(/fill="url\(#g\)" names no <LinearGradient> or <RadialGradient>/);
    expect(() =>
      compile(
        '<Rect width={9} height={9} fill="url(#g)" stroke={on ? "#fff" : "#000"} />',
        STATE,
      ),
    ).toThrow(/fill="url\(#g\)" names no <LinearGradient>/);
    expect(() => compile(`<Defs>${STOPS}</Defs>`)).toThrow(
      /a <Stop> goes inside a <LinearGradient> or <RadialGradient>/,
    );
    expect(() =>
      compile(
        '<LinearGradient id="g"><Rect width={9} height={9} /></LinearGradient>',
      ),
    ).toThrow(/a <LinearGradient> holds only <Stop>s, not <Rect>/);
    expect(() =>
      compile(
        `<LinearGradient id="g">${'<Stop offset={0} stopColor="#fff" />'.repeat(9)}</LinearGradient>`,
      ),
    ).toThrow(
      /a <LinearGradient> of 9 stops is more than the engine keeps \(8\)/,
    );
  });

  it('fades by an opacity, and refuses a state-driven one', () => {
    expect(() =>
      compile(
        '<Rect width={9} height={9} fill={on ? "#fff" : "#000"} fillOpacity={0.5} opacity={0.5} />',
        STATE,
      ),
    ).not.toThrow();
    expect(() =>
      compile(
        '<Rect width={9} height={9} fill="#fff" opacity={on ? 1 : 0.5} />',
        STATE,
      ),
    ).toThrow(/a state-driven <Svg> "opacity" is not supported/);
  });
});
