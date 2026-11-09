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

/*
 * A package's bytes. Whether the runtime loads and draws them right is test/erb-frames' job; this checks the layout the spec gives.
 */

import {describe, it, expect} from 'vitest';
import {crc32} from '../../assets/emit-container.mjs';
import {
  bakePackage,
  compilePackage,
  packPackage,
  resourceReport,
} from '../compile.mts';
import {
  ABI_PROPS,
  AnimationFlag,
  AnimationKind,
  ENGINE_ABI,
  ENGINE_VALUES,
  FLAG_DEBUG,
  FORMAT_MAJOR,
  FORMAT_MINOR,
  BindingKind,
  CHECK_ALWAYS,
  FunctionKind,
  GradientField,
  InterpolationFlag,
  MathFn,
  NONE,
  PaintField,
  PropKind,
  Region,
  Resource,
  SlotFlag,
  Table,
  TemplateKind,
  ValueType,
} from '../compiler/package/format.mts';
import {constantNumber, floatBits} from '../compiler/package/props.mts';
import {Arena} from '../compiler/package/arena.mts';
import {FunctionBuilder, OP} from '../compiler/package/bytecode.mts';
import {Codegen} from '../compiler/package/codegen.mts';
import {ProgramTables} from '../compiler/package/tables.mts';

const STATIC_APP = `
import {View, Text, StyleSheet} from 'embedded-react';
export function App() {
  return (
    <View style={styles.root}>
      <Text style={styles.title}>Hello</Text>
      <View style={styles.box} />
    </View>
  );
}
const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#102030', alignItems: 'center'},
  title: {color: '#ffffff', fontSize: 20},
  box: {width: 40, height: 40, borderRadius: 8, backgroundColor: '#33aa33'},
});`;

const SCREEN = {screen: {width: 240, height: 320}};

// Program layout: the header, then the panel records, the need records and the table directory.
const HEADER_BYTES = 24;
const PANEL_COUNT_AT = 18;
const NEED_COUNT_AT = 20;
const TABLE_COUNT_AT = 22;
const PANEL_RECORD_BYTES = 16;
const NEED_RECORD_BYTES = 8;
const DIRECTORY_ENTRY_BYTES = 12;
const FUNCTION_RECORD_BYTES = 16;
// The function record word holding its kind, parameter count and register cells.
const FUNCTION_INFO_AT = 12;
const SLOT_RECORD_BYTES = 16;
const ARENA_REGION_BYTES = 12;
const NODE_RECORD_BYTES = 12;

const OP_NAMES = Object.fromEntries(
  Object.entries(OP).map(([name, {code}]) => [code, name]),
);

/** The program section's header, panel and table directory. */
function readProgram(bytes) {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.length);
  const u16 = offset => view.getUint16(offset, true);
  const u32 = offset => view.getUint32(offset, true);
  const panelCount = u16(PANEL_COUNT_AT);
  const needCount = u16(NEED_COUNT_AT);
  const tableCount = u16(TABLE_COUNT_AT);
  const directory =
    HEADER_BYTES +
    panelCount * PANEL_RECORD_BYTES +
    needCount * NEED_RECORD_BYTES;
  const tables = new Map();
  for (let index = 0; index < tableCount; index++) {
    const entryOffset = directory + index * DIRECTORY_ENTRY_BYTES;
    tables.set(u16(entryOffset), {
      offset: u32(entryOffset + 4),
      length: u32(entryOffset + 8),
    });
  }
  return {
    view,
    u16,
    u32,
    header: {
      major: u16(0),
      minor: u16(2),
      abi: u16(4),
      flags: u16(6),
      buildId: u32(8),
      arenaBytes: u32(12),
      nodeCount: u16(16),
      panelCount,
    },
    panel: {
      check: u32(24),
      arenaBytes: u32(28),
      nodeCount: u16(32),
      rootTemplate: u16(34),
      boot: u16(36),
      update: u16(38),
    },
    tables,
  };
}

describe('constant props', () => {
  it('reads a constant, by number or by its er_scene.h name', () => {
    expect(constantNumber(12)).toBe(12);
    expect(constantNumber(0xff0d0d0f)).toBe(0xff0d0d0f);
    expect(constantNumber('ER_ALIGN_CENTER')).toBe(3);
    expect(constantNumber('ER_LAYOUT_AUTO')).toBe(-32768);
    expect(constantNumber('ER_NOT_A_NAME')).toBeNull();
  });

  it('stores a float prop as its f32 bits', () => {
    expect(floatBits(0.5)).toBe(0x3f000000);
    expect(ABI_PROPS.get('arc_min')).toEqual({id: 120, kind: 'float'});
    expect(ABI_PROPS.get('spans[1].color')).toEqual({id: 168, kind: 'color'});
  });
});

/** Each function's kind and its instructions, decoded. */
function functionsOf(bytes) {
  const {u32, tables} = readProgram(bytes);
  const functions = tables.get(Table.functions);
  const code = tables.get(Table.code);
  const word = index => u32(code.offset + index * 4);
  return Array.from(
    {length: functions.length / FUNCTION_RECORD_BYTES},
    (_, index) => {
      const recordOffset = functions.offset + index * FUNCTION_RECORD_BYTES;
      const start = u32(recordOffset);
      const instructions = [];
      for (
        let programCounter = start;
        programCounter < start + u32(recordOffset + 4);
      ) {
        const first = word(programCounter);
        const name = OP_NAMES[first & 0xff];
        instructions.push({
          name,
          a: (first >>> 8) & 0xff,
          bx: first >>> 16,
          c: first >>> 24,
          ext: OP[name].words === 2 ? word(programCounter + 1) : undefined,
        });
        programCounter += OP[name].words;
      }
      return {kind: u32(recordOffset + 12) >>> 24, ops: instructions};
    },
  );
}

/** The templates: each one's kind and nodes. */
function templatesOf(bytes) {
  const {u16, u32, tables} = readProgram(bytes);
  const {offset} = tables.get(Table.templates);
  const count = u32(offset);
  const nodes = offset + 4 + count * 8;
  return Array.from({length: count}, (_, index) => {
    const templateOffset = offset + 4 + index * 8;
    return {
      kind: u16(templateOffset + 4),
      nodes: Array.from({length: u16(templateOffset + 2)}, (_, nodeIndex) => ({
        handle: u16(
          nodes + (u16(templateOffset) + nodeIndex) * NODE_RECORD_BYTES + 6,
        ),
      })),
    };
  });
}

/** The arena's regions. */
function regionsOf(bytes) {
  const {u16, u32, tables} = readProgram(bytes);
  const arena = tables.get(Table.arena);
  return Array.from({length: arena.length / ARENA_REGION_BYTES}, (_, index) => {
    const regionOffset = arena.offset + index * ARENA_REGION_BYTES;
    return {
      kind: u16(regionOffset),
      count: u16(regionOffset + 2),
      bytes: u32(regionOffset + 8),
    };
  });
}

describe('package', () => {
  it('writes the header, the panel and the required tables', () => {
    const result = compilePackage(STATIC_APP, 'app', SCREEN);
    const {header, panel, tables} = readProgram(result.program);
    expect(header).toMatchObject({
      major: FORMAT_MAJOR,
      minor: FORMAT_MINOR,
      abi: ENGINE_ABI,
      flags: FLAG_DEBUG,
      panelCount: 1,
    });

    // The screen-sized root, then the app's three nodes.
    expect(panel).toEqual({
      check: CHECK_ALWAYS,
      arenaBytes: header.arenaBytes,
      nodeCount: 4,
      rootTemplate: 0,
      boot: NONE,
      update: NONE,
    });
    expect(header.nodeCount).toBe(4);

    for (const id of [
      Table.arena,
      Table.slots,
      Table.templates,
      Table.functions,
      Table.types,
      Table.code,
      Table.strings,
      Table.props,
    ]) {
      expect(tables.has(id), `table ${id}`).toBe(true);
    }
    for (const {offset, length} of tables.values()) {
      expect(offset % 4).toBe(0);
      expect(length % 4).toBe(0);
      expect(offset + length).toBeLessThanOrEqual(result.program.length);
    }
  });

  it('works the build_id out over the section with the field as 0', () => {
    const result = compilePackage(STATIC_APP, 'app', SCREEN);
    const zeroed = result.program.slice();
    new DataView(zeroed.buffer).setUint32(8, 0, true);
    expect(result.buildId).toBe(crc32(zeroed));
    expect(readProgram(result.program).header.buildId).toBe(result.buildId);
  });

  it('builds the tree from one template, in pre-order with child counts', () => {
    const result = compilePackage(STATIC_APP, 'app', SCREEN);
    const {u16, u32, tables} = readProgram(result.program);
    const templates = tables.get(Table.templates);
    expect(u32(templates.offset)).toBe(1);
    const nodes = templates.offset + 4 + 8;
    const node = index => ({
      type: result.program[nodes + index * NODE_RECORD_BYTES],
      children: u16(nodes + index * NODE_RECORD_BYTES + 2),
      props: u16(nodes + index * NODE_RECORD_BYTES + 4),
    });
    expect([0, 1, 2, 3].map(index => node(index).children)).toEqual([
      1, 2, 0, 0,
    ]);
    expect(node(2).type).toBe(1); // ER_NODE_TEXT

    // The runtime sizes the root to the real panel.
    expect(node(0).props).toBe(NONE);
  });

  it('shares identical prop sets', () => {
    const source = STATIC_APP.replace(
      '<View style={styles.box} />',
      '<View style={styles.box} /><View style={styles.box} />',
    );
    const one = compilePackage(STATIC_APP, 'app', SCREEN);
    const two = compilePackage(source, 'app', SCREEN);
    const sets = result => {
      const {u32, tables} = readProgram(result.program);
      return u32(tables.get(Table.props).offset);
    };
    expect(sets(two)).toBe(sets(one));
  });

  it('refuses what it does not lower yet, at its place in the source', () => {
    const source = `
import {useState, useRef} from 'react';
import {View, Pressable, Svg, updateVector} from 'embedded-react';
export function App() {
  const [rows, setRows] = useState([{id: 1}]);
  const pad = useRef(null);
  return (
    <Pressable onPress={() => updateVector(pad, [{circle: [5, 5, 4], fill: '#fff'}])}>
      {rows.map(row => (
        <View key={row.id}>
          <Svg ref={pad} width={10} height={10} />
        </View>
      ))}
    </Pressable>
  );
}`;
    expect(() => compilePackage(source, 'app', SCREEN)).toThrow(
      /a package doesn't support updateVector on a node in a list row yet[\s\S]*App\.jsx:11:11/,
    );
  });

  it('binds a handler to its event, and re-sets what state drives in the update', () => {
    const source = `
import {useState} from 'react';
import {Pressable, Text} from 'embedded-react';
export function App() {
  const [count, setCount] = useState(0);
  return <Pressable onPress={() => setCount(count + 1)}><Text>{count}</Text></Pressable>;
}`;
    const {u16, u32, tables, panel} = readProgram(
      compilePackage(source, 'app', SCREEN).program,
    );
    const functions = tables.get(Table.functions);
    const kinds = Array.from(
      {length: functions.length / FUNCTION_RECORD_BYTES},
      (_, index) =>
        u32(
          functions.offset + index * FUNCTION_RECORD_BYTES + FUNCTION_INFO_AT,
        ) >>> 24,
    );
    expect(kinds).toEqual([0, FunctionKind.update]); // ER_EVENT_PRESS, then the update
    expect(panel.update).toBe(1);
    expect(panel.boot).toBe(NONE);

    // One binding: the press, to function 0.
    const bindings = tables.get(Table.bindings);
    expect(bindings.length).toBe(4);
    expect(u32(bindings.offset)).toBe(BindingKind.event | (0 << 8) | (0 << 16));

    // The count is an int slot with a dirty bit; the handler's request bit is a tracked bool.
    const slots = tables.get(Table.slots);
    const slot = index => ({
      type: u16(slots.offset + index * SLOT_RECORD_BYTES) & 0xff,
      flags: u16(slots.offset + index * SLOT_RECORD_BYTES) >> 8,
    });
    expect(slot(0)).toEqual({type: ValueType.int, flags: SlotFlag.tracked});
    expect(slot(1)).toEqual({type: ValueType.bool, flags: SlotFlag.tracked});
  });

  it('works out values from the screen size in a boot function, in doubles', () => {
    const source = `
import {View} from 'embedded-react';
const HALF = screen.width / 2;
export function App() {
  return <View style={{width: HALF, height: 10}} />;
}`;
    const {u16, u32, tables, panel} = readProgram(
      compilePackage(source, 'app', SCREEN).program,
    );
    const functions = tables.get(Table.functions);
    expect(
      u32(
        functions.offset +
          panel.boot * FUNCTION_RECORD_BYTES +
          FUNCTION_INFO_AT,
      ) >>> 24,
    ).toBe(FunctionKind.boot);
    const slots = tables.get(Table.slots);
    const types = Array.from(
      {length: slots.length / SLOT_RECORD_BYTES},
      (_, index) => u16(slots.offset + index * SLOT_RECORD_BYTES),
    );
    expect(types).toContain(ValueType.double | (SlotFlag.bootOnly << 8));
  });

  it('interns a string state that holds one of a closed set of values', () => {
    const source = `
import {useState} from 'react';
import {Pressable, Text} from 'embedded-react';
export function App() {
  const [mode, setMode] = useState('cool');
  const [label, setLabel] = useState('none');
  return (
    <Pressable onPress={() => { setMode('heat'); setLabel(label + '!'); }}>
      <Text style={{color: mode === 'heat' ? '#ff0000' : '#0000ff'}}>{mode} {label}</Text>
    </Pressable>
  );
}`;
    const result = compilePackage(source, 'app', SCREEN);
    const {u16, u32, tables} = readProgram(result.program);
    const strings = tables.get(Table.strings);
    const string = index => {
      const start = strings.offset + u32(strings.offset + 4 + index * 4);
      return Buffer.from(
        result.program.subarray(start, result.program.indexOf(0, start)),
      ).toString();
    };

    // mode is an enum slot of set 0, which holds its initial value and the one it is set to.
    const slots = tables.get(Table.slots);
    expect(u16(slots.offset) & 0xff).toBe(ValueType.enum);
    expect(u16(slots.offset + 14)).toBe(0);
    const enums = tables.get(Table.enums);
    expect(u32(enums.offset)).toBe(1);
    expect(u32(enums.offset + 4)).toBe(0 | (2 << 16));
    expect(
      [0, 1].map(index => string(u16(enums.offset + 8 + index * 2))),
    ).toEqual(['cool', 'heat']);

    // label is read as a string to build its next value, so it stays a buffer.
    const types = Array.from(
      {length: slots.length / SLOT_RECORD_BYTES},
      (_, index) => u16(slots.offset + index * SLOT_RECORD_BYTES) & 0xff,
    );
    expect(types.filter(type => type === ValueType.enum)).toHaveLength(1);
    expect(types).toContain(ValueType.string);
  });

  it('lists the values the host feeds, by name', () => {
    const source = `
import {Text, useHostValue} from 'embedded-react';
export function App() {
  const zeta = useHostValue(0);
  const alpha = useHostValue(0.5);
  return <Text>{zeta} {alpha}</Text>;
}`;
    const result = compilePackage(source, 'app', SCREEN);
    const {u16, u32, tables} = readProgram(result.program);
    const inputs = tables.get(Table.inputs);
    const strings = tables.get(Table.strings);
    const string = index => {
      const start = strings.offset + u32(strings.offset + 4 + index * 4);
      const end = result.program.indexOf(0, start);
      return Buffer.from(result.program.subarray(start, end)).toString();
    };
    expect([0, 1].map(index => string(u16(inputs.offset + index * 4)))).toEqual(
      ['alpha', 'zeta'],
    );
  });

  it('refuses a constant its prop cannot hold, at the element', () => {
    for (const [style, message] of [
      ['fontSize: 300', /`font_size` is 300, but the engine holds 0 to 255/],
      [
        'width: 40000',
        /`width` is 40000, but the engine holds -32768 to 32767/,
      ],
      ['zIndex: -40000', /`z_index` is -40000/],
    ]) {
      const source = `
import {Text} from 'embedded-react';
export function App() {
  return <Text style={{${style}}}>x</Text>;
}`;
      expect(() => compilePackage(source, 'app', SCREEN), style).toThrow(
        new RegExp(`${message.source}[\\s\\S]*App\\.jsx:4:10`),
      );
    }
  });

  it('refuses an image the package does not bake', () => {
    const source = `
import {Image} from 'embedded-react';
export function App() {
  return <Image source={{uri: 'logo'}} style={{width: 20, height: 20}} />;
}`;
    expect(() => compilePackage(source, 'app', SCREEN)).toThrow(
      /the package has no image named "logo"[\s\S]*App\.jsx:4:10/,
    );
  });

  it('puts the program and debug table in an erb container, each section 4-aligned', () => {
    const result = compilePackage(STATIC_APP, 'app', SCREEN);
    for (const assetPack of [undefined, new Uint8Array(13).fill(7)]) {
      const file = packPackage(result, assetPack);
      const bytes = Buffer.from(file.bytes);
      expect(bytes.subarray(0, 4).toString('ascii')).toBe('ERCF');
      expect(bytes.readUInt32LE(8)).toBe(crc32(bytes.subarray(12)));
      expect(bytes.subarray(14, 17).toString('ascii')).toBe('erb');

      // Walk the sections: each one's data starts 4-aligned.
      let cursor = 17;
      const count = bytes.readUInt32LE(cursor);
      cursor += 4;
      const types = [];
      for (let index = 0; index < count; index++) {
        const type = bytes.readUInt32LE(cursor);
        const length = bytes.readUInt32LE(cursor + 4);
        if (type !== 0) {
          types.push(type);
          expect(
            (cursor + 8) % 4,
            `section ${type} with a ${assetPack?.length ?? 0} B pack`,
          ).toBe(0);
        }
        cursor += 8 + length;
      }
      expect(cursor).toBe(bytes.length);
      expect(types).toEqual(assetPack ? [2, 4, 5] : [4, 5]);
      const end = file.bytes.length - result.debug.length;
      expect(Buffer.from(file.bytes.subarray(end))).toEqual(
        Buffer.from(result.debug),
      );
      expect(
        Buffer.from(
          file.bytes.subarray(end - 8 - result.program.length, end - 8),
        ),
      ).toEqual(Buffer.from(result.program));
    }
  });

  it('reports each panel and the flash each section takes', () => {
    const result = compilePackage(STATIC_APP, 'app', SCREEN);
    const file = packPackage(result);
    const report = resourceReport(result, file);
    expect(report[0]).toMatch(/^ {2}layout 240x320: arena \d+ B, 4 nodes$/);
    expect(report[1]).toBe(
      `  flash: ${file.bytes.length} B (program ${result.program.length} B, debug ${result.debug.length} B)`,
    );
    expect(report[2]).toMatch(/templates \d+/);
  });
});

describe('panels', () => {
  const PANELS = {
    screens: [
      {width: 240, height: 320},
      {width: 320, height: 240},
    ],
  };

  // A layout picked from the panel's width, with state and a handler either way.
  const RESPONSIVE_APP = `
import {useState} from 'react';
import {View, Pressable, Text} from 'embedded-react';
const WIDE = screen.width > screen.height;
export function App() {
  const [count, setCount] = useState(0);
  return (
    <Pressable onPress={() => setCount(count + 1)}>
      {WIDE ? (
        <View style={{flexDirection: 'row'}}>
          <Text>{count}</Text>
          <Text>wide</Text>
        </View>
      ) : (
        <Text>{count}</Text>
      )}
    </Pressable>
  );
}`;

  /** Each directory entry, with the panel it is for. */
  function directoryOf(bytes) {
    const {u16, u32, header} = readProgram(bytes);
    const directoryOffset =
      HEADER_BYTES +
      header.panelCount * PANEL_RECORD_BYTES +
      u16(NEED_COUNT_AT) * NEED_RECORD_BYTES;
    return Array.from({length: u16(TABLE_COUNT_AT)}, (_, index) => ({
      id: u16(directoryOffset + index * DIRECTORY_ENTRY_BYTES),
      panel: u16(directoryOffset + index * DIRECTORY_ENTRY_BYTES + 2),
      offset: u32(directoryOffset + index * DIRECTORY_ENTRY_BYTES + 4),
      length: u32(directoryOffset + index * DIRECTORY_ENTRY_BYTES + 8),
    }));
  }

  /** Each panel record. */
  function panelsOf(bytes) {
    const {u16, u32, header} = readProgram(bytes);
    return Array.from({length: header.panelCount}, (_, index) => {
      const panelOffset = HEADER_BYTES + index * PANEL_RECORD_BYTES;
      return {
        check: u32(panelOffset),
        arenaBytes: u32(panelOffset + 4),
        nodeCount: u16(panelOffset + 8),
      };
    });
  }

  it('lists every table for every panel in a one-panel package', () => {
    const {program} = compilePackage(STATIC_APP, 'app', SCREEN);
    expect(directoryOf(program).every(entry => entry.panel === NONE)).toBe(
      true,
    );
  });

  it('has a layout for each breakpoint, the header naming the most any needs', () => {
    const result = compilePackage(RESPONSIVE_APP, 'app', {});
    const panels = panelsOf(result.program);
    expect(result.panels.map(panel => [panel.width, panel.height])).toEqual([
      [240, 320],
      [320, 240],
    ]);
    expect(panels.every(panel => panel.check !== CHECK_ALWAYS)).toBe(true);
    expect(panels[1].nodeCount).toBeGreaterThan(panels[0].nodeCount);
    const {header} = readProgram(result.program);
    expect(header.nodeCount).toBe(panels[1].nodeCount);
    expect(header.arenaBytes).toBe(
      Math.max(...panels.map(panel => panel.arenaBytes)),
    );
  });

  it('stores a table the layouts share once, and one that differs per layout', () => {
    const {program} = compilePackage(RESPONSIVE_APP, 'app', PANELS);
    const entries = directoryOf(program);
    const of = id => entries.filter(entry => entry.id === Table[id]);
    expect(of('templates').map(entry => entry.panel)).toEqual([0, 1]);
    expect(of('templates')[0].offset).not.toBe(of('templates')[1].offset);
    expect(of('arena').map(entry => entry.panel)).toEqual([NONE]);
    // Every layout's function bodies are in one code table, and the checks table is for all of them.
    expect(of('code').map(entry => entry.panel)).toEqual([NONE]);
    expect(of('checks').map(entry => entry.panel)).toEqual([NONE]);
  });

  it('has one layout for every size that picks it', () => {
    const one = compilePackage(RESPONSIVE_APP, 'app', SCREEN);
    const two = compilePackage(RESPONSIVE_APP, 'app', {
      screens: [
        {width: 240, height: 320},
        {width: 200, height: 300},
      ],
    });
    expect(two.panels).toHaveLength(1);
    expect(two.program).toEqual(one.program);
  });

  it('gives the same bytes at any size that picks the layout', () => {
    const at = (width, height) =>
      compilePackage(RESPONSIVE_APP, 'app', {screen: {width, height}}).program;
    expect(at(320, 240)).toEqual(at(1000, 7));
  });

  it("keeps each layout's needs to itself", () => {
    const app = `
import {View, Svg, Circle} from 'embedded-react';
const WIDE = screen.width > screen.height;
export function App() {
  return WIDE ? <Svg width={9} height={9}><Circle cx={1} cy={1} r={1} fill="#fff" /></Svg> : <View />;
}`;
    const {u16, u32, header} = readProgram(
      compilePackage(app, 'app', {}).program,
    );
    const needs = Array.from({length: u16(NEED_COUNT_AT)}, (_, index) => {
      const needOffset =
        HEADER_BYTES +
        header.panelCount * PANEL_RECORD_BYTES +
        index * NEED_RECORD_BYTES;
      return {
        resource: u16(needOffset),
        panel: u16(needOffset + 2),
        amount: u32(needOffset + 4),
      };
    });
    expect(needs).toContainEqual({
      resource: Resource.vectorNodes,
      panel: 1,
      amount: expect.any(Number),
    });
    expect(needs.some(need => need.panel === 0)).toBe(false);
  });

  it('refuses a panel listed twice', () => {
    expect(() =>
      compilePackage(STATIC_APP, 'app', {
        screens: [PANELS.screens[0], PANELS.screens[0]],
      }),
    ).toThrow(/the panel 240x320 is listed twice/);
  });

  it('reports each layout, and each copy of a table stored per layout', () => {
    const result = compilePackage(RESPONSIVE_APP, 'app', PANELS);
    const report = resourceReport(result, packPackage(result));
    expect(report[0]).toMatch(/^ {2}layout 240x320: arena \d+ B, \d+ nodes$/);
    expect(report[1]).toMatch(/^ {2}layout 320x240: arena \d+ B, \d+ nodes$/);
    expect(report[3]).toMatch(/templates \d+\+\d+/);
  });
});

describe('strings', () => {
  const STRINGS_APP = `
import {useState} from 'react';
import {Pressable, Text} from 'embedded-react';
export function App() {
  const [count, setCount] = useState(0);
  const [level, setLevel] = useState(0.5);
  const [label, setLabel] = useState('');
  return (
    <Pressable onPress={() => { const line = \`\${count} at \${level.toFixed(1)}\`; setLabel(line); setCount(count + 1); }}>
      <Text>{level.toFixed(2)} {count.toFixed(2).padStart(6, '0')} {label}</Text>
    </Pressable>
  );
}`;

  const instructions = (functionRecord, name) =>
    functionRecord.ops.filter(instruction => instruction.name === name);

  it('prints a float with FMTFX, and a whole number exactly, padded to leave room for its zeros', () => {
    const functions = functionsOf(
      compilePackage(STRINGS_APP, 'app', SCREEN).program,
    );
    const update = functions.at(-1);
    expect(update.kind).toBe(FunctionKind.update);
    expect(instructions(update, 'FMTFX').map(op => op.bx & 0xff)).toEqual([2]);
    expect(instructions(update, 'FMTPAD')).toEqual([
      expect.objectContaining({a: 3, bx: '0'.charCodeAt(0)}),
    ]);
    const names = update.ops.map(instruction => instruction.name);
    expect(names.indexOf('FMTPAD')).toBe(names.indexOf('FMTI') + 1);
    expect(names[names.indexOf('FMTPAD') + 1]).toBe('FMTK');
  });

  it("pads an interned string state's text right after printing it", () => {
    const source = `
import {useState} from 'react';
import {Pressable, Text} from 'embedded-react';
export function App() {
  const [mode, setMode] = useState('a');
  return <Pressable onPress={() => setMode('bb')}><Text>{mode.padStart(4, '-')}</Text></Pressable>;
}`;
    const update = functionsOf(
      compilePackage(source, 'app', SCREEN).program,
    ).at(-1);
    const names = update.ops.map(instruction => instruction.name);
    expect(names).toContain('FMTE');
    expect(names[names.indexOf('FMTE') + 1]).toBe('FMTPAD');
    expect(instructions(update, 'FMTPAD')).toEqual([
      expect.objectContaining({a: 4, bx: '-'.charCodeAt(0)}),
    ]);
  });

  it('pads a boolean word right after printing it', () => {
    const source = `
import {useState} from 'react';
import {Pressable, Text} from 'embedded-react';
export function App() {
  const [on, setOn] = useState(false);
  return <Pressable onPress={() => setOn(!on)}><Text>{String(on).padStart(6, '.')}</Text></Pressable>;
}`;
    const update = functionsOf(
      compilePackage(source, 'app', SCREEN).program,
    ).at(-1);
    const names = update.ops.map(instruction => instruction.name);
    expect(names[names.indexOf('FMTB') + 1]).toBe('FMTPAD');
    expect(instructions(update, 'FMTPAD')).toEqual([
      expect.objectContaining({a: 6, bx: '.'.charCodeAt(0)}),
    ]);
  });

  it('cuts and upper-cases a string right after printing it, its positions loaded first', () => {
    const source = `
import {useState} from 'react';
import {Pressable, Text} from 'embedded-react';
export function App() {
  const [name, setName] = useState('ada');
  const [count, setCount] = useState(1);
  return <Pressable onPress={() => { setName(name + 'b'); setCount(count + 1); }}><Text>{name.slice(count, -1).toUpperCase()}</Text></Pressable>;
}`;
    const update = functionsOf(
      compilePackage(source, 'app', SCREEN).program,
    ).at(-1);
    const names = update.ops.map(instruction => instruction.name);
    const printed = names.indexOf('FMTS');
    expect(names.slice(printed, printed + 3)).toEqual([
      'FMTS',
      'FMTSLICE',
      'FMTUPPER',
    ]);
    const [slice] = instructions(update, 'FMTSLICE');
    const loads = update.ops.slice(0, update.ops.indexOf(slice));
    const loadOf = register =>
      loads.findLast(instruction => instruction.a === register)?.name;
    expect(loadOf(slice.a)).toBe('GET32');
    expect(loadOf(slice.bx & 0xff)).toBe('LOADI');
    expect(loads.indexOf(loads.findLast(op => op.a === slice.a))).toBeLessThan(
      names.indexOf('FMTS'),
    );
  });

  it("cuts an interned string state's text right after printing it", () => {
    const source = `
import {useState} from 'react';
import {Pressable, Text} from 'embedded-react';
export function App() {
  const [mode, setMode] = useState('auto');
  return <Pressable onPress={() => setMode('heat')}><Text>{mode.slice(0, 1)}</Text></Pressable>;
}`;
    const update = functionsOf(
      compilePackage(source, 'app', SCREEN).program,
    ).at(-1);
    const names = update.ops.map(instruction => instruction.name);
    expect(names[names.indexOf('FMTE') + 1]).toBe('FMTSLICE');
  });

  it('searches one string for another with SHAS', () => {
    const source = `
import {useState} from 'react';
import {Pressable, Text} from 'embedded-react';
export function App() {
  const [name, setName] = useState('ada');
  return <Pressable onPress={() => setName('bob')}><Text>{name.includes('d') ? 'has d' : 'no d'}</Text></Pressable>;
}`;
    const update = functionsOf(
      compilePackage(source, 'app', SCREEN).program,
    ).at(-1);
    const names = update.ops.map(instruction => instruction.name);
    expect(names).toContain('SHAS');
    expect(names).toContain('GETS');
    expect(names).not.toContain('FMTE');
  });

  it("builds a handler's template local in a buffer of its own", () => {
    const [press] = functionsOf(
      compilePackage(STRINGS_APP, 'app', SCREEN).program,
    );
    const names = press.ops.map(instruction => instruction.name);
    expect(names).toEqual(
      expect.arrayContaining(['FMTI', 'FMTK', 'FMTFX', 'FMTSET', 'FMTS']),
    );
    expect(instructions(press, 'FMTFX').map(op => op.bx & 0xff)).toEqual([1]);
  });
});

describe('timers and effects', () => {
  const TIMER_APP = `
import {useState, useRef} from 'react';
import {View, Pressable, Text} from 'embedded-react';
export function App() {
  const [ticks, setTicks] = useState(0);
  const timer = useRef(-1);
  return (
    <View>
      <Pressable onPress={() => { const id = setInterval(() => setTicks(ticks + 1), 100); timer.current = id; }}>
        <Text>{ticks}</Text>
      </Pressable>
      <Pressable onPress={() => clearInterval(timer.current)}><Text>stop</Text></Pressable>
    </View>
  );
}`;

  const EFFECT_APP = `
import {useState, useEffect} from 'react';
import {Pressable, Text} from 'embedded-react';
export function App() {
  const [count, setCount] = useState(0);
  const [doubled, setDoubled] = useState(0);
  useEffect(() => {
    setDoubled(count * 2);
    const id = setTimeout(() => setCount(count), 100);
    return () => clearTimeout(id);
  }, [count]);
  useEffect(() => {
    setCount(1);
  }, []);
  return <Pressable onPress={() => setCount(count + 1)}><Text>{count} {doubled}</Text></Pressable>;
}`;

  const names = functionRecord =>
    functionRecord.ops.map(instruction => instruction.name);

  it('starts each timer on its callback, in a table maxTimers sizes', () => {
    const {program} = compilePackage(TIMER_APP, 'app', SCREEN);
    const functions = functionsOf(program);
    expect(functions.map(functionRecord => functionRecord.kind)).toEqual([
      0, // ER_EVENT_PRESS
      0,
      FunctionKind.timer,
      FunctionKind.update,
    ]);
    // A repeating timer on function 2, and the clear.
    expect(
      functions[0].ops.find(instruction => instruction.name === 'TSTART'),
    ).toMatchObject({
      c: 1,
      ext: 2,
    });
    expect(names(functions[1])).toContain('TCLEAR');
    // The callback set state, so it requests the update as a handler does.
    expect(names(functions[2])).toContain('NOT');

    const timers = regions =>
      regions.find(region => region.kind === Region.timers);
    expect(timers(regionsOf(program))).toEqual({
      kind: Region.timers,
      count: 8,
      bytes: 128,
    });
    const smaller = compilePackage(TIMER_APP, 'app', {...SCREEN, maxTimers: 3});
    expect(timers(regionsOf(smaller.program))).toMatchObject({
      count: 3,
      bytes: 48,
    });
    // No effect queues anything, so there is no event queue.
    expect(
      regionsOf(program).some(region => region.kind === Region.events),
    ).toBe(false);
  });

  it('converts a fractional delay to whole milliseconds, and a whole one as it is', () => {
    const delayed = delay => `
import {useState} from 'react';
import {Pressable, Text} from 'embedded-react';
export function App() {
  const [done, setDone] = useState(0);
  return <Pressable onPress={() => setTimeout(() => setDone(1), ${delay})}><Text>{done}</Text></Pressable>;
}`;
    const handler = delay =>
      names(
        functionsOf(compilePackage(delayed(delay), 'app', SCREEN).program)[0],
      );
    // ToInt32 is the low 32 bits; past 2^31 - 1 the delay is 0.
    const fraction = handler('1.5');
    expect(fraction).toEqual(
      expect.arrayContaining(['FTOL', 'MODL', 'LLT', 'LTOI', 'TSTART']),
    );
    expect(fraction.indexOf('FTOL')).toBeLessThan(fraction.indexOf('TSTART'));
    expect(handler('100')).not.toContain('MODL');
  });

  it('queues each effect from the update, and the mount effects one after another', () => {
    const {program} = compilePackage(EFFECT_APP, 'app', SCREEN);
    const functions = functionsOf(program);
    // The press, the timeout, the dep check, the two mount effects, then the update.
    expect(functions.map(functionRecord => functionRecord.kind)).toEqual([
      0,
      FunctionKind.timer,
      FunctionKind.effect,
      FunctionKind.effect,
      FunctionKind.effect,
      FunctionKind.update,
    ]);
    const [, , check, firstMount, secondMount, update] = functions;
    const effects = functionRecord =>
      functionRecord.ops
        .filter(instruction => instruction.name === 'EFFECT')
        .map(instruction => instruction.bx);
    // Every pass queues the dep check; the first one, the first mount effect too.
    expect(effects(update)).toEqual([2, 3]);
    // The dep-driven mount effect asks for no update, so it queues the next one itself.
    expect(effects(firstMount)).toEqual([4]);
    expect(effects(secondMount)).toEqual([]);
    expect(names(secondMount)).toContain('NOT');
    // Each run clears the last run's timeout, then starts its own.
    expect(names(check).indexOf('TCLEAR')).toBeLessThan(
      names(check).indexOf('TSTART'),
    );
    // A pass only the effect's write started returns before it sets anything.
    expect(update.ops[0].name).toBe('JDIRTY');

    expect(
      regionsOf(program).find(region => region.kind === Region.events),
    ).toEqual({kind: Region.events, count: 2, bytes: 64});
  });

  it('keeps an effect local its cleanup reads, the armed flag and the dep copy in slots', () => {
    const {program} = compilePackage(EFFECT_APP, 'app', SCREEN);
    const {u16, tables} = readProgram(program);
    const slots = tables.get(Table.slots);
    const untracked = Array.from(
      {length: slots.length / SLOT_RECORD_BYTES},
      (_, index) => u16(slots.offset + index * SLOT_RECORD_BYTES),
    ).filter(word => !(word >> 8) && (word & 0xff) !== ValueType.node);
    // The timeout's id, the armed flag and the copy of count, then the pending flag and the mount's next.
    expect(untracked.map(word => word & 0xff).sort()).toEqual(
      [
        ValueType.int,
        ValueType.bool,
        ValueType.int,
        ValueType.bool,
        ValueType.int,
      ].sort(),
    );
  });

  it('refuses an effect without deps', () => {
    const source = `
import {useEffect, useRef} from 'react';
import {View} from 'embedded-react';
export function App() {
  const runs = useRef(0);
  useEffect(() => {
    runs.current += 1;
  });
  return <View />;
}`;
    expect(() => compilePackage(source, 'app', SCREEN)).toThrow(
      'useEffect needs a dependency list',
    );
  });
});

describe('lists', () => {
  const LIST_APP = `
import {useState} from 'react';
import {View, Pressable, Text} from 'embedded-react';
export function App() {
  const [items, setItems] = useState([{label: 'one', n: 1, w: 2.5}, {label: 'two', n: 2, w: 3}]);
  const [picked, setPicked] = useState(0);
  return (
    <View>
      <Pressable onPress={() => setItems([...items, {label: 'row ' + items.length, n: items.length, w: 1.5}])}><Text>add</Text></Pressable>
      <Pressable onPress={() => setItems(items.slice(0, -1))}><Text>drop</Text></Pressable>
      <Pressable onPress={() => setItems(items.slice(0, 1))}><Text>keep</Text></Pressable>
      <Pressable onPress={() => setItems(items.slice(0, picked))}><Text>cut</Text></Pressable>
      <Pressable onPress={() => setItems([])}><Text>clear</Text></Pressable>
      {items.map((item, index) => (
        <Pressable key={index} style={{width: item.w}} onPress={() => setPicked(picked + item.n)}>
          <Text>{item.label} {index}</Text>
        </Pressable>
      ))}
      <Text>{picked}</Text>
    </View>
  );
}`;
  const CAPS = {...SCREEN, listCap: 4};

  const names = functionRecord =>
    functionRecord.ops.map(instruction => instruction.name);

  /** The slots: each one's type, flags, cap, and aux. */
  function slotsOf(bytes) {
    const {u16, tables} = readProgram(bytes);
    const {offset, length} = tables.get(Table.slots);
    return Array.from({length: length / SLOT_RECORD_BYTES}, (_, index) => {
      const slotOffset = offset + index * SLOT_RECORD_BYTES;
      return {
        type: u16(slotOffset) & 0xff,
        flags: u16(slotOffset) >> 8,
        cap: u16(slotOffset + 12),
        aux: u16(slotOffset + 14),
      };
    });
  }

  it('keeps a list as a pool of rows, laid out by its row layout', () => {
    const {program} = compilePackage(LIST_APP, 'app', CAPS);
    const {u16, u32, tables} = readProgram(program);

    // One layout: the label as a 16-byte string ('row ' and an int, and a NUL), then the int and the float.
    const rows = tables.get(Table.rows);
    expect(u32(rows.offset)).toBe(1);
    expect([u16(rows.offset + 4), u16(rows.offset + 8)]).toEqual([24, 3]);
    const fields = Array.from({length: 3}, (_, index) => {
      const fieldOffset = rows.offset + 12 + index * 8;
      return {
        type: program[fieldOffset],
        cap: u16(fieldOffset + 2),
        offset: u32(fieldOffset + 4),
      };
    });
    expect(fields).toEqual([
      {type: ValueType.string, cap: 15, offset: 0},
      {type: ValueType.int, cap: 0, offset: 16},
      {type: ValueType.float, cap: 0, offset: 20},
    ]);

    // A node reads it, so it has a dirty bit; its pool is the row count, then four rows.
    expect(slotsOf(program).find(slot => slot.type === ValueType.list)).toEqual(
      {type: ValueType.list, flags: SlotFlag.tracked, cap: 4, aux: 0},
    );
    expect(
      regionsOf(program).find(region => region.kind === Region.lists),
    ).toEqual({kind: Region.lists, count: 0, bytes: 4 + 4 * 24});
  });

  it('builds every row from one row template, its handles in arrays by row', () => {
    const {program} = compilePackage(LIST_APP, 'app', CAPS);
    const templates = templatesOf(program);
    expect(templates.map(template => template.kind)).toEqual([
      TemplateKind.root,
      TemplateKind.row,
    ]);
    const [root, row] = templates;
    // The root, the view, five buttons with their text, the picked text; then four rows of two.
    expect(readProgram(program).panel.nodeCount).toBe(
      root.nodes.length + 4 * row.nodes.length,
    );
    expect(root.nodes.length).toBe(13);
    const slots = slotsOf(program);
    for (const {handle} of row.nodes) {
      expect(slots[handle]).toMatchObject({type: ValueType.node, cap: 4});
    }

    const functions = functionsOf(program);
    const update = functions.at(-1);
    expect(update.kind).toBe(FunctionKind.update);
    const builds = update.ops.filter(
      instruction => instruction.name === 'BUILD',
    );
    expect(builds.map(build => build.ext & 0xffff)).toEqual([1, 1, 1, 1]);
    expect(names(update)).toContain('GETNI');
    expect(names(update)).toContain('ROWGET');

    // The boot function appends the first two items.
    const boot = functions[readProgram(program).panel.boot];
    expect(names(boot).filter(name => name === 'APPEND')).toHaveLength(2);
  });

  it('shares the rows one handler, entered with its row', () => {
    const {program} = compilePackage(LIST_APP, 'app', CAPS);
    const {u32, tables} = readProgram(program);
    const functions = tables.get(Table.functions);
    const records = Array.from(
      {length: functions.length / FUNCTION_RECORD_BYTES},
      (_, index) =>
        u32(
          functions.offset + index * FUNCTION_RECORD_BYTES + FUNCTION_INFO_AT,
        ),
    );
    // Five buttons and one row press; only the row's takes its row.
    const presses = records.filter(record => record >>> 24 === 0);
    expect(presses).toHaveLength(6);
    expect(presses.map(record => (record >>> 8) & 0xff)).toEqual([
      0, 0, 0, 0, 0, 1,
    ]);

    const instructionNames = functionsOf(program).slice(0, 5).map(names);
    expect(instructionNames[0]).toEqual(
      expect.arrayContaining(['FMTSET', 'APPEND']),
    );
    expect(instructionNames[1]).toContain('POP');
    expect(instructionNames[2]).toContain('KEEP');
    expect(instructionNames[3]).toContain('SLICE');
    expect(instructionNames[4]).toContain('CLEAR');
  });

  it('gives rows that differ by their index their own templates', () => {
    const source = `
import {useState} from 'react';
import {View} from 'embedded-react';
export function App() {
  const [dots, setDots] = useState([{size: 4}]);
  return <View>{dots.map((dot, index) => <View key={index} style={{width: dot.size}}><View style={{height: index % 2 ? 2 : 3}} /></View>)}</View>;
}`;
    const templates = templatesOf(compilePackage(source, 'app', CAPS).program);
    expect(templates.map(template => template.kind)).toEqual([
      TemplateKind.root,
      TemplateKind.row,
      TemplateKind.row,
    ]);
  });

  it('refuses a row or a node count too big for its u16 field', () => {
    const long = 'x'.repeat(32767);
    const texts = `
import {useState} from 'react';
import {Text} from 'embedded-react';
export function App() {
  const [items] = useState([{a: '${long}', b: '${long}'}]);
  return <Text>{items.length}</Text>;
}`;
    expect(() => compilePackage(texts, 'app', SCREEN)).toThrow(
      'a row of list state "items" takes 65536 bytes, but a package row holds 65532',
    );

    const rows = `
import {useState} from 'react';
import {View} from 'embedded-react';
export function App() {
  const [items] = useState([{n: 1}]);
  return <View>{items.map((item, index) => <View key={index} style={{width: item.n}}><View /><View /></View>)}</View>;
}`;
    expect(() =>
      compilePackage(rows, 'app', {...SCREEN, listCap: 22000}),
    ).toThrow('the app needs 66002 nodes, but a package holds 65535');
  });

  it('refuses more row fields than ROWGET and the rows table can name', () => {
    const item = count =>
      `{${Array.from({length: count}, (_, index) => `f${index}: ${index}`).join(', ')}}`;
    const lists = (count, fields) => `
import {useState} from 'react';
import {View} from 'embedded-react';
export function App() {
  ${Array.from({length: count}, (_, index) => `const [l${index}] = useState([${item(fields)}]);`).join('\n  ')}
  return <View />;
}`;
    expect(() => compilePackage(lists(1, 256), 'app', SCREEN)).toThrow(
      'list state "l0" has 256 fields, but a package row holds 255',
    );
    expect(() => compilePackage(lists(258, 255), 'app', SCREEN)).toThrow(
      'the lists have 65790 fields in all, but a package holds 65535',
    );
  });

  it('reuses the registers each first item is appended from', () => {
    const items = `[${Array.from({length: 16}, (_, index) => `{a: ${index}, b: 1.5, c: 'x', d: 2}`).join(', ')}]`;
    const source = `
import {useState} from 'react';
import {Text} from 'embedded-react';
export function App() {
  ${Array.from({length: 5}, (_, index) => `const [l${index}] = useState(${items});`).join('\n  ')}
  return <Text>{l0.length}</Text>;
}`;
    const {program} = compilePackage(source, 'app', SCREEN);
    const boot = functionsOf(program)[readProgram(program).panel.boot];
    expect(
      boot.ops.filter(instruction => instruction.name === 'APPEND'),
    ).toHaveLength(80);
    // Each append reads the same four registers.
    expect(
      new Set(
        boot.ops
          .filter(instruction => instruction.name === 'APPEND')
          .map(instruction => instruction.a),
      ).size,
    ).toBe(1);
  });

  /** The fields of the one row layout: each one's type and cap. */
  function rowFieldsOf(bytes) {
    const {u16, tables} = readProgram(bytes);
    const rows = tables.get(Table.rows);
    return Array.from({length: u16(rows.offset + 8)}, (_, index) => {
      const fieldOffset = rows.offset + 12 + index * 8;
      return {type: bytes[fieldOffset], cap: u16(fieldOffset + 2)};
    });
  }

  it('flattens nested fields, and gives each optional field a flag after the values', () => {
    const source = `
import {useState} from 'react';
import {Pressable, Text} from 'embedded-react';
export function App() {
  const [items, setItems] = useState([{t: 'a', pos: {x: 1}, done: false, note: 'n'}, {t: 'b', pos: {x: 2}, done: true}]);
  return <Pressable onPress={() => setItems([...items, {t: 'c', done: true}])}><Text>{items[0].pos.x} {items[1].note}</Text></Pressable>;
}`;
    const {program} = compilePackage(source, 'app', CAPS);
    // t, pos.x, done, note; then the flags of note (left out of the second item) and pos.x (of the append).
    expect(rowFieldsOf(program).map(field => field.type)).toEqual([
      ValueType.string,
      ValueType.int,
      ValueType.bool,
      ValueType.string,
      ValueType.bool,
      ValueType.bool,
    ]);
    const boot = functionsOf(program)[readProgram(program).panel.boot];
    expect(names(boot).filter(name => name === 'APPEND')).toHaveLength(2);
  });

  it('faults on items[i].field past the end, and reads items[i]?.field as missing there', () => {
    const app = read => `
import {useState} from 'react';
import {Text} from 'embedded-react';
export function App() {
  const [items] = useState([{t: 'a', note: 'n'}, {t: 'b'}]);
  const [at, setAt] = useState(0);
  return <Text onPress={() => setAt(at + 1)}>{${read}}</Text>;
}`;
    const updateOf = read => {
      const {program} = compilePackage(app(read), 'app', CAPS);
      return names(functionsOf(program).at(-1));
    };
    const plain = updateOf('items[at].t');
    expect(plain).toContain('ROWGET');
    expect(plain).not.toContain('LEN');
    // A test of whether a field is there reads it first, so it faults past the end as well.
    const tested = updateOf("items[at].t === undefined ? 'gone' : 'here'");
    expect(tested).toContain('ROWGET');
    expect(tested).not.toContain('LEN');
    // An optional field's test reads its flag, which faults the same way.
    const optional = updateOf("items[at].note ?? 'none'");
    expect(optional).toContain('ROWGET');
    expect(optional).not.toContain('LEN');
    const guarded = updateOf('items[at]?.t');
    expect(guarded).toEqual(
      expect.arrayContaining(['LEN', 'LT', 'LE', 'ROWGET']),
    );
  });

  it('keeps a constant array read by index as a list its boot fills, which nothing tracks', () => {
    const source = `
import {useState} from 'react';
import {Text} from 'embedded-react';
const NAMES = ['zero', 'one', 'two'];
export function App() {
  const [at, setAt] = useState(0);
  return <Text onPress={() => setAt(at + 1)}>{NAMES[at] ?? 'none'}</Text>;
}`;
    const {program} = compilePackage(source, 'app', CAPS);
    expect(slotsOf(program).find(slot => slot.type === ValueType.list)).toEqual(
      {type: ValueType.list, flags: 0, cap: 3, aux: 0},
    );
    expect(rowFieldsOf(program)).toEqual([{type: ValueType.string, cap: 4}]);

    // Its capacity is a u16 in the package, as a list state's is.
    const long = source.replace(
      "['zero', 'one', 'two']",
      `[${Array.from({length: 32768}, (_, index) => index).join(', ')}]`,
    );
    expect(() => compilePackage(long, 'app', CAPS)).toThrow(
      'the array "NAMES" has 32768 elements, but an array read by a runtime index holds 32767',
    );
    const boot = functionsOf(program)[readProgram(program).panel.boot];
    expect(names(boot).filter(name => name === 'APPEND')).toHaveLength(3);
  });

  it('refuses list shapes and reads it cannot lower', () => {
    const app = (initial, read, extra = '', press = 'setAt(at + 1)') => `
import {useState} from 'react';
import {Pressable, Text} from 'embedded-react';
export function App() {
  const [items, setItems] = useState(${initial});
  const [at, setAt] = useState(0.5);
  ${extra}
  return <Pressable onPress={() => ${press}}><Text>{${read}}</Text></Pressable>;
}`;
    const refuses = (source, message) =>
      expect(() => compilePackage(source, 'app', CAPS)).toThrow(message);
    refuses(
      app("[{t: 'a'}, {t: 1}]", 'items.length'),
      'field "t" of list state "items" is a string in one item and not in another',
    );
    refuses(
      app("[{t: 'a', tags: ['x']}]", 'items.length'),
      'field "tags" of list state "items" must be a string',
    );
    refuses(
      app("[1, 'a']", 'items.length'),
      'list state "items" must hold objects, or values of one kind',
    );
    refuses(
      app("[{t: 'a'}]", 'items[at].t'),
      'an index into "items" must be a whole number',
    );
    refuses(
      app("[{t: 'a'}]", 'rows[Math.floor(at)].v', 'const rows = [{v: at}];'),
      'an array built from runtime values cannot be read by a runtime index',
    );
    refuses(app("[{t: 'a'}]", 'at?.x'), '`?.` is only lowered on a list item');
    refuses(
      app(
        "[{t: 'a'}]",
        'items.length',
        '',
        "setItems([...items, {t: 'b', z: 1}])",
      ),
      'list state "items" has no field "z"',
    );
  });

  it('refuses a list inside a list row, and onChangeText in one', () => {
    const nested = `
import {useState} from 'react';
import {View} from 'embedded-react';
export function App() {
  const [rows] = useState([{n: 1}]);
  const [cells] = useState([{n: 2}]);
  return <View>{rows.map((row, index) => <View key={index}>{cells.map((cell, at) => <View key={at} style={{width: cell.n}} />)}</View>)}</View>;
}`;
    expect(() => compilePackage(nested, 'app', CAPS)).toThrow(
      /doesn't support a list inside a list row yet[\s\S]*App\.jsx:7:/,
    );
    const input = `
import {useState} from 'react';
import {View, TextInput} from 'embedded-react';
export function App() {
  const [rows] = useState([{n: 1}]);
  const [text, setText] = useState('');
  return <View>{rows.map((row, index) => <TextInput key={index} value={text} onChangeText={value => setText(value)} />)}</View>;
}`;
    expect(() => compilePackage(input, 'app', CAPS)).toThrow(
      "doesn't support onChangeText in a list row yet",
    );
  });
});

describe('list edits', () => {
  const CAPS = {...SCREEN, listCap: 4};

  /** An app whose one button runs a handler on a list of tasks, and a list of plain counts. */
  const editApp = handler => `
import {useState} from 'react';
import {View, Pressable, Text} from 'embedded-react';
export function App() {
  const [items, setItems] = useState([{title: 'a', n: 1, done: false, pos: {x: 1, y: 2}}, {title: 'b', n: 2, done: true, pos: {x: 3, y: 4}}]);
  const [counts, setCounts] = useState([3, 1]);
  const [picked, setPicked] = useState(0);
  return (
    <View>
      <Pressable onPress={() => ${handler}}><Text>edit</Text></Pressable>
      <Pressable onPress={() => setPicked(picked + 1)}><Text>next</Text></Pressable>
      {items.map((item, index) => (
        <Text key={index}>{item.title} {item.n} {item.done ? 'y' : 'n'} {item.pos.x} {item.pos.y}</Text>
      ))}
      {counts.map((count, index) => <Text key={index}>{count}</Text>)}
    </View>
  );
}`;

  /** The instructions of the edit button's handler. */
  const handlerOps = handler =>
    functionsOf(
      compilePackage(editApp(handler), 'app', CAPS).program,
    )[0].ops.map(instruction => instruction.name);

  const refuses = (handler, message) =>
    expect(() => compilePackage(editApp(handler), 'app', CAPS)).toThrow(
      message,
    );

  it('removes an item by index with REMOVE, however it is written', () => {
    for (const handler of [
      'setItems(items.filter((_, i) => i !== picked))',
      'setItems(items.filter((item, i) => picked != i))',
      'setItems([...items.slice(0, picked), ...items.slice(picked + 1)])',
      'setItems(prev => prev.filter((_, i) => i !== picked))',
      'setCounts([...counts.slice(0, 1), ...counts.slice(2)])',
    ]) {
      const ops = handlerOps(handler);
      expect(ops.filter(name => name === 'REMOVE')).toHaveLength(1);
      expect(ops).not.toContain('MAX');
    }
    // toSpliced counts a negative index back from the end, and no further than the first item.
    expect(handlerOps('setItems(items.toSpliced(picked, 1))')).toEqual(
      expect.arrayContaining(['LEN', 'ADD', 'MAX', 'REMOVE']),
    );
  });

  it('updates items in a counted loop that stops at the count', () => {
    const ops = handlerOps(
      'setItems(items.map((item, i) => i === picked ? {...item, n: item.n + 1} : item))',
    );
    const loop = ops.slice(ops.indexOf('LOOPSET'), ops.indexOf('LOOP') + 1);
    expect(loop.slice(0, 4)).toEqual(['LOOPSET', 'SUB', 'LT', 'JF']);
    expect(loop).toContain('EQ');
    expect(loop.filter(name => name === 'ROWSET')).toHaveLength(1);
    // Every row is visited, so an unconditional map has no test.
    expect(
      handlerOps('setCounts(counts.map(count => count * 2))').filter(
        name => name === 'JT' || name === 'EQ',
      ),
    ).toEqual([]);
  });

  it('works every value out before it stores any', () => {
    const ops = handlerOps(
      "setItems(items.map(item => ({...item, title: item.title + '!', n: item.pos.x, pos: {...item.pos, x: item.n}})))",
    );
    const stores = ops.flatMap((name, index) =>
      name === 'ROWSET' ? [index] : [],
    );
    expect(stores).toHaveLength(3);
    expect(ops.lastIndexOf('ROWGET')).toBeLessThan(stores[0]);
    expect(stores).toEqual([stores[0], stores[0] + 1, stores[0] + 2]);
  });

  it('sets one item with `with`, which faults past the end', () => {
    const ops = handlerOps(
      "setItems(items.with(picked, {...items[picked], title: 'z', done: !items[picked].done}))",
    );
    expect(ops).not.toContain('LOOP');
    expect(ops.indexOf('ROWGET')).toBeLessThan(ops.indexOf('ROWSET'));
    expect(ops.filter(name => name === 'ROWSET')).toHaveLength(2);
  });

  it('makes a field set to undefined optional, its flag cleared', () => {
    const {program} = compilePackage(
      editApp(
        'setItems(items.map((item, i) => i === picked ? {...item, pos: {...item.pos, y: undefined}} : item))',
      ),
      'app',
      CAPS,
    );
    const {u16, u32, tables} = readProgram(program);
    const rows = tables.get(Table.rows);
    // title, n, done, pos.x, pos.y, then pos.y's flag.
    expect(u16(rows.offset + 8)).toBe(6);
    expect(u32(rows.offset)).toBe(2);
    const ops = functionsOf(program)[0].ops.filter(
      instruction => instruction.name === 'ROWSET',
    );
    expect(ops.map(instruction => instruction.ext >>> 16)).toEqual([4, 5]);
  });

  it('replaces an item without a spread, leaving out what it does not set', () => {
    const ops = handlerOps(
      "setItems(items.with(0, {title: 'z', n: 1, done: true, pos: {x: 1}}))",
    );
    // Every field, and pos.y's flag.
    expect(ops.filter(name => name === 'ROWSET')).toHaveLength(6);
  });

  it('checks the index of a `with` that changes nothing', () => {
    const ops = handlerOps('setItems(items.with(picked, items[picked]))');
    expect(ops).toContain('ROWGET');
    expect(ops).not.toContain('ROWSET');
  });

  it('counts a negative index back for a pair of slices, stopping at -1', () => {
    const sliced = handlerOps(
      'setItems([...items.slice(0, picked), ...items.slice(picked + 1)])',
    );
    expect(sliced).toEqual(
      expect.arrayContaining(['EQ', 'TRAP', 'LEN', 'ADD', 'REMOVE']),
    );
    expect(sliced).not.toContain('MAX');
    expect(
      handlerOps('setItems(items.filter((_, i) => i !== picked))'),
    ).not.toContain('TRAP');
  });

  it('takes the last of a key written twice', () => {
    const stores = handler =>
      handlerOps(handler).filter(name => name === 'ROWSET').length;
    // The value is kept, so the field isn't optional: one store.
    expect(
      stores('setItems(items.map(item => ({...item, n: undefined, n: 5})))'),
    ).toBe(1);
    // It is left out: the value and its flag.
    expect(
      stores('setItems(items.map(item => ({...item, n: 5, n: undefined})))'),
    ).toBe(2);
  });

  it('refuses what it cannot lower', () => {
    refuses(
      'setItems(items.filter(item => item.done))',
      'only items.filter((_, i) => i !== n) is supported on a list',
    );
    refuses(
      'setItems(items.filter((_, i) => i !== i + picked))',
      'only items.filter((_, i) => i !== n) is supported on a list',
    );
    refuses(
      'setItems(items.toSpliced(picked, 2))',
      'only items.toSpliced(n, 1) is supported on a list',
    );
    refuses(
      'setItems([...items.slice(0, picked), ...items.slice(picked + 2)])',
      'only [...items.slice(0, n), ...items.slice(n + 1)] is supported',
    );
    refuses(
      'setItems([...items.slice(0, -1), ...items.slice(0)])',
      'only [...items.slice(0, n), ...items.slice(n + 1)] is supported',
    );
    refuses(
      'setItems(items.with(0, {...items[1], n: 5}))',
      'an updated item can spread only the item it replaces, first',
    );
    refuses(
      'setItems(items.map(item => ({n: 5, ...item})))',
      'an updated item can spread only the item it replaces, first',
    );
    refuses(
      'setItems(items.map(item => item.n))',
      'an item of "items" must be replaced with an object literal',
    );
    refuses(
      'setItems(items.map(item => ({...item, size: 5})))',
      'list state "items" has no field "size"',
    );
    refuses(
      'setItems(items.map(item => ({...item, pos: 5})))',
      'field "pos" of list state "items" holds an object',
    );
    refuses(
      "setItems(items.map(item => ({...item, done: 'yes'})))",
      'field "done" of list state "items" holds a boolean',
    );
    refuses(
      'setItems(items.map(({n}) => n))',
      "items.map's function must name its parameters",
    );
    refuses(
      'setItems(items.with(picked + 0.5, items[0]))',
      'an index into "items" must be a whole number',
    );
    refuses(
      'setItems(items.map((item, i) => ({...item, n: items[0].n})))',
      "items.map's function can't read other items of items",
    );
    refuses(
      'setItems(prev => prev.map(item => (prev[1]?.done ? item : {...item, done: true})))',
      "prev.map's function can't read other items of prev",
    );
    refuses(
      'setItems(items.reverse())',
      'unsupported list operation on "items"',
    );
  });
});

describe('conditionals', () => {
  const SCREENS_APP = `
import {useState} from 'react';
import {View, Pressable, Text} from 'embedded-react';
export function App() {
  const [page, setPage] = useState('home');
  const [items, setItems] = useState([{n: 1}]);
  return (
    <View>
      <Pressable onPress={() => setPage(page === 'home' ? 'list' : 'home')}><Text>go</Text></Pressable>
      {page === 'home' ? (
        <View><Text>home</Text><Text>{page}</Text></View>
      ) : (
        <View>{items.map((item, index) => <View key={index} style={{width: item.n}} />)}</View>
      )}
      {page !== 'home' && <Text>away</Text>}
      <Text>footer</Text>
    </View>
  );
}`;
  const CAPS = {...SCREEN, listCap: 4};
  const names = functionRecord =>
    functionRecord.ops.map(instruction => instruction.name);

  it('gives each branch a template, and the panel the nodes of the largest', () => {
    const {program} = compilePackage(SCREENS_APP, 'app', CAPS);
    const templates = templatesOf(program);
    expect(templates.map(template => template.kind)).toEqual([
      TemplateKind.root,
      TemplateKind.branch,
      TemplateKind.branch,
      TemplateKind.branch,
      TemplateKind.row,
    ]);
    // The root, the view, the button and its text, the footer; home's three, or the list's view and four rows;
    // and the `&&` text.
    const [root, home, list, away, row] = templates;
    expect(root.nodes).toHaveLength(5);
    expect([home, list, away].map(branch => branch.nodes.length)).toEqual([
      3, 1, 1,
    ]);
    expect(row.nodes).toHaveLength(1);
    expect(readProgram(program).panel.nodeCount).toBe(5 + 1 + 4 + 1);
    // Both branches of the ternary keep their first node's handle in one slot.
    expect(home.nodes[0].handle).toBe(list.nodes[0].handle);
  });

  it('destroys the branches a test no longer picks before it builds any', () => {
    const {program} = compilePackage(SCREENS_APP, 'app', CAPS);
    const update = functionsOf(program).at(-1);
    expect(update.kind).toBe(FunctionKind.update);
    const ops = names(update);
    expect(ops.filter(name => name === 'DESTROY')).toHaveLength(2);
    expect(ops.lastIndexOf('DESTROY')).toBeLessThan(ops.indexOf('BUILD'));

    // A branch is no row: BUILD names no instance register. The list's rows are built with their branch.
    const builds = update.ops.filter(
      instruction => instruction.name === 'BUILD',
    );
    const branches = builds.filter(build => build.ext >>> 16 === 0xff);
    expect(new Set(branches.map(build => build.ext & 0xffff))).toEqual(
      new Set([1, 2, 3]),
    );
    expect(builds.length - branches.length).toBeGreaterThanOrEqual(4);
  });

  it('builds every branch of a conditional in a list row, and toggles it', () => {
    const source = `
import {useState} from 'react';
import {View, Text} from 'embedded-react';
export function App() {
  const [items] = useState([{on: 1}]);
  return <View>{items.map((item, index) => <View key={index}>{item.on > 0 ? <Text>on</Text> : <Text>off</Text>}</View>)}</View>;
}`;
    const {program} = compilePackage(source, 'app', CAPS);
    const templates = templatesOf(program);
    expect(templates.map(template => template.kind)).toEqual([
      TemplateKind.root,
      TemplateKind.row,
    ]);
    expect(templates[1].nodes).toHaveLength(3);
    expect(names(functionsOf(program).at(-1))).not.toContain('DESTROY');
  });

  it('refuses updateVector on a node a conditional builds and destroys', () => {
    const source = `
import {useRef, useState} from 'react';
import {View, Pressable, Svg, updateVector} from 'embedded-react';
export function App() {
  const pad = useRef(null);
  const [shown, setShown] = useState(true);
  return (
    <View>
      <Pressable onPress={() => updateVector(pad, [{rect: [0, 0, 10, 10], fill: '#ffffff'}], [0, 0, 10, 10])} />
      <Pressable onPress={() => setShown(!shown)} />
      {shown && <Svg ref={pad} width={100} height={20} />}
    </View>
  );
}`;
    expect(() => compilePackage(source, 'app', SCREEN)).toThrow(
      /updateVector draws on a node a conditional builds and destroys[\s\S]*App\.jsx:11:/,
    );
  });
});

describe('animations', () => {
  const ANIM_APP = `
import {useState} from 'react';
import {View, Pressable, Text, Animated, Easing, useAnimatedValue} from 'embedded-react';
export function App() {
  const [status, setStatus] = useState('idle');
  const [to, setTo] = useState(2);
  const fade = useAnimatedValue(0.5);
  const slide = useAnimatedValue(0);
  return (
    <View>
      <Pressable onPress={() => Animated.timing(fade, {toValue: 1, duration: 300, delay: 20, easing: Easing.out(Easing.quad)}).start(({finished}) => setStatus(finished ? 'done' : 'cut'))}>
        <Text>{status}</Text>
      </Pressable>
      <Pressable onPress={() => Animated.spring(slide, {toValue: to, stiffness: 90}).start()}><Text>go</Text></Pressable>
      <Pressable onPress={() => Animated.timing(fade, {toValue: 0}).stop()}><Text>stop</Text></Pressable>
      <View style={{opacity: fade, transform: [{translateX: slide.interpolate({inputRange: [0, 2], outputRange: [0, 50], extrapolate: 'clamp'})}]}} />
      <View style={{backgroundColor: slide.interpolate({inputRange: [0, 1], outputRange: ['#000000', '#ff8000']})}} />
    </View>
  );
}`;

  const names = functionRecord =>
    functionRecord.ops.map(instruction => instruction.name);

  /** The slots: each one's type and initial value. */
  function slotsOf(bytes) {
    const {u32, tables, view} = readProgram(bytes);
    const {offset, length} = tables.get(Table.slots);
    return Array.from({length: length / SLOT_RECORD_BYTES}, (_, index) => ({
      type: view.getUint8(offset + index * SLOT_RECORD_BYTES),
      init: u32(offset + index * SLOT_RECORD_BYTES + 8),
    }));
  }

  /** The animation descriptors. */
  function animsOf(bytes) {
    const {u16, u32, tables, view} = readProgram(bytes);
    const {offset, length} = tables.get(Table.animations);
    return Array.from({length: length / 40}, (_, index) => {
      const animationOffset = offset + index * 40;
      return {
        kind: view.getUint8(animationOffset),
        easing: view.getUint8(animationOffset + 1),
        flags: view.getUint8(animationOffset + 2),
        target: u16(animationOffset + 4),
        first: u16(animationOffset + 8),
        count: u16(animationOffset + 10),
        amount: u32(animationOffset + 12),
        delayMs: u32(animationOffset + 16),
        to: u32(animationOffset + 20),
        params: [0, 1, 2, 3].map(paramIndex =>
          u32(animationOffset + 24 + paramIndex * 4),
        ),
      };
    });
  }

  /** The bindings. */
  function bindingsOf(bytes) {
    const {u16, tables, view} = readProgram(bytes);
    const {offset, length} = tables.get(Table.bindings);
    return Array.from({length: length / 4}, (_, index) => ({
      kind: view.getUint8(offset + index * 4),
      event: view.getUint8(offset + index * 4 + 1),
      target: u16(offset + index * 4 + 2),
    }));
  }

  it('keeps each animated value in a slot, and declares the values and their animations as needs', () => {
    const {program} = compilePackage(ANIM_APP, 'app', SCREEN);
    const values = slotsOf(program).filter(
      slot => slot.type === ValueType.animatedValue,
    );
    expect(values.map(slot => slot.init)).toEqual([
      floatBits(0.5),
      floatBits(0),
    ]);

    const {u16, u32} = readProgram(program);
    const needs = Array.from({length: u16(NEED_COUNT_AT)}, (_, index) => ({
      resource: u16(
        HEADER_BYTES + PANEL_RECORD_BYTES + index * NEED_RECORD_BYTES,
      ),
      amount: u32(
        HEADER_BYTES + PANEL_RECORD_BYTES + index * NEED_RECORD_BYTES + 4,
      ),
    }));
    expect(needs).toEqual([
      {resource: Resource.animationValues, amount: 2},
      {resource: Resource.animations, amount: 2},
    ]);
    // A value's animation, and one more while a start cancels it.
    expect(
      regionsOf(program).find(region => region.kind === Region.animations),
    ).toEqual({kind: Region.animations, count: 3, bytes: 60});
  });

  it('starts each animation as a leaf, its toValue held in the descriptor or passed in a register', () => {
    const {program} = compilePackage(ANIM_APP, 'app', SCREEN);
    const anims = animsOf(program);
    const slots = slotsOf(program);
    const fade = slots.findIndex(slot => slot.type === ValueType.animatedValue);
    expect(anims[0]).toEqual({
      kind: AnimationKind.timing,
      easing: ENGINE_VALUES.get('ER_EASE_QUAD_OUT'),
      flags: 0,
      target: fade,
      first: 0,
      count: 0,
      amount: 300,
      delayMs: 20,
      to: floatBits(1),
      params: [0, 0, 0, 0],
    });
    expect(anims[1]).toMatchObject({
      kind: AnimationKind.spring,
      flags: AnimationFlag.toRegister,
      target: fade + 1,
      params: [floatBits(90), floatBits(18), floatBits(1), 0],
    });

    const functions = functionsOf(program);
    const press = functions[0].ops.find(
      instruction => instruction.name === 'ASTART',
    );
    expect(press.bx).toBe(0);
    expect(press.ext >>> 16).toBe(0xff);
    const go = functions[1].ops.find(
      instruction => instruction.name === 'ASTART',
    );
    expect(go).toMatchObject({bx: 1, ext: NONE | (go.ext & 0xff0000)});
    expect(go.ext >>> 16).not.toBe(0xff);
    // The stop freezes the value where it is.
    expect(names(functions[2])).toEqual(['AVGET', 'AVSET', 'RET']);
  });

  it('runs a completion as a helper, entered through a done function and called with finished', () => {
    const {program} = compilePackage(ANIM_APP, 'app', SCREEN);
    const functions = functionsOf(program);
    const done = functions.findIndex(
      functionRecord => functionRecord.kind === FunctionKind.animationDone,
    );
    const helper = functions.findIndex(
      functionRecord => functionRecord.kind === FunctionKind.helper,
    );
    expect(
      functions[0].ops.find(instruction => instruction.name === 'ASTART').ext &
        0xffff,
    ).toBe(done);
    expect(names(functions[done])).toEqual(['EVT', 'CALL', 'RET']);
    expect(functions[done].ops[1].bx).toBe(helper);
    // The helper comes after every caller, and requests the update its setState needs.
    expect(helper).toBeGreaterThan(done);
    expect(names(functions[helper])).toContain('NOT');

    // The register stack holds the done function's frame and the helper's.
    const {u32, tables} = readProgram(program);
    const records = tables.get(Table.functions);
    const cells = index =>
      u32(records.offset + index * FUNCTION_RECORD_BYTES + FUNCTION_INFO_AT) &
      0xff;
    const stack = regionsOf(program).find(
      region => region.kind === Region.stack,
    );
    expect(stack.count).toBeGreaterThanOrEqual(
      3 + cells(done) + 3 + cells(helper),
    );
  });

  it('binds a value straight, or through an interpolation onto numbers or colors', () => {
    const {program} = compilePackage(ANIM_APP, 'app', SCREEN);
    const {u16, u32, tables, view} = readProgram(program);
    const prop = field => ABI_PROPS.get(field).id;
    const slots = slotsOf(program);
    const fade = slots.findIndex(slot => slot.type === ValueType.animatedValue);
    expect(
      bindingsOf(program).filter(binding => binding.kind !== BindingKind.event),
    ).toEqual([
      {kind: BindingKind.animated, event: prop('opacity'), target: fade},
      {
        kind: BindingKind.interpolated,
        event: prop('transform_translate_x'),
        target: 0,
      },
      {
        kind: BindingKind.interpolated,
        event: prop('background_color'),
        target: 1,
      },
    ]);

    const {offset, length} = tables.get(Table.interpolations);
    expect(length).toBe(2 * 72);
    const record = index => {
      const recordOffset = offset + index * 72;
      return {
        slot: u16(recordOffset),
        points: view.getUint8(recordOffset + 2),
        flags: view.getUint8(recordOffset + 3),
        left: view.getUint8(recordOffset + 4),
        right: view.getUint8(recordOffset + 5),
        input: [
          u32(recordOffset + 8),
          u32(recordOffset + 12),
          u32(recordOffset + 16),
        ],
        output: [
          u32(recordOffset + 40),
          u32(recordOffset + 44),
          u32(recordOffset + 48),
        ],
      };
    };
    expect(record(0)).toEqual({
      slot: fade + 1,
      points: 2,
      flags: 0,
      left: ENGINE_VALUES.get('ER_EXTRAPOLATE_CLAMP'),
      right: ENGINE_VALUES.get('ER_EXTRAPOLATE_CLAMP'),
      input: [floatBits(0), floatBits(2), 0],
      output: [floatBits(0), floatBits(50), 0],
    });
    expect(record(1)).toMatchObject({
      flags: InterpolationFlag.color,
      left: ENGINE_VALUES.get('ER_EXTRAPOLATE_EXTEND'),
      output: [0xff000000, 0xffff8000, 0],
    });
  });

  it('chains a sequence through done functions, and waits on a trailing delay with a delay leaf', () => {
    const source = `
import {useState} from 'react';
import {Pressable, Text, Animated, useAnimatedValue} from 'embedded-react';
export function App() {
  const [n, setN] = useState(0);
  const a = useAnimatedValue(0);
  return (
    <Pressable onPress={() => Animated.sequence([Animated.timing(a, {toValue: 1, duration: 100}), Animated.timing(a, {toValue: 0, duration: 100}), Animated.delay(250)]).start(() => setN(n + 1))}>
      <Text>{n}</Text>
    </Pressable>
  );
}`;
    const {program} = compilePackage(source, 'app', SCREEN);
    const functions = functionsOf(program);
    const anims = animsOf(program);
    const dones = functions.flatMap((functionRecord, index) =>
      functionRecord.kind === FunctionKind.animationDone ? [index] : [],
    );
    // The completion's, the wait's, then the second step's.
    expect(dones).toHaveLength(3);
    const [completion, wait, second] = dones;
    expect(
      functions[0].ops.find(instruction => instruction.name === 'ASTART').ext &
        0xffff,
    ).toBe(second);
    expect(
      functions[second].ops.find(instruction => instruction.name === 'ASTART')
        .ext & 0xffff,
    ).toBe(wait);
    // An interrupted step calls the completion as not finished.
    expect(names(functions[second]).slice(0, 3)).toEqual(['EVT', 'JT', 'CALL']);
    const delay = functions[wait].ops.find(
      instruction => instruction.name === 'ASTART',
    );
    expect(delay.ext & 0xffff).toBe(completion);
    expect(anims[delay.bx]).toMatchObject({
      kind: AnimationKind.timing,
      target: NONE,
      amount: 250,
    });
    // The wait's handle is kept where a stop of the sequence finds it.
    expect(names(functions[wait])).toContain('SET32');
  });

  it('joins a spring and a longer delay as a parallel group', () => {
    const source = `
import {useState} from 'react';
import {Pressable, Text, Animated, useAnimatedValue} from 'embedded-react';
export function App() {
  const [n, setN] = useState(0);
  const a = useAnimatedValue(0);
  return (
    <Pressable onPress={() => Animated.parallel([Animated.spring(a, {toValue: 1}), Animated.delay(500)]).start(() => setN(n + 1))}>
      <Text>{n}</Text>
    </Pressable>
  );
}`;
    const {program} = compilePackage(source, 'app', SCREEN);
    const anims = animsOf(program);
    expect(anims.map(animation => animation.kind)).toEqual([
      AnimationKind.parallel,
      AnimationKind.timing,
      AnimationKind.spring,
    ]);
    expect(anims[0]).toMatchObject({first: 1, count: 2});
    expect(anims[1]).toMatchObject({target: NONE, amount: 500});
    const start = functionsOf(program)[0].ops.find(
      instruction => instruction.name === 'ASTART',
    );
    expect(start.bx).toBe(0);
  });

  it('gives an app that only waits a running-animation table for its delay', () => {
    const source = `
import {useState} from 'react';
import {Pressable, Text, Animated} from 'embedded-react';
export function App() {
  const [n, setN] = useState(0);
  return (
    <Pressable onPress={() => Animated.delay(250).start(() => setN(n + 1))}>
      <Text>{n}</Text>
    </Pressable>
  );
}`;
    const {program} = compilePackage(source, 'app', SCREEN);
    const regions = regionsOf(program);
    expect(regions.find(region => region.kind === Region.animations)).toEqual({
      kind: Region.animations,
      count: 17,
      bytes: 340,
    });
    expect(
      regions.find(region => region.kind === Region.events).count,
    ).toBeGreaterThan(0);
    const start = functionsOf(program)[0].ops.find(
      instruction => instruction.name === 'ASTART',
    );
    expect(animsOf(program)[start.bx]).toMatchObject({
      target: NONE,
      amount: 250,
    });
  });

  it('sizes the event queue for an app whose values nothing starts', () => {
    const source = `
import {View, useAnimatedValue} from 'embedded-react';
export function App() {
  const fade = useAnimatedValue(0.5);
  return <View style={{opacity: fade}} />;
}`;
    const {program} = compilePackage(source, 'app', SCREEN);
    expect(
      regionsOf(program).find(region => region.kind === Region.events),
    ).toEqual({kind: Region.events, count: 2, bytes: 64});
  });

  it("fades a <TouchableOpacity> before the app's own handler runs", () => {
    const source = `
import {useState} from 'react';
import {TouchableOpacity, Text} from 'embedded-react';
export function App() {
  const [n, setN] = useState(0);
  return (
    <TouchableOpacity activeOpacity={0.4} onPressIn={() => setN(n + 1)}>
      <Text>{n}</Text>
    </TouchableOpacity>
  );
}`;
    const {program} = compilePackage(source, 'app', SCREEN);
    const functions = functionsOf(program);
    const anims = animsOf(program);
    const pressIn = functions.find(
      functionRecord =>
        functionRecord.kind === ENGINE_VALUES.get('ER_EVENT_PRESS_IN'),
    );
    expect(names(pressIn)[0]).toBe('ASTART');
    expect(names(pressIn)).toContain('SET32');
    const fade = anims[pressIn.ops[0].bx];
    expect(fade).toMatchObject({
      kind: AnimationKind.timing,
      easing: ENGINE_VALUES.get('ER_EASE_QUAD_IN_OUT'),
      amount: 0,
      to: floatBits(0.4),
    });
    const pressOut = functions.find(
      functionRecord =>
        functionRecord.kind === ENGINE_VALUES.get('ER_EVENT_PRESS_OUT'),
    );
    expect(names(pressOut)).toEqual(['ASTART', 'RET']);
    expect(anims[pressOut.ops[0].bx]).toMatchObject({to: floatBits(1)});
  });
});

describe('PanResponder', () => {
  const PAN_APP = `
import {useState, useRef} from 'react';
import {View, Text, PanResponder} from 'embedded-react';
export function App() {
  const [x, setX] = useState(0);
  const pan = useRef(PanResponder.create({
    onStartShouldSetPanResponder: () => true,
    onMoveShouldSetPanResponder: (e, g) => Math.abs(g.dx) > 4,
    onPanResponderMove: (e, g) => setX(g.dx + g.y0 + g.numberActiveTouches),
    onPanResponderReject: () => setX(0),
  })).current;
  return (
    <View>
      <View {...pan.panHandlers}><Text>{x}</Text></View>
      <View {...pan.panHandlers} />
    </View>
  );
}`;

  const event = name => ENGINE_VALUES.get(name);
  const query = name => FunctionKind.query + ENGINE_VALUES.get(name);
  const names = functionRecord =>
    functionRecord.ops.map(instruction => instruction.name);

  /** Each function's result type. */
  function resultsOf(bytes) {
    const {u32, tables} = readProgram(bytes);
    const functions = tables.get(Table.functions);
    return Array.from(
      {length: functions.length / FUNCTION_RECORD_BYTES},
      (_, index) =>
        (u32(
          functions.offset + index * FUNCTION_RECORD_BYTES + FUNCTION_INFO_AT,
        ) >>>
          16) &
        0xff,
    );
  }

  it('binds a handler to each responder event and a query to each predicate, once for every node', () => {
    const {program} = compilePackage(PAN_APP, 'app', SCREEN);
    const kinds = functionsOf(program).map(
      functionRecord => functionRecord.kind,
    );
    expect(kinds).toEqual([
      event('ER_EVENT_RESPONDER_GRANT'),
      event('ER_EVENT_RESPONDER_RELEASE'),
      event('ER_EVENT_RESPONDER_TERMINATE'),
      event('ER_EVENT_RESPONDER_MOVE'),
      event('ER_EVENT_RESPONDER_REJECT'),
      query('ER_QUERY_START_SHOULD_SET'),
      query('ER_QUERY_MOVE_SHOULD_SET'),
      FunctionKind.update,
    ]);
    expect(resultsOf(program).slice(5, 7)).toEqual([
      ValueType.bool,
      ValueType.bool,
    ]);

    const {u16, tables, view} = readProgram(program);
    const {offset, length} = tables.get(Table.bindings);
    const bindings = Array.from({length: length / 4}, (_, index) => [
      view.getUint8(offset + index * 4),
      view.getUint8(offset + index * 4 + 1),
      u16(offset + index * 4 + 2),
    ]);
    const wiring = [
      [BindingKind.event, event('ER_EVENT_RESPONDER_GRANT'), 0],
      [BindingKind.event, event('ER_EVENT_RESPONDER_RELEASE'), 1],
      [BindingKind.event, event('ER_EVENT_RESPONDER_TERMINATE'), 2],
      [BindingKind.event, event('ER_EVENT_RESPONDER_MOVE'), 3],
      [BindingKind.event, event('ER_EVENT_RESPONDER_REJECT'), 4],
      [BindingKind.query, ENGINE_VALUES.get('ER_QUERY_START_SHOULD_SET'), 5],
      [BindingKind.query, ENGINE_VALUES.get('ER_QUERY_MOVE_SHOULD_SET'), 6],
    ];
    expect(bindings).toEqual([...wiring, ...wiring]);
  });

  it('keeps the gesture in slots: anchored by the grant, cleared after release and terminate', () => {
    const {program} = compilePackage(PAN_APP, 'app', SCREEN);
    const [grant, release, terminate, move, reject] = functionsOf(program);

    // A second grant is ignored; the first sets granted, then the anchor and x0/y0 off the event.
    expect(names(grant)).toEqual([
      'GET32',
      'JT',
      'LOADI',
      'SET32',
      ...Array(4).fill(['EVT', 'SET32']).flat(),
      'RET',
    ]);
    expect(
      grant.ops
        .filter(instruction => instruction.name === 'EVT')
        .map(instruction => instruction.bx & 0xff),
    ).toEqual([2, 3, 0, 1]); // dx, dy, x, y

    // With no callback, release and terminate only clear the gesture, and only while it is granted.
    for (const functionRecord of [release, terminate]) {
      expect(names(functionRecord)).toEqual([
        'GET32',
        'JF',
        'LOADI',
        'SET32',
        'LOADI',
        'SET32',
        'SET32',
        'RET',
      ]);
    }

    // A move reads travel from the anchor, y0 from its slot and the fingers through EVT.
    expect(names(move).slice(0, 2)).toEqual(['GET32', 'JF']);
    expect(names(move)).toContain('SUB');
    expect(
      move.ops
        .filter(instruction => instruction.name === 'EVT')
        .map(instruction => instruction.bx & 0xff),
    ).toEqual([2, 15]); // dx, touches

    // A reject runs as it is: the node never owned the gesture.
    expect(names(reject)[0]).not.toBe('GET32');
  });

  it('keeps each responder its own gesture slots', () => {
    const source = PAN_APP.replace(
      '<View {...pan.panHandlers} />',
      '<View {...other.panHandlers} />',
    ).replace(
      '  return (',
      '  const other = useRef(PanResponder.create({onPanResponderGrant: (e, g) => setX(g.stateID)})).current;\n  return (',
    );
    const {program} = compilePackage(source, 'app', SCREEN);
    const functions = functionsOf(program);
    // The second responder's grant, after the first's seven functions.
    const otherGrant = functions[7];
    expect(otherGrant.kind).toBe(event('ER_EVENT_RESPONDER_GRANT'));
    expect(otherGrant.ops[0].bx).not.toBe(functions[0].ops[0].bx);
    // stateID is a constant: the second responder is gesture 2.
    expect(otherGrant.ops).toContainEqual(
      expect.objectContaining({name: 'LOADI', bx: 2}),
    );
  });

  it('shares one responder across list rows, its functions entered with no row', () => {
    const source = `
import {useState, useRef} from 'react';
import {View, Text, PanResponder} from 'embedded-react';
export function App() {
  const [items, setItems] = useState([{id: 1, name: 'a'}, {id: 2, name: 'b'}]);
  const [x, setX] = useState(0);
  const pan = useRef(PanResponder.create({
    onStartShouldSetPanResponder: () => true,
    onPanResponderMove: (e, g) => setX(g.dx),
  })).current;
  return (
    <View>
      <Text>{x}</Text>
      {items.map(item => <View key={item.id} {...pan.panHandlers}><Text>{item.name}</Text></View>)}
    </View>
  );
}`;
    const {program} = compilePackage(source, 'app', SCREEN);
    const {u32, tables, view} = readProgram(program);
    const functions = tables.get(Table.functions);
    const params = index =>
      (u32(
        functions.offset + index * FUNCTION_RECORD_BYTES + FUNCTION_INFO_AT,
      ) >>>
        8) &
      0xff;
    const kinds = functionsOf(program).map(
      functionRecord => functionRecord.kind,
    );
    const grant = kinds.indexOf(event('ER_EVENT_RESPONDER_GRANT'));
    const startQuery = kinds.indexOf(query('ER_QUERY_START_SHOULD_SET'));
    expect(params(grant)).toBe(0);
    expect(params(startQuery)).toBe(0);

    // Every row binds the same grant.
    const {offset, length} = tables.get(Table.bindings);
    const grantTargets = Array.from({length: length / 4}, (_, index) => ({
      event: view.getUint8(offset + index * 4 + 1),
      target: view.getUint16(offset + index * 4 + 2, true),
    }))
      .filter(binding => binding.event === event('ER_EVENT_RESPONDER_GRANT'))
      .map(binding => binding.target);
    expect(grantTargets.length).toBeGreaterThan(0);
    expect(new Set(grantTargets)).toEqual(new Set([grant]));
  });

  it("lowers a row component's responder that reads what the row passes it", () => {
    const source = `
import {useState, useRef} from 'react';
import {View, Text, PanResponder} from 'embedded-react';
function Row({label}) {
  const [n, setN] = useState(0);
  const pan = useRef(PanResponder.create({
    onPanResponderMove: (e, g) => setN((label === 'a' ? 100 : 200) + g.dx),
  })).current;
  return <View {...pan.panHandlers}><Text>{n}</Text></View>;
}
export function App() {
  const [items, setItems] = useState([{id: 1, name: 'a'}]);
  return <View>{items.map(item => <Row key={item.id} label={item.name} />)}</View>;
}`;
    expect(() => compilePackage(source, 'app', SCREEN)).not.toThrow();
  });
});

describe('drawings and the keyboard', () => {
  const names = functionRecord =>
    functionRecord.ops.map(instruction => instruction.name);
  const updateOf = program =>
    functionsOf(program).find(
      functionRecord => functionRecord.kind === FunctionKind.update,
    );

  /** The tapes table: each tape's floats, and its paints' and gradients' counts. */
  function tapesOf(bytes) {
    const {u16, u32, view, tables} = readProgram(bytes);
    const {offset} = tables.get(Table.tapes);
    return Array.from({length: u32(offset)}, (_, index) => {
      const tapeOffset = offset + 16 + index * 16;
      const floats = u32(tapeOffset);
      return {
        floats: Array.from({length: u16(tapeOffset + 4)}, (_, floatIndex) =>
          view.getFloat32(offset + floats + floatIndex * 4, true),
        ),
        paints: u16(tapeOffset + 8),
        gradients: u16(tapeOffset + 12),
      };
    });
  }

  /** Each template node's bindings, as [kind, event, target]. */
  function bindingsOf(bytes) {
    const {u16, view, tables} = readProgram(bytes);
    const {offset, length} = tables.get(Table.bindings);
    return Array.from({length: length / 4}, (_, index) => [
      view.getUint8(offset + index * 4),
      view.getUint8(offset + index * 4 + 1),
      u16(offset + index * 4 + 2),
    ]);
  }

  it('bakes a drawing into a tape its node commits when it is built', () => {
    const source = `
import {View, Svg, Circle, Path} from 'embedded-react';
export function App() {
  return (
    <View>
      <Svg width={40} height={40}>
        <Circle cx={20} cy={20} r={10} fill="#ff0000" />
        <Path d="M0 0 L10 10" stroke="#ffffff" strokeWidth={2} />
      </Svg>
      <Svg width={10} height={10} />
    </View>
  );
}`;
    const result = compilePackage(source, 'app', SCREEN);
    const [tape] = tapesOf(result.program);
    expect(tapesOf(result.program)).toHaveLength(1);
    expect(tape.paints).toBe(2);
    expect(tape.floats.slice(0, 2)).toEqual([0, 0]); // the first shape, with paint 0
    expect(bindingsOf(result.program)).toEqual([[BindingKind.tape, 0, 0]]);
    expect(updateOf(result.program)).toBeUndefined();
    const {u16, u32} = readProgram(result.program);
    // Panels, then the needs: the two vector nodes, the empty one included.
    expect([u16(40), u32(44)]).toEqual([Resource.vectorNodes, 2]);
  });

  it('rebuilds a drawing state drives in the update, after the nodes, from its tape', () => {
    const source = `
import {useState} from 'react';
import {Pressable, Text, Svg, Arc} from 'embedded-react';
export function App() {
  const [level, setLevel] = useState(10);
  return (
    <Pressable onPress={() => setLevel(level + 5)}>
      <Text>{level}</Text>
      <Svg width={100} height={100}>
        <Arc cx={50} cy={50} r={40} startAngle={-90} endAngle={level * 2} stroke="#f4a261" strokeWidth={8} />
      </Svg>
    </Pressable>
  );
}`;
    const {program} = compilePackage(source, 'app', SCREEN);
    const update = names(updateOf(program));
    expect(update.indexOf('VTAPE')).toBeGreaterThan(update.indexOf('COMMIT'));
    expect(update).toContain('VSET');
    expect(update.at(-1)).toBe('RET');
    expect(update).toContain('VCOMMIT');
    expect(update).not.toContain('VPAINT');
    const [tape] = tapesOf(program);
    // A float state drives is 0 until the update works it out; the opcodes are in place.
    expect(tape.floats).toContain(5);
    expect(regionsOf(program)).toContainEqual({
      kind: Region.tape,
      count: tape.floats.length,
      bytes: tape.floats.length * 4,
    });
    expect(
      bindingsOf(program).some(([kind]) => kind === BindingKind.tape),
    ).toBe(false);
  });

  it('sets the paints and gradients state drives in the scratch, and commits with them', () => {
    const source = `
import {useState} from 'react';
import {Pressable, Svg, Rect} from 'embedded-react';
export function App() {
  const [on, setOn] = useState(false);
  return (
    <Pressable onPress={() => setOn(!on)}>
      <Svg width={100} height={40}>
        <Rect x={0} y={0} width={80} height={20} fill={on ? '#ff0000' : '#00ff00'} strokeWidth={on ? 4 : 2}
          stroke="#ffffff"
          fillGradient={on ? {type: 1, ax: 0, ay: 0, bx: 80, by: 0,
            stops: [{color: '#000000', offset: 0}, {color: on ? '#ffffff' : '#808080', offset: 1}]} : null} />
      </Svg>
    </Pressable>
  );
}`;
    const {program} = compilePackage(source, 'app', SCREEN);
    const instructions = updateOf(program).ops;
    const fields = name =>
      instructions
        .filter(instruction => instruction.name === name)
        .map(instruction => instruction.c);
    expect(fields('VPAINT')).toEqual([
      PaintField.fill,
      PaintField.strokeWidth,
      PaintField.fillGradient,
    ]);
    expect(fields('VGRAD')).toEqual([GradientField.stops + 2]);
    expect(names({ops: instructions})).toContain('VCOMMITP');
    expect(tapesOf(program)[0].gradients).toBe(1);
  });

  it('runs updateVector where it is called, with its damage hint in ints or worked out in floats', () => {
    const source = `
import {useRef} from 'react';
import {View, Pressable, Svg, updateVector} from 'embedded-react';
export function App() {
  const pad = useRef(null);
  const step = useRef(0);
  return (
    <View>
      <Pressable onPress={() => {
        step.current += 1;
        updateVector(pad, [{rect: [step.current, 0, 10, 10], fill: '#ffffff'}], [0, 0, 20, 10]);
      }} />
      <Pressable onPress={() => updateVector(pad, [{circle: [5, 5, step.current / 3], fill: '#ff0000'}], [0, 0, step.current / 2, 10])} />
      <Svg ref={pad} width={100} height={20} />
    </View>
  );
}`;
    const {program} = compilePackage(source, 'app', SCREEN);
    const [ints, floats] = functionsOf(program);
    expect(names(ints)).toEqual(
      expect.arrayContaining(['VTAPE', 'VSET', 'VCOMMIT', 'VDIRTY']),
    );
    expect(names(ints)).not.toContain('FMATH');
    const rounding = floats.ops
      .filter(instruction => instruction.name === 'FMATH')
      .map(instruction => instruction.ext);
    expect(rounding.slice(0, 2)).toEqual([MathFn.floor, MathFn.max]);
    expect(rounding).toContain(MathFn.ceil);
    expect(names(floats).at(-2)).toBe('VDIRTY');
    expect(tapesOf(program)).toHaveLength(2);
  });

  it('gives an empty tape a scratch to copy into', () => {
    const source = `
import {useRef} from 'react';
import {View, Pressable, Svg, updateVector} from 'embedded-react';
export function App() {
  const pad = useRef(null);
  return (
    <View>
      <Pressable onPress={() => updateVector(pad, [])} />
      <Svg ref={pad} width={100} height={20} />
    </View>
  );
}`;
    const {program} = compilePackage(source, 'app', SCREEN);
    expect(tapesOf(program)[0].floats).toEqual([]);
    expect(regionsOf(program)).toContainEqual({
      kind: Region.tape,
      count: 1,
      bytes: 4,
    });
  });

  it('refuses an <Svg source> gradient the runtime would not load', () => {
    const artifact = {
      ops: [0, 0, 1, 0, 0, 2, 10, 10, 6],
      paints: [0xffffffff, 0, 0, 4, 0, 0, 0, 1, 0],
      gradients: [{type: 1, stops: [{color: 0xff000000, offset: 0}], bx: 10}],
      width: 10,
      height: 10,
    };
    const source = `
import {View, Svg} from 'embedded-react';
import badge from './badge.svg';
export function App() {
  return (
    <View>
      <Svg source={badge} width={10} height={10} />
    </View>
  );
}`;
    expect(() =>
      compilePackage(source, 'app', {
        ...SCREEN,
        svgArtifacts: {badge: artifact},
      }),
    ).toThrow(
      /an <Svg> gradient has 1 stop, but a gradient needs at least two[\s\S]*App\.jsx:7:7/,
    );
  });

  it('writes setKeyboardConfig as the keyboard table', () => {
    const source = `
import {View, setKeyboardConfig} from 'embedded-react';
setKeyboardConfig({
  gridColumns: 10,
  rowHeight: 30,
  keyColor: '#333',
  layers: [
    [[{char: 'a', span: 5}, {label: 'up', layer: 1, span: 5}], [{done: true}]],
    [[{char: 'A'}, {backspace: true}]],
  ],
});
export function App() {
  return <View />;
}`;
    const {program} = compilePackage(source, 'app', SCREEN);
    const {u16, u32, view, tables} = readProgram(program);
    const {offset, length} = tables.get(Table.keyboard);
    const u8 = fieldOffset => view.getUint8(offset + fieldOffset);
    expect(length).toBe(28 + (2 + 3) * 4 + 5 * 8);
    expect([0, 2, 4, 6].map(fieldOffset => u16(offset + fieldOffset))).toEqual([
      2, 3, 5, 30,
    ]);
    expect([u8(8), u8(9), u32(offset + 16), u32(offset + 12)]).toEqual([
      10, 0, 0xff333333, 0,
    ]);
    // Layers name their rows, rows their keys, each after the one before.
    const groups = Array.from({length: 5}, (_, index) => [
      u16(offset + 28 + index * 4),
      u16(offset + 30 + index * 4),
    ]);
    expect(groups).toEqual([
      [0, 2],
      [2, 1],
      [0, 2],
      [2, 1],
      [3, 2],
    ]);
    const key = index => {
      const keyOffset = 48 + index * 8;
      return [
        u16(offset + keyOffset),
        u16(offset + keyOffset + 2),
        u8(keyOffset + 4),
        u8(keyOffset + 5),
        u8(keyOffset + 6),
        u8(keyOffset + 7),
      ];
    };
    const kind = name => ENGINE_VALUES.get(name);
    expect(key(1).slice(1)).toEqual([
      NONE,
      kind('ER_KEYBOARD_KEY_LAYER'),
      1,
      5,
      255,
    ]);
    expect(key(2)[2]).toBe(kind('ER_KEYBOARD_KEY_DONE'));
    expect(key(3)[1]).not.toBe(NONE);
  });

  it('keeps the built-in layout when the config gives only a look', () => {
    const source = `
import {View, setKeyboardConfig} from 'embedded-react';
setKeyboardConfig({labelColor: 'white'});
export function App() {
  return <View />;
}`;
    const {program} = compilePackage(source, 'app', SCREEN);
    const {u16, u32, tables} = readProgram(program);
    const {offset, length} = tables.get(Table.keyboard);
    expect(length).toBe(28);
    expect([u16(offset), u32(offset + 24)]).toEqual([0, 0xffffffff]);
  });
});

describe('bytecode', () => {
  it('keeps a function within the 255 register cells its record counts', () => {
    const builder = new FunctionBuilder(FunctionKind.update, 'the update');
    for (let index = 0; index < 255; index++) {
      builder.register('int');
    }
    expect(() => builder.register('int')).toThrow(/more than 255 registers/);
  });

  it('keeps the empty string register out of the pool', () => {
    const builder = new FunctionBuilder(FunctionKind.update, 'the update');
    const tables = new ProgramTables();
    const codegen = new Codegen(builder, {
      arena: new Arena(),
      tables,
      states: new Map(),
      refs: new Map(),
      boot: new Map(),
      stringLocals: new Map(),
      enums: new Map(),
      hoisted: new Map(),
      armed: new Map(),
      timerFunctions: new Map(),
    });
    const loadString = text => {
      const register = builder.register('string');
      builder.abx('LOADS', register.index, tables.string(text));
      return register;
    };

    // A string test, a string written in another scope, then the test again.
    codegen.scoped(() => codegen.truthy(loadString('a')));
    codegen.scoped(() => loadString('b'));
    codegen.scoped(() => codegen.truthy(loadString('c')));

    const {code} = builder.finish();
    const opOf = word => word & 0xff;
    const empty = code
      .filter(word => opOf(word) === OP.SEQ.code)
      .map(word => word >>> 24);
    expect(new Set(empty).size).toBe(1);
    const written = code
      .filter(word => opOf(word) === OP.LOADS.code)
      .map(word => (word >> 8) & 0xff);
    expect(written).not.toContain(empty[0]);
  });
});

describe('debug table', () => {
  const DEBUG_APP = `import {useState} from 'react';
import {Pressable, Text} from 'embedded-react';
export function App() {
  const [count, setCount] = useState(0);
  return (
    <Pressable
      onPress={() => {
        setCount(count + 1);
        setCount(count + 2);
      }}>
      <Text>{count}</Text>
    </Pressable>
  );
}`;
  const OPTIONS = {...SCREEN, filename: '/projects/clock/src/App.jsx'};

  /** Each of the section's tables: its header, entries, function names and strings. */
  function readDebug(bytes) {
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.length);
    const u16 = offset => view.getUint16(offset, true);
    const u32 = offset => view.getUint32(offset, true);
    const tables = [];
    for (let start = 0; start < bytes.length; start += u32(start + 16)) {
      const entryCount = u32(start + 4);
      const functionCount = u16(start + 8);
      const strings = start + u32(start + 12);
      const text = index => {
        const from = strings + u32(strings + 4 + index * 4);
        return new TextDecoder().decode(
          bytes.subarray(from, bytes.indexOf(0, from)),
        );
      };
      const names = start + 20 + entryCount * 16;
      tables.push({
        buildId: u32(start),
        panel: u16(start + 10),
        file: text(0),
        names: Array.from({length: functionCount}, (_, index) =>
          text(u16(names + index * 2)),
        ),
        entries: Array.from({length: entryCount}, (_, index) => {
          const entryOffset = start + 20 + index * 16;
          return {
            function: u16(entryOffset),
            offset: u32(entryOffset + 4),
            file: u16(entryOffset + 8),
            column: u16(entryOffset + 10),
            line: u32(entryOffset + 12),
          };
        }),
      });
    }
    return tables;
  }

  /** The word offset of each instruction in each function. */
  const startsOf = bytes =>
    functionsOf(bytes).map(functionRecord => {
      let wordOffset = 0;
      return functionRecord.ops.map(instruction => {
        const start = wordOffset;
        wordOffset += OP[instruction.name].words;
        return start;
      });
    });

  it('maps each handler statement and node to its line, and names each function', () => {
    const result = compilePackage(DEBUG_APP, 'app', OPTIONS);
    const [table, ...others] = readDebug(result.debug);
    expect(others).toEqual([]);
    expect(table.buildId).toBe(result.buildId);
    expect(table.panel).toBe(NONE);
    expect(table.file).toBe('App.jsx');
    expect(table.names.length).toBe(functionsOf(result.program).length);
    const handler = table.names.findIndex(name => name.startsWith('handler'));
    expect(handler).toBeGreaterThanOrEqual(0);
    const at = functionIndex =>
      table.entries
        .filter(entry => entry.function === functionIndex)
        .map(({line, column}) => [line, column]);
    expect(at(handler)).toEqual([
      [8, 9],
      [9, 9],
    ]);
    expect(at(table.names.indexOf('the update'))).toEqual([[11, 7]]);

    // Every entry is on an instruction, in order.
    const starts = startsOf(result.program);
    for (const entry of table.entries) {
      expect(starts[entry.function]).toContain(entry.offset);
      expect(entry.file).toBe(0);
    }
  });

  it('is in a debug package, beside the debug flag', () => {
    const result = compilePackage(DEBUG_APP, 'app', OPTIONS);
    const file = packPackage(result);
    expect(file.sections.map(section => section.type)).toEqual([4, 5]);
    expect(readProgram(result.program).header.flags).toBe(FLAG_DEBUG);
  });

  it('is left out of a release package, which clears the flag', () => {
    const debug = compilePackage(DEBUG_APP, 'app', OPTIONS);
    const release = compilePackage(DEBUG_APP, 'app', {
      ...OPTIONS,
      release: true,
    });
    expect(release.release).toBe(true);
    expect(packPackage(release).sections.map(section => section.type)).toEqual([
      4,
    ]);
    const {header} = readProgram(release.program);
    expect(header.flags).toBe(0);
    expect(readDebug(release.debug)[0].buildId).toBe(release.buildId);
    // The same program otherwise.
    const without = bytes => {
      const copy = Buffer.from(bytes);
      copy.writeUInt16LE(0, 6);
      copy.writeUInt32LE(0, 8);
      return copy;
    };
    expect(without(release.program)).toEqual(without(debug.program));
  });

  it("has a table for each panel when the panels' functions differ", () => {
    const RESPONSIVE_APP = `
import {useState} from 'react';
import {View, Pressable, Text} from 'embedded-react';
const WIDE = screen.width > screen.height;
export function App() {
  const [count, setCount] = useState(0);
  return (
    <Pressable onPress={() => setCount(count + 1)}>
      {WIDE ? (
        <View style={{flexDirection: 'row'}}>
          <Text>{count}</Text>
          <Text>wide</Text>
        </View>
      ) : (
        <Text>{count}</Text>
      )}
    </Pressable>
  );
}`;
    const narrow = {width: 240, height: 320};
    const differ = compilePackage(RESPONSIVE_APP, 'app', {
      screens: [narrow, {width: 320, height: 240}],
    });
    expect(readDebug(differ.debug).map(table => table.panel)).toEqual([0, 1]);
    const same = compilePackage(RESPONSIVE_APP, 'app', {
      screens: [narrow, {width: 200, height: 300}],
    });
    expect(readDebug(same.debug).map(table => table.panel)).toEqual([NONE]);
  });

  it('bakes a package as a word array, with its length', () => {
    const file = packPackage(compilePackage(DEBUG_APP, 'app', OPTIONS));
    const source = bakePackage(file.bytes);
    const words = [...source.matchAll(/0x([0-9a-f]{8})u/g)].map(match =>
      Number.parseInt(match[1], 16),
    );
    expect(source).toContain(`const uint32_t app_erpkg[${words.length}] = {`);
    expect(source).toContain(
      `const size_t app_erpkg_bytes = ${file.bytes.length}u;`,
    );
    const bytes = Buffer.alloc(words.length * 4);
    words.forEach((word, index) => bytes.writeUInt32LE(word, index * 4));
    expect(bytes.subarray(0, file.bytes.length)).toEqual(
      Buffer.from(file.bytes),
    );
  });

  it('marks code once for each place, replacing a mark nothing followed', () => {
    const builder = new FunctionBuilder(FunctionKind.timer, 'timer tick');
    builder.at({line: 3, column: 0});
    builder.at({line: 4, column: 2});
    builder.abx('LOADI', 0, 1);
    builder.at({line: 4, column: 2});
    builder.abx('LOADI', 0, 1);
    builder.at(undefined);
    builder.at({line: 5, column: 2});
    expect(builder.finish().debug).toEqual({
      name: 'timer tick',
      lines: [
        {offset: 0, line: 4, column: 2},
        {offset: 2, line: 5, column: 2},
      ],
    });
  });
});
