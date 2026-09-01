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
import {FlatList} from '../FlatList.js';

// FlatList is a plain component, so calling it IS the render — no reconciler needed. What matters is the
// element tree it returns: a ScrollView whose children are `data` mapped through `renderItem`, matching
// the AOT's compile-time rewrite (see the "AOT FlatList" block in aot/__tests__/compile.unit.test.js).

const Row = props => createElement('Text', props);
const render = props => FlatList(props);

afterEach(() => vi.restoreAllMocks());

describe('FlatList renders as a ScrollView + .map', () => {
  it('returns a ScrollView holding one child per data item', () => {
    const el = render({
      data: [{id: 'a'}, {id: 'b'}, {id: 'c'}],
      renderItem: ({item}) => createElement(Row, {label: item.id}),
    });
    expect(el.type).toBe('ScrollView');
    expect(el.props.children).toHaveLength(3);
    expect(el.props.children.map(c => c.props.label)).toEqual(['a', 'b', 'c']);
  });

  it('passes item and index to renderItem', () => {
    const seen = [];
    render({
      data: ['x', 'y'],
      renderItem: info => {
        seen.push(info);
        return createElement(Row, null);
      },
    });
    expect(seen).toEqual([
      {item: 'x', index: 0},
      {item: 'y', index: 1},
    ]);
  });

  it('forwards style to the ScrollView and nothing else', () => {
    const style = {flex: 1};
    const el = render({data: [], renderItem: () => null, style});
    expect(el.props.style).toBe(style);
  });

  it('omits style entirely when none was given', () => {
    const el = render({data: [], renderItem: () => null});
    expect('style' in el.props).toBe(false);
  });

  it('renders an empty scroller for missing / non-array data', () => {
    for (const data of [undefined, null, 0, 'abc', {length: 2}]) {
      const el = render({data, renderItem: () => createElement(Row, null)});
      expect(el.type).toBe('ScrollView');
      expect(el.props.children).toEqual([]);
    }
  });

  it('renders an empty scroller when renderItem is missing', () => {
    expect(render({data: [1, 2, 3]}).props.children).toEqual([]);
  });

  // React always hands a component an object, but the empty-scroller contract should hold for a
  // direct call too rather than throwing at the destructure.
  it('renders an empty scroller when called with no props at all', () => {
    for (const props of [undefined, {}]) {
      const el = FlatList(props);
      expect(el.type).toBe('ScrollView');
      expect(el.props.children).toEqual([]);
    }
  });
});

describe('FlatList keying', () => {
  it('keys each row from keyExtractor', () => {
    const el = render({
      data: [{id: 7}, {id: 9}],
      keyExtractor: item => item.id,
      renderItem: ({item}) => createElement(Row, {label: item.id}),
    });
    expect(el.props.children.map(c => c.key)).toEqual(['7', '9']);
  });

  it('passes item and index to keyExtractor', () => {
    const seen = [];
    render({
      data: ['x', 'y'],
      keyExtractor: (item, index) => {
        seen.push([item, index]);
        return item;
      },
      renderItem: () => createElement(Row, null),
    });
    expect(seen).toEqual([
      ['x', 0],
      ['y', 1],
    ]);
  });

  it('keeps a key renderItem already set when there is no keyExtractor', () => {
    const el = render({
      data: ['a', 'b'],
      renderItem: ({item}) => createElement(Row, {key: `row-${item}`}),
    });
    expect(el.props.children.map(c => c.key)).toEqual(['row-a', 'row-b']);
  });

  it('lets keyExtractor override a key renderItem set', () => {
    const el = render({
      data: ['a'],
      keyExtractor: item => `k-${item}`,
      renderItem: ({item}) => createElement(Row, {key: `row-${item}`}),
    });
    expect(el.props.children[0].key).toBe('k-a');
  });

  it('falls back to the index when nothing supplies a key', () => {
    const el = render({
      data: ['a', 'b'],
      renderItem: () => createElement(Row, null),
    });
    expect(el.props.children.map(c => c.key)).toEqual(['0', '1']);
  });

  it('preserves row props when it clones to attach a key', () => {
    const el = render({
      data: [{id: 'a'}],
      keyExtractor: item => item.id,
      renderItem: ({item}) => createElement(Row, {label: item.id, bold: true}),
    });
    expect(el.props.children[0].props).toEqual({label: 'a', bold: true});
  });
});

describe('FlatList row results that are not elements', () => {
  it('drops rows that render to null / undefined / false', () => {
    const el = render({
      data: [1, 2, 3, 4],
      renderItem: ({item}) =>
        item === 1
          ? null
          : item === 2
            ? undefined
            : item === 3
              ? false
              : createElement(Row, null),
    });
    expect(el.props.children).toHaveLength(1);
  });

  it('does not ask keyExtractor for the key of a row that rendered nothing', () => {
    const seen = [];
    render({
      data: ['a', 'b'],
      keyExtractor: item => {
        seen.push(item);
        return item;
      },
      renderItem: ({item}) => (item === 'a' ? null : createElement(Row, null)),
    });
    expect(seen).toEqual(['b']);
  });

  it('keeps a raw string row as-is (no key to clone onto)', () => {
    const el = render({data: ['hi'], renderItem: ({item}) => item});
    expect(el.props.children).toEqual(['hi']);
  });

  it('does not ask keyExtractor for the key of a raw string row', () => {
    const seen = [];
    render({
      data: ['a', 'b'],
      keyExtractor: item => {
        seen.push(item);
        return item;
      },
      renderItem: ({item}) => (item === 'a' ? item : createElement(Row, null)),
    });
    expect(seen).toEqual(['b']);
  });
});

describe('FlatList warns about props neither flow honours', () => {
  it('names the ignored props and stays quiet for the supported four', () => {
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => {});
    render({data: [], renderItem: () => null, keyExtractor: x => x, style: {}});
    expect(warn).not.toHaveBeenCalled();

    render({
      data: [],
      renderItem: () => null,
      horizontal: true,
      onEndReached: () => {},
    });
    expect(warn).toHaveBeenCalledTimes(1);
    expect(warn.mock.calls[0][0]).toContain('horizontal');
    expect(warn.mock.calls[0][0]).toContain('onEndReached');

    // Warn-once: a screen full of misconfigured lists must not flood the device console.
    render({data: [], renderItem: () => null, numColumns: 2});
    render({data: [], renderItem: () => null, windowSize: 5});
    expect(warn).toHaveBeenCalledTimes(1);
  });
});
