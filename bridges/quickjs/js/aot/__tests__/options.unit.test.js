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

import {describe, it, expect, vi, beforeEach, afterEach} from 'vitest';
import {compileSource} from '../compile.mts';

// Screen size and buffer caps are options; one left out falls back to its ER_AOT_* variable, read per call.
const IMPORTS = `import { useState } from 'react';
import { View, Text, Pressable } from 'embedded-react';
`;
const LIST_APP = `${IMPORTS}export function App() {
  const [items, setItems] = useState([{ label: 'a', count: 1 }]);
  return (<View>{items.map(item => <Text>{item.label}</Text>)}</View>);
}`;
const RESPONSIVE_APP = `${IMPORTS}export function App() {
  return (<View style={{width: screen.width < 400 ? 111 : 333}} />);
}`;
const THREE_SPANS_APP = `${IMPORTS}export function App() {
  return (<Text>A <Text style={{color: '#f00'}}>B</Text> C</Text>);
}`;
const BAD_APP = `${IMPORTS}export function App() {
  const [count, setCount] = useState(0);
  return (<Pressable onPress={() => setCount(window.x)}><Text>{count}</Text></Pressable>);
}`;

const errorOf = compile => {
  try {
    compile();
  } catch (error) {
    return error;
  }
  throw new Error('expected the compile to fail');
};

// Start each case from an environment with none of the variables set, whatever the shell exports.
beforeEach(() => {
  for (const name of [
    'ER_AOT_SCREEN_W',
    'ER_AOT_SCREEN_H',
    'ER_AOT_LIST_CAP',
    'ER_AOT_LIST_STR_CAP',
    'ER_AOT_MAX_TEXT_SPANS',
  ]) {
    vi.stubEnv(name, undefined);
  }
});
afterEach(() => vi.unstubAllEnvs());

describe('AOT compile options', () => {
  it('compiles two screen sizes in one process', () => {
    const small = compileSource(RESPONSIVE_APP, 'demo', {
      screen: {width: 240, height: 320},
    });
    const wide = compileSource(RESPONSIVE_APP, 'demo', {
      screen: {width: 800, height: 480},
    });
    expect(small.c).toContain('p.width = 111;');
    expect(small.h).toContain('#define ER_AOT_SCREEN_W 240');
    expect(wide.c).toContain('p.width = 333;');
    expect(wide.h).toContain('#define ER_AOT_SCREEN_W 800');
  });

  it('prefers the screen option over ER_AOT_SCREEN_W/H', () => {
    vi.stubEnv('ER_AOT_SCREEN_W', '800');
    vi.stubEnv('ER_AOT_SCREEN_H', '480');
    const result = compileSource(RESPONSIVE_APP, 'demo', {
      screen: {width: 240, height: 320},
    });
    expect(result.c).toContain('p.width = 111;');
  });

  it('reads ER_AOT_SCREEN_W/H on each call, not at import', () => {
    vi.stubEnv('ER_AOT_SCREEN_W', '240');
    vi.stubEnv('ER_AOT_SCREEN_H', '320');
    expect(compileSource(RESPONSIVE_APP, 'demo').c).toContain('p.width = 111;');
    vi.stubEnv('ER_AOT_SCREEN_W', '800');
    expect(compileSource(RESPONSIVE_APP, 'demo').c).toContain('p.width = 333;');
  });

  it('names the screen size an error was compiled at when it came from the option', () => {
    const error = errorOf(() =>
      compileSource(BAD_APP, 'demo', {screen: {width: 240, height: 320}}),
    );
    expect(error.message.trimEnd().endsWith('screen: 240×320.')).toBe(true);
    expect(error.message).not.toMatch(/did not supply both dimensions/);
  });

  it('sizes a list state from listCap and its string fields from listStrCap', () => {
    const generated = compileSource(LIST_APP, 'demo', {
      listCap: 4,
      listStrCap: 20,
    }).c;
    expect(generated).toContain('static ErItem_items s_items[4] = {');
    expect(generated).toContain('char label[20];');
  });

  it('falls back to ER_AOT_LIST_CAP / ER_AOT_LIST_STR_CAP, then 16 / 48', () => {
    expect(compileSource(LIST_APP, 'demo').c).toContain('s_items[16] = {');
    expect(compileSource(LIST_APP, 'demo').c).toContain('char label[48];');
    vi.stubEnv('ER_AOT_LIST_CAP', '5');
    vi.stubEnv('ER_AOT_LIST_STR_CAP', '24');
    const generated = compileSource(LIST_APP, 'demo').c;
    expect(generated).toContain('s_items[5] = {');
    expect(generated).toContain('char label[24];');
  });

  it('refuses a nested <Text> with more segments than maxTextSpans', () => {
    expect(() => compileSource(THREE_SPANS_APP, 'demo')).not.toThrow();
    const error = errorOf(() =>
      compileSource(THREE_SPANS_APP, 'demo', {maxTextSpans: 2}),
    );
    expect(error.message).toContain(
      'a <Text> has 3 inline segments but the engine renders at most 2',
    );
  });
});
