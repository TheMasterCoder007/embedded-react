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

import {readFileSync} from 'node:fs';
import {dirname, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';
import {describe, expect, it, vi} from 'vitest';
import {bakeSvgArtifacts, compilePackage} from '../compile.mts';
import {checkOverSizes, runCheck} from '../compiler/package/checks.mts';
import {CheckOp, MathFn} from '../compiler/package/format.mts';
import {SizeSearch} from '../compiler/package/layouts.mts';

const demosDir = resolve(
  dirname(fileURLToPath(import.meta.url)),
  '../../../../demos',
);

const IMPORTS = `import {useState, useRef} from 'react';
import {View, Text, Pressable, Svg, Circle} from 'embedded-react';
`;

/** An app whose tree changes with `expression`. */
const branching = expression => `${IMPORTS}
const PICK = ${expression};
export function App() {
  return PICK ? <View><Text>a</Text><Text>b</Text></View> : <Text>a</Text>;
}`;

const checksOf = result => result.panels.map(panel => panel.check);
const sizesOf = result => result.panels.map(p => `${p.width}x${p.height}`);

/** A repeatable random number below `limit`. */
function randomSource(seed = 12345) {
  let state = seed;
  return limit => {
    state = (state * 1664525 + 1013904223) >>> 0;
    return state % limit;
  };
}

describe('layouts of an app that names no sizes', () => {
  it('has one layout with no check when nothing depends on the panel', () => {
    const result = compilePackage(`${IMPORTS}export function App() {
  return <Text>a</Text>;
}`);
    expect(checksOf(result).map(check => check.steps)).toEqual([null]);
  });

  it('has one layout for a breakpoint that only changes values', () => {
    const result = compilePackage(`${IMPORTS}
const WIDE = screen.width >= 480;
export function App() {
  return <View style={{width: WIDE ? 200 : 100, padding: screen.height / 10}} />;
}`);
    expect(result.panels).toHaveLength(1);
  });

  it("compiles the issue's example, picking at boot from any panel", () => {
    const result = compilePackage(`${IMPORTS}
const WIDE = screen.width >= 480;
function TwoColumns() {
  return <View style={{flexDirection: 'row'}}><Text>a</Text><Text>b</Text></View>;
}
function OneColumn() {
  return <Text>a</Text>;
}
export function App() {
  return WIDE ? <TwoColumns /> : <OneColumn />;
}`);
    expect(result.panels).toHaveLength(2);
    const [narrow, wide] = checksOf(result);
    expect(runCheck(narrow, 479, 1)).toBe(true);
    expect(runCheck(narrow, 480, 1)).toBe(false);
    expect(runCheck(wide, 480, 32767)).toBe(true);
    expect(runCheck(wide, 479, 32767)).toBe(false);
  });

  it('leaves no size without a layout', () => {
    const result = compilePackage(
      branching(
        'Math.min(screen.width, screen.height) >= 600 || screen.width > screen.height * 2',
      ),
    );
    const checks = checksOf(result);
    const random = randomSource();
    for (let index = 0; index < 20000; index++) {
      const width = 1 + random(32767);
      const height = 1 + random(32767);
      expect(checks.some(check => runCheck(check, width, height))).toBe(true);
    }
    expect(new SizeSearch().next(checks)).toBeNull();
  });

  it('takes Math.min and Math.max of more than two values', () => {
    const result = compilePackage(
      branching('Math.min(screen.width, screen.height, 400) > 300'),
    );
    const checks = checksOf(result);
    expect(checks).toHaveLength(2);
    expect(checks.every(check => !check.exactBecause)).toBe(true);

    const random = randomSource();
    for (let index = 0; index < 2000; index++) {
      const width = 1 + random(1000);
      const height = 1 + random(1000);
      const picked = checks.findIndex(check => runCheck(check, width, height));
      expect(picked).toBe(Math.min(width, height, 400) > 300 ? 1 : 0);
    }
  });

  it('proves a breakpoint at the exact pixel, across a diagonal too', () => {
    const result = compilePackage(
      branching('screen.width > screen.height && screen.width % 2 === 0'),
    );
    expect(result.panels).toHaveLength(2);
  }, 60000);

  it('keeps the bytes the same at any size in a layout', () => {
    const src = `${IMPORTS}
const WIDE = screen.width >= 480;
export function App() {
  const [half] = useState(screen.width / 2);
  return (
    <View style={{width: screen.width, height: screen.height / 2}}>
      <Svg width={screen.width} height={40} viewBox="0 0 100 40">
        <Circle cx={50} cy={20} r={10} fill="#ffffff" />
      </Svg>
      {WIDE ? <Text>{half}</Text> : <View />}
    </View>
  );
}`;
    const at = (width, height) =>
      compilePackage(src, 'app', {screen: {width, height}}).program;
    expect(at(240, 320)).toEqual(at(301, 77));
    expect(at(800, 480)).toEqual(at(1001, 77));
    expect(at(240, 320)).not.toEqual(at(800, 480));
  });

  it('fails the build naming the expression past the cap', () => {
    expect(() =>
      compilePackage(`${IMPORTS}
export function App() {
  return (
    <View>
      {screen.width > 100 && <View />}
      {screen.width > 200 && <View />}
      {screen.width > 300 && <View />}
      {screen.width > 400 && <View />}
      {screen.width > 500 && <View />}
      {screen.height > 100 && <View />}
      {screen.height > 200 && <View />}
      {screen.height > 300 && <View />}
    </View>
  );
}`),
    ).toThrow(
      /AOT: this app needs more than 16 layouts, because `screen\.width > \d+`/,
    );
  });

  it('fails the build naming a value only the exact size can give', () => {
    expect(() =>
      compilePackage(`${IMPORTS}
export function App() {
  const [label] = useState('w' + screen.width);
  return <Text>{label}</Text>;
}`),
    ).toThrow(/more than 16 layouts, because `'w' \+ screen\.width`/);
  });

  it('names a size that picks a layout that does not compile', () => {
    expect(() =>
      compilePackage(`${IMPORTS}
export function App() {
  if (screen.width >= 700) return <Nope />;
  return <View />;
}`),
    ).toThrow(/this is the layout a 800×480 panel picks/);
  });

  it('keeps only the layouts the named sizes pick', () => {
    const src = branching('screen.width >= 480');
    expect(sizesOf(compilePackage(src))).toEqual(['240x320', '480x320']);
    expect(
      sizesOf(compilePackage(src, 'app', {screen: {width: 800, height: 480}})),
    ).toEqual(['800x480']);
    expect(
      sizesOf(
        compilePackage(src, 'app', {
          screens: [
            {width: 240, height: 320},
            {width: 200, height: 100},
          ],
        }),
      ),
    ).toEqual(['240x320']);
  });

  it('takes ER_AOT_SCREEN_W/H as a named size', () => {
    vi.stubEnv('ER_AOT_SCREEN_W', '800');
    vi.stubEnv('ER_AOT_SCREEN_H', '480');
    try {
      expect(sizesOf(compilePackage(branching('screen.width >= 480')))).toEqual(
        ['800x480'],
      );
    } finally {
      vi.unstubAllEnvs();
    }
  });

  it.each([
    ['thermostat', ['240x320', '320x240', '480x800']],
    ['watch-face', ['240x320']],
  ])('compiles the %s at every layout', async (demo, layouts) => {
    const dir = resolve(demosDir, demo);
    const src = readFileSync(resolve(dir, 'App.jsx'), 'utf8');
    const result = compilePackage(src, demo, {
      filename: resolve(dir, 'App.jsx'),
      svgArtifacts: await bakeSvgArtifacts(src, dir),
    });
    expect(sizesOf(result)).toEqual(layouts);
    // The S3, the CYD, the RP2040 and a size nothing names each have a layout.
    for (const [width, height] of [
      [800, 480],
      [240, 320],
      [240, 280],
      [277, 391],
    ]) {
      expect(
        checksOf(result).some(check => runCheck(check, width, height)),
      ).toBe(true);
    }
  });
});

describe('a layout check over a range of sizes', () => {
  const EXPRESSIONS = [
    'screen.width >= 480',
    'screen.width > screen.height',
    'screen.width * screen.height > 100000',
    'screen.width + screen.height < 1000 && screen.width !== 320',
    'Math.min(screen.width, screen.height) >= 240',
    'Math.max(screen.width - 100, screen.height) <= 500',
    'Math.min(screen.width, screen.height, 400) > 300',
    'Math.max(screen.width / 2, screen.height / 3, 150.5, 20) > 200',
    'Math.floor(screen.width / 3) === 80',
    'Math.round(screen.width / 7) > 40 || !(screen.height < 100)',
    'Math.ceil(screen.height / 9) % 2 === 0',
    'Math.abs(screen.width - screen.height) < 50',
    'screen.width % 16 === 0',
    '(screen.width > 400 ? screen.height : screen.width) > 300',
    "(screen.width > 400 ? 'big' : 'small') === 'big'",
    'screen.width / screen.height > 1.5',
    '(screen.width - 240) / (screen.height - 320) > 0',
  ];

  it.each(EXPRESSIONS)('never contradicts the exact check: %s', expression => {
    const {panels} = compilePackage(branching(expression), 'app', {
      screen: {width: 240, height: 320},
    });
    const {check} = panels[0];
    expect(check.steps).not.toBeNull();

    const random = randomSource();
    for (let round = 0; round < 300; round++) {
      const span = [1, 2, 8, 64, 512, 4096][random(6)];
      const minWidth = 1 + random(2000);
      const minHeight = 1 + random(2000);
      const sizes = {
        minWidth,
        maxWidth: minWidth + random(span),
        minHeight,
        maxHeight: minHeight + random(span),
      };
      const verdict = checkOverSizes(check, sizes);
      if (verdict === 'unsure') continue;
      for (let sample = 0; sample < 30; sample++) {
        const width =
          sizes.minWidth + random(sizes.maxWidth - sizes.minWidth + 1);
        const height =
          sizes.minHeight + random(sizes.maxHeight - sizes.minHeight + 1);
        expect(runCheck(check, width, height)).toBe(verdict === 'pass');
      }
    }
  });

  // Math functions a fold doesn't take yet, and the corners of the VM's division, as the check would run them.
  const step = (op, arg = 0) => ({op, arg});
  const DIRECT = {
    sqrt: [step(CheckOp.width), step(CheckOp.math, MathFn.sqrt)],
    pow: [
      step(CheckOp.width),
      step(CheckOp.pushi, 2),
      step(CheckOp.math, MathFn.pow),
    ],
    sin: [step(CheckOp.height), step(CheckOp.math, MathFn.sin)],
    tan: [step(CheckOp.width), step(CheckOp.math, MathFn.tan)],
    atan2: [
      step(CheckOp.width),
      step(CheckOp.height),
      step(CheckOp.math, MathFn.atan2),
    ],
    'div by a difference': [
      step(CheckOp.width),
      step(CheckOp.width),
      step(CheckOp.height),
      step(CheckOp.sub),
      step(CheckOp.div),
    ],
    'whole division by a difference': [
      step(CheckOp.width),
      step(CheckOp.width),
      step(CheckOp.height),
      step(CheckOp.sub),
      step(CheckOp.idiv, 1),
    ],
    'remainder of a difference': [
      step(CheckOp.width),
      step(CheckOp.height),
      step(CheckOp.sub),
      step(CheckOp.pushi, 7),
      step(CheckOp.idiv, 4),
    ],
    'float remainder': [
      step(CheckOp.width),
      step(CheckOp.pushk, 2.5),
      step(CheckOp.math, MathFn.mod),
    ],
    'not a number': [
      step(CheckOp.width),
      step(CheckOp.pushi, 300),
      step(CheckOp.sub),
      step(CheckOp.math, MathFn.sqrt),
      step(CheckOp.pushi, 5),
      step(CheckOp.ne),
    ],
  };

  it.each(Object.keys(DIRECT))(
    'never contradicts the exact check: %s',
    name => {
      const compare = [
        step(CheckOp.pushi, 10),
        step(CheckOp.gt),
        step(CheckOp.end),
      ];
      const steps =
        name === 'not a number'
          ? [...DIRECT[name], step(CheckOp.end)]
          : [...DIRECT[name], ...compare];
      const check = {
        steps,
        reads: {width: true, height: true},
        exactBecause: null,
      };
      const random = randomSource(7);
      for (let round = 0; round < 500; round++) {
        const span = [1, 2, 8, 64, 512, 4096][random(6)];
        const minWidth = 1 + random(1000);
        const minHeight = 1 + random(1000);
        const maxWidth = minWidth + random(span);
        const maxHeight = minHeight + random(span);
        const verdict = checkOverSizes(check, {
          minWidth,
          maxWidth,
          minHeight,
          maxHeight,
        });
        if (verdict === 'unsure') continue;
        for (let sample = 0; sample < 30; sample++) {
          const width = minWidth + random(maxWidth - minWidth + 1);
          const height = minHeight + random(maxHeight - minHeight + 1);
          expect(runCheck(check, width, height)).toBe(verdict === 'pass');
        }
      }
    },
  );

  it.each(EXPRESSIONS)(
    'agrees with the compile at each size: %s',
    expression => {
      const at = (width, height) =>
        compilePackage(branching(expression), 'app', {
          screen: {width, height},
        }).panels[0];
      const random = randomSource(99);
      const base = at(240, 320);
      for (let round = 0; round < 12; round++) {
        const width = 1 + random(1500);
        const height = 1 + random(1500);
        const other = at(width, height);
        const sameLayout =
          JSON.stringify(other.check.steps) ===
          JSON.stringify(base.check.steps);
        expect(runCheck(other.check, width, height)).toBe(true);
        // A layout the base check takes is the base's own layout.
        if (runCheck(base.check, width, height)) expect(sameLayout).toBe(true);
      }
    },
  );
});

describe('values worked out at boot', () => {
  it("doesn't take a layout per value for a state's initial value", () => {
    const result = compilePackage(`${IMPORTS}
export function App() {
  const [half, setHalf] = useState(screen.width / 2);
  const [wide] = useState(screen.width > 400);
  return (
    <Pressable onPress={() => setHalf(half + 1)}>
      <Text>{half}{wide ? 'y' : 'n'}</Text>
    </Pressable>
  );
}`);
    expect(result.panels).toHaveLength(1);
  });

  it('types the state by its expression, not the size it was compiled at', () => {
    const src = `${IMPORTS}
export function App() {
  const [half] = useState(screen.width / 2);
  return <Text>{half}</Text>;
}`;
    const at = width =>
      compilePackage(src, 'app', {screen: {width, height: 320}}).program;
    expect(at(240)).toEqual(at(241));
  });

  it("doesn't take a layout per size for a viewBox drawing sized from the screen", () => {
    const result = compilePackage(`${IMPORTS}
export function App() {
  return (
    <Svg width={screen.width} height={screen.width / 2} viewBox="0 0 100 50">
      <Circle cx={50} cy={25} r={20} fill="#ffffff" stroke="#ff0000" strokeWidth={2} />
    </Svg>
  );
}`);
    expect(result.panels).toHaveLength(1);
  });

  it('fails the build naming a shape calculated from the screen', () => {
    expect(() =>
      compilePackage(`${IMPORTS}
const R = screen.width / 4;
export function App() {
  return <Svg width={100} height={100}><Circle cx={50} cy={50} r={R} fill="#fff" /></Svg>;
}`),
    ).toThrow(/an <Svg> shape can't be calculated from `screen`: R/);
    expect(() =>
      compilePackage(`${IMPORTS}
export function App() {
  const [x] = useState(1);
  return <Svg width={100} height={100}><Circle cx={x} cy={50} r={screen.height / 2} fill="#fff" /></Svg>;
}`),
    ).toThrow(
      /an <Svg> shape can't be calculated from `screen`: screen\.height \/ 2/,
    );
  });
});
