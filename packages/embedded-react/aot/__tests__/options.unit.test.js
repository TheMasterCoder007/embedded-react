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
import {compilePackage} from '../compile.mts';

// Screen size and buffer caps are options; one left out falls back to its ER_AOT_* variable, read per call.
const IMPORTS = `import { useState } from 'react';
import { View, Text, Pressable } from 'embedded-react';
`;
const LIST_APP = `${IMPORTS}export function App() {
  const [items, setItems] = useState([{ label: 'a', count: 1 }]);
  return (<View>{items.map(item => <Text>{item.label}</Text>)}</View>);
}`;
const RESPONSIVE_APP = `${IMPORTS}export function App() {
  if (screen.width < 400) {
    return (<Text>small</Text>);
  }
  return (<View><Text>wide</Text></View>);
}`;

// Start each case from an environment with none of the variables set, whatever the shell exports.
beforeEach(() => {
  for (const name of [
    'ER_AOT_SCREEN_W',
    'ER_AOT_SCREEN_H',
    'ER_AOT_LIST_CAP',
    'ER_AOT_LIST_STR_CAP',
    'ER_AOT_MAX_TEXT_SPANS',
    'ER_AOT_MAX_TIMERS',
  ]) {
    vi.stubEnv(name, undefined);
  }
});
afterEach(() => vi.unstubAllEnvs());

describe('AOT compile options', () => {
  it.each([0, -1, 1.5, NaN, Infinity, 1e21])(
    'refuses listCap, listStrCap and maxTextSpans of %s',
    value => {
      for (const name of ['listCap', 'listStrCap', 'maxTextSpans']) {
        expect(() => compilePackage(LIST_APP, 'demo', {[name]: value})).toThrow(
          `AOT: ${name} must be a whole number from 1 to`,
        );
      }
    },
  );

  it.each([0, -1, 1.5, NaN, Infinity, 1e21, 32768])(
    'refuses a screen dimension of %s',
    value => {
      expect(() =>
        compilePackage(RESPONSIVE_APP, 'demo', {
          screen: {width: value, height: 320},
        }),
      ).toThrow(
        `AOT: screen.width must be a whole number from 1 to 32767, got ${value}`,
      );
      expect(() =>
        compilePackage(RESPONSIVE_APP, 'demo', {
          screen: {width: 240, height: value},
        }),
      ).toThrow('AOT: screen.height must be a whole number from 1 to 32767');
    },
  );

  it.each([
    ['ER_AOT_LIST_CAP', '-1'],
    ['ER_AOT_LIST_STR_CAP', '1.5'],
    ['ER_AOT_MAX_TEXT_SPANS', '0'],
    ['ER_AOT_MAX_TIMERS', '4097'],
    ['ER_AOT_SCREEN_W', '1e21'],
    ['ER_AOT_SCREEN_H', 'Infinity'],
    ['ER_AOT_LIST_CAP', 'abc'],
  ])('refuses %s=%s, naming the variable', (name, raw) => {
    vi.stubEnv(name, raw);
    expect(() => compilePackage(LIST_APP, 'demo')).toThrow(
      `AOT: ${name} must be a whole number from 1 to`,
    );
    expect(() => compilePackage(LIST_APP, 'demo')).toThrow(`got "${raw}"`);
  });
});
