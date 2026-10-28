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
 * Writes a lowered program as the bytes of a Flow B program section: the header, panels, needs and table
 * directory, then each table, as website/docs/internals/flow-b-packages.md lays them out.
 */

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {crc32} from '../../../assets/emit-container.mjs';
import {
  DIRECTORY_SIZE,
  ENGINE_ABI,
  FORMAT_MAJOR,
  FORMAT_MINOR,
  HEADER_SIZE,
  NEED_SIZE,
  PANEL_SIZE,
  Table,
} from './format.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

export interface ProgramPanel {
  width: number;
  height: number;
  arenaBytes: number;
  nodeCount: number;
  rootTemplate: number;
  bootFunction: number;
  updateFunction: number;
}

export interface ProgramNeed {
  resource: number;
  amount: number;
}

export interface ProgramRegion {
  kind: number;
  count: number;
  offset: number;
  bytes: number;
}

export interface ProgramSlot {
  type: number;
  flags: number;
  dirtyBit: number;
  offset: number;
  init: number;
  cap: number;
  aux: number;
}

export interface ProgramPropEntry {
  prop: number;
  kind: number;
  /** The value as its 32 bits. */
  value: number;
}

export interface ProgramTemplateNode {
  type: number;
  children: number;
  props: number;
  handle: number;
  bindings: number;
  bindingCount: number;
}

export interface ProgramTemplate {
  kind: number;
  nodes: ProgramTemplateNode[];
}

export interface ProgramBinding {
  kind: number;
  event: number;
  target: number;
}

export interface ProgramInput {
  /** Its name's string index. */
  name: number;
  slot: number;
}

export interface ProgramFunction {
  kind: number;
  params: number;
  result: number;
  /** One type per register cell. */
  types: number[];
  /** The instruction words. */
  code: number[];
}

/** Everything a program section holds, before it is laid out. */
export interface Program {
  flags: number;
  panels: ProgramPanel[];
  needs: ProgramNeed[];
  strings: string[];
  /** 32-bit constants, as their bits. */
  const32: number[];
  const64: bigint[];
  regions: ProgramRegion[];
  slots: ProgramSlot[];
  /** Interned string sets: each one's values, as string indices. */
  enums: number[][];
  propSets: ProgramPropEntry[][];
  templates: ProgramTemplate[];
  bindings: ProgramBinding[];
  functions: ProgramFunction[];
  /** Host-fed values, sorted by name. */
  inputs: ProgramInput[];
}

/** The bytes of one table, for the resource report. */
export interface TableSize {
  id: number;
  bytes: number;
}

export interface WrittenProgram {
  bytes: Uint8Array;
  buildId: number;
  tables: TableSize[];
}

/*----------------------------------------------------------------------------------------------------------------------
 - Classes
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Little-endian bytes, grown as they are written.
 *
 * @class ByteWriter
 */
export class ByteWriter {
  private buffer = new Uint8Array(256);
  private view = new DataView(this.buffer.buffer);
  length = 0;

  private reserve(bytes: number): number {
    const at = this.length;
    if (at + bytes > this.buffer.length) {
      const grown = new Uint8Array(
        Math.max(this.buffer.length * 2, at + bytes),
      );
      grown.set(this.buffer);
      this.buffer = grown;
      this.view = new DataView(grown.buffer);
    }
    this.length += bytes;
    return at;
  }

  // Each writer reserves before it touches the buffer, since reserving may replace it.
  u8(value: number): void {
    const at = this.reserve(1);
    this.view.setUint8(at, value);
  }

  u16(value: number): void {
    const at = this.reserve(2);
    this.view.setUint16(at, value, true);
  }

  u32(value: number): void {
    const at = this.reserve(4);
    this.view.setUint32(at, value >>> 0, true);
  }

  bytes(data: Uint8Array): void {
    const at = this.reserve(data.length);
    this.buffer.set(data, at);
  }

  /** Zeros up to the next multiple of 4. */
  pad4(): void {
    while (this.length % 4) {
      this.u8(0);
    }
  }

  /** Overwrites a u32 already written. */
  putU32(at: number, value: number): void {
    this.view.setUint32(at, value >>> 0, true);
  }

  done(): Uint8Array {
    return this.buffer.slice(0, this.length);
  }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * A table with a header array: its count, the `first`/`count` header of each group, then every group's records.
 *
 * @param groups  Each group's records.
 * @param writeRecord  Writes one record.
 *
 * @returns The table's bytes.
 */
function headedTable<Record>(
  groups: Record[][],
  writeRecord: (out: ByteWriter, record: Record) => void,
): Uint8Array {
  const out = new ByteWriter();
  out.u32(groups.length);
  let first = 0;
  for (const group of groups) {
    out.u16(first);
    out.u16(group.length);
    first += group.length;
  }
  for (const group of groups) {
    for (const record of group) {
      writeRecord(out, record);
    }
  }
  return out.done();
}

/**
 * Bytes padded with zeros to a multiple of 4.
 *
 * @param bytes  The bytes.
 *
 * @returns The padded bytes.
 */
function padded(bytes: Uint8Array): Uint8Array {
  const out = new ByteWriter();
  out.bytes(bytes);
  out.pad4();
  return out.done();
}

/**
 * The strings table: the count, each string's byte offset from the table's start, then the NUL-terminated UTF-8.
 *
 * @param strings  The strings.
 *
 * @returns The table's bytes.
 */
export function stringsTable(strings: string[]): Uint8Array {
  const out = new ByteWriter();
  out.u32(strings.length);
  const encoded = strings.map(text => new TextEncoder().encode(`${text}\0`));
  let offset = 4 + strings.length * 4;
  for (const bytes of encoded) {
    out.u32(offset);
    offset += bytes.length;
  }
  for (const bytes of encoded) {
    out.bytes(bytes);
  }
  out.pad4();
  return out.done();
}

/**
 * Every table the program has, by id. Tables the runtime requires are written even when empty.
 *
 * @param program  The program.
 *
 * @returns The tables, in id order.
 */
function tablesOf(program: Program): Map<number, Uint8Array> {
  const tables = new Map<number, Uint8Array>();
  const table = (id: number, write: (out: ByteWriter) => void): void => {
    const out = new ByteWriter();
    write(out);
    tables.set(id, out.done());
  };

  if (program.strings.length) {
    tables.set(Table.strings, stringsTable(program.strings));
  }
  if (program.const32.length) {
    table(Table.const32, out => program.const32.forEach(bits => out.u32(bits)));
  }
  if (program.const64.length) {
    table(Table.const64, out => {
      for (const value of program.const64) {
        const bits = BigInt.asUintN(64, value);
        out.u32(Number(bits & 0xffffffffn));
        out.u32(Number(bits >> 32n));
      }
    });
  }
  table(Table.arena, out => {
    for (const region of program.regions) {
      out.u16(region.kind);
      out.u16(region.count);
      out.u32(region.offset);
      out.u32(region.bytes);
    }
  });
  table(Table.slots, out => {
    for (const slot of program.slots) {
      out.u8(slot.type);
      out.u8(slot.flags);
      out.u16(slot.dirtyBit);
      out.u32(slot.offset);
      out.u32(slot.init);
      out.u16(slot.cap);
      out.u16(slot.aux);
    }
  });
  if (program.enums.length) {
    const enums = headedTable(program.enums, (out, value) => out.u16(value));
    tables.set(Table.enums, padded(enums));
  }
  if (program.propSets.length) {
    tables.set(
      Table.props,
      headedTable(program.propSets, (out, entry) => {
        out.u16(entry.prop);
        out.u8(entry.kind);
        out.u8(0);
        out.u32(entry.value);
      }),
    );
  }
  table(Table.templates, out => {
    out.u32(program.templates.length);
    let firstNode = 0;
    for (const template of program.templates) {
      out.u16(firstNode);
      out.u16(template.nodes.length);
      out.u16(template.kind);
      out.u16(0);
      firstNode += template.nodes.length;
    }
    for (const node of program.templates.flatMap(template => template.nodes)) {
      out.u8(node.type);
      out.u8(0);
      out.u16(node.children);
      out.u16(node.props);
      out.u16(node.handle);
      out.u16(node.bindings);
      out.u8(node.bindingCount);
      out.u8(0);
    }
  });
  if (program.bindings.length) {
    table(Table.bindings, out => {
      for (const binding of program.bindings) {
        out.u8(binding.kind);
        out.u8(binding.event);
        out.u16(binding.target);
      }
    });
  }
  table(Table.functions, out => {
    let code = 0;
    let types = 0;
    for (const fn of program.functions) {
      out.u32(code);
      out.u32(fn.code.length);
      out.u32(types);
      out.u8(fn.types.length);
      out.u8(fn.params);
      out.u8(fn.result);
      out.u8(fn.kind);
      code += fn.code.length;
      types += fn.types.length;
    }
  });
  table(Table.types, out => {
    program.functions.forEach(fn => fn.types.forEach(type => out.u8(type)));
    out.pad4();
  });
  table(Table.code, out =>
    program.functions.forEach(fn => fn.code.forEach(word => out.u32(word))),
  );
  if (program.inputs.length) {
    table(Table.inputs, out => {
      for (const input of program.inputs) {
        out.u16(input.name);
        out.u16(input.slot);
      }
    });
  }
  return tables;
}

/**
 * Writes a program section, its build_id worked out over the finished bytes.
 *
 * @param program  The program.
 *
 * @returns The section's bytes, its build_id, and each table's size.
 */
export function writeProgram(program: Program): WrittenProgram {
  const tables = tablesOf(program);
  const out = new ByteWriter();
  out.u16(FORMAT_MAJOR);
  out.u16(FORMAT_MINOR);
  out.u16(ENGINE_ABI);
  out.u16(program.flags);
  const buildIdAt = out.length;
  out.u32(0);
  out.u32(Math.max(...program.panels.map(panel => panel.arenaBytes)));
  out.u16(Math.max(...program.panels.map(panel => panel.nodeCount)));
  out.u16(program.panels.length);
  out.u16(program.needs.length);
  out.u16(tables.size);
  for (const panel of program.panels) {
    out.u16(panel.width);
    out.u16(panel.height);
    out.u32(panel.arenaBytes);
    out.u16(panel.nodeCount);
    out.u16(panel.rootTemplate);
    out.u16(panel.bootFunction);
    out.u16(panel.updateFunction);
  }
  for (const need of program.needs) {
    out.u16(need.resource);
    out.u16(0);
    out.u32(need.amount);
  }

  // The directory, then the tables, each starting 4-aligned since every one is a multiple of 4 long.
  let offset =
    HEADER_SIZE +
    program.panels.length * PANEL_SIZE +
    program.needs.length * NEED_SIZE +
    tables.size * DIRECTORY_SIZE;
  for (const [id, bytes] of tables) {
    out.u16(id);
    out.u16(0);
    out.u32(offset);
    out.u32(bytes.length);
    offset += bytes.length;
  }
  for (const bytes of tables.values()) {
    out.bytes(bytes);
  }

  const bytes = out.done();
  const buildId = crc32(bytes);
  new DataView(bytes.buffer).setUint32(buildIdAt, buildId, true);
  return {
    bytes,
    buildId,
    tables: [...tables].map(([id, table]) => ({id, bytes: table.length})),
  };
}
