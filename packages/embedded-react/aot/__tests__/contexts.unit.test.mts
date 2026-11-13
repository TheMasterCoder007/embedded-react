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
import {compilePackage} from '../compile.mts';
import {lowerApp} from '../compiler/lower.mts';
import {resolveOptions} from '../compiler/options.mts';
import {nodesOf} from '../compiler/ir/nodes.mts';
import {nodeReads} from '../compiler/ir/reads.mts';

// Contexts, resolved at compile time to the nearest provider.

const SCREEN = {screen: {width: 240, height: 320}};

const HEAD =
  "import {createContext, useCallback, useContext, useState} from 'react';\n" +
  "import {Pressable, Text, View} from 'embedded-react';\n";

const THEME =
  "const Theme = createContext('light');\n" +
  'function Label() { const theme = useContext(Theme); return <Text>{theme}</Text>; }\n';

/** An app module with `before` at module level and `body` as the App's body. */
const appOf = (before: string, body: string) =>
  `${HEAD}${before}\nexport function App() {\n${body}\n}`;

const compile = (source: string) => compilePackage(source, 'app', SCREEN);
const lower = (source: string) => lowerApp(source, resolveOptions(SCREEN));

/** The slots each node reads, for every node. */
const readsOf = (source: string) =>
  nodesOf(lower(source).root).map(node =>
    [...(nodeReads(node) ?? [])].map(state => state.slotName),
  );

/** The text each node draws, where it is a constant. */
const textsOf = (source: string) => JSON.stringify(nodesOf(lower(source).root));

describe('useContext', () => {
  it('reads the nearest provider, and the default with none', () => {
    const source = appOf(
      THEME,
      "  const [mode, setMode] = useState('dark');\n" +
        '  return <View><Label/>' +
        '<Theme.Provider value={mode}><Label/>' +
        '<Theme.Provider value="blue"><Label/></Theme.Provider></Theme.Provider></View>;',
    );
    expect(readsOf(source)).toEqual([[], [], ['mode'], []]);
    const texts = textsOf(source);
    expect(texts).toContain('"light"');
    expect(texts).toContain('"blue"');
  });

  it('reads the default in the App', () => {
    const source = appOf(
      THEME,
      '  const theme = useContext(Theme);\n  return <Text>{theme}</Text>;',
    );
    expect(textsOf(source)).toContain('"light"');
  });

  it('reaches children rendered inside a provider a component returns', () => {
    const source = appOf(
      THEME +
        'function ThemeProvider({children}) {\n' +
        "  const [mode] = useState('dark');\n" +
        '  return <Theme.Provider value={mode}>{children}</Theme.Provider>;\n}\n',
      '  return <View><ThemeProvider><Label/></ThemeProvider><Label/></View>;',
    );
    expect(readsOf(source)).toEqual([[], ['c0_mode'], []]);
  });

  it('destructures an object value, callbacks among its keys', () => {
    const source = appOf(
      'const Count = createContext({n: 0, inc: () => {}});\n' +
        'function Button() {\n' +
        '  const {n, inc} = useContext(Count);\n' +
        '  return <Pressable onPress={inc}><Text>{n}</Text></Pressable>;\n}\n',
      '  const [n, setN] = useState(0);\n' +
        '  const inc = useCallback(() => setN(n + 1), [n]);\n' +
        '  return <View><Count.Provider value={{n, inc}}><Button/></Count.Provider><Button/></View>;',
    );
    const app = lower(source);
    expect(app.output.handlers).toHaveLength(2);
    expect(JSON.stringify(app.output.handlers[0].callback.body)).toContain(
      '"slotName":"n"',
    );
    expect(readsOf(source)).toEqual([[], [], ['n'], [], []]);
  });

  it('reads a context in a custom hook', () => {
    const source = appOf(
      THEME + 'function useTheme() { return useContext(Theme); }\n',
      "  const [mode] = useState('dark');\n" +
        '  return <Theme.Provider value={mode}><View><Badge/></View></Theme.Provider>;',
    ).replace(
      'export function App',
      'function Badge() { const theme = useTheme(); return <Text>{theme}</Text>; }\nexport function App',
    );
    expect(readsOf(source)).toEqual([[], ['mode']]);
  });

  it('refuses a setter in the value, a consumer, and a read off the top of the component', () => {
    expect(() =>
      compile(
        appOf(
          'const C = createContext(null);\n' +
            'function S() { const {setN} = useContext(C); return <Text>x</Text>; }\n',
          '  const [n, setN] = useState(0);\n' +
            '  return <C.Provider value={{n, setN}}><S/></C.Provider>;',
        ),
      ),
    ).toThrow(/a context's value can't hold the setter "setN"/);
    expect(() =>
      compile(
        appOf(
          'const C = createContext(1);\n',
          '  return <View><C.Consumer>{v => <Text>{v}</Text>}</C.Consumer></View>;',
        ),
      ),
    ).toThrow(/<C.Consumer> is not supported/);
    expect(() =>
      compile(
        appOf(
          'const C = createContext(1);\n',
          '  return <View><Text>{useContext(C)}</Text></View>;',
        ),
      ),
    ).toThrow(/useContext must be called at the top of a component/);
  });

  it('refuses a provider that wraps several elements where one is wanted', () => {
    expect(() =>
      compile(
        appOf(
          THEME,
          '  return <Theme.Provider value="x"><Label/><Label/></Theme.Provider>;',
        ),
      ),
    ).toThrow(/<Theme.Provider> must wrap one element here/);
  });

  it('refuses a context name a binding hides', () => {
    expect(() =>
      compile(
        appOf(
          "const Theme = createContext('light');\n" +
            'function Shown({Theme}) { const theme = useContext(Theme); return <Text>{theme}</Text>; }\n',
          '  return <View><Shown Theme="x"/></View>;',
        ),
      ),
    ).toThrow(/"Theme" here is not the context/);
    expect(() =>
      compile(
        appOf(
          THEME +
            'function Wrap({Theme}) { return <View><Theme.Provider value="dark"><Label/></Theme.Provider></View>; }\n',
          '  return <View><Wrap Theme={1}/></View>;',
        ),
      ),
    ).toThrow(/"Theme" here is not the context/);
  });
});
