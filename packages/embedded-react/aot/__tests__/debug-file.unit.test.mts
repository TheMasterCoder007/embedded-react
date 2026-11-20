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

import {describe, expect, it} from 'vitest';
import {compilePackage, placeOf, readDebugFile} from '../compile.mts';
import {DEBUG_FILE_FORMAT} from '../compiler/package/debug-file.mts';
import type {DebugFile} from '../compiler/package/debug-file.mts';

const APP = `import {useState} from 'react';
import {View, Text, Pressable} from 'embedded-react';
function Counter({label}) {
  const [taps, setTaps] = useState(0);
  return (
    <Pressable onPress={() => setTaps(taps + 1)}>
      <Text>{label}{taps}</Text>
    </Pressable>
  );
}
export function App() {
  const [mode, setMode] = useState('heat');
  const [target, setTarget] = useState(21.5);
  const [items, setItems] = useState([{name: 'a', note: 'x', done: false}, {name: 'b', done: true}]);
  return (
    <View>
      <Counter label="one" />
      <Pressable
        onPress={() => {
          setMode('cool');
          setTarget(target + 0.5);
        }}>
        <Text>{mode}{target}</Text>
      </Pressable>
      {items.filter(item => !item.done).map(item => (
        <Text key={item.name}>{item.name}</Text>
      ))}
    </View>
  );
}`;

const OPTIONS = {
  screen: {width: 240, height: 320},
  filename: '/projects/clock/src/App.jsx',
};

/** The debug file of a compile. */
const debugFileOf = (
  source: string,
  options: Parameters<typeof compilePackage>[2] = OPTIONS,
): DebugFile => readDebugFile(compilePackage(source, 'app', options).debugFile);

describe('debug file', () => {
  it("carries the package's build_id and source file", () => {
    const result = compilePackage(APP, 'app', OPTIONS);
    const debugFile = readDebugFile(result.debugFile);
    expect(debugFile.format).toBe(DEBUG_FILE_FORMAT);
    expect(debugFile.buildId).toBe(result.buildId);
    expect(debugFile.file).toBe('App.jsx');
    expect(debugFile.panels.map(({width, height}) => [width, height])).toEqual([
      [240, 320],
    ]);
  });

  it('is written for a release build too, which leaves the debug section out of the package', () => {
    const release = compilePackage(APP, 'app', {...OPTIONS, release: true});
    const debug = compilePackage(APP, 'app', OPTIONS);
    expect(readDebugFile(release.debugFile).buildId).toBe(release.buildId);
    expect(readDebugFile(release.debugFile).panels).toEqual(
      readDebugFile(debug.debugFile).panels,
    );
  });

  it('names each function and where its code starts in the source', () => {
    const [panel] = debugFileOf(APP).panels;
    const names = panel.functions.map(programFunction => programFunction.name);
    expect(names).toContain('the update');
    // The button's handler: one line for each statement, columns 1-based.
    const handler = panel.functions.find(programFunction =>
      programFunction.lines.some(line => line.line === 20),
    )!;
    expect(handler.lines.map(({line, column}) => [line, column])).toEqual([
      [20, 11],
      [21, 11],
    ]);
  });

  it('names each state, with its type, slot and place in the arena', () => {
    const [panel] = debugFileOf(APP).panels;
    const byName = new Map(panel.states.map(state => [state.storage, state]));
    expect(byName.get('mode')).toMatchObject({
      name: 'mode',
      hidden: false,
      type: 'enum',
      values: ['heat', 'cool'],
    });
    expect(byName.get('target')).toMatchObject({type: 'float'});
    expect(byName.get('items')).toMatchObject({
      type: 'list',
      fields: [
        {name: 'name', type: 'string'},
        {name: 'note', type: 'string'},
        {name: 'done', type: 'bool'},
        {name: 'note?', type: 'bool'},
      ],
    });
    // Each state has its own slot and place.
    const slots = panel.states.map(state => state.slot);
    expect(new Set(slots).size).toBe(slots.length);
    const offsets = panel.states.map(state => state.offset);
    expect(new Set(offsets).size).toBe(offsets.length);
  });

  it("names an inlined component's state by its component and instance line", () => {
    const [panel] = debugFileOf(APP).panels;
    const taps = panel.states.find(state => state.name === 'taps')!;
    expect(taps.storage).not.toBe('taps');
    expect(taps.component).toEqual({name: 'Counter', line: 17});
    expect(
      panel.states.find(state => state.name === 'mode')!.component,
    ).toBeUndefined();
  });

  it('marks the state the compiler made as hidden', () => {
    const [panel] = debugFileOf(APP).panels;
    const hidden = panel.states.filter(state => state.hidden);
    expect(hidden.map(state => state.type)).toEqual(['list']);
    expect(hidden[0].name).toBe('items.filter');
  });

  it("gives each node its element's line, and its handle slot when code reaches it", () => {
    const [panel] = debugFileOf(APP).panels;
    const [root, row] = panel.templates;
    expect(root.kind).toBe('root');
    expect(
      root.nodes.map(({type, line, column}) => [type, line, column]),
    ).toEqual([
      ['ER_NODE_VIEW', null, null],
      ['ER_NODE_VIEW', 16, 5],
      // Counter's nodes, in its own body.
      ['ER_NODE_PRESSABLE', 6, 5],
      ['ER_NODE_TEXT', 7, 7],
      ['ER_NODE_PRESSABLE', 18, 7],
      ['ER_NODE_TEXT', 23, 9],
    ]);
    expect(root.nodes[3].handle).toEqual(expect.any(Number));
    expect(root.nodes[0].handle).toBeUndefined();
    expect(row.kind).toBe('row');
    expect(row.nodes.map(({line}) => line)).toEqual([26]);
  });

  it("has every panel's states and nodes", () => {
    const debugFile = debugFileOf(APP, {
      filename: OPTIONS.filename,
      screens: [
        {width: 240, height: 320},
        {width: 320, height: 240},
      ],
    });
    // The app doesn't read the screen, so one layout takes both sizes.
    expect(debugFile.panels).toHaveLength(1);
    const responsive = debugFileOf(
      APP.replace(
        '<Text>{mode}{target}</Text>',
        '{screen.width > screen.height ? <View><Text>{mode}</Text></View> : <Text>{mode}</Text>}',
      ),
      {
        filename: OPTIONS.filename,
        screens: [
          {width: 240, height: 320},
          {width: 320, height: 240},
        ],
      },
    );
    expect(responsive.panels).toHaveLength(2);
    for (const panel of responsive.panels) {
      expect(panel.states.map(state => state.name)).toContain('mode');
      expect(panel.templates[0].nodes.length).toBeGreaterThan(1);
    }
  });

  it('refuses a file of another format, or not a debug file at all', () => {
    expect(() => readDebugFile('\x00\x01')).toThrow(/not a debug file/);
    expect(() =>
      readDebugFile(
        JSON.stringify({format: DEBUG_FILE_FORMAT + 1, panels: []}),
      ),
    ).toThrow(/this version reads format/);
  });

  it('refuses a file whose shape is wrong, naming what is wrong', () => {
    const good = JSON.parse(compilePackage(APP, 'app', OPTIONS).debugFile);
    const broken = (change: (file: any) => void): string => {
      const copy = structuredClone(good);
      change(copy);
      return JSON.stringify(copy);
    };
    expect(() =>
      readDebugFile(JSON.stringify({format: DEBUG_FILE_FORMAT, panels: [{}]})),
    ).toThrow(/malformed debug file \(no buildId\)/);
    expect(() => readDebugFile(broken(file => (file.panels = [{}])))).toThrow(
      /malformed debug file \(panel 0 has no size\)/,
    );
    expect(() =>
      readDebugFile(broken(file => (file.panels[0].functions[1].lines = [{}]))),
    ).toThrow(/panel 0, function 1/);
    expect(() =>
      readDebugFile(broken(file => delete file.panels[0].states[0].slot)),
    ).toThrow(/panel 0, state 0/);
    expect(() =>
      readDebugFile(broken(file => (file.panels[0].templates[0].nodes = [1]))),
    ).toThrow(/panel 0, template 0/);
  });
});

describe('placeOf', () => {
  /** A file whose panels have the given functions. */
  const fileOf = (
    ...panels: {name: string; lines: [number, number][]}[][]
  ): DebugFile => ({
    format: DEBUG_FILE_FORMAT,
    buildId: 1,
    file: 'App.jsx',
    panels: panels.map((functions, index) => ({
      width: 240 + index,
      height: 320,
      functions: functions.map(({name, lines}) => ({
        name,
        lines: lines.map(([offset, line]) => ({offset, line, column: 3})),
      })),
      states: [],
      templates: [],
    })),
  });

  const ONE = {
    name: 'handler',
    lines: [
      [0, 10],
      [4, 11],
      [9, 12],
    ] as [number, number][],
  };

  it('finds the last line at or before the offset, as the error screen does', () => {
    const debugFile = fileOf([ONE]);
    expect(placeOf(debugFile, 0, 0)).toEqual([
      {panels: [0], function: 'handler', line: 10, column: 3},
    ]);
    expect(placeOf(debugFile, 0, 8)[0].line).toBe(11);
    expect(placeOf(debugFile, 0, 9)[0].line).toBe(12);
    expect(placeOf(debugFile, 0, 500)[0].line).toBe(12);
  });

  it('finds nothing in a function with no lines, or past the last function', () => {
    const debugFile = fileOf([{name: 'the update', lines: [[2, 5]]}]);
    expect(placeOf(debugFile, 0, 1)).toEqual([]);
    expect(placeOf(debugFile, 1, 0)).toEqual([]);
  });

  it('gives a place for each panel that numbers its functions apart, sharing one where they agree', () => {
    const OTHER = {
      name: 'the boot values',
      lines: [[0, 40]] as [number, number][],
    };
    const debugFile = fileOf([ONE], [ONE], [OTHER]);
    expect(placeOf(debugFile, 0, 5)).toEqual([
      {panels: [0, 1], function: 'handler', line: 11, column: 3},
      {panels: [2], function: 'the boot values', line: 40, column: 3},
    ]);
  });
});
