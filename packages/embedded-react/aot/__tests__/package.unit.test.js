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
  NONE,
  PropKind,
  Table,
} from '../compiler/package/format.mts';
import {floatBits, readConstant} from '../compiler/package/props.mts';

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
  return <Pressable onPress={() => setCount(count + 1)}><Text>{count}</Text></Pressable>;
}`;
    expect(() => compilePackage(src, 'app', SCREEN)).toThrow(
      /the package output doesn't support event handlers yet[\s\S]*App\.jsx:6:10/,
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
