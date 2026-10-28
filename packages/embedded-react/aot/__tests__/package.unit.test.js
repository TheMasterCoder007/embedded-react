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
 * The package output's bytes. Whether the runtime loads and draws them the way the C output does is
 * test/erb-parity's job; this checks the layout the spec gives.
 */

import {describe, it, expect} from 'vitest';
import {crc32} from '../../assets/emit-container.mjs';
import {compilePackage, packPackage, resourceReport} from '../compile.mts';
import {
  ABI_PROPS,
  ENGINE_ABI,
  FLAG_DEBUG,
  FORMAT_MAJOR,
  FORMAT_MINOR,
  BindingKind,
  FunctionKind,
  NONE,
  PropKind,
  SlotFlag,
  Table,
  ValueType,
} from '../compiler/package/format.mts';
import {floatBits, readConstant} from '../compiler/package/props.mts';
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

/** The program section's header, panel and table directory. */
function readProgram(bytes) {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.length);
  const u16 = at => view.getUint16(at, true);
  const u32 = at => view.getUint32(at, true);
  const panelCount = u16(18);
  const needCount = u16(20);
  const tableCount = u16(22);
  const directory = 24 + panelCount * 16 + needCount * 8;
  const tables = new Map();
  for (let index = 0; index < tableCount; index++) {
    const at = directory + index * 12;
    tables.set(u16(at), {offset: u32(at + 4), length: u32(at + 8)});
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
      width: u16(24),
      height: u16(26),
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
  it('reads the C output spelling of a constant', () => {
    expect(readConstant('12')).toBe(12);
    expect(readConstant('-5')).toBe(-5);
    expect(readConstant('0xFF0D0D0Fu')).toBe(0xff0d0d0f);
    expect(readConstant('0.5f')).toBe(0.5);
    expect(readConstant('1e3f')).toBe(1000);
    expect(readConstant('ER_ALIGN_CENTER')).toBe(3);
    expect(readConstant('ER_LAYOUT_AUTO')).toBe(-32768);
    expect(readConstant('(int16_t)7')).toBe(7);
    expect(readConstant('(uint8_t)(3)')).toBe(3);
    expect(readConstant('app_round_dim(s_boot.value)')).toBeNull();
    expect(readConstant('ER_NOT_A_NAME')).toBeNull();
  });

  it('stores a float prop as its f32 bits', () => {
    expect(floatBits(0.5)).toBe(0x3f000000);
    expect(ABI_PROPS.get('arc_min')).toEqual({id: 120, kind: 'float'});
    expect(ABI_PROPS.get('spans[1].color')).toEqual({id: 168, kind: 'color'});
  });
});

describe('package output', () => {
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
      width: 240,
      height: 320,
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
      type: result.program[nodes + index * 12],
      children: u16(nodes + index * 12 + 2),
      props: u16(nodes + index * 12 + 4),
    });
    expect([0, 1, 2, 3].map(index => node(index).children)).toEqual([
      1, 2, 0, 0,
    ]);
    expect(node(2).type).toBe(1); // ER_NODE_TEXT

    // The root's prop set is the panel's size.
    const props = tables.get(Table.props);
    const entries = props.offset + 4 + u32(props.offset) * 4;
    const first = u16(props.offset + 4 + node(0).props * 4);
    const entry = at => ({
      prop: u16(entries + at * 8),
      kind: result.program[entries + at * 8 + 2],
      value: u32(entries + at * 8 + 4),
    });
    expect([entry(first), entry(first + 1)]).toEqual([
      {prop: ABI_PROPS.get('width').id, kind: PropKind.i16, value: 240},
      {prop: ABI_PROPS.get('height').id, kind: PropKind.i16, value: 320},
    ]);
  });

  it('shares identical prop sets', () => {
    const src = STATIC_APP.replace(
      '<View style={styles.box} />',
      '<View style={styles.box} /><View style={styles.box} />',
    );
    const one = compilePackage(STATIC_APP, 'app', SCREEN);
    const two = compilePackage(src, 'app', SCREEN);
    const sets = result => {
      const {u32, tables} = readProgram(result.program);
      return u32(tables.get(Table.props).offset);
    };
    expect(sets(two)).toBe(sets(one));
  });

  it('refuses what it does not lower yet, at its place in the source', () => {
    const src = `
import {useState} from 'react';
import {Pressable, Text} from 'embedded-react';
export function App() {
  const [count, setCount] = useState(0);
  return <Pressable onPress={() => setInterval(() => setCount(count + 1), 100)}><Text>{count}</Text></Pressable>;
}`;
    expect(() => compilePackage(src, 'app', SCREEN)).toThrow(
      /the package output doesn't support timers yet[\s\S]*App\.jsx:6:/,
    );
  });

  it('binds a handler to its event, and re-sets what state drives in the update', () => {
    const src = `
import {useState} from 'react';
import {Pressable, Text} from 'embedded-react';
export function App() {
  const [count, setCount] = useState(0);
  return <Pressable onPress={() => setCount(count + 1)}><Text>{count}</Text></Pressable>;
}`;
    const {u16, u32, tables, panel} = readProgram(
      compilePackage(src, 'app', SCREEN).program,
    );
    const functions = tables.get(Table.functions);
    const kinds = Array.from(
      {length: functions.length / 16},
      (_, index) => u32(functions.offset + index * 16 + 12) >>> 24,
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
      type: u16(slots.offset + index * 16) & 0xff,
      flags: u16(slots.offset + index * 16) >> 8,
    });
    expect(slot(0)).toEqual({type: ValueType.int, flags: SlotFlag.tracked});
    expect(slot(1)).toEqual({type: ValueType.bool, flags: SlotFlag.tracked});
  });

  it('works out values from the screen size in a boot function, in doubles as C does', () => {
    const src = `
import {View} from 'embedded-react';
const HALF = screen.width / 2;
export function App() {
  return <View style={{width: HALF, height: 10}} />;
}`;
    const {u16, u32, tables, panel} = readProgram(
      compilePackage(src, 'app', SCREEN).program,
    );
    const functions = tables.get(Table.functions);
    expect(u32(functions.offset + panel.boot * 16 + 12) >>> 24).toBe(
      FunctionKind.boot,
    );
    const slots = tables.get(Table.slots);
    const types = Array.from({length: slots.length / 16}, (_, index) =>
      u16(slots.offset + index * 16),
    );
    expect(types).toContain(ValueType.double | (SlotFlag.bootOnly << 8));
  });

  it('interns a string state that holds one of a closed set of values', () => {
    const src = `
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
    const result = compilePackage(src, 'app', SCREEN);
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
      {length: slots.length / 16},
      (_, index) => u16(slots.offset + index * 16) & 0xff,
    );
    expect(types.filter(type => type === ValueType.enum)).toHaveLength(1);
    expect(types).toContain(ValueType.string);
  });

  it('lists the values the host feeds, by name', () => {
    const src = `
import {Text, useHostValue} from 'embedded-react';
export function App() {
  const zeta = useHostValue(0);
  const alpha = useHostValue(0.5);
  return <Text>{zeta} {alpha}</Text>;
}`;
    const result = compilePackage(src, 'app', SCREEN);
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

  it('points what has no node of its own at its first call', () => {
    const src = `
import {useEffect, useRef} from 'react';
import {View} from 'embedded-react';
export function App() {
  const mounted = useRef(0);
  useEffect(() => {
    mounted.current = 1;
  }, []);
  return <View />;
}`;
    expect(() => compilePackage(src, 'app', SCREEN)).toThrow(
      /the package output doesn't support effects yet[\s\S]*App\.jsx:6:3/,
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
      const src = `
import {Text} from 'embedded-react';
export function App() {
  return <Text style={{${style}}}>x</Text>;
}`;
      expect(() => compilePackage(src, 'app', SCREEN), style).toThrow(
        new RegExp(`${message.source}[\\s\\S]*App\\.jsx:4:10`),
      );
    }
  });

  it('refuses an image the package does not bake', () => {
    const src = `
import {Image} from 'embedded-react';
export function App() {
  return <Image source={{uri: 'logo'}} style={{width: 20, height: 20}} />;
}`;
    expect(() => compilePackage(src, 'app', SCREEN)).toThrow(
      /the package has no image named "logo"[\s\S]*App\.jsx:4:10/,
    );
  });

  it('puts the program in an erb container, each section 4-aligned', () => {
    const result = compilePackage(STATIC_APP, 'app', SCREEN);
    for (const assetPack of [undefined, new Uint8Array(13).fill(7)]) {
      const file = packPackage(result, assetPack);
      const bytes = Buffer.from(file.bytes);
      expect(bytes.subarray(0, 4).toString('ascii')).toBe('ERCF');
      expect(bytes.readUInt32LE(8)).toBe(crc32(bytes.subarray(12)));
      expect(bytes.subarray(14, 17).toString('ascii')).toBe('erb');

      // Walk the sections: each one's data starts 4-aligned.
      let at = 17;
      const count = bytes.readUInt32LE(at);
      at += 4;
      const types = [];
      for (let index = 0; index < count; index++) {
        const type = bytes.readUInt32LE(at);
        const length = bytes.readUInt32LE(at + 4);
        if (type !== 0) {
          types.push(type);
          expect(
            (at + 8) % 4,
            `section ${type} with a ${assetPack?.length ?? 0} B pack`,
          ).toBe(0);
        }
        at += 8 + length;
      }
      expect(at).toBe(bytes.length);
      expect(types).toEqual(assetPack ? [2, 4] : [4]);
      expect(
        Buffer.from(
          file.bytes.subarray(file.bytes.length - result.program.length),
        ),
      ).toEqual(Buffer.from(result.program));
    }
  });

  it('reports each panel and the flash each section takes', () => {
    const result = compilePackage(STATIC_APP, 'app', SCREEN);
    const file = packPackage(result);
    const report = resourceReport(result, file);
    expect(report[0]).toMatch(/^ {2}panel 240x320: arena \d+ B, 4 nodes$/);
    expect(report[1]).toBe(
      `  flash: ${file.bytes.length} B (program ${result.program.length} B)`,
    );
    expect(report[2]).toMatch(/templates \d+/);
  });
});

describe('bytecode', () => {
  it('keeps a function within the 255 register cells its record counts', () => {
    const fn = new FunctionBuilder(FunctionKind.update, 'the update');
    for (let index = 0; index < 255; index++) {
      fn.register('int');
    }
    expect(() => fn.register('int')).toThrow(/more than 255 registers/);
  });

  it('keeps the empty string register out of the pool', () => {
    const fn = new FunctionBuilder(FunctionKind.update, 'the update');
    const tables = new ProgramTables();
    const codegen = new Codegen(fn, {
      arena: new Arena(),
      tables,
      states: new Map(),
      refs: new Map(),
      boot: new Map(),
      stringLocals: new Map(),
      enums: new Map(),
    });
    const loadString = text => {
      const register = fn.register('string');
      fn.abx('LOADS', register.index, tables.string(text));
      return register;
    };

    // A string test, a string written in another scope, then the test again.
    codegen.scoped(() => codegen.truthy(loadString('a')));
    codegen.scoped(() => loadString('b'));
    codegen.scoped(() => codegen.truthy(loadString('c')));

    const {code} = fn.finish();
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
