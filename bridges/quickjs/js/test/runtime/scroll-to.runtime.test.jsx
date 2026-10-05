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

// Runtime e2e: NativeUI.scrollTo(handle, x, y) sets a ScrollView's offset through the engine's clamp,
// fires its onScroll, and answers [x, y, maxX, maxY]; NaN keeps an axis, so (NaN, NaN) only reads.
import {createRoot} from '../../src/renderer.js';
import {ScrollView, View} from 'embedded-react';
import {check, report} from './harness.js';

let scroller = null;
let box = null;
const scrolls = [];

const root = createRoot({width: screen.width, height: screen.height});
root.render(
  <ScrollView
    ref={h => (scroller = h)}
    style={{width: 100, height: 100}}
    onScroll={e => scrolls.push(e.scrollY)}>
    <View ref={h => (box = h)} style={{width: 100, height: 120}} />
    <View style={{width: 100, height: 180}} />
  </ScrollView>,
);
NativeUI.tick(0);

const same = (a, b) =>
  Array.isArray(a) && a.length === b.length && a.every((v, i) => v === b[i]);

let r = NativeUI.scrollTo(scroller, NaN, NaN);
check(same(r, [0, 0, 0, 200]), `NaN, NaN only reads (got ${r})`);
check(scrolls.length === 0, 'a read fires no onScroll');

r = NativeUI.scrollTo(scroller, NaN, 50);
check(same(r, [0, 50, 0, 200]), `scrolls to y = 50 (got ${r})`);
check(
  scrolls.length === 1 && scrolls[0] === 50,
  `onScroll fired once with scrollY = 50 (got ${scrolls})`,
);

r = NativeUI.scrollTo(scroller, 40, 999);
check(same(r, [0, 200, 0, 200]), `clamps to the scroll range (got ${r})`);

r = NativeUI.scrollTo(box, 0, 10);
check(r === undefined, 'a View is not a ScrollView: undefined');
r = NativeUI.scrollTo(0, 0, 10);
check(r === undefined, 'an invalid handle: undefined');

report('scroll-to');
