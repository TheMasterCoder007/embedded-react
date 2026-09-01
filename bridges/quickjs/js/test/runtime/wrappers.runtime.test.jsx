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

// Runtime e2e for the JS-only RN wrappers — <Button>, <ImageBackground>, <SectionList>. The unit tests
// assert the element tree each one returns; this proves that tree actually becomes engine nodes: the
// button sizes itself to its label and takes a real press, the background image genuinely fills its
// container while the children lay out over it, and a section list's headers and rows land in one
// scroller as ordered siblings.
//
// Touches come from the runner's __touch(phase, x, y[, finger]): 0 down / 1 move / 2 up / 3 cancel.
import {createRoot} from '../../src/renderer.js';
import {Button, ImageBackground, SectionList, Text, View} from 'embedded-react';
import {check, report} from './harness.js';

const root = createRoot({width: screen.width, height: screen.height});

/** Taps the centre of a laid-out box. */
const tap = box => {
  const x = box.x + box.width / 2;
  const y = box.y + box.height / 2;
  __touch(0, x, y);
  __touch(2, x, y);
};

// ====================================================================================================
// 1. <Button> — two nodes, self-sizing, pressable, and dead once disabled
// ====================================================================================================
{
  const presses = [];
  let box = null;
  let bare = null;

  const App = ({disabled}) => (
    // alignSelf shrink-wraps each wrapper onto its child, so the rects ARE the child's.
    <View>
      <View style={{alignSelf: 'flex-start'}} onLayout={e => (box = e.layout)}>
        <Button
          title="Save"
          disabled={disabled}
          onPress={() => presses.push('press')}
        />
      </View>
      {/* The same label with no button around it — the baseline the padding is measured against. */}
      <View style={{alignSelf: 'flex-start'}} onLayout={e => (bare = e.layout)}>
        <Text>Save</Text>
      </View>
    </View>
  );

  root.render(<App disabled={false} />);
  check(box != null, 'Button mounted (wrapper onLayout fired)');
  check(
    box != null && box.width > 0 && box.height > 0,
    `Button sized itself to its label (${box && box.width}x${box && box.height})`,
  );
  // padding: 8 on all four sides, set on the label exactly as RN sets it — which is only meaningful
  // because an auto-sized text node now grows around its own padding (engine/layout/layout_engine.c).
  check(
    box != null &&
      bare != null &&
      box.width === bare.width + 16 &&
      box.height === bare.height + 16,
    `Button pads its label on all four sides (${box && box.width}x${box && box.height} around ${bare && bare.width}x${bare && bare.height})`,
  );

  tap(box);
  check(presses.length === 1, 'Button fired onPress on a tap');

  root.render(<App disabled={true} />);
  tap(box);
  check(presses.length === 1, 'a disabled Button ignores the same tap');

  root.render(<App disabled={false} />);
  tap(box);
  check(presses.length === 2, 're-enabling the Button restores onPress');
}

// ====================================================================================================
// 2. <ImageBackground> — the image fills the container, the children lay out over it
// ====================================================================================================
{
  const seen = {};
  const refs = {};

  root.render(
    <ImageBackground
      source="logo"
      resizeMode="cover"
      style={{width: 200, height: 120, padding: 10}}
      imageStyle={{opacity: 0.5}}
      onLayout={e => (seen.image = e.layout)}
      ref={h => (refs.view = h)}
      imageRef={h => (refs.image = h)}>
      <Text style={{height: 20}} onLayout={e => (seen.child = e.layout)}>
        on top
      </Text>
    </ImageBackground>,
  );

  check(
    seen.image != null,
    'ImageBackground mounted its Image (onLayout fired)',
  );
  // The absolute fill resolves against the container's CONTENT box, so padding insets it.
  check(
    seen.image && seen.image.width === 180 && seen.image.height === 100,
    `the image fills the container's content box (got ${seen.image && seen.image.width}x${seen.image && seen.image.height}, want 180x100)`,
  );
  check(
    seen.child != null && seen.child.width === 180,
    'the child lays out against the container, not against the absolute image',
  );
  check(
    typeof refs.view === 'number' && typeof refs.image === 'number',
    'ref and imageRef both resolved to engine node handles',
  );
  check(refs.view !== refs.image, 'ref and imageRef point at different nodes');
}

// ====================================================================================================
// 3. <SectionList> — headers, rows and footers as flat siblings of one scroller
// ====================================================================================================
{
  const ys = [];
  const row = label => (
    <Text
      key={label}
      style={{height: 20}}
      onLayout={e => ys.push([label, e.layout.y])}>
      {label}
    </Text>
  );

  const sections = [
    {title: 'A', data: ['ant', 'ape']},
    {title: 'B', data: ['bee']},
  ];

  root.render(
    <View style={{width: 200, height: 300}}>
      <SectionList
        style={{flex: 1}}
        sections={sections}
        renderSectionHeader={({section}) => row(`#${section.title}`)}
        renderSectionFooter={({section}) => row(`/${section.title}`)}
        renderItem={({item}) => row(item)}
      />
    </View>,
  );

  const order = ys.map(e => e[0]).join(',');
  check(
    order === '#A,ant,ape,/A,#B,bee,/B',
    `sections laid out header-rows-footer in order (got ${order})`,
  );
  const tops = ys.map(e => e[1]);
  check(
    tops.every((y, i) => i === 0 || y > tops[i - 1]),
    `every row stacked below the one before it (${tops.join(',')})`,
  );
  check(
    tops.length === 7 && tops[1] - tops[0] === 20,
    'rows are siblings of the scroller — no wrapper node per section',
  );

  // A data change re-renders through the same scroller without crashing.
  root.render(
    <View style={{width: 200, height: 300}}>
      <SectionList
        style={{flex: 1}}
        sections={[{title: 'A', data: ['ant']}]}
        renderSectionHeader={({section}) => row(`#${section.title}`)}
        renderItem={({item}) => row(item)}
      />
    </View>,
  );
  check(true, 'SectionList re-rendered with fewer sections without crashing');
}

report('wrappers');
