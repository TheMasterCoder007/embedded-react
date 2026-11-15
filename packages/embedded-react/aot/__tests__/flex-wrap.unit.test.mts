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
import {lowerApp} from '../compiler/lower.mts';
import {resolveOptions} from '../compiler/options.mts';
import {nodesOf} from '../compiler/ir/nodes.mts';
import {lowerStyle} from '../style-map.mts';

// flexWrap: the constant it lowers to, and a value state picks. How a wrapped row is laid out is erb:frames' job
// (the wrap fixture).

const SCREEN = {screen: {width: 240, height: 320}};

/** An app that is one View with a style. */
const appOf = (style: string) =>
  "import {useState} from 'react';\n" +
  "import {View} from 'embedded-react';\n" +
  'export function App() {\n' +
  '  const [wraps, setWraps] = useState(false);\n' +
  `  return <View style={${style}} />;\n` +
  '}\n';

/** The message a compile fails with. */
const failure = (source: string) => {
  try {
    compilePackage(source, 'App.jsx', SCREEN);
  } catch (error) {
    return (error as Error).message;
  }
  throw new Error('it compiled');
};

describe('flexWrap', () => {
  it('lowers each value to its engine constant', () => {
    expect(
      ['nowrap', 'wrap', 'wrap-reverse'].map(flexWrap =>
        lowerStyle({flexWrap}),
      ),
    ).toEqual([
      [{field: 'flex_wrap', value: 'ER_WRAP_NOWRAP'}],
      [{field: 'flex_wrap', value: 'ER_WRAP_WRAP'}],
      [{field: 'flex_wrap', value: 'ER_WRAP_WRAP_REVERSE'}],
    ]);
  });

  it('is a constant prop when it is constant, and a state prop when state picks it', () => {
    const [constant] = nodesOf(
      lowerApp(appOf("{flexWrap: 'wrap'}"), resolveOptions(SCREEN)).root,
    );
    expect(constant.constantProps).toContainEqual({
      field: 'flex_wrap',
      value: 'ER_WRAP_WRAP',
    });

    const [driven] = nodesOf(
      lowerApp(
        appOf("{flexWrap: wraps ? 'wrap' : 'nowrap'}"),
        resolveOptions(SCREEN),
      ).root,
    );
    expect(driven.stateProps.map(prop => prop.field)).toEqual(['flex_wrap']);
  });

  it('refuses a value the engine has no constant for', () => {
    expect(failure(appOf("{flexWrap: 'balance'}"))).toContain('flexWrap');
  });
});
