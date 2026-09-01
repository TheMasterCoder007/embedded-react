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

// SectionList — a thin API-compat wrapper over <ScrollView>, NOT a virtualized list. It is <FlatList>
// with a header and a footer around each group:
//
//   <SectionList sections={[{title: 'A', data: ['ant']}]}
//                renderSectionHeader={({section}) => <Header title={section.title} />}
//                renderItem={({item}) => <Row item={item} />} />
//     renders the same tree as
//   <ScrollView><Header title="A" /><Row item="ant" /></ScrollView>
//
// Header, rows and footer are FLAT SIBLINGS, as in RN — no wrapper node per section — so a section
// costs exactly the nodes it draws. Nothing sticks: `stickySectionHeadersEnabled` needs a scroll
// listener re-laying the header out every frame, which is the per-event JS cost Flow A can least
// afford.
//
// Everything the FlatList section of the README says about the node budget applies here and then
// some: a section adds its header and footer on top of its rows, and every one of them mounts and
// stays mounted. Size `sections x (1 + rows) x nodes-per-row` against ERUI_MAX_NODES before writing
// the screen.
//
// Unlike <FlatList>, this has no Flow B counterpart yet — the AOT unrolls one `.map` per container
// and a section expands to a header AND a variable number of rows. It is a Flow A / simulator
// component until that lands; the AOT says so by name rather than "unknown element".
import {createElement} from 'react';
import {ScrollView} from './components.js';
import {isElement, pushKeyedChild, rendersNothing} from './list-child.js';
import {createPropWarner} from './warn-props.js';

/** The props this honours. The rest are virtualization / platform knobs with no meaning on a panel. */
const SUPPORTED = [
  'sections',
  'renderItem',
  'renderSectionHeader',
  'renderSectionFooter',
  'keyExtractor',
  'style',
];

const warnUnsupportedProps = createPropWarner(
  'SectionList',
  SUPPORTED,
  'it is a thin <ScrollView> alias with no virtualization and no sticky headers. For separators, ' +
    'list headers/footers or onEndReached, use <ScrollView> + .map directly.',
);

/**
 * Renders `sections` into a <ScrollView>, each as an optional header, its rows, and an optional footer.
 *
 * Row keys are namespaced by section (`<section>:<row>`), because the rows of every section end up as
 * siblings of one another — an index or an id that is only unique WITHIN a section would collide.
 *
 * @param {object} [props] Missing / empty renders an empty scroller, like a missing `sections`.
 * @param {Array} [props.sections] Sections, each `{data, key?, renderItem?, keyExtractor?, ...}`.
 *   Anything else on a section (RN's convention is `title`) is yours, and reaches the render callbacks.
 * @param {(info: {item: *, index: number, section: object}) => *} [props.renderItem] Row renderer.
 *   A section's own `renderItem` wins for that section, as in RN.
 * @param {(info: {section: object}) => *} [props.renderSectionHeader] Rendered before a section's rows.
 * @param {(info: {section: object}) => *} [props.renderSectionFooter] Rendered after a section's rows.
 * @param {(item: *, index: number) => string|number} [props.keyExtractor] React key per row (a
 *   section's own `keyExtractor` wins); defaults to the key `renderItem` already set, else the index.
 * @param {object} [props.style] Forwarded to the ScrollView untouched.
 * @returns {*} A <ScrollView> holding every section's header, rows and footer as flat children.
 */
export function SectionList(props = {}) {
  const {
    sections,
    renderItem,
    renderSectionHeader,
    renderSectionFooter,
    keyExtractor,
    style,
  } = props;

  warnUnsupportedProps(props);

  const children = [];
  if (Array.isArray(sections)) {
    for (let s = 0; s < sections.length; s++) {
      const section = sections[s];
      if (section === null || typeof section !== 'object') continue;
      const sectionKey =
        section.key === null || section.key === undefined
          ? String(s)
          : String(section.key);

      if (typeof renderSectionHeader === 'function')
        pushKeyedChild(
          children,
          renderSectionHeader({section}),
          `${sectionKey}:$header`,
        );

      const rowRenderer =
        typeof section.renderItem === 'function'
          ? section.renderItem
          : renderItem;
      const rowKey =
        typeof section.keyExtractor === 'function'
          ? section.keyExtractor
          : keyExtractor;
      if (Array.isArray(section.data) && typeof rowRenderer === 'function') {
        for (let index = 0; index < section.data.length; index++) {
          const item = section.data[index];
          const row = rowRenderer({item, index, section});
          // Drop a row that rendered nothing before keying it — keyExtractor is never asked for the
          // key of something that isn't there.
          if (rendersNothing(row)) continue;
          const key = rowKey
            ? String(rowKey(item, index))
            : isElement(row) && row.key !== null && row.key !== undefined
              ? row.key
              : String(index);
          pushKeyedChild(children, row, `${sectionKey}:${key}`);
        }
      }

      if (typeof renderSectionFooter === 'function')
        pushKeyedChild(
          children,
          renderSectionFooter({section}),
          `${sectionKey}:$footer`,
        );
    }
  }

  return createElement(
    ScrollView,
    style === undefined ? null : {style},
    children,
  );
}
