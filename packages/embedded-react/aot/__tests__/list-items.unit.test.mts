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
import {compilePackage} from '../compile.mts';
import {
  fieldsUnder,
  inferItemStruct,
  itemValueAt,
  rowEntries,
} from '../compiler/list-items.mts';

// The fields of a list's items, and the list forms that aren't a list state's own `.map`.

const SCREEN = {screen: {width: 240, height: 320}};

/** An app module around a component body, the JSX it returns, and what precedes App. */
const appOf = (body: string, jsx: string, before = ''): string =>
  "import {useMemo, useState} from 'react';\n" +
  "import {Pressable, Text, View} from 'embedded-react';\n" +
  `${before}\nexport function App() {\n${body}\n  return ${jsx};\n}`;

const compiles = (source: string) => () =>
  compilePackage(source, 'app', SCREEN);

describe('item fields', () => {
  it('takes the union of the items, nested objects flattened, and marks what some leave out', () => {
    const struct = inferItemStruct(
      [
        {title: 'a', pos: {x: 1, y: 2}, note: 'n'},
        {title: 'b', pos: {x: 3}, done: true, note: null},
      ],
      'list',
    );
    expect(
      struct.fields.map(({key, kind, optional}) => ({key, kind, optional})),
    ).toEqual([
      {key: 'title', kind: 'string', optional: undefined},
      {key: 'pos.x', kind: 'int', optional: undefined},
      {key: 'pos.y', kind: 'int', optional: true},
      {key: 'note', kind: 'string', optional: true},
      {key: 'done', kind: 'bool', optional: true},
    ]);
    // Values first, then a flag for each optional field.
    expect(
      rowEntries(struct).map(
        ({field, presence}) => `${field.key}${presence ? '?' : ''}`,
      ),
    ).toEqual([
      'title',
      'pos.x',
      'pos.y',
      'note',
      'done',
      'pos.y?',
      'note?',
      'done?',
    ]);
    expect(fieldsUnder(struct, ['pos']).map(field => field.key)).toEqual([
      'pos.x',
      'pos.y',
    ]);
  });

  it('reads a field along its path, missing where the item or an object on the way leaves it out', () => {
    const struct = inferItemStruct([{pos: {x: 1}}, {}], 'list');
    const [x] = struct.fields;
    expect(itemValueAt({pos: {x: 4}}, x)).toEqual({present: true, value: 4});
    expect(itemValueAt({pos: {}}, x).present).toBe(false);
    expect(itemValueAt({}, x).present).toBe(false);
    expect(itemValueAt({pos: {x: null}}, x).present).toBe(false);
  });

  it('keeps plain values as one field, a fraction when any value is one', () => {
    expect(inferItemStruct([1, 2.5], 'list')).toEqual({
      fields: [{key: '', path: [], kind: 'float'}],
      plain: true,
    });
    expect(inferItemStruct([1, 2], 'list').fields[0].kind).toBe('int');
    expect(inferItemStruct([{n: 1}, {n: 2.6}], 'list').fields[0].kind).toBe(
      'float',
    );
    expect(inferItemStruct([true, false], 'list').fields[0].kind).toBe('bool');
  });

  it('refuses items it has no fields for', () => {
    expect(() => inferItemStruct([], 'list')).toThrow(
      'needs ≥1 initial element',
    );
    expect(() => inferItemStruct([{a: 1}, 2], 'list')).toThrow(
      'elements must all be objects',
    );
    expect(() => inferItemStruct([{a: {}}], 'list')).toThrow(
      'field "a" of list is an empty object',
    );
    expect(() => inferItemStruct([{a: 1}, {a: {b: 1}}], 'list')).toThrow(
      'is a value in one item and an object in another',
    );
    expect(() => inferItemStruct([{a: null}], 'list')).toThrow(
      'field "a" of list is null in every item',
    );
    expect(() => inferItemStruct([{a: true}, {a: 1}], 'list')).toThrow(
      'field "a" of list is a boolean in one item and not in another',
    );
  });
});

describe('lists that are not a list state of the component', () => {
  it('passes a list state, a row item and an item by index to a component', () => {
    const before = `
function Name({task}) { return <Text>{task.title} {task.done ? 'y' : 'n'}</Text>; }
function Names({list}) { return <View>{list.map((task, index) => <Name key={index} task={task} />)}</View>; }`;
    const source = appOf(
      "  const [tasks] = useState([{title: 'a', done: false}]);\n  const [at, setAt] = useState(0);",
      '<View><Names list={tasks} /><Name task={tasks[at]} /><Text>{tasks.length}</Text></View>',
      before,
    );
    expect(compiles(source)).not.toThrow();
  });

  it('unrolls an array built in render: inline, a const, a memo and a prop', () => {
    const before =
      'function Rows({rows}) { return <View>{rows.map(row => <Text key={row.k}>{row.k} {row.v}</Text>)}</View>; }';
    const source = appOf(
      "  const [n, setN] = useState(1);\n  const facts = [{k: 'a', v: n}, {k: 'b', v: 2}];\n" +
        '  const memo = useMemo(() => [n, n * 2], [n]);',
      "<View><Rows rows={[{k: 'p', v: n}]} />{facts.map(fact => <Text key={fact.k}>{fact.v}</Text>)}" +
        '{memo.map((value, index) => <Text key={index}>{value}</Text>)}' +
        "{[{on: n > 1}].map((flag, index) => <Text key={index}>{flag.on ? 'y' : 'n'}</Text>)}</View>",
      before,
    );
    expect(compiles(source)).not.toThrow();
  });

  it('leaves out every field of a nested object an append sets to null', () => {
    const source = appOf(
      '  const [items, setItems] = useState([{t: "a", pos: {x: 1, y: 2}}]);',
      '<Pressable onPress={() => setItems([...items, {t: "b", pos: null}, {t: "c", pos: undefined}])}>' +
        '<Text>{items[0].pos.x}</Text></Pressable>',
    );
    expect(compiles(source)).not.toThrow();
    expect(
      compiles(
        appOf(
          '  const [items, setItems] = useState([{t: "a"}]);',
          '<Pressable onPress={() => setItems([...items, {t: "b", pos: null}])}><Text>{items.length}</Text></Pressable>',
        ),
      ),
    ).toThrow('list state "items" has no field "pos"');
  });

  it('refuses holes and spreads in an array built in render', () => {
    const source = appOf(
      '  const [n, setN] = useState(1);',
      '<View>{[n, , 2].map((value, index) => <Text key={index}>{value}</Text>)}</View>',
    );
    expect(compiles(source)).toThrow('cannot have holes or spreads');
  });
});
