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

const IMPORTS = `import {useState, useMemo} from 'react';
import {View, Text, Pressable, Dimensions, useWindowDimensions} from 'embedded-react';
`;

/** The package an app compiles to. */
const programOf = (body: string): Uint8Array =>
  compilePackage(`${IMPORTS}${body}`).program;

describe('the panel size read the React Native way', () => {
  it('compiles useWindowDimensions() as it does the screen global', () => {
    const app = (size: string) => `
export function App() {
  ${size}
  const half = Math.round(width / 2);
  return <View style={{width: half, height: height - 20}}><Text>{width}</Text></View>;
}`;
    expect(
      programOf(app('const {width, height} = useWindowDimensions();')),
    ).toEqual(
      programOf(app('const width = screen.width, height = screen.height;')),
    );
  });

  it('compiles a module-level Dimensions.get() as it does the screen global', () => {
    const app = (size: string) => `
${size}
const HALF = Math.round(width / 2);
export function App() {
  return <View style={{width: HALF, height: height - 20}} />;
}`;
    const expected = programOf(
      app('const width = screen.width, height = screen.height;'),
    );
    expect(
      programOf(app("const {width, height} = Dimensions.get('window');")),
    ).toEqual(expected);
    expect(
      programOf(app("const {width, height} = Dimensions.get('screen');")),
    ).toEqual(expected);
  });

  it('adds a layout for a breakpoint that changes the tree', () => {
    const app = (size: string) => `
export function App() {
  const panel = ${size};
  const WIDE = panel.width >= 480;
  return WIDE ? <View><Text>a</Text><Text>b</Text></View> : <Text>a</Text>;
}`;
    const result = compilePackage(`${IMPORTS}${app('useWindowDimensions()')}`);
    expect(result.panels).toHaveLength(2);
    expect(result.program).toEqual(programOf(app('screen')));
  });

  it('reads the size in a child component, a custom hook and a memo', () => {
    const app = (size: string, panel: string) => `
function useHalf() {
  ${size}
  return width / 2;
}
function Card({label}) {
  const panel = ${panel};
  return <View style={{height: panel.height - 20}}><Text>{label}</Text></View>;
}
export function App() {
  ${size}
  const half = useHalf();
  const third = useMemo(() => width / 3, [width]);
  return <View style={{width: half, height: third}}><Card label="a" /></View>;
}`;
    expect(
      programOf(
        app('const {width} = useWindowDimensions();', 'useWindowDimensions()'),
      ),
    ).toEqual(programOf(app('const width = screen.width;', 'screen')));
  });

  it('has a scale and a font scale of 1', () => {
    const app = (scale: string, fontScale: string) => `
export function App() {
  return <View style={{width: 10 * ${scale}, height: 20 * ${fontScale}}} />;
}`;
    expect(
      programOf(
        app(
          "Dimensions.get('window').scale",
          'useWindowDimensions().fontScale',
        ),
      ),
    ).toEqual(programOf(app('1', '1')));
  });

  it('still reads the panel when a state is named screen', () => {
    const app = (height: string) => `
export function App() {
  const [screen, setScreen] = useState(0);
  ${height}
  return (
    <Pressable onPress={() => setScreen(screen + 1)} style={{height: height - screen}}>
      <Text>{screen}</Text>
    </Pressable>
  );
}`;
    expect(() =>
      programOf(app('const {height} = useWindowDimensions();')),
    ).not.toThrow();
  });

  it('refuses any other use of Dimensions', () => {
    const app = (read: string) => `
${read}
export function App() {
  return <View style={{width: 10}} />;
}`;
    const refusal = /Dimensions has only get\('window'\) and get\('screen'\)/;
    expect(() =>
      programOf(app("const size = Dimensions.get('display');")),
    ).toThrow(refusal);
    expect(() =>
      programOf(app("Dimensions.addEventListener('change', () => {});")),
    ).toThrow(refusal);
  });

  it('follows the import, whatever it is named', () => {
    const app = (imports: string, size: string) => `
import {View} from 'embedded-react';
${imports}
export function App() {
  const {width} = ${size};
  return <View style={{width: width / 2}} />;
}`;
    const expected = compilePackage(app('', 'screen')).program;
    expect(
      compilePackage(
        app(
          "import {useWindowDimensions as usePanel} from 'embedded-react';",
          'usePanel()',
        ),
      ).program,
    ).toEqual(expected);
    expect(
      compilePackage(
        app(
          "import {Dimensions as Panel} from 'embedded-react';",
          "Panel.get('window')",
        ),
      ).program,
    ).toEqual(expected);
  });

  it('leaves a prop, a state or a local of either name alone', () => {
    // Each is the app's own value, so none of them reads the panel: the build refuses the call.
    const shadowed = [
      `function Card({Dimensions}) {
  const {width} = Dimensions.get('window');
  return <View style={{width}} />;
}
export function App() {
  const [size, setSize] = useState(0);
  return <Pressable onPress={() => setSize(size + 1)}><Card Dimensions={size} /></Pressable>;
}`,
      `export function App() {
  const [Dimensions, setDimensions] = useState(0);
  const {width} = Dimensions.get('window');
  return <Pressable onPress={() => setDimensions(Dimensions + 1)} style={{width}} />;
}`,
      `export function App() {
  const [useWindowDimensions, setHook] = useState(0);
  const {width} = useWindowDimensions();
  return <Pressable onPress={() => setHook(useWindowDimensions + 1)} style={{width}} />;
}`,
    ];
    for (const body of shadowed) {
      expect(() => programOf(body)).toThrow(/AOT/);
    }
  });

  it('leaves a name that is not imported from the package alone', () => {
    expect(() =>
      compilePackage(`import {View} from 'embedded-react';
export function App() {
  const {width} = useWindowDimensions();
  return <View style={{width}} />;
}`),
    ).toThrow(/AOT/);
  });
});
