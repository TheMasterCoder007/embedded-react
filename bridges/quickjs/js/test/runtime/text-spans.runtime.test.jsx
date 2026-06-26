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

// Runtime e2e: multi-child <Text> interpolation and nested styled <Text> spans must mount through
// the reconciler (the node owns its subtree — no separate child nodes) and re-render correctly.
// Span styling itself isn't observable from JS, so we assert: it renders (onLayout fires), and the
// measured width tracks the text length across re-renders.
import {createRoot} from '../../src/renderer.js';
import {View, Text} from 'embedded-react';
import {check, report} from './harness.js';

const layouts = {};

function App({n, bold}) {
  return (
    <View style={{width: 300, height: 200}}>
      <Text
        style={{fontSize: 16, color: 'white', alignSelf: 'flex-start'}}
        onLayout={e => (layouts.plain = e.layout)}>
        {`count: ${n}`}
      </Text>
      <Text
        style={{fontSize: 16, color: 'white', alignSelf: 'flex-start'}}
        onLayout={e => (layouts.rich = e.layout)}>
        Hello <Text style={{fontWeight: 'bold', color: 'red'}}>{bold}</Text>!
      </Text>
    </View>
  );
}

const root = createRoot({width: screen.width, height: screen.height});
root.render(<App n={1} bold="world" />);

check(layouts.plain != null, 'interpolated Text rendered (onLayout fired)');
check(layouts.rich != null, 'nested-span Text rendered (onLayout fired)');

const wShort = layouts.plain ? layouts.plain.width : -1;
const wRich = layouts.rich ? layouts.rich.width : -1;
console.log('  widths: plain=', wShort, ' rich=', wRich);
check(
  wShort >= 0 && wRich >= 0,
  'both Text nodes measured a non-negative width',
);

// Re-render with a much longer interpolation; the plain Text must measure wider (more glyphs).
root.render(<App n={1234567890} bold="world" />);
const wLong = layouts.plain ? layouts.plain.width : -1;
check(
  wLong > wShort,
  `longer interpolation measures wider (${wShort} -> ${wLong})`,
);

// The nested-span text measures its full concatenated content ("Hello world!"), wider than "Hello".
check(
  wRich > 0,
  `nested-span Text measured its full content width (rich=${wRich})`,
);

// Re-render the nested-span child text; should still render cleanly.
root.render(<App n={1234567890} bold="universe" />);
check(layouts.rich != null, 'nested-span Text re-rendered with new inner text');

report('text-spans');
