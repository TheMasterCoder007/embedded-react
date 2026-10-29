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
  AnimFlag,
  AnimKind,
  ENGINE_ABI,
  ENGINE_VALUES,
  FLAG_DEBUG,
  FORMAT_MAJOR,
  FORMAT_MINOR,
  BindingKind,
  FunctionKind,
  InterpFlag,
  NONE,
  PropKind,
  Region,
  Resource,
  SlotFlag,
  Table,
  TemplateKind,
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

const OP_NAMES = Object.fromEntries(
  Object.entries(OP).map(([name, {code}]) => [code, name]),
);

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
    expect(readConstant('0u')).toBe(0);
    expect(readConstant('255u')).toBe(255);
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

/** Each function's kind and its instructions, decoded. */
function functionsOf(bytes) {
  const {u32, tables} = readProgram(bytes);
  const functions = tables.get(Table.functions);
  const code = tables.get(Table.code);
  const word = index => u32(code.offset + index * 4);
  return Array.from({length: functions.length / 16}, (_, index) => {
    const at = functions.offset + index * 16;
    const start = u32(at);
    const ops = [];
    for (let pc = start; pc < start + u32(at + 4); ) {
      const first = word(pc);
      const name = OP_NAMES[first & 0xff];
      ops.push({
        name,
        a: (first >>> 8) & 0xff,
        bx: first >>> 16,
        c: first >>> 24,
        ext: OP[name].words === 2 ? word(pc + 1) : undefined,
      });
      pc += OP[name].words;
    }
    return {kind: u32(at + 12) >>> 24, ops};
  });
}

/** The arena's regions. */
function regionsOf(bytes) {
  const {u16, u32, tables} = readProgram(bytes);
  const arena = tables.get(Table.arena);
  return Array.from({length: arena.length / 12}, (_, index) => {
    const at = arena.offset + index * 12;
    return {kind: u16(at), count: u16(at + 2), bytes: u32(at + 8)};
  });
}

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
import {View, Svg, Circle} from 'embedded-react';
export function App() {
  return (
    <View>
      <Svg width={10} height={10}><Circle cx={5} cy={5} r={4} fill="#ffffff" /></Svg>
    </View>
  );
}`;
    expect(() => compilePackage(src, 'app', SCREEN)).toThrow(
      /the package output doesn't support <Svg> yet[\s\S]*App\.jsx:6:7/,
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
import {View, setKeyboardConfig} from 'embedded-react';

setKeyboardConfig({rowHeight: 30});
export function App() {
  return <View />;
}`;
    expect(() => compilePackage(src, 'app', SCREEN)).toThrow(
      /the package output doesn't support setKeyboardConfig yet[\s\S]*App\.jsx:4:1/,
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

  const names = fn => fn.ops.map(op => op.name);

  it('starts each timer on its callback, in a table maxTimers sizes', () => {
    const {program} = compilePackage(TIMER_APP, 'app', SCREEN);
    const functions = functionsOf(program);
    expect(functions.map(fn => fn.kind)).toEqual([
      0, // ER_EVENT_PRESS
      0,
      FunctionKind.timer,
      FunctionKind.update,
    ]);
    // A repeating timer on function 2, and the clear.
    expect(functions[0].ops.find(op => op.name === 'TSTART')).toMatchObject({
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

  it('converts a fractional delay as app_delay_msf does, and a whole one as it is', () => {
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
    expect(functions.map(fn => fn.kind)).toEqual([
      0,
      FunctionKind.timer,
      FunctionKind.effect,
      FunctionKind.effect,
      FunctionKind.effect,
      FunctionKind.update,
    ]);
    const [, , check, firstMount, secondMount, update] = functions;
    const effects = fn =>
      fn.ops.filter(op => op.name === 'EFFECT').map(op => op.bx);
    // Every pass queues the dep check; the first one, the first mount effect too.
    expect(effects(update)).toEqual([2, 3]);
    // The dep-driven mount effect sets no app_update(), so it queues the next one itself.
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
    const untracked = Array.from({length: slots.length / 16}, (_, index) =>
      u16(slots.offset + index * 16),
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

  it('refuses an effect without deps, as the C output does', () => {
    const src = `
import {useEffect, useRef} from 'react';
import {View} from 'embedded-react';
export function App() {
  const runs = useRef(0);
  useEffect(() => {
    runs.current += 1;
  });
  return <View />;
}`;
    expect(() => compilePackage(src, 'app', SCREEN)).toThrow(
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

  const names = fn => fn.ops.map(op => op.name);

  /** The templates: each one's kind and nodes. */
  function templatesOf(bytes) {
    const {u16, u32, tables} = readProgram(bytes);
    const {offset} = tables.get(Table.templates);
    const count = u32(offset);
    const nodes = offset + 4 + count * 8;
    return Array.from({length: count}, (_, index) => {
      const at = offset + 4 + index * 8;
      return {
        kind: u16(at + 4),
        nodes: Array.from({length: u16(at + 2)}, (_, node) => ({
          handle: u16(nodes + (u16(at) + node) * 12 + 6),
        })),
      };
    });
  }

  /** The slots: each one's type, flags, cap and aux. */
  function slotsOf(bytes) {
    const {u16, tables} = readProgram(bytes);
    const {offset, length} = tables.get(Table.slots);
    return Array.from({length: length / 16}, (_, index) => {
      const at = offset + index * 16;
      return {
        type: u16(at) & 0xff,
        flags: u16(at) >> 8,
        cap: u16(at + 12),
        aux: u16(at + 14),
      };
    });
  }

  it('keeps a list as a pool of rows, laid out by its row layout', () => {
    const {program} = compilePackage(LIST_APP, 'app', CAPS);
    const {u16, u32, tables} = readProgram(program);

    // One layout: the label as a 48-byte string (47 and a NUL), then the int and the float.
    const rows = tables.get(Table.rows);
    expect(u32(rows.offset)).toBe(1);
    expect([u16(rows.offset + 4), u16(rows.offset + 8)]).toEqual([56, 3]);
    const fields = Array.from({length: 3}, (_, index) => {
      const at = rows.offset + 12 + index * 8;
      return {type: program[at], cap: u16(at + 2), offset: u32(at + 4)};
    });
    expect(fields).toEqual([
      {type: ValueType.string, cap: 47, offset: 0},
      {type: ValueType.int, cap: 0, offset: 48},
      {type: ValueType.float, cap: 0, offset: 52},
    ]);

    // A node reads it, so it has a dirty bit; its pool is the row count, then four rows.
    expect(slotsOf(program).find(slot => slot.type === ValueType.list)).toEqual(
      {type: ValueType.list, flags: SlotFlag.tracked, cap: 4, aux: 0},
    );
    expect(
      regionsOf(program).find(region => region.kind === Region.lists),
    ).toEqual({kind: Region.lists, count: 0, bytes: 4 + 4 * 56});
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
    const builds = update.ops.filter(op => op.name === 'BUILD');
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
    const records = Array.from({length: functions.length / 16}, (_, index) =>
      u32(functions.offset + index * 16 + 12),
    );
    // Five buttons and one row press; only the row's takes its row.
    const presses = records.filter(record => record >>> 24 === 0);
    expect(presses).toHaveLength(6);
    expect(presses.map(record => (record >>> 8) & 0xff)).toEqual([
      0, 0, 0, 0, 0, 1,
    ]);

    const ops = functionsOf(program).slice(0, 5).map(names);
    expect(ops[0]).toEqual(expect.arrayContaining(['FMTSET', 'APPEND']));
    expect(ops[1]).toContain('POP');
    expect(ops[2]).toContain('KEEP');
    expect(ops[3]).toContain('SLICE');
    expect(ops[4]).toContain('CLEAR');
  });

  it('gives rows that differ by their index their own templates', () => {
    const src = `
import {useState} from 'react';
import {View} from 'embedded-react';
export function App() {
  const [dots, setDots] = useState([{size: 4}]);
  return <View>{dots.map((dot, index) => <View key={index} style={{width: dot.size}}><View style={{height: index % 2 ? 2 : 3}} /></View>)}</View>;
}`;
    const templates = templatesOf(compilePackage(src, 'app', CAPS).program);
    expect(templates.map(template => template.kind)).toEqual([
      TemplateKind.root,
      TemplateKind.row,
      TemplateKind.row,
    ]);
  });

  it('refuses a row or a node count too big for its u16 field', () => {
    const texts = `
import {useState} from 'react';
import {Text} from 'embedded-react';
export function App() {
  const [items] = useState([{a: 'x', b: 'y'}]);
  return <Text>{items.length}</Text>;
}`;
    expect(() =>
      compilePackage(texts, 'app', {...SCREEN, listStrCap: 32767}),
    ).toThrow(
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
    const src = `
import {useState} from 'react';
import {Text} from 'embedded-react';
export function App() {
  ${Array.from({length: 5}, (_, index) => `const [l${index}] = useState(${items});`).join('\n  ')}
  return <Text>{l0.length}</Text>;
}`;
    const {program} = compilePackage(src, 'app', SCREEN);
    const boot = functionsOf(program)[readProgram(program).panel.boot];
    expect(boot.ops.filter(op => op.name === 'APPEND')).toHaveLength(80);
    // Each append reads the same four registers.
    expect(
      new Set(boot.ops.filter(op => op.name === 'APPEND').map(op => op.a)).size,
    ).toBe(1);
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

  const names = fn => fn.ops.map(op => op.name);

  /** The slots: each one's type and initial value. */
  function slotsOf(bytes) {
    const {u32, tables, view} = readProgram(bytes);
    const {offset, length} = tables.get(Table.slots);
    return Array.from({length: length / 16}, (_, index) => ({
      type: view.getUint8(offset + index * 16),
      init: u32(offset + index * 16 + 8),
    }));
  }

  /** The animation descriptors. */
  function animsOf(bytes) {
    const {u16, u32, tables, view} = readProgram(bytes);
    const {offset, length} = tables.get(Table.anims);
    return Array.from({length: length / 40}, (_, index) => {
      const at = offset + index * 40;
      return {
        kind: view.getUint8(at),
        easing: view.getUint8(at + 1),
        flags: view.getUint8(at + 2),
        target: u16(at + 4),
        first: u16(at + 8),
        count: u16(at + 10),
        amount: u32(at + 12),
        delayMs: u32(at + 16),
        to: u32(at + 20),
        params: [0, 1, 2, 3].map(param => u32(at + 24 + param * 4)),
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
      slot => slot.type === ValueType.avalue,
    );
    expect(values.map(slot => slot.init)).toEqual([
      floatBits(0.5),
      floatBits(0),
    ]);

    const {u16, u32} = readProgram(program);
    const needs = Array.from({length: u16(20)}, (_, index) => ({
      resource: u16(24 + 16 + index * 8),
      amount: u32(24 + 16 + index * 8 + 4),
    }));
    expect(needs).toEqual([
      {resource: Resource.animValues, amount: 2},
      {resource: Resource.animations, amount: 2},
    ]);
    // A value's animation, and one more while a start cancels it.
    expect(
      regionsOf(program).find(region => region.kind === Region.anims),
    ).toEqual({kind: Region.anims, count: 3, bytes: 60});
  });

  it('starts each animation as a leaf, its toValue held in the descriptor or passed in a register', () => {
    const {program} = compilePackage(ANIM_APP, 'app', SCREEN);
    const anims = animsOf(program);
    const slots = slotsOf(program);
    const fade = slots.findIndex(slot => slot.type === ValueType.avalue);
    expect(anims[0]).toEqual({
      kind: AnimKind.timing,
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
      kind: AnimKind.spring,
      flags: AnimFlag.toRegister,
      target: fade + 1,
      params: [floatBits(90), floatBits(18), floatBits(1), 0],
    });

    const functions = functionsOf(program);
    const press = functions[0].ops.find(op => op.name === 'ASTART');
    expect(press.bx).toBe(0);
    expect(press.ext >>> 16).toBe(0xff);
    const go = functions[1].ops.find(op => op.name === 'ASTART');
    expect(go).toMatchObject({bx: 1, ext: NONE | (go.ext & 0xff0000)});
    expect(go.ext >>> 16).not.toBe(0xff);
    // The stop freezes the value where it is.
    expect(names(functions[2])).toEqual(['AVGET', 'AVSET', 'RET']);
  });

  it('runs a completion as a helper, entered through a done function and called with finished', () => {
    const {program} = compilePackage(ANIM_APP, 'app', SCREEN);
    const functions = functionsOf(program);
    const done = functions.findIndex(fn => fn.kind === FunctionKind.animDone);
    const helper = functions.findIndex(fn => fn.kind === FunctionKind.helper);
    expect(functions[0].ops.find(op => op.name === 'ASTART').ext & 0xffff).toBe(
      done,
    );
    expect(names(functions[done])).toEqual(['EVT', 'CALL', 'RET']);
    expect(functions[done].ops[1].bx).toBe(helper);
    // The helper comes after every caller, and requests the update its setState needs.
    expect(helper).toBeGreaterThan(done);
    expect(names(functions[helper])).toContain('NOT');

    // The register stack holds the done function's frame and the helper's.
    const {u32, tables} = readProgram(program);
    const records = tables.get(Table.functions);
    const cells = index => u32(records.offset + index * 16 + 12) & 0xff;
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
    const fade = slots.findIndex(slot => slot.type === ValueType.avalue);
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

    const {offset, length} = tables.get(Table.interps);
    expect(length).toBe(2 * 72);
    const record = index => {
      const at = offset + index * 72;
      return {
        slot: u16(at),
        points: view.getUint8(at + 2),
        flags: view.getUint8(at + 3),
        left: view.getUint8(at + 4),
        right: view.getUint8(at + 5),
        input: [u32(at + 8), u32(at + 12), u32(at + 16)],
        output: [u32(at + 40), u32(at + 44), u32(at + 48)],
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
      flags: InterpFlag.color,
      left: ENGINE_VALUES.get('ER_EXTRAPOLATE_EXTEND'),
      output: [0xff000000, 0xffff8000, 0],
    });
  });

  it('chains a sequence through done functions, and waits on a trailing delay with a delay leaf', () => {
    const src = `
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
    const {program} = compilePackage(src, 'app', SCREEN);
    const functions = functionsOf(program);
    const anims = animsOf(program);
    const dones = functions.flatMap((fn, index) =>
      fn.kind === FunctionKind.animDone ? [index] : [],
    );
    // The completion's, the wait's, then the second step's.
    expect(dones).toHaveLength(3);
    const [completion, wait, second] = dones;
    expect(functions[0].ops.find(op => op.name === 'ASTART').ext & 0xffff).toBe(
      second,
    );
    expect(
      functions[second].ops.find(op => op.name === 'ASTART').ext & 0xffff,
    ).toBe(wait);
    // An interrupted step calls the completion as not finished.
    expect(names(functions[second]).slice(0, 3)).toEqual(['EVT', 'JT', 'CALL']);
    const delay = functions[wait].ops.find(op => op.name === 'ASTART');
    expect(delay.ext & 0xffff).toBe(completion);
    expect(anims[delay.bx]).toMatchObject({
      kind: AnimKind.timing,
      target: NONE,
      amount: 250,
    });
    // The wait's handle is kept where a stop of the sequence finds it.
    expect(names(functions[wait])).toContain('SET32');
  });

  it('joins a spring and a longer delay as a parallel group', () => {
    const src = `
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
    const {program} = compilePackage(src, 'app', SCREEN);
    const anims = animsOf(program);
    expect(anims.map(anim => anim.kind)).toEqual([
      AnimKind.parallel,
      AnimKind.timing,
      AnimKind.spring,
    ]);
    expect(anims[0]).toMatchObject({first: 1, count: 2});
    expect(anims[1]).toMatchObject({target: NONE, amount: 500});
    const start = functionsOf(program)[0].ops.find(op => op.name === 'ASTART');
    expect(start.bx).toBe(0);
  });

  it('gives an app that only waits a running-animation table for its delay', () => {
    const src = `
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
    const {program} = compilePackage(src, 'app', SCREEN);
    const regions = regionsOf(program);
    expect(regions.find(region => region.kind === Region.anims)).toEqual({
      kind: Region.anims,
      count: 17,
      bytes: 340,
    });
    expect(
      regions.find(region => region.kind === Region.events).count,
    ).toBeGreaterThan(0);
    const start = functionsOf(program)[0].ops.find(op => op.name === 'ASTART');
    expect(animsOf(program)[start.bx]).toMatchObject({
      target: NONE,
      amount: 250,
    });
  });

  it('sizes the event queue for an app whose values nothing starts', () => {
    const src = `
import {View, useAnimatedValue} from 'embedded-react';
export function App() {
  const fade = useAnimatedValue(0.5);
  return <View style={{opacity: fade}} />;
}`;
    const {program} = compilePackage(src, 'app', SCREEN);
    expect(
      regionsOf(program).find(region => region.kind === Region.events),
    ).toEqual({kind: Region.events, count: 2, bytes: 64});
  });

  it("fades a <TouchableOpacity> before the app's own handler runs", () => {
    const src = `
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
    const {program} = compilePackage(src, 'app', SCREEN);
    const functions = functionsOf(program);
    const anims = animsOf(program);
    const pressIn = functions.find(
      fn => fn.kind === ENGINE_VALUES.get('ER_EVENT_PRESS_IN'),
    );
    expect(names(pressIn)[0]).toBe('ASTART');
    expect(names(pressIn)).toContain('SET32');
    const fade = anims[pressIn.ops[0].bx];
    expect(fade).toMatchObject({
      kind: AnimKind.timing,
      easing: ENGINE_VALUES.get('ER_EASE_QUAD_IN_OUT'),
      amount: 0,
      to: floatBits(0.4),
    });
    const pressOut = functions.find(
      fn => fn.kind === ENGINE_VALUES.get('ER_EVENT_PRESS_OUT'),
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
  const names = fn => fn.ops.map(op => op.name);

  /** Each function's result type. */
  function resultsOf(bytes) {
    const {u32, tables} = readProgram(bytes);
    const functions = tables.get(Table.functions);
    return Array.from(
      {length: functions.length / 16},
      (_, index) => (u32(functions.offset + index * 16 + 12) >>> 16) & 0xff,
    );
  }

  it('binds a handler to each responder event and a query to each predicate, once for every node', () => {
    const {program} = compilePackage(PAN_APP, 'app', SCREEN);
    const kinds = functionsOf(program).map(fn => fn.kind);
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
      grant.ops.filter(op => op.name === 'EVT').map(op => op.bx & 0xff),
    ).toEqual([2, 3, 0, 1]); // dx, dy, x, y

    // With no callback, release and terminate only clear the gesture, and only while it is granted.
    for (const fn of [release, terminate]) {
      expect(names(fn)).toEqual([
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
      move.ops.filter(op => op.name === 'EVT').map(op => op.bx & 0xff),
    ).toEqual([2, 15]); // dx, touches

    // A reject runs as it is: the node never owned the gesture.
    expect(names(reject)[0]).not.toBe('GET32');
  });

  it('keeps each responder its own gesture slots', () => {
    const src = PAN_APP.replace(
      '<View {...pan.panHandlers} />',
      '<View {...other.panHandlers} />',
    ).replace(
      '  return (',
      '  const other = useRef(PanResponder.create({onPanResponderGrant: (e, g) => setX(g.stateID)})).current;\n  return (',
    );
    const {program} = compilePackage(src, 'app', SCREEN);
    const fns = functionsOf(program);
    // The second responder's grant, after the first's seven functions.
    const otherGrant = fns[7];
    expect(otherGrant.kind).toBe(event('ER_EVENT_RESPONDER_GRANT'));
    expect(otherGrant.ops[0].bx).not.toBe(fns[0].ops[0].bx);
    // stateID is a constant: the second responder is gesture 2.
    expect(otherGrant.ops).toContainEqual(
      expect.objectContaining({name: 'LOADI', bx: 2}),
    );
  });

  it('shares one responder across list rows, its functions entered with no row', () => {
    const src = `
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
    const {program} = compilePackage(src, 'app', SCREEN);
    const {u32, tables, view} = readProgram(program);
    const functions = tables.get(Table.functions);
    const params = index =>
      (u32(functions.offset + index * 16 + 12) >>> 8) & 0xff;
    const kinds = functionsOf(program).map(fn => fn.kind);
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

  it("refuses a row component's responder that reads what the row passes it", () => {
    const src = `
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
    expect(() => compilePackage(src, 'app', SCREEN)).toThrow(
      /doesn't support the value an element passes its callback yet/,
    );
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
      hoisted: new Map(),
      armed: new Map(),
      timerFunctions: new Map(),
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
