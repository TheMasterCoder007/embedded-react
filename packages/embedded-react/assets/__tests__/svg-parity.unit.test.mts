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

// The same drawing written inline and as a file bakes to the same tape.
import {describe, it, expect} from 'vitest';
import {svgToVector} from '../bake-svg.mts';
import {flattenSvg} from '../../src/embedded-react/svg-ops.mts';
import type {PackedGradient} from '../../src/embedded-react/svg-ops.mts';

const el = (type: string, props: Record<string, unknown>) => ({type, props});

const stops = (opacity = 1) => [
  el('Stop', {offset: '10%', stopColor: '#e04030'}),
  el('Stop', {offset: 0.9, stopColor: '#3060d0', stopOpacity: opacity}),
];
const FILE_STOPS = (opacity = 1) =>
  '<stop offset="10%" stop-color="#e04030"/>' +
  `<stop offset="0.9" stop-color="#3060d0" stop-opacity="${opacity}"/>`;

/** Each number rounded, so float noise between the two bakers does not count. */
const rounded = (values: readonly number[]): number[] =>
  values.map(value => Math.round(value * 1e4) / 1e4);
const roundedGradients = (gradients: readonly PackedGradient[]) =>
  gradients.map(gradient => ({
    ...gradient,
    ax: rounded([gradient.ax ?? 0])[0],
    ay: rounded([gradient.ay ?? 0])[0],
    bx: rounded([gradient.bx ?? 0])[0],
    by: rounded([gradient.by ?? 0])[0],
    r: rounded([gradient.r ?? 0])[0],
  }));

describe('an inline <Svg> and a file', () => {
  it('bake polygons, polylines, gradients by reference and opacity alike', async () => {
    const inline = flattenSvg({
      width: 200,
      height: 160,
      viewBox: '0 0 100 80',
      children: [
        el('Defs', {
          children: [
            el('LinearGradient', {
              id: 'across',
              x1: 0,
              y1: 0,
              x2: 1,
              y2: '100%',
              children: stops(),
            }),
            el('LinearGradient', {
              id: 'user',
              x1: 10,
              y1: 0,
              x2: 90,
              y2: 0,
              gradientUnits: 'userSpaceOnUse',
              children: stops(0.5),
            }),
            el('RadialGradient', {
              id: 'glow',
              cx: '30%',
              r: 0.4,
              children: stops(),
            }),
            el('LinearGradient', {
              id: 'solid',
              children: [el('Stop', {stopColor: '#30a060'})],
            }),
          ],
        }),
        el('Polygon', {
          points: '10,10 40,10 25,35',
          fill: 'url(#across)',
          fillRule: 'evenodd',
        }),
        el('Polyline', {
          points: [50, 10, 70, 30, 90, 10],
          fill: 'none',
          stroke: 'url(#user)',
          strokeWidth: 3,
          strokeLinejoin: 'round',
          strokeMiterlimit: 8,
        }),
        el('G', {
          x: 10,
          y: 40,
          opacity: 0.5,
          children: [
            el('Ellipse', {
              cx: 20,
              cy: 15,
              rx: 18,
              ry: 12,
              fill: 'url(#glow)',
              opacity: 0.5,
            }),
            el('Rect', {
              x: 45,
              y: 5,
              width: 30,
              height: 20,
              fill: 'url(#solid)',
              stroke: '#202020',
              strokeOpacity: 0.25,
            }),
          ],
        }),
      ],
    } as never);

    const file = await svgToVector(
      `<svg xmlns="http://www.w3.org/2000/svg" width="200" height="160" viewBox="0 0 100 80">
        <defs>
          <linearGradient id="across" x1="0" y1="0" x2="1" y2="100%">${FILE_STOPS()}</linearGradient>
          <linearGradient id="user" x1="10" y1="0" x2="90" y2="0" gradientUnits="userSpaceOnUse">${FILE_STOPS(0.5)}</linearGradient>
          <radialGradient id="glow" cx="30%" r="0.4">${FILE_STOPS()}</radialGradient>
          <linearGradient id="solid"><stop stop-color="#30a060"/></linearGradient>
        </defs>
        <polygon points="10,10 40,10 25,35" fill="url(#across)" fill-rule="evenodd"/>
        <polyline points="50 10 70 30 90 10" fill="none" stroke="url(#user)" stroke-width="3"
          stroke-linejoin="round" stroke-miterlimit="8"/>
        <g transform="translate(10 40)" opacity="0.5">
          <ellipse cx="20" cy="15" rx="18" ry="12" fill="url(#glow)" opacity="0.5"/>
          <rect x="45" y="5" width="30" height="20" fill="url(#solid)" stroke="#202020" stroke-opacity="0.25"/>
        </g>
      </svg>`,
    );

    expect(file.dropped).toEqual([]);
    expect(rounded(inline.ops)).toEqual(rounded(file.ops));
    expect(rounded(inline.paints)).toEqual(rounded(file.paints));
    expect(roundedGradients(inline.gradients)).toEqual(
      roundedGradients(file.gradients),
    );
    // The faded ellipse and the one-stop rect: 0.25 and 0.5 of the way to opaque.
    expect(inline.gradients[2].stops[0].color >>> 24).toBe(0x40);
    expect(inline.paints[3 * 9] >>> 24).toBe(0x80);
  });
});
