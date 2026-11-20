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

import {describe, it, expect, vi} from 'vitest';
import {
  flattenSvg,
  packStops,
  polyPathD,
  GRAD_LINEAR,
  GRAD_RADIAL,
  PAINT_STRIDE,
} from '../svg-ops.mts';

const el = (type: string, props: Record<string, unknown>) => ({type, props});

const RED = 0xffff0000;
const BLUE = 0xff0000ff;
const STOPS = [
  el('Stop', {offset: 0, stopColor: '#ff0000'}),
  el('Stop', {offset: 1, stopColor: '#0000ff'}),
];

/** One paint record of a tape, by index. */
const paintAt = (paints: number[], index: number): number[] =>
  paints.slice(index * PAINT_STRIDE, (index + 1) * PAINT_STRIDE);

const svg = (...children: unknown[]) =>
  flattenSvg({width: 100, height: 100, children} as never);

describe('<Polygon> and <Polyline>', () => {
  it('draws a closed and an open path through the points', () => {
    expect(polyPathD('0,0 10,0 5,8', true)).toBe('M0 0L10 0L5 8Z');
    expect(polyPathD([0, 0, 10, 0, 5, 8], false)).toBe('M0 0L10 0L5 8');
    expect(polyPathD('0 0', true)).toBe('');
    const {ops} = svg(
      el('Polygon', {points: '0,0 10,0 5,8'}),
      el('Polyline', {points: [0, 0, 10, 0], fill: 'none', stroke: 'red'}),
    );
    expect(ops).toEqual([
      0, 0, 1, 0, 0, 2, 10, 0, 2, 5, 8, 6, 0, 1, 1, 0, 0, 2, 10, 0,
    ]);
  });
});

describe('gradients by reference', () => {
  it('places a <LinearGradient> across the shape it fills, from inside a <Defs>', () => {
    const {paints, gradients} = svg(
      el('Defs', {children: el('LinearGradient', {id: 'g', children: STOPS})}),
      el('Rect', {x: 10, y: 20, width: 40, height: 10, fill: 'url(#g)'}),
    );
    // The ramp's middle, drawn only by a board that drops the gradient.
    expect(paintAt(paints, 0)[0]).toBe(0xff800080);
    expect(paintAt(paints, 0)[7]).toBe(1);
    expect(gradients).toEqual([
      {
        type: GRAD_LINEAR,
        stops: [
          {color: RED, offset: 0},
          {color: BLUE, offset: 1},
        ],
        ax: 10,
        ay: 20,
        bx: 50,
        by: 20,
        r: 0,
      },
    ]);
  });

  it('takes percentages, user space, and the <Svg>s viewBox scale', () => {
    const {gradients} = flattenSvg({
      width: 200,
      height: 200,
      viewBox: '0 0 100 100',
      children: [
        el('LinearGradient', {
          id: 'g',
          x1: '0%',
          y1: '50%',
          x2: 30,
          y2: 40,
          gradientUnits: 'userSpaceOnUse',
          children: STOPS,
        }),
        el('Rect', {width: 50, height: 50, stroke: 'url(#g)'}),
      ],
    } as never);
    expect(gradients[0]).toMatchObject({ax: 0, ay: 1, bx: 60, by: 80});
  });

  it('places a <RadialGradient> by the box, scaled with a <G>', () => {
    const {gradients} = svg(
      el('RadialGradient', {id: 'r', cx: 0.25, children: STOPS}),
      el('G', {
        x: 10,
        scale: 2,
        children: el('Rect', {width: 20, height: 20, fill: 'url(#r)'}),
      }),
    );
    expect(gradients[0]).toMatchObject({
      type: GRAD_RADIAL,
      ax: 20,
      ay: 20,
      r: 20,
    });
  });

  it('carries a faded middle color beside a gradient, for a board without radial gradients', () => {
    const {paints} = svg(
      el('RadialGradient', {
        id: 'r',
        children: [
          el('Stop', {offset: 0, stopColor: '#ffffff'}),
          el('Stop', {offset: 0.5, stopColor: '#3060d0'}),
          el('Stop', {offset: 1, stopColor: '#102040'}),
        ],
      }),
      el('Circle', {cx: 50, cy: 50, r: 40, fill: 'url(#r)', fillOpacity: 0.5}),
      el('Rect', {
        width: 9,
        height: 9,
        fill: 'url(#r)',
        fillGradient: {
          type: 1,
          stops: [
            {color: '#ff0000', offset: 0},
            {color: '#0000ff', offset: 1},
          ],
        },
      }),
    );
    expect(paintAt(paints, 0)[0]).toBe(0x803060d0);
    expect(paintAt(paints, 1)[0]).toBe(0xff800080);
  });

  it('paints one stop as its color, and none, or a missing gradient, as nothing', () => {
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => {});
    const {paints, gradients} = svg(
      el('LinearGradient', {id: 'one', children: [STOPS[1]]}),
      el('LinearGradient', {id: 'none', children: []}),
      el('Rect', {width: 9, height: 9, fill: 'url(#one)'}),
      el('Rect', {width: 9, height: 9, fill: 'url(#none)'}),
      el('Rect', {width: 9, height: 9, fill: 'url(#missing)'}),
    );
    expect(gradients).toEqual([]);
    expect([0, 1, 2].map(index => paintAt(paints, index)[0])).toEqual([
      BLUE,
      0,
      0,
    ]);
    expect(warn).toHaveBeenCalledWith(
      expect.stringContaining('"url(#missing)" names no <LinearGradient>'),
    );
    warn.mockRestore();
  });

  it('keeps stop offsets in order, and fades a stop by its opacity', () => {
    expect(
      packStops([
        {color: '#ff0000', offset: '50%', opacity: undefined},
        {color: '#0000ff', offset: 0.2, opacity: 0.5},
        {color: undefined, offset: 2, opacity: undefined},
      ]),
    ).toEqual([
      {color: RED, offset: 0.5},
      {color: 0x800000ff, offset: 0.5},
      {color: 0xff000000, offset: 1},
    ]);
  });
});

describe('opacity', () => {
  it('fades the fill and stroke, gradients included', () => {
    const {paints, gradients} = svg(
      el('LinearGradient', {id: 'g', children: STOPS}),
      el('Rect', {
        width: 9,
        height: 9,
        fill: '#ff0000',
        fillOpacity: 0.5,
        stroke: 'url(#g)',
        strokeOpacity: '0.5',
      }),
    );
    expect(paintAt(paints, 0)[0]).toBe(0x80ff0000);
    expect(gradients[0].stops.map(stop => stop.color)).toEqual([
      0x80ff0000, 0x800000ff,
    ]);
  });

  it('multiplies an opacity into the one around it, from the <Svg> down', () => {
    const {paints} = flattenSvg({
      width: 100,
      height: 100,
      opacity: 0.5,
      children: el('G', {
        opacity: 0.5,
        children: el('Rect', {
          width: 9,
          height: 9,
          fill: '#ff0000',
          stroke: '#0000ff',
          opacity: 0.5,
          fillOpacity: 0.5,
        }),
      }),
    } as never);
    expect(paintAt(paints, 0)[0]).toBe(0x10ff0000);
    expect(paintAt(paints, 0)[1]).toBe(0x200000ff);
  });
});
