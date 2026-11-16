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

// Runtime e2e: scrollTo() scrolls the ScrollView behind a ref, clamps to the scroll range,
// fires onScroll, and returns where the offset landed.
import {createRoot} from '../../src/renderer.mts';
import {View, ScrollView, scrollTo} from 'embedded-react';
import type {NodeHandle} from 'embedded-react';
import {check, report} from './harness.ts';

const handles: {list: NodeHandle | null; row: NodeHandle | null} = {
  list: null,
  row: null,
};
const scrolled: number[] = [];

// Five 100 px rows in a 200 px viewport: the list scrolls 0..300 vertically.
const root = createRoot({width: screen.width, height: screen.height});
root.render(
  <ScrollView
    ref={(handle: NodeHandle | null) => (handles.list = handle)}
    style={{width: 200, height: 200}}
    onScroll={event => scrolled.push(event.scrollY)}>
    {[0, 1, 2, 3, 4].map(rowIndex => (
      <View
        key={rowIndex}
        ref={
          rowIndex === 0
            ? (handle: NodeHandle | null) => (handles.row = handle)
            : null
        }
        style={{width: 200, height: 100}}
      />
    ))}
  </ScrollView>,
);

const start = scrollTo(handles.list);
check(
  start?.x === 0 && start.y === 0 && start.maxX === 0 && start.maxY === 300,
  `no offset reads the position and range (${JSON.stringify(start)})`,
);
check(scrolled.length === 0, 'reading fires no onScroll');

const middle = scrollTo(handles.list, {y: 120});
check(middle?.y === 120, `scrolls to y=120 (got ${middle?.y})`);
check(
  scrolled.length === 1 && scrolled[0] === 120,
  `onScroll fired once with 120 (got ${JSON.stringify(scrolled)})`,
);

const end = scrollTo(handles.list, {y: 999});
check(end?.y === 300, `clamps to the end of the range (got ${end?.y})`);

const top = scrollTo(handles.list, {y: -50});
check(top?.y === 0, `clamps to the start of the range (got ${top?.y})`);

scrollTo(handles.list, {y: 80});
const kept = scrollTo(handles.list, {x: 0});
check(kept?.y === 80, `an axis left out stays where it is (got ${kept?.y})`);

check(
  scrollTo(handles.row, {y: 10}) === undefined,
  'a node that does not scroll returns undefined',
);
check(scrollTo(-1, {y: 10}) === undefined, 'a bad handle returns undefined');

check(scrollTo(null, {y: 10}) === undefined, 'a null ref returns undefined');
check(
  (
    globalThis as unknown as {NativeUI: {scrollTo(): unknown}}
  ).NativeUI.scrollTo() === undefined,
  'the bridge method with no handle returns undefined',
);

// An onScroll that unmounts the list: the scroll's own node is gone by the time its position is read.
let gone: NodeHandle | null = null;
const unmounting = createRoot({width: screen.width, height: screen.height});
const Unmounting = ({shown}: {shown: boolean}) => (
  <View>
    {shown && (
      <ScrollView
        ref={(handle: NodeHandle | null) => {
          if (handle != null) gone = handle;
        }}
        style={{width: 200, height: 200}}
        onScroll={() => unmounting.render(<Unmounting shown={false} />)}>
        <View style={{width: 200, height: 500}} />
      </ScrollView>
    )}
  </View>
);
unmounting.render(<Unmounting shown={true} />);
check(
  scrollTo(gone, {y: 50}) === undefined,
  'a list its own onScroll unmounted returns undefined',
);

report('scroll-to');
