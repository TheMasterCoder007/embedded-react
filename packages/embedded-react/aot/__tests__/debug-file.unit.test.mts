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

import {readFileSync} from 'node:fs';
import {dirname} from 'node:path';
import {fileURLToPath} from 'node:url';
import {describe, expect, it} from 'vitest';
import {
  compilePackage,
  placeOf,
  readDebugFile,
  readNativeFiles,
} from '../compile.mts';
import {DEBUG_FILE_FORMAT} from '../compiler/package/debug-file.mts';
import type {
  DebugFile,
  DebugFunction,
  DebugLocal,
  DebugPanel,
} from '../compiler/package/debug-file.mts';

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
    expect(debugFile.files).toEqual(['App.jsx']);
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

  it('names the component of the lists the compiler made in an inlined instance', () => {
    const source = `import {useState} from 'react';
import {View, Text, Pressable} from 'embedded-react';
function Todo() {
  const [items, setItems] = useState([{name: 'a', done: false}]);
  const [csv, setCsv] = useState('a,b');
  return (
    <Pressable onPress={() => setCsv(csv + ',c')}>
      {items.filter(item => !item.done).map(item => <Text key={item.name}>{item.name}</Text>)}
      {csv.split(',').map(part => <Text key={part}>{part}</Text>)}
    </Pressable>
  );
}
export function App() {
  return (
    <View>
      <Todo />
    </View>
  );
}`;
    const [panel] = debugFileOf(source).panels;
    const hidden = panel.states.filter(state => state.hidden);
    expect(hidden.map(state => state.name).sort()).toEqual([
      'items.filter',
      'split',
    ]);
    for (const state of hidden) {
      expect(state.component).toEqual({name: 'Todo', line: 16});
    }
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
    expect(root.nodes[3].handle).toEqual({
      slot: expect.any(Number),
      offset: expect.any(Number),
      count: 1,
    });
    expect(row.nodes[0].handle!.count).toBe(16);
    expect(root.nodes[0].handle).toBeUndefined();
    expect(row.kind).toBe('row');
    expect(row.nodes.map(({line}) => line)).toEqual([26]);
  });

  it("gives a component's nodes the line its element is used on", () => {
    const nodes = debugFileOf(APP).panels[0].templates.flatMap(
      template => template.nodes,
    );
    // Counter's Pressable and Text are made on lines 6 and 7, inside the <Counter /> of line 17.
    const pressable = nodes.find(node => node.line === 6);
    expect(pressable?.uses).toEqual([{line: 17, column: 7}]);
    expect(nodes.find(node => node.line === 7)?.uses).toEqual([
      {line: 17, column: 7},
    ]);
    // The app's own elements are used nowhere.
    expect(nodes.find(node => node.line === 18)?.uses).toBeUndefined();
  });

  it("leaves the children a component is given as the caller's nodes", () => {
    const source = `import {View, Text} from 'embedded-react';
function Card({children}) {
  return <View>{children}</View>;
}
function Panel() {
  return (
    <Card>
      <Text>inside</Text>
    </Card>
  );
}
export function App() {
  return (
    <View>
      <Panel />
    </View>
  );
}`;
    const uses = Object.fromEntries(
      debugFileOf(source)
        .panels[0].templates.flatMap(template => template.nodes)
        .map(node => [node.line, node.uses?.map(use => use.line)]),
    );
    // Card's View is Card's, used in Panel, which App uses.
    expect(uses[3]).toEqual([15, 7]);
    // The Text is written in Panel, so it is Panel's and not Card's.
    expect(uses[8]).toEqual([15]);
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
    expect(() =>
      readDebugFile(broken(file => delete file.panels[0].functions[0].locals)),
    ).toThrow(/panel 0, function 0/);
    expect(() => readDebugFile(broken(file => delete file.nodeTypes))).toThrow(
      /no nodeTypes/,
    );
    expect(() =>
      readDebugFile(
        broken(file => delete file.panels[0].templates[0].nodes[0].children),
      ),
    ).toThrow(/panel 0, template 0/);
  });
});

describe('locals', () => {
  const LOCALS_APP = `import {useState} from 'react';
import {View, Text, Pressable, Animated, useAnimatedValue} from 'embedded-react';
function bump(amount) {
  setCount(count + amount);
}
export function App() {
  const [count, setCount] = useState(0);
  const [label, setLabel] = useState('none');
  const [items, setItems] = useState([{id: 1, done: false}, {id: 2, done: true}]);
  const fade = useAnimatedValue(0);
  return (
    <View>
      <Pressable
        onPress={() => {
          const step = 2;
          let total = count + step;
          if (total > 10) {
            const over = total - 10;
            total = over;
          }
          for (let index = 0; index < 3; index++) {
            total += index;
          }
          for (const [position, item] of items.entries()) {
            total += position + item.id;
          }
          const text = \`n\${total}\`;
          const parts = text.split('1');
          const anim = Animated.timing(fade, {toValue: 1, duration: 100});
          anim.start(({finished}) => setLabel(finished ? 'done' : 'cut'));
          bump(parts.length);
          setLabel(text);
        }}>
        <Text>{count}{label}</Text>
      </Pressable>
      {items.filter(item => !item.done).map(item => (
        <Pressable key={item.id} onPress={() => setCount(count + item.id)}>
          <Text>{item.id}</Text>
        </Pressable>
      ))}
    </View>
  );
}`;

  /** A panel's function that has code on a line. */
  const functionAt = (panel: DebugPanel, line: number): DebugFunction =>
    panel.functions.find(programFunction =>
      programFunction.lines.some(entry => entry.line === line),
    )!;

  /** Where a line's code starts in a function. */
  const offsetOf = (programFunction: DebugFunction, line: number): number =>
    programFunction.lines.find(entry => entry.line === line)!.offset;

  /** A function's locals, by name. */
  const localsOf = (programFunction: DebugFunction): Map<string, DebugLocal> =>
    new Map(programFunction.locals.map(local => [local.name, local]));

  const [panel] = debugFileOf(LOCALS_APP).panels;
  const handler = functionAt(panel, 15);
  const locals = localsOf(handler);
  const slotOf = (name: string): number =>
    panel.states.find(state => state.name === name)!.slot;

  it("names a handler's locals, each with its register and the words it holds its value in", () => {
    expect(handler.locals.map(local => local.name)).toEqual([
      'step',
      'total',
      'over',
      'index',
      'item',
      'position',
      'text',
      'parts',
      'anim',
      'amount',
    ]);
    expect(locals.get('step')).toMatchObject({type: 'int', register: 0});
    expect(locals.get('total')).toMatchObject({type: 'int'});
    // Each holds its value from the end of its declaration.
    expect(locals.get('step')!.from).toBe(offsetOf(handler, 16));
    expect(locals.get('total')!.from).toBe(offsetOf(handler, 17));
    // A string is kept in a buffer of its own.
    expect(locals.get('text')).toMatchObject({
      type: 'string',
      offset: expect.any(Number),
      cap: expect.any(Number),
    });
    expect(locals.get('text')!.register).toBeUndefined();
    for (const local of handler.locals) {
      expect(local.parameter).toBeUndefined();
      expect(local.to).toBeGreaterThanOrEqual(local.from);
    }
  });

  it("ends a block's locals with it, and a loop's counter and item with the loop", () => {
    const after = (line: number): number => offsetOf(handler, line);
    // `over` is the if's; `index` is the for loop's, which ends before the for…of starts.
    expect(locals.get('over')!.to).toBeLessThanOrEqual(after(21));
    expect(locals.get('index')!.to).toBeLessThanOrEqual(after(24));
    expect(locals.get('item')!.to).toBeLessThanOrEqual(after(27));
    // Those that aren't in a block last to the end of the handler.
    expect(locals.get('step')!.to).toBe(locals.get('total')!.to);
    expect(locals.get('step')!.to).toBeGreaterThan(after(32));
  });

  it("names a for…of's item as a row of its list, and its index as the visit's number", () => {
    expect(locals.get('item')).toMatchObject({
      type: 'row',
      list: slotOf('items'),
    });
    expect(locals.get('item')!.through).toBeUndefined();
    expect(locals.get('position')).toMatchObject({
      type: 'int',
      register: locals.get('item')!.register,
    });
  });

  it('names a split as its list, and gives no place to a name the compiler folded away', () => {
    expect(locals.get('parts')).toMatchObject({type: 'list'});
    expect(
      panel.states.find(state => state.slot === locals.get('parts')!.list)!
        .hidden,
    ).toBe(true);
    // An animation, and an inlined helper's parameter, which stands for the value it is called with.
    for (const name of ['anim', 'amount']) {
      const local = locals.get(name)!;
      expect(local.type).toBeUndefined();
      expect(local.register).toBeUndefined();
      expect(local.offset).toBeUndefined();
    }
  });

  it("names what a function is entered with: a row, and a completion's finished", () => {
    const row = functionAt(panel, 37);
    expect(row.locals).toEqual([
      expect.objectContaining({
        name: 'item',
        type: 'row',
        parameter: true,
        register: 0,
        list: slotOf('items'),
        through: slotOf('items.filter'),
        from: 0,
      }),
    ]);
    const finished = panel.functions
      .filter(programFunction => programFunction.name.startsWith('completion'))
      .flatMap(programFunction => programFunction.locals);
    expect(finished).toEqual([
      expect.objectContaining({
        name: 'finished',
        type: 'bool',
        parameter: true,
        register: 0,
        from: 0,
      }),
    ]);
  });

  it('names what the app declares, even a name like one the compiler makes, and leaves out what the compiler makes', () => {
    const source = `import {useState} from 'react';
import {Text, Pressable} from 'embedded-react';
export function App() {
  const [count, setCount] = useState(0);
  return (
    <Pressable
      onPress={() => {
        const object$3 = count + 1;
        const {low, high} = {low: count, high: object$3};
        setCount(low + high);
      }}>
      <Text>{count}</Text>
    </Pressable>
  );
}`;
    const [panel] = debugFileOf(source).panels;
    expect(functionAt(panel, 8).locals.map(local => local.name)).toEqual([
      'object$3',
      'low',
      'high',
    ]);
  });

  it("names an object's constant fields with their values, and no place", () => {
    const source = `import {useState} from 'react';
import {Text, Pressable} from 'embedded-react';
const LIMITS = {low: 2, unit: 'psi'};
export function App() {
  const [count, setCount] = useState(0);
  return (
    <Pressable
      onPress={() => {
        const limits = LIMITS;
        const next = {x: count + 1};
        setCount(next.x + limits.low);
      }}>
      <Text>{count}</Text>
    </Pressable>
  );
}`;
    const [panel] = debugFileOf(source).panels;
    expect(functionAt(panel, 10).locals).toMatchObject([
      {name: 'limits.low', value: 2},
      {name: 'limits.unit', value: 'psi'},
      {name: 'next.x', type: 'int'},
    ]);
    const [low] = functionAt(panel, 10).locals;
    expect(low.type).toBeUndefined();
    expect(low.register).toBeUndefined();
  });

  it("names a native event's values, a record's fields by their path", () => {
    const fixtures = `${dirname(fileURLToPath(import.meta.url))}/../../test/erb-frames/fixtures`;
    const debugFileOfFixture = (name: string): DebugFile => {
      const filename = `${fixtures}/${name}.tsx`;
      const source = readFileSync(filename, 'utf8');
      return readDebugFile(
        compilePackage(source, 'app', {
          ...OPTIONS,
          filename,
          nativeFiles: readNativeFiles(source, fixtures),
        }).debugFile,
      );
    };
    const parameters = (debugFile: DebugFile, name: string): string[] =>
      debugFile.panels[0].functions
        .find(programFunction => programFunction.name === name)!
        .locals.filter(local => local.parameter)
        .map(local => `${local.name} ${local.type}`);
    expect(
      parameters(
        debugFileOfFixture('native-events'),
        'native event probe.done',
      ),
    ).toEqual(['value int', 'halfValue float', 'isEven bool']);
    expect(
      parameters(
        debugFileOfFixture('native-records'),
        'native event probe.located',
      ),
    ).toEqual(['point.x int', 'point.y int']);
  });
});

describe('placeOf', () => {
  /** A file whose panels have the given functions. */
  const fileOf = (
    ...panels: {name: string; lines: [number, number][]}[][]
  ): DebugFile => ({
    format: DEBUG_FILE_FORMAT,
    buildId: 1,
    files: ['App.jsx'],
    nodeTypes: [],
    panels: panels.map((functions, index) => ({
      width: 240 + index,
      height: 320,
      arenaBytes: 0,
      regions: {},
      functions: functions.map(({name, lines}) => ({
        name,
        lines: lines.map(([offset, line]) => ({offset, line, column: 3})),
        locals: [],
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
      {
        panels: [0],
        function: 'handler',
        file: 'App.jsx',
        line: 10,
        column: 3,
      },
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
      {
        panels: [0, 1],
        function: 'handler',
        file: 'App.jsx',
        line: 11,
        column: 3,
      },
      {
        panels: [2],
        function: 'the boot values',
        file: 'App.jsx',
        line: 40,
        column: 3,
      },
    ]);
  });
});
