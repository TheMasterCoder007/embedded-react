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
import {Table} from '../compiler/package/format.mts';
import {OP} from '../compiler/package/bytecode.mts';
import type {CompileOptions} from '../compiler/options.mts';

// scrollTo(ref.current, {x, y}): the SCROLL and SCROLLAT each call lowers to, and what the compiler refuses. That
// the scroll lands where Flow A's does is the frame tests' job (erb-frames, scroll-to).

const OPTIONS: CompileOptions = {
  filename: 'App.tsx',
  screen: {width: 240, height: 320},
};

/** An app with `handler` as a button's onPress body, and `list` as the element the ref is on. */
const appOf = (
  handler: string,
  list = '<ScrollView ref={list} style={{height: 100}} />',
) =>
  "import {useRef, useState} from 'react';\n" +
  "import {Pressable, ScrollView, Text, View, scrollTo} from 'embedded-react';\n" +
  'export function App() {\n' +
  '  const list = useRef(null);\n' +
  '  const [at, setAt] = useState(0);\n' +
  '  const [shown, setShown] = useState(true);\n' +
  '  return (\n' +
  '    <View>\n' +
  `      <Pressable onPress={() => { ${handler} }}><Text>{at}</Text></Pressable>\n` +
  `      ${list}\n` +
  '    </View>\n' +
  '  );\n' +
  '}\n';

const compile = (source: string) => compilePackage(source, 'app', OPTIONS);

/** The message a compile fails with. */
const failure = (source: string) => {
  try {
    compile(source);
  } catch (error) {
    return (error as Error).message;
  }
  throw new Error('it compiled');
};

const OP_NAMES = Object.fromEntries(
  Object.entries(OP).map(([name, {code}]) => [code, name]),
);

/**
 * The scroll instructions a program has, in code order.
 *
 * @param bytes  The program section.
 *
 * @returns Each SCROLL and SCROLLAT.
 */
function scrollsOf(bytes: Uint8Array): string[] {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.length);
  const u16 = (offset: number) => view.getUint16(offset, true);
  const u32 = (offset: number) => view.getUint32(offset, true);
  const directory = 24 + u16(18) * 16 + u16(20) * 8;
  let code = {offset: 0, length: 0};
  for (let index = 0; index < u16(22); index++) {
    const entry = directory + index * 12;
    if (u16(entry) === Table.code) {
      code = {offset: u32(entry + 4), length: u32(entry + 8)};
    }
  }
  const scrolls: string[] = [];
  for (let at = 0; at < code.length / 4;) {
    const name = OP_NAMES[u32(code.offset + at * 4) & 0xff];
    if (name.startsWith('SCROLL')) scrolls.push(name);
    at += OP[name as keyof typeof OP].words;
  }
  return scrolls;
}

describe('scrollTo in a package', () => {
  it('scrolls without reading when the position is dropped', () => {
    const {program} = compile(appOf('scrollTo(list.current, {y: 120});'));
    expect(scrollsOf(program)).toEqual(['SCROLL']);
  });

  it('reads without scrolling when no offset is given', () => {
    const {program} = compile(
      appOf('const position = scrollTo(list.current); setAt(position.y);'),
    );
    expect(scrollsOf(program)).toEqual(['SCROLLAT']);
  });

  it('scrolls, then reads where it landed', () => {
    const {program} = compile(
      appOf(
        'const {y, maxY} = scrollTo(list.current, {y: at}); setAt(maxY - y);',
      ),
    );
    expect(scrollsOf(program)).toEqual(['SCROLL', 'SCROLLAT']);
  });

  it('keeps a position read as a setter argument', () => {
    const {program} = compile(
      appOf('setAt(scrollTo(list.current, {x: 0, y: 40}).y);'),
    );
    expect(scrollsOf(program)).toEqual(['SCROLL', 'SCROLLAT']);
  });

  it('takes the last of an axis written twice, as JS does', () => {
    const twice = compile(appOf('scrollTo(list.current, {y: 7, y: 120});'));
    const once = compile(appOf('scrollTo(list.current, {y: 120});'));
    const first = compile(appOf('scrollTo(list.current, {y: 7});'));
    expect(twice.program).toEqual(once.program);
    expect(twice.program).not.toEqual(first.program);
  });

  it('refuses a handle that is not a node ref’s current', () => {
    expect(failure(appOf('scrollTo(list, {y: 1});'))).toMatch(
      /takes the `\.current` of a node ref/,
    );
  });

  it('refuses an offset that is not an object literal of x and y', () => {
    expect(failure(appOf('scrollTo(list.current, at);'))).toMatch(
      /offset as an object literal/,
    );
    expect(failure(appOf('scrollTo(list.current, {top: 1});'))).toMatch(
      /offset as an object literal/,
    );
  });

  it('refuses a position where it cannot be kept', () => {
    expect(
      failure(appOf('if (scrollTo(list.current).y > 0) setAt(1);')),
    ).toMatch(/returns a position, which only a const keeps/);
  });

  it('refuses a ref that is not on a ScrollView', () => {
    expect(
      failure(appOf('scrollTo(list.current, {y: 1});', '<View ref={list} />')),
    ).toMatch(/a ref that is on a <ScrollView>/);
    expect(
      failure(appOf('scrollTo(list.current, {y: 1});', '<View />')),
    ).toMatch(/a ref that is on a <ScrollView>/);
  });

  it('refuses a ScrollView a conditional builds', () => {
    expect(
      failure(
        appOf(
          'scrollTo(list.current, {y: 1});',
          '{shown && <ScrollView ref={list} />}',
        ),
      ),
    ).toMatch(/scrolls a node a conditional builds and destroys/);
  });
});
