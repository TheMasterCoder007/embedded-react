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
import {compileSource} from '../compile.mjs';

// Flow B TypeScript support: an App.tsx is parsed with the `typescript` plugin and scrubbed of all
// type-only syntax before the JSX→C walker runs. The contract is that the generated C is IDENTICAL to the
// same component written untyped (App.jsx) — types are erased, nothing else changes.

const PRE = `import { useState } from 'react';
import { View, Text, Pressable, StyleSheet } from 'embedded-react';`;

const jsGen = src => compileSource(src, 'test').c;
const tsGen = src => compileSource(src, 'test', {filename: 'App.tsx'}).c;

describe('AOT TypeScript: type erasure produces identical C', () => {
  it('a TS-annotated app compiles to the same C as its untyped twin', () => {
    const js = `${PRE}
      export function App() {
        const [count, setCount] = useState(0);
        return (
          <View style={s.root}>
            <Text style={s.label}>count is {count}</Text>
            <Pressable onPress={() => setCount(c => c + 1)}>
              <Text style={s.label}>tap</Text>
            </Pressable>
          </View>
        );
      }
      const s = StyleSheet.create({ root: { flex: 1, padding: 10 }, label: { color: '#fff', fontSize: 18 } });`;

    // Same app, fully annotated: type imports, an interface + type alias, a typed param + return type, a
    // generic on useState, a typed arrow param, and an `as` assertion on a style. All must erase to nothing.
    const ts = `${PRE}
      import type { ViewStyle } from 'embedded-react';
      interface Props { title?: string }
      type Inc = (n: number) => number;
      export function App({ title }: Props): JSX.Element {
        const [count, setCount] = useState<number>(0);
        return (
          <View style={s.root}>
            <Text style={s.label}>count is {count}</Text>
            <Pressable onPress={() => setCount((c: number) => c + 1)}>
              <Text style={s.label}>tap</Text>
            </Pressable>
          </View>
        );
      }
      const s = StyleSheet.create({ root: { flex: 1, padding: 10 }, label: { color: '#fff', fontSize: 18 } as ViewStyle });`;

    expect(tsGen(ts)).toBe(jsGen(js));
  });

  it('erases `as`, `satisfies`, and non-null `!` in expressions', () => {
    // Twins are aligned: each TS wrapper (`(0 as number)!`, `n satisfies number`) must strip to exactly
    // the untyped expression on the JS side.
    const js = `${PRE}
      export function App() {
        const [n, setN] = useState(0);
        const x = 7;
        return <Text>v {n} {x}</Text>;
      }`;
    const ts = `${PRE}
      export function App() {
        const [n, setN] = useState((0 as number)!);
        const x = (7 satisfies number);
        return <Text>v {n} {x}</Text>;
      }`;
    expect(tsGen(ts)).toBe(jsGen(js));
  });
});

describe('AOT TypeScript: error locations survive type stripping', () => {
  it('points the code frame at the real .tsx line after stripped type decls above it', () => {
    // The interface + type import on lines 3-4 are stripped; the unsupported expression on line 8 must
    // still report its true source line, proving remaining nodes keep their original .loc.
    const ts = `${PRE}
      import type { ViewStyle } from 'embedded-react';
      interface Props { n?: number }
      export function App({ n }: Props) {
        const [count, setCount] = useState(0);
        const bump: ViewStyle = n;
        return <Pressable onPress={() => setCount(bump)}><Text>x</Text></Pressable>;
      }`;
    expect(() => tsGen(ts)).toThrow(/AOT:/);
  });
});

describe('AOT TypeScript: the JS path is unchanged', () => {
  it('does not enable the typescript parser for a non-TS entry (type syntax is a parse error)', () => {
    // With no .tsx filename the parser is jsx-only, so TS syntax must NOT silently parse.
    const withTsSyntax = `${PRE}
      export function App() {
        const [n] = useState<number>(0);
        return <Text>{n}</Text>;
      }`;
    expect(() => jsGen(withTsSyntax)).toThrow();
  });
});
