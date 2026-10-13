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
import {writeApp} from '../compiler/assemble.mts';
import {resolveOptions} from '../compiler/options.mts';
import {printKeyboard} from '../compiler/c/keyboard.mts';
import {printPanDecls} from '../compiler/c/pan-responder.mts';
import {printVectorBlocks} from '../compiler/c/vectors.mts';

// An app lowers to one IrApp, which the C backend writes as app.gen.c and app.gen.h.

const opts = resolveOptions({});

/** The app a source lowers to. */
const lower = source => lowerApp(source, opts, new Set(), new Set());

describe('the lowered app', () => {
  it('keeps the keyboard config as IR, which the writer prints with its setup call', () => {
    const app = lower(
      "import {View, setKeyboardConfig} from 'embedded-react';\n" +
        "setKeyboardConfig({panelColor: '#333333', rowHeight: 30});\n" +
        'export function App() { return <View />; }',
    );
    expect(app.keyboard).toMatchObject({
      layers: null,
      rowHeight: 30,
      panelColor: 0xff333333,
    });

    const {c} = writeApp(app, 'probe', opts);
    expect(c).toContain(printKeyboard(app.keyboard).data);
    expect(c).toContain(
      '    er_keyboard_set_config(&kbd_cfg); /* app-supplied on-screen keyboard layout/appearance */',
    );
  });

  it('has no keyboard config when the module sets none', () => {
    const app = lower(
      "import {View} from 'embedded-react';\nexport function App() { return <View />; }",
    );
    expect(app.keyboard).toBeNull();
    expect(app.rootNodeId).toBe('n0');
    expect(writeApp(app, 'probe', opts).c).not.toContain('kbd_cfg');
  });

  it('holds drawings and responders as IR, which the writer declares at file scope', () => {
    const app = lower(
      "import React, {useRef, useState} from 'react';\n" +
        "import {View, Svg, Circle, PanResponder} from 'embedded-react';\n" +
        'export function App() {\n' +
        '  const [n, setN] = useState(0);\n' +
        '  const pan = useRef(PanResponder.create({\n' +
        '    onStartShouldSetPanResponder: () => n > 2,\n' +
        '    onPanResponderMove: (e, g) => setN(g.dx),\n' +
        '  })).current;\n' +
        '  return (\n' +
        '    <View {...pan.panHandlers}>\n' +
        '      <Svg width={10} height={10}><Circle cx={5} cy={5} r={2} fill="#0f0" /></Svg>\n' +
        '      <Svg width={10} height={10}><Circle cx={n} cy={5} r={2} fill="#00f" /></Svg>\n' +
        '    </View>\n' +
        '  );\n' +
        '}',
    );
    const {out} = app;
    expect(out.vectors.map(vector => vector.kind)).toEqual([
      'tables',
      'drawing',
    ]);
    expect(out.panResponders).toMatchObject([
      {name: 'pan', fnPrefix: 'er_pan_pan', stateID: 1},
    ]);

    const {c} = writeApp(app, 'probe', opts);
    const {data, builders} = printVectorBlocks(out.vectors);
    expect(c).toContain(data);
    expect(c).toContain(builders);
    expect(c).toContain(printPanDecls(out.panResponders[0]).join('\n'));
    expect(c).toContain(
      'static bool er_pan_pan_start_should_set(ERNode* node, const EREventData* data, void* user_data)',
    );
  });
});
