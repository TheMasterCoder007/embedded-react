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

// FlatList — a thin API-compat wrapper over <ScrollView>, NOT a virtualized list.
//
//   <FlatList data={items} keyExtractor={(it) => it.id}
//             renderItem={({item}) => <Row item={item} />} />
//     renders the same tree as
//   <ScrollView>{items.map((item) => <Row key={item.id} item={item} />)}</ScrollView>
//
// Keys are the one part that is not literal: this component attaches each row's key itself (see
// below), so callers never write one. Flow B ignores keys entirely — it unrolls at compile time.
//
// There is no windowing: every row mounts as a real engine node and stays mounted, so the list costs
// data.length x (nodes per row) slots out of the fixed ERUI_MAX_NODES pool. See "FlatList is a
// ScrollView alias" in the top-level README for the limits and when to reach for something else.
//
// This mirrors the AOT's emitFlatList (bridges/quickjs/js/aot/compile.mjs), which performs the same
// rewrite at compile time. The two flows accept the SAME four props on purpose — a list that renders
// in the simulator must also compile for the device, so anything Flow B rejects is warned about here
// rather than silently forwarded.
import {createElement} from 'react';
import {ScrollView} from './components.js';
import {pushKeyedChild} from './list-child.js';
import {createPropWarner} from './warn-props.js';

/** The only props both flows honour. Everything else is a virtualization/platform knob with no meaning. */
const SUPPORTED = ['data', 'renderItem', 'keyExtractor', 'style'];

// Warns once about props that do nothing here and that the AOT refuses to compile, so a list built in
// the simulator doesn't fail the device build later.
const warnUnsupportedProps = createPropWarner(
  'FlatList',
  SUPPORTED,
  'it is a thin <ScrollView> alias with no virtualization, and the AOT rejects these props outright ' +
    '("AOT: <FlatList> prop ... is not supported"). For headers/footers/separators/horizontal/' +
    'onEndReached, use <ScrollView> + .map directly.',
);

/**
 * Renders `data` through `renderItem` into a <ScrollView>. Rows that render to nothing (null / false)
 * are dropped, matching a plain `.map` + conditional.
 *
 * @param {object} [props] Missing / empty renders an empty scroller, like a missing `data`.
 * @param {Array} [props.data] Row data. A non-array (or missing) renders an empty scroller.
 * @param {(info: {item: *, index: number}) => *} [props.renderItem] Row renderer, called per item.
 * @param {(item: *, index: number) => string|number} [props.keyExtractor] React key per row; defaults
 *   to the key `renderItem` already set, else the index.
 * @param {object} [props.style] Forwarded to the ScrollView untouched.
 * @returns {*} A <ScrollView> element holding one child per rendered row.
 */
export function FlatList(props = {}) {
  const {data, renderItem, keyExtractor, style} = props;

  warnUnsupportedProps(props);

  const rows = [];
  if (Array.isArray(data) && typeof renderItem === 'function') {
    for (let index = 0; index < data.length; index++) {
      const item = data[index];
      const row = renderItem({item, index});
      // The key is built lazily: pushKeyedChild calls this only for a row that can hold one, so
      // keyExtractor never runs for a row that rendered nothing or for a raw string.
      pushKeyedChild(rows, row, el =>
        keyExtractor
          ? String(keyExtractor(item, index))
          : el.key !== null && el.key !== undefined
            ? el.key
            : String(index),
      );
    }
  }

  return createElement(ScrollView, style === undefined ? null : {style}, rows);
}
