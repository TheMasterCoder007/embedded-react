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
import {EVENT_SIZE} from '../compiler/package/format.mts';
import type {CompileOptions} from '../compiler/options.mts';

// onScroll on a ScrollView: what compiles and what the compiler refuses. That the handler sees the offsets Flow A's
// does is the frame tests' job (erb-frames, scroll-events).

const OPTIONS: CompileOptions = {
  filename: 'App.tsx',
  screen: {width: 240, height: 320},
};

/** An app whose body is `body`, with `elements` under its root. */
const appOf = (elements: string, body = '') =>
  "import {useCallback, useRef, useState} from 'react';\n" +
  "import {Pressable, ScrollView, Text, View, scrollTo} from 'embedded-react';\n" +
  'export function App() {\n' +
  '  const list = useRef(null);\n' +
  '  const [at, setAt] = useState(0);\n' +
  `  ${body}\n` +
  '  return (\n' +
  '    <View>\n' +
  '      <Text>{at}</Text>\n' +
  `      ${elements}\n` +
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

describe('onScroll in a package', () => {
  it('reads the offset on either axis', () => {
    const vertical = compile(
      appOf('<ScrollView onScroll={event => setAt(event.scrollY)} />'),
    );
    const horizontal = compile(
      appOf('<ScrollView onScroll={event => setAt(event.scrollX)} />'),
    );
    expect(vertical.program).not.toEqual(horizontal.program);
  });

  it('takes a useCallback, which a press can share while it reads no event', () => {
    const body = 'const moved = useCallback(() => setAt(1), []);';
    compile(
      appOf(
        '<ScrollView onScroll={moved} /><Pressable onPress={moved} />',
        body,
      ),
    );
    compile(
      appOf(
        '<ScrollView onScroll={scrolled} />',
        'const scrolled = useCallback(event => setAt(event.scrollY), []);',
      ),
    );
  });

  it('keeps room for the event a scrollTo fires', () => {
    const scroller =
      '<Pressable onPress={() => { scrollTo(list.current, {y: 9}); setAt(2); }} />';
    const arenaOf = (list: string) =>
      compile(appOf(`${list}${scroller}`)).panels[0].arenaBytes;
    const heard = arenaOf(
      '<ScrollView ref={list} onScroll={() => setAt(1)} />',
    );
    expect(heard - arenaOf('<ScrollView ref={list} />')).toBe(EVENT_SIZE);
  });

  it('refuses a scroll offset outside an onScroll handler', () => {
    expect(
      failure(appOf('<Pressable onPress={event => setAt(event.scrollY)} />')),
    ).toMatch(/only an onScroll event carries a scroll offset/);
  });

  it('refuses a touch field, which a scroll event does not carry', () => {
    expect(
      failure(appOf('<ScrollView onScroll={event => setAt(event.y)} />')),
    ).toMatch(/an onScroll event carries no touch, so it has no "y"/);
    expect(
      failure(appOf('<ScrollView onScroll={event => setAt(event.vy)} />')),
    ).toMatch(/an onScroll event carries no touch, so it has no "vy"/);
  });

  it('refuses onScroll on a node that does not scroll', () => {
    expect(failure(appOf('<View onScroll={() => setAt(1)} />'))).toMatch(
      /prop "onScroll" is not supported/,
    );
  });
});
