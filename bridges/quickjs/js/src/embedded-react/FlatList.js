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
//   <FlatList data={items} renderItem={({item}) => <Row item={item} />} />
//     renders exactly
//   <ScrollView>{items.map((item, index) => <Row item={item} />)}</ScrollView>
//
// There is no windowing: every row mounts as a real engine node and stays mounted, so the list costs
// data.length x (nodes per row) slots out of the fixed ERUI_MAX_NODES pool. See "FlatList is a
// ScrollView alias" in the top-level README for the limits and when to reach for something else.
//
// This mirrors the AOT's emitFlatList (bridges/quickjs/js/aot/compile.mjs), which performs the same
// rewrite at compile time. The two flows accept the SAME four props on purpose — a list that renders
// in the simulator must also compile for the device, so anything Flow B rejects is warned about here
// rather than silently forwarded.
import {createElement, cloneElement} from 'react';
import {ScrollView} from './components.js';

/** The only props both flows honour. Everything else is a virtualization/platform knob with no meaning. */
const SUPPORTED = ['data', 'renderItem', 'keyExtractor', 'style'];

let _warnedProps = false;

/**
 * Warns once about props that do nothing here and that the AOT refuses to compile, so a list built in
 * the simulator doesn't fail the device build later. Scans nothing once it has warned — a list
 * re-renders on every data change, and this runs on the Flow A commit path.
 *
 * @param {object} props The full prop object.
 */
function warnUnsupportedProps(props) {
  if (_warnedProps) return;
  const names = Object.keys(props).filter(k => !SUPPORTED.includes(k));
  if (names.length === 0) return;
  _warnedProps = true;
  console.warn(
    `embedded-react: <FlatList> ignores ${names.join(', ')} — it is a thin <ScrollView> alias with no ` +
      `virtualization, and the AOT rejects these props outright ("AOT: <FlatList> prop ... is not supported"). ` +
      `For headers/footers/separators/horizontal/onEndReached, use <ScrollView> + .map directly; ` +
      `supported here: ${SUPPORTED.join(', ')}.`,
  );
}

/**
 * Renders `data` through `renderItem` into a <ScrollView>. Rows that render to nothing (null / false)
 * are dropped, matching a plain `.map` + conditional.
 *
 * @param {object} props
 * @param {Array} [props.data] Row data. A non-array (or missing) renders an empty scroller.
 * @param {(info: {item: *, index: number}) => *} [props.renderItem] Row renderer, called per item.
 * @param {(item: *, index: number) => string|number} [props.keyExtractor] React key per row; defaults
 *   to the key `renderItem` already set, else the index.
 * @param {object} [props.style] Forwarded to the ScrollView untouched.
 * @returns {*} A <ScrollView> element holding one child per rendered row.
 */
export function FlatList(props) {
  const {data, renderItem, keyExtractor, style} = props;

  warnUnsupportedProps(props);

  const rows = [];
  if (Array.isArray(data) && typeof renderItem === 'function') {
    for (let index = 0; index < data.length; index++) {
      const item = data[index];
      const row = renderItem({item, index});
      // A row may legitimately render nothing; keying a non-element (a raw string) is not possible.
      if (row === null || row === undefined || row === false) continue;
      if (typeof row !== 'object' || row.$$typeof === undefined) {
        rows.push(row);
        continue;
      }
      const key = keyExtractor
        ? String(keyExtractor(item, index))
        : row.key !== null && row.key !== undefined
          ? row.key
          : String(index);
      rows.push(row.key === key ? row : cloneElement(row, {key}));
    }
  }

  return createElement(ScrollView, style === undefined ? null : {style}, rows);
}
