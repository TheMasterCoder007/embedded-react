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

// Date.now() / performance.now() in Flow B: the engine clock as 64-bit whole milliseconds, the same surface
// Flow A's lite profile gives.
const PREAMBLE = `import { useState, useEffect, useRef } from 'react';
import { View, Text, Pressable } from 'embedded-react';
`;
const gen = source => compilePackage(source, 'test');
const app = (body, jsx) => `${PREAMBLE}
export function App() {
  ${body}
  return (${jsx});
}`;

describe('AOT Date.now() / performance.now()', () => {
  it.each([
    ['a plain `/`', '<Text>{Date.now() / 1000}</Text>', /`\/` on a 64-bit/],
    ['a float', '<Text>{Date.now() * 0.5}</Text>', /mixed with a float/],
    [
      'a float comparison',
      "<Text>{Date.now() > 0.5 ? 'a' : 'b'}</Text>",
      /mixed with a float/,
    ],
    [
      'Math.sqrt',
      '<Text>{Math.sqrt(Date.now())}</Text>',
      /Math\.sqrt\(\.\.\.\) on a 64-bit/,
    ],
    ['new Date()', '<Text>{new Date()}</Text>', /Date objects/],
    ['Date()', '<Text>{Date()}</Text>', /Date objects/],
    ['Date.parse', "<Text>{Date.parse('2026')}</Text>", /Date objects/],
    [
      'a timestamp as a style value',
      '<View style={{width: Date.now()}} />',
      /cannot be used here/,
    ],
  ])('refuses %s', (_, jsx, pattern) => {
    expect(() => gen(app('', `<View>${jsx}</View>`))).toThrow(pattern);
  });

  it('refuses to store a timestamp in a float or boolean state', () => {
    expect(() =>
      gen(
        app(
          'const [f, setF] = useState(0.0);',
          '<Pressable onPress={() => setF(Date.now())}><Text>{f}</Text></Pressable>',
        ),
      ),
    ).toThrow(/cannot be stored in state "f"/);
    expect(() =>
      gen(
        app(
          'const [on, setOn] = useState(false);',
          '<Pressable onPress={() => setOn(Date.now())}><Text>x</Text></Pressable>',
        ),
      ),
    ).toThrow(/cannot be stored in state "on"/);
  });

  it('refuses `/=` on a 64-bit ref', () => {
    expect(() =>
      gen(
        app(
          'const r = useRef(0);',
          '<Pressable onPress={() => { r.current = Date.now(); r.current /= 2; }}><Text>x</Text></Pressable>',
        ),
      ),
    ).toThrow(/`\/=` on a 64-bit/);
  });

  it('refuses a timestamp divided by anything but a constant', () => {
    const withN = jsx =>
      gen(app('const [n, setN] = useState(3);', `<View>${jsx}</View>`));
    expect(() => withN('<Text>{Date.now() % n}</Text>')).toThrow(
      /divided by a constant/,
    );
    expect(() => withN('<Text>{Math.floor(Date.now() / n)}</Text>')).toThrow(
      /divided by a constant/,
    );
    expect(() => withN('<Text>{Math.ceil(Date.now() / n)}</Text>')).toThrow(
      /divided by a constant/,
    );
    expect(() =>
      gen(
        app(
          'const [n, setN] = useState(3);\n  const r = useRef(0);',
          '<Pressable onPress={() => { r.current = Date.now(); r.current %= n; }}><Text>x</Text></Pressable>',
        ),
      ),
    ).toThrow(/divided by a constant/);
  });

  it('leaves a `Date` / `performance` that the app binds itself alone', () => {
    // A handler param and a module const shadow the globals, so neither call is the engine clock.
    expect(() =>
      gen(
        app(
          'const [t, setT] = useState(0);',
          '<Pressable onPress={(performance) => setT(performance.now())}><Text>{t}</Text></Pressable>',
        ),
      ),
    ).toThrow(/unsupported call expression/);
    expect(() =>
      gen(`${PREAMBLE}
const performance = {now: 5};
export function App() {
  return (<Text>{performance.now()}</Text>);
}`),
    ).toThrow(/unsupported call expression/);
  });
});
