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

import {describe, it, expect} from 'vitest';
import {
  attemptLowering,
  check,
  checkNumber,
  checkProblem,
  lowerWithChecks,
  numberProblem,
  runChecks,
} from '../compiler/check.mts';
import {lowerApp} from '../compiler/lower.mts';
import {resolveOptions} from '../compiler/options.mts';

// Lowering records the values it can't vouch for; the check pass reports the first bad one, where it was lowered.

const opts = resolveOptions({});

/** The app a source lowers to. */
const lower = source => lowerApp(source, opts, new Map(), []);

/** An app whose component returns `jsx`. */
const app = jsx =>
  "import React, {useState} from 'react';\n" +
  "import {View, Svg, Circle, Animated, useAnimatedValue} from 'embedded-react';\n" +
  `export function App() {\n  const [n, setN] = useState(0);\n  return ${jsx};\n}`;

/** What a call throws. */
function thrown(call) {
  try {
    call();
  } catch (error) {
    return error;
  }
  throw new Error('expected a throw');
}

describe('check pass: numbers', () => {
  it('refuses NaN, Infinity and a number past the float range', () => {
    expect(numberProblem(1.5)).toBeNull();
    expect(numberProblem(0 / 0).message).toBe(
      'AOT: a numeric constant folded to NaN, which has no C form',
    );
    expect(numberProblem(undefined).message).toBe(
      'AOT: a numeric constant folded to undefined, which has no C form',
    );
    expect(numberProblem(1e40).message).toBe(
      'AOT: the numeric constant 1e+40 is past the float range',
    );
  });

  it('checks at once outside a compile', () => {
    expect(() => checkNumber(1 / 0)).toThrow(/folded to Infinity/);
  });

  it('records checks during a compile, and runs them at once inside a fallback attempt', () => {
    const {checks} = lowerWithChecks(() => {
      checkNumber(1 / 0);
      expect(() => attemptLowering(() => checkNumber(0 / 0))).toThrow(
        /folded to NaN/,
      );
    });
    expect(checks).toEqual([{kind: 'number', value: 1 / 0, loc: undefined}]);
    expect(() => runChecks(checks)).toThrow(/folded to Infinity/);
  });

  it('keeps where each value was lowered', () => {
    const lowered = lower(
      app(
        '<Svg width={10} height={10}><Circle cx={5} cy={5} r={2} fill="#0f0" /></Svg>',
      ),
    );
    expect(lowered.checks.length).toBeGreaterThan(0);
    expect(lowered.checks[0]).toMatchObject({
      kind: 'number',
      loc: {line: 5, column: 9},
    });
  });

  it("reports a bad value at its element, ahead of a later element's error", () => {
    const error = thrown(() =>
      lower(
        app(
          '<View><Svg width={10} height={10}><Circle cx={5} cy={5} r={2} stroke="#f00" strokeWidth={1 / 0} /></Svg>' +
            '<View onTouchStart={5} /></View>',
        ),
      ),
    );
    expect(error.message).toBe(
      'AOT: a numeric constant folded to Infinity, which has no C form',
    );
    expect(error.aotLoc).toMatchObject({line: 5, column: 15});
  });

  it("reports an earlier element's error ahead of a later bad value", () => {
    const error = thrown(() =>
      lower(
        app(
          '<View><View onTouchStart={5} />' +
            '<Svg width={10} height={10}><Circle cx={5} cy={5} r={2} stroke="#f00" strokeWidth={1 / 0} /></Svg></View>',
        ),
      ),
    );
    expect(error.message).toMatch(/onTouchStart must be an inline function/);
  });
});

describe('check pass: state-driven drawings', () => {
  it('refuses a gradient value that folds to NaN, Infinity or a non-number, at its <Svg>', () => {
    /** A state-driven circle whose radial gradient's center x is `ax`. */
    const gradientApp = ax =>
      app(
        '<Svg width={10} height={10}><Circle cx={n} cy={5} r={2} fillGrad={{type: 2, ' +
          `ax: ${ax}, ay: 5, r: 3, stops: [{offset: 0, color: '#f00'}, {offset: 1, color: '#00f'}]}} /></Svg>`,
      );
    expect(lower(gradientApp('2.5')).checks).toContainEqual(
      expect.objectContaining({kind: 'number', value: 2.5}),
    );
    for (const [ax, folded] of [
      ['0 / 0', 'NaN'],
      ['1 / 0', 'Infinity'],
      ["'oops'", 'oops'],
    ]) {
      const error = thrown(() => lower(gradientApp(ax)));
      expect(error.message).toBe(
        `AOT: a numeric constant folded to ${folded}, which has no C form`,
      );
      expect(error.aotLoc).toMatchObject({line: 5, column: 9});
    }
  });
});

describe('check pass: fixed caps', () => {
  it('holds each count to the most the engine keeps', () => {
    expect(checkProblem({kind: 'interpolationPoints', count: 8})).toBeNull();
    expect(checkProblem({kind: 'interpolationPoints', count: 9}).message).toBe(
      'AOT: .interpolate() supports up to 8 breakpoints (ER_INTERPOLATE_MAX_POINTS)',
    );
    expect(checkProblem({kind: 'textSpans', count: 4, max: 4})).toBeNull();
    expect(checkProblem({kind: 'textSpans', count: 5, max: 4})).toMatchObject({
      message:
        'AOT: a <Text> has 5 inline segments but the engine renders at most 4',
      aotHint: expect.stringContaining('ER_AOT_MAX_TEXT_SPANS'),
    });
    for (const count of [2, 8]) {
      expect(
        checkProblem({kind: 'gradientStops', attribute: 'fillGrad', count}),
      ).toBeNull();
    }
    for (const count of [1, 9]) {
      expect(
        checkProblem({kind: 'gradientStops', attribute: 'fillGrad', count})
          .message,
      ).toBe(`AOT: "fillGrad.stops" needs 2..8 entries (got ${count})`);
    }
    expect(checkProblem({kind: 'dialGradientStops', count: 2})).toBeNull();
    expect(checkProblem({kind: 'dialGradientStops', count: 1}).message).toBe(
      'AOT: <Dial indicatorGradient> needs at least two stops (max 4)',
    );
  });

  it('records a cap with the rest of the checks during a compile', () => {
    const {checks} = lowerWithChecks(() =>
      check({kind: 'interpolationPoints', count: 9}),
    );
    expect(checks).toEqual([
      {kind: 'interpolationPoints', count: 9, loc: undefined},
    ]);
  });

  it('reports a gradient with too many stops ahead of the stops lowering cannot read', () => {
    const error = thrown(() =>
      lower(
        app(
          '<Svg width={10} height={10}><Circle cx={n} cy={5} r={2} ' +
            'fillGrad={{type: 1, stops: [1, 2, 3, 4, 5, 6, 7, 8, 9]}} /></Svg>',
        ),
      ),
    );
    expect(error.message).toBe(
      'AOT: "fillGrad.stops" needs 2..8 entries (got 9)',
    );
    expect(error.aotLoc).toMatchObject({line: 5, column: 9});
  });

  it("reports an earlier element's error ahead of a later cap", () => {
    const points = '[0, 1, 2, 3, 4, 5, 6, 7, 8]';
    /** An app with an over-long interpolation after `before`. */
    const capApp = before =>
      "import React from 'react';\n" +
      "import {View, Animated, useAnimatedValue} from 'embedded-react';\n" +
      'export function App() {\n  const a = useAnimatedValue(0);\n' +
      `  return <View>${before}<Animated.View style={{opacity: a.interpolate(` +
      `{inputRange: ${points}, outputRange: ${points}})}} /></View>;\n}`;
    expect(() => lower(capApp(''))).toThrow(/up to 8 breakpoints/);
    expect(() => lower(capApp('<View onTouchStart={5} />'))).toThrow(
      /onTouchStart must be an inline function/,
    );
  });
});
