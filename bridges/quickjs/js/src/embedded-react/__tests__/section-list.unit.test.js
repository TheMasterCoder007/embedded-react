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

import {describe, it, expect, vi, afterEach} from 'vitest';
import {createElement} from 'react';
import {SectionList} from '../SectionList.js';

// SectionList is a plain component, so calling it IS the render. What matters is the element tree: one
// ScrollView whose children are every section's header, rows and footer as FLAT siblings — and the keys,
// which have to stay unique across sections because of exactly that flattening.

const Row = props => createElement('Text', props);
const render = props => SectionList(props);
const labels = el => el.props.children.map(c => c.props.label);
const keys = el => el.props.children.map(c => c.key);

const SECTIONS = [
  {title: 'A', data: ['ant', 'ape']},
  {title: 'B', data: ['bee']},
];
const header = ({section}) => createElement(Row, {label: `#${section.title}`});
const footer = ({section}) => createElement(Row, {label: `/${section.title}`});
const row = ({item}) => createElement(Row, {label: item});

afterEach(() => vi.restoreAllMocks());

describe('SectionList renders as a ScrollView of flat siblings', () => {
  it('interleaves headers, rows and footers in source order', () => {
    const el = render({
      sections: SECTIONS,
      renderSectionHeader: header,
      renderSectionFooter: footer,
      renderItem: row,
    });
    expect(el.type).toBe('ScrollView');
    expect(labels(el)).toEqual(['#A', 'ant', 'ape', '/A', '#B', 'bee', '/B']);
  });

  it('wraps nothing around a section — the rows are direct children', () => {
    const el = render({sections: SECTIONS, renderItem: row});
    expect(labels(el)).toEqual(['ant', 'ape', 'bee']);
    for (const child of el.props.children) expect(child.type).toBe(Row);
  });

  it('omits a header / footer that was not asked for', () => {
    expect(labels(render({sections: SECTIONS, renderItem: row}))).toEqual([
      'ant',
      'ape',
      'bee',
    ]);
  });

  it('passes item, index and the section to renderItem', () => {
    const seen = [];
    render({
      sections: SECTIONS,
      renderItem: info => {
        seen.push([info.item, info.index, info.section.title]);
        return createElement(Row, null);
      },
    });
    expect(seen).toEqual([
      ['ant', 0, 'A'],
      ['ape', 1, 'A'],
      ['bee', 0, 'B'],
    ]);
  });

  it('gives the header and footer the whole section object', () => {
    const seen = [];
    render({
      sections: SECTIONS,
      renderSectionHeader: ({section}) => {
        seen.push(section);
        return null;
      },
      renderItem: row,
    });
    expect(seen).toEqual(SECTIONS);
  });

  it('forwards style to the ScrollView, and omits it when absent', () => {
    const style = {flex: 1};
    expect(render({sections: [], style}).props.style).toBe(style);
    expect('style' in render({sections: []}).props).toBe(false);
  });

  it('renders an empty scroller for missing / malformed input', () => {
    for (const sections of [undefined, null, 0, 'abc', {length: 2}]) {
      const el = render({sections, renderItem: row});
      expect(el.type).toBe('ScrollView');
      expect(el.props.children).toEqual([]);
    }
    expect(SectionList().props.children).toEqual([]);
    expect(SectionList({}).props.children).toEqual([]);
  });

  it('skips a null section and a section with no data array', () => {
    const el = render({
      sections: [null, 'nope', {title: 'C'}, {title: 'D', data: ['d']}],
      renderItem: row,
    });
    expect(labels(el)).toEqual(['d']);
  });

  it('still renders a section header when the section has no rows', () => {
    const el = render({
      sections: [{title: 'E', data: []}],
      renderSectionHeader: header,
      renderItem: row,
    });
    expect(labels(el)).toEqual(['#E']);
  });
});

describe('SectionList keying', () => {
  it('namespaces row keys by section, so equal indices do not collide', () => {
    const el = render({sections: SECTIONS, renderItem: row});
    expect(keys(el)).toEqual(['0:0', '0:1', '1:0']);
  });

  it('uses a section `key` in place of its index', () => {
    const el = render({
      sections: [{key: 'letters', data: ['a']}],
      renderSectionHeader: header,
      renderSectionFooter: footer,
      renderItem: row,
    });
    expect(keys(el)).toEqual([
      'letters:$header',
      'letters:0',
      'letters:$footer',
    ]);
  });

  it('keys rows from keyExtractor, still namespaced', () => {
    const el = render({
      sections: [{data: [{id: 7}, {id: 9}]}],
      keyExtractor: item => item.id,
      renderItem: ({item}) => createElement(Row, {label: item.id}),
    });
    expect(keys(el)).toEqual(['0:7', '0:9']);
  });

  it('keeps a key renderItem already set when there is no keyExtractor', () => {
    const el = render({
      sections: [{data: ['a']}],
      renderItem: ({item}) => createElement(Row, {key: `row-${item}`}),
    });
    expect(keys(el)).toEqual(['0:row-a']);
  });

  it('preserves row props when it clones to attach a key', () => {
    const el = render({
      sections: [{data: ['a']}],
      renderItem: ({item}) => createElement(Row, {label: item, bold: true}),
    });
    expect(el.props.children[0].props).toEqual({label: 'a', bold: true});
  });
});

describe('SectionList per-section overrides', () => {
  it('lets a section supply its own renderItem', () => {
    const el = render({
      sections: [
        {title: 'A', data: ['ant']},
        {
          title: 'B',
          data: ['bee'],
          renderItem: ({item}) => createElement(Row, {label: `!${item}`}),
        },
      ],
      renderItem: row,
    });
    expect(labels(el)).toEqual(['ant', '!bee']);
  });

  it('lets a section supply its own keyExtractor', () => {
    const el = render({
      sections: [{data: ['a'], keyExtractor: item => `s-${item}`}],
      keyExtractor: item => `g-${item}`,
      renderItem: row,
    });
    expect(keys(el)).toEqual(['0:s-a']);
  });

  it('renders nothing for a section with data but no renderer anywhere', () => {
    expect(render({sections: SECTIONS}).props.children).toEqual([]);
  });
});

describe('SectionList rows that are not elements', () => {
  it('drops rows and headers that render to null / undefined / false', () => {
    const el = render({
      sections: [{data: [1, 2, 3, 4]}],
      renderSectionHeader: () => null,
      renderSectionFooter: () => false,
      renderItem: ({item}) =>
        item === 1
          ? null
          : item === 2
            ? undefined
            : item === 3
              ? false
              : createElement(Row, {label: 'kept'}),
    });
    expect(labels(el)).toEqual(['kept']);
  });

  it('does not ask keyExtractor for the key of a row that rendered nothing', () => {
    const seen = [];
    render({
      sections: [{data: ['a', 'b']}],
      keyExtractor: item => {
        seen.push(item);
        return item;
      },
      renderItem: ({item}) => (item === 'a' ? null : createElement(Row, null)),
    });
    expect(seen).toEqual(['b']);
  });

  it('keeps a raw string row as-is (no key to clone onto)', () => {
    const el = render({
      sections: [{data: ['hi']}],
      renderItem: ({item}) => item,
    });
    expect(el.props.children).toEqual(['hi']);
  });

  it('does not ask keyExtractor for the key of a raw string row', () => {
    const seen = [];
    render({
      sections: [{data: ['a', 'b']}],
      keyExtractor: item => {
        seen.push(item);
        return item;
      },
      renderItem: ({item}) => (item === 'a' ? item : createElement(Row, null)),
    });
    expect(seen).toEqual(['b']);
  });
});

describe('SectionList prop warnings', () => {
  // The warner latches ONCE per module, so the silent case has to be asserted first.
  it('stays quiet for the six props it honours, and for RN platform no-ops', () => {
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => {});
    render({
      sections: [],
      renderItem: row,
      renderSectionHeader: header,
      renderSectionFooter: footer,
      keyExtractor: x => x,
      style: {},
    });
    // A list ported from RN carries these; they need an OS this board hasn't got, so they are
    // dropped without a word rather than making the app strip them (the set <Button> quiets too).
    render({
      sections: [],
      renderItem: row,
      accessibilityLabel: 'Contacts',
      testID: 'contact-list',
      importantForAccessibility: 'yes',
    });
    expect(warn).not.toHaveBeenCalled();
  });

  it('warns once about props neither this nor the AOT honours', () => {
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => {});
    render({
      sections: [],
      renderItem: row,
      stickySectionHeadersEnabled: true,
      ItemSeparatorComponent: Row,
    });
    expect(warn).toHaveBeenCalledTimes(1);
    expect(warn.mock.calls[0][0]).toContain('stickySectionHeadersEnabled');
    expect(warn.mock.calls[0][0]).toContain('ItemSeparatorComponent');

    render({sections: [], renderItem: row, onEndReached: () => {}});
    expect(warn).toHaveBeenCalledTimes(1);
  });
});
