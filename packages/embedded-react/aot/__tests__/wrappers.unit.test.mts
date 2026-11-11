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
import {compilePackage, packPackage} from '../compile.mts';
import {lowerApp} from '../compiler/lower.mts';
import {resolveOptions} from '../compiler/options.mts';
import {nodesOf} from '../compiler/ir/nodes.mts';
import type {IrNode} from '../compiler/ir/nodes.mts';

// Button, ImageBackground and SectionList, lowered to the trees their Flow A components render.

const SCREEN = {screen: {width: 240, height: 320}};

const HEAD =
  "import {useRef, useState} from 'react';\n" +
  "import {Button, Image, ImageBackground, Pressable, ScrollView, SectionList, Text, View} from 'embedded-react';\n" +
  "import cog from './cog.png';\n";

/** An app module with `before` at module level and `body` as the App's body. */
const appOf = (before: string, body: string) =>
  `${HEAD}${before}\nexport function App() {\n${body}\n}`;

const lower = (source: string) =>
  lowerApp(source, resolveOptions(SCREEN), new Map(), []);

/** The package an app compiles to, as text to compare. */
const packageOf = (source: string) =>
  Buffer.from(
    packPackage(compilePackage(source, 'App.jsx', SCREEN)).bytes,
  ).toString('hex');

/** Every node, as its type without the prefix. */
const typesOf = (source: string) =>
  nodesOf(lower(source).root).map(node => node.type.replace('ER_NODE_', ''));

/** Every node's constant text, joined, or '' where it has none or it is worked out at run time. */
const textOf = (node: IrNode) =>
  (node.text?.parts ?? [])
    .map(part => (part.kind === 'literal' ? part.text : '?'))
    .join('');

/** Every node's constant props, by field. */
const propsOf = (node: IrNode) =>
  Object.fromEntries(
    node.constantProps.map(assign => [assign.field, assign.value]),
  );

/** The events wired on a node. */
const eventsOf = (node: IrNode) =>
  node.wiring.flatMap(wire => (wire.kind === 'event' ? [wire.event] : []));

describe('Button', () => {
  it('lowers to a Pressable around one Text', () => {
    const source = appOf(
      '',
      '  const [n, setN] = useState(0);\n' +
        '  return <Button title="Save" onPress={() => setN(n + 1)} />;',
    );
    const [pressable, text] = nodesOf(lower(source).root);
    expect(pressable.type).toBe('ER_NODE_PRESSABLE');
    expect(eventsOf(pressable)).toEqual(['ER_EVENT_PRESS']);
    expect(propsOf(pressable)).toMatchObject({
      border_radius: 2,
      align_items: 'ER_ALIGN_CENTER',
    });
    expect(text.type).toBe('ER_NODE_TEXT');
    expect(textOf(text)).toBe('Save');
    expect(propsOf(text)).toMatchObject({padding: 8});
  });

  it('greys out and drops onPress when disabled, whatever the color', () => {
    const enabled = nodesOf(
      lower(appOf('', '  return <Button title="x" onPress={() => {}} />;'))
        .root,
    );
    const disabled = nodesOf(
      lower(
        appOf(
          '',
          '  return <Button title="x" color="#f00" disabled onPress={() => {}} />;',
        ),
      ).root,
    );
    expect(eventsOf(disabled[0])).toEqual([]);
    expect(propsOf(disabled[0]).background_color).not.toBe(
      propsOf(enabled[0]).background_color,
    );
    expect(propsOf(disabled[1]).color).not.toBe(propsOf(enabled[1]).color);
  });

  it('takes its fill from color, from state too', () => {
    const plain = packageOf(appOf('', '  return <Button title="x" />;'));
    expect(
      packageOf(appOf('', '  return <Button title="x" color="" />;')),
    ).toBe(plain);
    expect(
      packageOf(appOf('', '  return <Button title="x" color="#f00" />;')),
    ).not.toBe(plain);
    const [pressable] = nodesOf(
      lower(
        appOf(
          '',
          '  const [on, setOn] = useState(false);\n' +
            "  return <Button title=\"x\" color={on ? '#f00' : '#0f0'} onPress={() => setOn(!on)} />;",
        ),
      ).root,
    );
    expect(pressable.stateProps.map(assign => assign.field)).toEqual([
      'background_color',
    ]);
  });

  it('draws a title worked out at run time', () => {
    const [, text] = nodesOf(
      lower(
        appOf(
          '',
          '  const [n, setN] = useState(0);\n' +
            "  return <Button title={'n ' + n} onPress={() => setN(n + 1)} />;",
        ),
      ).root,
    );
    expect(textOf(text)).toBe('n ?');
  });

  it('draws a boolean title as text, as String(title) does', () => {
    const [, constant] = nodesOf(
      lower(appOf('', '  return <Button title={true} />;')).root,
    );
    expect(textOf(constant)).toBe('true');
    const [, fromState] = nodesOf(
      lower(
        appOf(
          '',
          '  const [on, setOn] = useState(false);\n' +
            '  return <Button title={on} onPress={() => setOn(!on)} />;',
        ),
      ).root,
    );
    expect(textOf(fromState)).toBe('?');
  });

  it('refuses a disabled worked out at run time', () => {
    expect(() =>
      lower(
        appOf(
          '',
          '  const [d, setD] = useState(false);\n' +
            '  return <Button title="x" disabled={d} onPress={() => setD(true)} />;',
        ),
      ),
    ).toThrow(/<Button disabled> must be a compile-time constant/);
  });

  it('gives way to a component of the same name in the file', () => {
    const source =
      "import {Text, View} from 'embedded-react';\n" +
      'function Button({label}) { return <Text>{label}</Text>; }\n' +
      'export function App() { return <View><Button label="mine" /></View>; }';
    const [, text] = nodesOf(lower(source).root);
    expect(textOf(text)).toBe('mine');
  });
});

describe('ImageBackground', () => {
  it('lowers to a View holding a filling Image, then the children', () => {
    const source = appOf(
      '',
      '  return <ImageBackground source={cog} resizeMode="cover" style={{width: 100, height: 50}}\n' +
        '    imageStyle={{borderRadius: 4}}><Text>hi</Text></ImageBackground>;',
    );
    expect(typesOf(source)).toEqual(['VIEW', 'IMAGE', 'TEXT']);
    const [view, image] = nodesOf(lower(source).root);
    expect(propsOf(view)).toMatchObject({width: 100, height: 50});
    expect(propsOf(image)).toMatchObject({
      position: 'ER_POSITION_ABSOLUTE',
      left: 0,
      right: 0,
      top: 0,
      bottom: 0,
      border_radius: 4,
      resize_mode: 'ER_RESIZE_COVER',
    });
  });

  it('puts ref on the View and imageRef and the events on the Image', () => {
    const source = appOf(
      '',
      '  const outer = useRef(null);\n  const inner = useRef(null);\n' +
        '  return <ImageBackground ref={outer} imageRef={inner} source={cog} onPress={() => {}} />;',
    );
    const [view, image] = nodesOf(lower(source).root);
    const refsOf = (node: IrNode) =>
      node.wiring.flatMap(wire => (wire.kind === 'ref' ? [wire.slot] : []));
    expect(refsOf(view)).toEqual(['ref_outer']);
    expect(refsOf(image)).toEqual(['ref_inner']);
    expect(eventsOf(view)).toEqual([]);
    expect(eventsOf(image)).toEqual(['ER_EVENT_PRESS']);
  });
});

describe('SectionList', () => {
  const ROWS =
    'renderSectionHeader={({section}) => <Text>{section.title}</Text>}\n' +
    '    renderItem={({item, index, section: s}) => <Text>{s.title}{index}{item}</Text>}\n' +
    '    renderSectionFooter={({section}) => <Text>{section.data.length} rows</Text>}';

  it('lays each section out as its header, rows and footer, side by side', () => {
    const source = appOf(
      "const SECTIONS = [{title: 'A', data: ['x', 'y']}, null, {title: 'B', data: ['z']}];",
      `  return <SectionList sections={SECTIONS}\n    ${ROWS} />;`,
    );
    const [scroll, ...rows] = nodesOf(lower(source).root);
    expect(scroll.type).toBe('ER_NODE_SCROLL_VIEW');
    expect(scroll.children).toHaveLength(7);
    expect(rows.map(textOf)).toEqual([
      'A',
      'A0x',
      'A1y',
      '2 rows',
      'B',
      'B0z',
      '1 rows',
    ]);
  });

  it('is the ScrollView it stands for', () => {
    const list = appOf(
      '',
      "  return <SectionList style={{flex: 1}} sections={[{title: 'A', data: [1, 2]}]}\n" +
        '    renderSectionHeader={({section}) => <Text>{section.title}</Text>}\n' +
        '    renderItem={({item}) => <Text>{item}</Text>} />;',
    );
    const scroll = appOf(
      '',
      '  return <ScrollView style={{flex: 1}}><Text>A</Text>\n' +
        '    {[1, 2].map(item => <Text>{item}</Text>)}</ScrollView>;',
    );
    expect(packageOf(list)).toBe(packageOf(scroll));
  });

  it("uses a section's own renderItem over the list's", () => {
    const source = appOf(
      '',
      "  return <SectionList sections={[{title: 'A', data: [1]},\n" +
        "    {title: 'B', data: [2], renderItem: ({item}) => <Text>own {item}</Text>}]}\n" +
        '    renderItem={({item}) => <Text>list {item}</Text>} />;',
    );
    expect(nodesOf(lower(source).root).slice(1).map(textOf)).toEqual([
      'list 1',
      'own 2',
    ]);
  });

  it('reads a data that is list state through section.data', () => {
    const source = appOf(
      '',
      "  const [names, setNames] = useState(['a']);\n" +
        "  return <SectionList sections={[{title: 'Saved', data: names}]}\n" +
        '    renderItem={({item}) => <Text>{item}</Text>}\n' +
        '    renderSectionFooter={({section}) => <Pressable onPress={() => setNames([...names, "b"])}>' +
        '<Text>{section.data.length} rows</Text></Pressable>} />;',
    );
    const footer = nodesOf(lower(source).root).at(-1)!;
    expect(textOf(footer)).toBe('? rows');
  });

  it('maps over a data that is list state as a pool of rows', () => {
    const source = appOf(
      '',
      "  const [names, setNames] = useState(['a']);\n" +
        "  return <SectionList sections={[{title: 'Saved', data: names}]}\n" +
        '    renderSectionHeader={({section}) => <Pressable onPress={() => setNames([...names, "b"])}>' +
        '<Text>{section.title}</Text></Pressable>}\n' +
        '    renderItem={({item, section}) => <Text>{section.title}: {item}</Text>} />;',
    );
    const [scroll] = nodesOf(lower(source).root);
    expect(scroll.children.map(child => child.kind)).toEqual([
      'element',
      'pool',
    ]);
  });

  it('reads a section field worked out at run time', () => {
    const source = appOf(
      '',
      '  const [count, setCount] = useState(2);\n' +
        "  return <SectionList sections={[{title: 'A', count, data: [1]}]}\n" +
        '    renderSectionHeader={({section}) => <Text>{section.title} {section.count}</Text>}\n' +
        '    renderItem={({item}) => <Pressable onPress={() => setCount(count + 1)}><Text>{item}</Text></Pressable>} />;',
    );
    expect(textOf(nodesOf(lower(source).root)[1])).toBe('A ?');
  });

  it('renders nothing for a header that returns null', () => {
    const source = appOf(
      '',
      "  return <SectionList sections={[{title: '', data: [1]}, {title: 'B', data: [2]}]}\n" +
        '    renderSectionHeader={({section}) => section.title ? <Text>{section.title}</Text> : null}\n' +
        '    renderItem={({item}) => <Text>{item}</Text>} />;',
    );
    expect(nodesOf(lower(source).root).slice(1).map(textOf)).toEqual([
      '1',
      'B',
      '2',
    ]);
  });

  it.each([
    [
      'sections it cannot unroll',
      "  const [s, setS] = useState(['a']);\n  return <SectionList sections={s.map(name => name)} />;",
      /must fold to an array at build time/,
    ],
    [
      'rows with no renderItem',
      "  return <SectionList sections={[{title: 'A', data: [1]}]} />;",
      /section with data needs a renderItem/,
    ],
    [
      'a callback that does not destructure',
      "  return <SectionList sections={[{title: 'A', data: [1]}]} renderItem={info => <Text>{info.item}</Text>} />;",
      /may destructure only item \/ index \/ section/,
    ],
    [
      'a renderItem with no item',
      "  return <SectionList sections={[{title: 'A', data: [1]}]} renderItem={({index}) => <Text>{index}</Text>} />;",
      /renderItem must destructure item/,
    ],
    [
      'a data that is not an array',
      "  return <SectionList sections={[{title: 'A', data: 3}]} renderItem={({item}) => <Text>{item}</Text>} />;",
      /target did not resolve to an array/,
    ],
    [
      'no sections',
      '  return <SectionList renderItem={({item}) => <Text>{item}</Text>} />;',
      /needs a sections prop/,
    ],
  ])('refuses %s', (_what, body, message) => {
    expect(() => lower(appOf('', body))).toThrow(message);
  });
});
