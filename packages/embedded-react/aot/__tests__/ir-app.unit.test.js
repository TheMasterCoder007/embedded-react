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
import {lowerApp} from '../compiler/lower.mts';
import {resolveOptions} from '../compiler/options.mts';

// An app lowers to one IrApp, which the package backend writes as a package.

const options = resolveOptions({});

/** The app a source lowers to. */
const lower = source => lowerApp(source, options, new Map(), []);

describe('the lowered app: functions', () => {
  /** An app module around a component body and the JSX it returns. */
  const app = (body, jsx) =>
    "import React, {useState, useEffect} from 'react';\n" +
    "import {View, Text, TouchableOpacity, Animated, useAnimatedValue} from 'embedded-react';\n" +
    `export function App() {\n${body}\n  return ${jsx};\n}`;

  it('runs a mount effect inline, or calls it when it returns early', () => {
    const {output} = lower(
      app(
        '  const [n, setN] = useState(0);\n' +
          '  useEffect(() => { setN(1); }, []);\n' +
          '  useEffect(() => { if (n > 1) return; setN(2); }, []);',
        '<Text>{n}</Text>',
      ),
    );
    expect(output.mountEffects).toMatchObject([
      {kind: 'inline', callback: {updatesState: true}},
      {kind: 'call', name: 'effect_0', updatesState: true},
    ]);
    expect(output.effectFunctions).toMatchObject([
      {kind: 'mount', name: 'effect_0'},
    ]);
  });

  it('drops the wait bookkeeping from a stop no start waits on', () => {
    const {output} = lower(
      app(
        '  const a = useAnimatedValue(0);',
        '<View onTouchStart={() => Animated.sequence([Animated.timing(a, {toValue: 1}), Animated.delay(50)]).stop()} />',
      ),
    );
    expect(output.handlers[0].callback.body).toMatchObject([
      {kind: 'stopAnimations', handles: ['anim_a'], waitOwner: null},
    ]);
  });
});
