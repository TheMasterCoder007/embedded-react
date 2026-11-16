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
import {parseBackgroundImage} from '../background-image.mts';
import {buildProps} from '../../props.js';

const gradientOf = (value: unknown) => {
  const {gradient, problem} = parseBackgroundImage(value);
  expect(problem).toBeUndefined();
  return gradient!;
};

const problemOf = (value: unknown) => parseBackgroundImage(value).problem;

describe('parseBackgroundImage: strings', () => {
  it('defaults to top to bottom, the ends at 0 and 1', () => {
    expect(gradientOf('linear-gradient(red, blue)')).toEqual({
      type: 'linear',
      angle: 0,
      corner: 0,
      stops: [
        {color: 'red', offset: 0},
        {color: 'blue', offset: 1},
      ],
    });
  });

  it('turns a CSS angle into the engine angle', () => {
    expect(gradientOf('linear-gradient(0deg, red, blue)').angle).toBe(180);
    expect(gradientOf('linear-gradient(90deg, red, blue)').angle).toBe(90);
    expect(gradientOf('linear-gradient(270deg, red, blue)').angle).toBe(270);
    expect(gradientOf('linear-gradient(0.25turn, red, blue)').angle).toBe(90);
    expect(gradientOf('linear-gradient(-90deg, red, blue)').angle).toBe(270);
  });

  it('reads a side as an angle and a corner as a corner', () => {
    expect(gradientOf('linear-gradient(to right, red, blue)')).toMatchObject({
      angle: 90,
      corner: 0,
    });
    expect(gradientOf('linear-gradient(to top, red, blue)').angle).toBe(180);
    expect(
      gradientOf('linear-gradient(to bottom right, red, blue)').corner,
    ).toBe(2);
    expect(gradientOf('linear-gradient(to left top, red, blue)').corner).toBe(
      4,
    );
  });

  it('keeps a color with commas and spaces whole', () => {
    expect(
      gradientOf(
        'linear-gradient(to right, rgba(0, 0, 0, 0.5) 10%, #38bdf8 90%)',
      ).stops,
    ).toEqual([
      {color: 'rgba(0, 0, 0, 0.5)', offset: 0.1},
      {color: '#38bdf8', offset: 0.9},
    ]);
  });

  it('spaces stops without a position evenly between their neighbours', () => {
    expect(
      gradientOf('linear-gradient(red, lime, blue, white 80%)').stops.map(
        stop => stop.offset,
      ),
    ).toEqual([0, 0.8 / 3, 1.6 / 3, 0.8]);
  });

  it('raises a position below an earlier one', () => {
    expect(
      gradientOf('linear-gradient(red 60%, lime 20%, blue)').stops.map(
        stop => stop.offset,
      ),
    ).toEqual([0.6, 0.6, 1]);
  });

  it('paints a color with two positions as two stops', () => {
    expect(gradientOf('linear-gradient(red 20% 40%, blue)').stops).toEqual([
      {color: 'red', offset: 0.2},
      {color: 'red', offset: 0.4},
      {color: 'blue', offset: 1},
    ]);
  });

  it('takes the radial circle the engine draws', () => {
    for (const text of [
      'radial-gradient(circle, red, blue)',
      'radial-gradient(circle farthest-corner at center, red, blue)',
      'RADIAL-GRADIENT(circle at 50% 50%, red, blue)',
    ]) {
      expect(gradientOf(text)).toMatchObject({type: 'radial', corner: 0});
    }
  });

  it('refuses what the engine does not draw', () => {
    expect(problemOf('radial-gradient(red, blue)')).toMatch(/circle/);
    expect(
      problemOf('radial-gradient(circle closest-side, red, blue)'),
    ).toMatch(/circle/);
    expect(problemOf('radial-gradient(circle at top left, red, blue)')).toMatch(
      /circle/,
    );
    expect(
      problemOf('linear-gradient(red, blue), linear-gradient(red, blue)'),
    ).toMatch(/one gradient/);
    expect(problemOf('url(photo.png)')).toMatch(/expected linear-gradient/);
    expect(problemOf('conic-gradient(red, blue)')).toMatch(
      /expected linear-gradient/,
    );
    expect(problemOf('linear-gradient(red)')).toMatch(/2 to 4 stops, got 1/);
    expect(problemOf('linear-gradient(red, lime, blue, white, black)')).toMatch(
      /2 to 4 stops, got 5/,
    );
    expect(problemOf('linear-gradient(red 10px, blue)')).toMatch(/percentage/);
    expect(problemOf('linear-gradient(red, 30%, blue)')).toMatch(
      /not a color stop/,
    );
    expect(problemOf('linear-gradient(red -10%, blue)')).toMatch(/0% to 100%/);
    expect(problemOf('linear-gradient(to middle, red, blue)')).toMatch(
      /not a direction/,
    );
  });
});

describe('parseBackgroundImage: objects', () => {
  it('reads a linear gradient', () => {
    expect(
      gradientOf([
        {
          type: 'linear-gradient',
          direction: 'to bottom right',
          colorStops: [
            {color: '#0f172a'},
            {color: '#38bdf8', positions: ['70%']},
          ],
        },
      ]),
    ).toEqual({
      type: 'linear',
      angle: 0,
      corner: 2,
      stops: [
        {color: '#0f172a', offset: 0},
        {color: '#38bdf8', offset: 0.7},
      ],
    });
  });

  it('reads the same gradient as its string', () => {
    expect(
      gradientOf([
        {
          type: 'linear-gradient',
          direction: '45deg',
          colorStops: [
            {color: 'red', positions: ['20%', '40%']},
            {color: 'blue'},
          ],
        },
      ]),
    ).toEqual(gradientOf('linear-gradient(45deg, red 20% 40%, blue)'));
  });

  it('defaults to top to bottom without a direction', () => {
    expect(
      gradientOf([
        {
          type: 'linear-gradient',
          colorStops: [{color: 'red'}, {color: 'blue'}],
        },
      ]),
    ).toEqual(gradientOf('linear-gradient(red, blue)'));
  });

  it('reads a radial circle', () => {
    expect(
      gradientOf([
        {
          type: 'radial-gradient',
          shape: 'circle',
          colorStops: [{color: 'red'}, {color: 'blue'}],
        },
      ]).type,
    ).toBe('radial');
  });

  it('refuses what the engine does not draw', () => {
    const colorStops = [{color: 'red'}, {color: 'blue'}];
    expect(
      problemOf([
        {type: 'linear-gradient', colorStops},
        {type: 'linear-gradient', colorStops},
      ]),
    ).toMatch(/one gradient/);
    expect(
      problemOf([{type: 'radial-gradient', shape: 'ellipse', colorStops}]),
    ).toMatch(/circle/);
    expect(
      problemOf([
        {
          type: 'radial-gradient',
          shape: 'circle',
          position: {top: '0%', left: '50%'},
          colorStops,
        },
      ]),
    ).toMatch(/circle/);
    expect(problemOf([{type: 'conic-gradient', colorStops}])).toMatch(
      /expected linear-gradient/,
    );
    expect(problemOf([{type: 'radial-gradient', colorStops}])).toMatch(
      /circle/,
    );
    expect(
      problemOf([
        {type: 'linear-gradient', direction: 'to middle', colorStops},
      ]),
    ).toMatch(/not a direction/);
    expect(
      problemOf([
        {
          type: 'linear-gradient',
          colorStops: [
            {color: 'red', positions: ['1%', '2%', '3%']},
            {color: 'blue'},
          ],
        },
      ]),
    ).toMatch(/not a color stop/);
    expect(
      problemOf([
        {
          type: 'linear-gradient',
          colorStops: [{color: 'red', positions: [0.5]}, {color: 'blue'}],
        },
      ]),
    ).toMatch(/percentage/);
    expect(problemOf([{type: 'linear-gradient'}])).toMatch(/colorStops/);
    expect(
      problemOf([
        {type: 'linear-gradient', colorStops: [{color: null}, {color: 'red'}]},
      ]),
    ).toMatch(/`color` string/);
    expect(problemOf(42)).toMatch(/expected a gradient string/);
  });
});

describe('buildProps: backgroundImage', () => {
  it('hands the bridge the gradient', () => {
    const flat = buildProps('View', {
      style: {backgroundImage: 'linear-gradient(to right, red, blue)'},
    });
    expect(flat.backgroundImage).toEqual({
      type: 'linear',
      angle: 90,
      corner: 0,
      stops: [
        {color: 'red', offset: 0},
        {color: 'blue', offset: 1},
      ],
    });
  });

  it('drops a value the engine does not draw, warning once', () => {
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => {});
    const style = {backgroundColor: '#123', backgroundImage: 'url(a.png)'};
    expect(buildProps('View', {style}).backgroundImage).toBeUndefined();
    expect(buildProps('View', {style}).backgroundColor).toBe('#123');
    expect(warn).toHaveBeenCalledTimes(1);
    expect(warn.mock.calls[0][0]).toMatch(/backgroundImage: expected/);
    warn.mockRestore();
  });

  it('leaves a Dial its indicator gradient', () => {
    const flat = buildProps('Dial', {
      style: {backgroundImage: 'linear-gradient(red, blue)'},
    });
    expect(flat.backgroundImage).toBeUndefined();
  });
});
