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
  GRADIENT_SIZE,
  HEADER_SIZE,
  NEED_SIZE,
  NONE,
  PAINT_SIZE,
  PANEL_SIZE,
  Table,
  TAPE_HEADER_SIZE,
  TAPE_SIZE,
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

/** A list row's field: its type, a string's most bytes, and where it sits in the row. */
export interface ProgramRowField {
  type: number;
  cap: number;
  offset: number;
}

/** A list row layout. */
export interface ProgramRow {
  size: number;
  fields: ProgramRowField[];
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

/** An animation descriptor. Its floats are their 32 bits. */
export interface ProgramAnim {
  kind: number;
  easing: number;
  flags: number;
  target: number;
  prop: number;
  first: number;
  count: number;
  amount: number;
  delayMs: number;
  to: number;
  params: [number, number, number, number];
}

/** An interpolation: the animated value, the breakpoints, and the outputs as floats' or colors' 32 bits. */
export interface ProgramInterp {
  slot: number;
  flags: number;
  extrapolateLeft: number;
  extrapolateRight: number;
  input: number[];
  output: number[];
}

/** A shape's paint, as the engine's ERVectorPaint holds it. */
export interface ProgramPaint {
  fill: number;
  stroke: number;
  strokeWidth: number;
  miter: number;
  cap: number;
  join: number;
  fillRule: number;
  /** 1-based gradients of the tape, or 0 for none. */
  fillGradient: number;
  strokeGradient: number;
}

export interface ProgramGradient {
  type: number;
  stops: {color: number; position: number}[];
  ax: number;
  ay: number;
  bx: number;
  by: number;
  radius: number;
}

/** A vector op-tape, with the paints and gradients its shapes name. */
export interface ProgramTape {
  floats: number[];
  paints: ProgramPaint[];
  gradients: ProgramGradient[];
}

/** A keyboard key. Its label and text are string indices, or NONE. */
export interface ProgramKey {
  label: number;
  text: number;
  type: number;
  layer: number;
  span: number;
  highlight: number;
}

/** The on-screen keyboard's look, and its layout (no layers keeps the engine's own). */
export interface ProgramKeyboard {
  layers: ProgramKey[][][];
  rowHeight: number;
  gridCols: number;
  keyGap: number;
  keyRadius: number;
  fontSize: number;
  panelColor: number;
  keyColor: number;
  keyActiveColor: number;
  labelColor: number;
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
  rows: ProgramRow[];
  propSets: ProgramPropEntry[][];
  templates: ProgramTemplate[];
  bindings: ProgramBinding[];
  functions: ProgramFunction[];
  anims: ProgramAnim[];
  /** Host-fed values, sorted by name. */
  inputs: ProgramInput[];
  interps: ProgramInterp[];
  tapes: ProgramTape[];
  keyboard: ProgramKeyboard | null;
}

/** The bytes of one table, for the resource report: one size for each copy stored. */
export interface TableSize {
  id: number;
  bytes: number[];
}

export interface WrittenProgram {
  bytes: Uint8Array;
  buildId: number;
  tables: TableSize[];
}

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** The breakpoints an interpolation record has room for (ER_INTERPOLATE_MAX_POINTS). */
const MAX_POINTS = 8;

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

  f32(value: number): void {
    const at = this.reserve(4);
    this.view.setFloat32(at, value, true);
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

/**
 * Every panel's function bodies, each stored once: the code and types tables every panel's functions point
 * into. A jump is relative to its function, so a body reads the same wherever it sits.
 *
 * @class Bodies
 */
class Bodies {
  readonly code: number[] = [];
  readonly types: number[] = [];
  private codeAt = new Map<string, number>();
  private typesAt = new Map<string, number>();

  /** Where a function's code and register types sit, storing each the first time it is seen. */
  place(fn: ProgramFunction): {code: number; types: number} {
    return {
      code: Bodies.store(this.codeAt, this.code, fn.code),
      types: Bodies.store(this.typesAt, this.types, fn.types),
    };
  }

  private static store(
    placed: Map<string, number>,
    into: number[],
    values: number[],
  ): number {
    const key = values.join(',');
    let at = placed.get(key);
    if (at === undefined) {
      at = into.length;
      into.push(...values);
      placed.set(key, at);
    }
    return at;
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
 * Every table the program has, by id, but the code and types its functions are placed in. Tables the runtime
 * requires are written even when empty.
 *
 * @param program  The program.
 * @param bodies  Where its function bodies go.
 *
 * @returns The tables.
 */
function tablesOf(program: Program, bodies: Bodies): Map<number, Uint8Array> {
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
  if (program.rows.length) {
    table(Table.rows, out => {
      out.u32(program.rows.length);
      let first = 0;
      for (const row of program.rows) {
        out.u16(row.size);
        out.u16(first);
        out.u16(row.fields.length);
        out.u16(0);
        first += row.fields.length;
      }
      for (const field of program.rows.flatMap(row => row.fields)) {
        out.u8(field.type);
        out.u8(0);
        out.u16(field.cap);
        out.u32(field.offset);
      }
    });
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
    for (const fn of program.functions) {
      const {code, types} = bodies.place(fn);
      out.u32(code);
      out.u32(fn.code.length);
      out.u32(types);
      out.u8(fn.types.length);
      out.u8(fn.params);
      out.u8(fn.result);
      out.u8(fn.kind);
    }
  });
  if (program.anims.length) {
    table(Table.anims, out => {
      for (const anim of program.anims) {
        out.u8(anim.kind);
        out.u8(anim.easing);
        out.u8(anim.flags);
        out.u8(0);
        out.u16(anim.target);
        out.u16(anim.prop);
        out.u16(anim.first);
        out.u16(anim.count);
        out.u32(anim.amount);
        out.u32(anim.delayMs);
        out.u32(anim.to);
        anim.params.forEach(bits => out.u32(bits));
      }
    });
  }
  if (program.inputs.length) {
    table(Table.inputs, out => {
      for (const input of program.inputs) {
        out.u16(input.name);
        out.u16(input.slot);
      }
    });
  }
  if (program.interps.length) {
    table(Table.interps, out => {
      for (const interp of program.interps) {
        out.u16(interp.slot);
        out.u8(interp.input.length);
        out.u8(interp.flags);
        out.u8(interp.extrapolateLeft);
        out.u8(interp.extrapolateRight);
        out.u16(0);
        for (const values of [interp.input, interp.output]) {
          for (let point = 0; point < MAX_POINTS; point++) {
            out.u32(values[point] ?? 0);
          }
        }
      }
    });
  }
  if (program.tapes.length) {
    tables.set(Table.tapes, tapesTable(program.tapes));
  }
  if (program.keyboard) {
    tables.set(Table.keyboard, keyboardTable(program.keyboard));
  }
  return tables;
}

/**
 * The tapes table: its header, the tape records, then every tape's floats, paints, gradients, and stops, each
 * tape's after the one before it.
 *
 * @param tapes  The tapes.
 *
 * @returns The table's bytes.
 */
function tapesTable(tapes: ProgramTape[]): Uint8Array {
  const floatCount = tapes.reduce(
    (count, tape) => count + tape.floats.length,
    0,
  );
  const paints = tapes.flatMap(tape => tape.paints);
  const gradients = tapes.flatMap(tape => tape.gradients);
  const floatsAt = TAPE_HEADER_SIZE + tapes.length * TAPE_SIZE;
  const paintsAt = floatsAt + floatCount * 4;
  const gradientsAt = paintsAt + paints.length * PAINT_SIZE;
  const out = new ByteWriter();
  out.u32(tapes.length);
  out.u32(paintsAt);
  out.u32(gradientsAt);
  out.u32(gradientsAt + gradients.length * GRADIENT_SIZE);
  let floatFirst = 0;
  let paintFirst = 0;
  let gradientFirst = 0;
  for (const tape of tapes) {
    out.u32(floatsAt + floatFirst * 4);
    out.u16(tape.floats.length);
    out.u16(paintFirst);
    out.u16(tape.paints.length);
    out.u16(gradientFirst);
    out.u16(tape.gradients.length);
    out.u16(0);
    floatFirst += tape.floats.length;
    paintFirst += tape.paints.length;
    gradientFirst += tape.gradients.length;
  }
  tapes.forEach(tape => tape.floats.forEach(value => out.f32(value)));
  for (const paint of paints) {
    out.u32(paint.fill);
    out.u32(paint.stroke);
    out.f32(paint.strokeWidth);
    out.f32(paint.miter);
    out.u8(paint.cap);
    out.u8(paint.join);
    out.u8(paint.fillRule);
    out.u8(0);
    out.u16(paint.fillGradient);
    out.u16(paint.strokeGradient);
  }
  let stopFirst = 0;
  for (const gradient of gradients) {
    out.u8(gradient.type);
    out.u8(gradient.stops.length);
    out.u16(stopFirst);
    for (const value of [
      gradient.ax,
      gradient.ay,
      gradient.bx,
      gradient.by,
      gradient.radius,
    ]) {
      out.f32(value);
    }
    stopFirst += gradient.stops.length;
  }
  for (const stop of gradients.flatMap(gradient => gradient.stops)) {
    out.u32(stop.color);
    out.f32(stop.position);
  }
  return out.done();
}

/**
 * The keyboard table: its look, then its layers, rows and keys, each group's after the one before it.
 *
 * @param keyboard  The keyboard.
 *
 * @returns The table's bytes.
 */
function keyboardTable(keyboard: ProgramKeyboard): Uint8Array {
  const rows = keyboard.layers.flat();
  const keys = rows.flat();
  const out = new ByteWriter();
  out.u16(keyboard.layers.length);
  out.u16(rows.length);
  out.u16(keys.length);
  out.u16(keyboard.rowHeight);
  out.u8(keyboard.gridCols);
  out.u8(keyboard.keyGap);
  out.u8(keyboard.keyRadius);
  out.u8(keyboard.fontSize);
  out.u32(keyboard.panelColor);
  out.u32(keyboard.keyColor);
  out.u32(keyboard.keyActiveColor);
  out.u32(keyboard.labelColor);
  const groups = (sizes: number[]): void => {
    let first = 0;
    for (const size of sizes) {
      out.u16(first);
      out.u16(size);
      first += size;
    }
  };
  groups(keyboard.layers.map(layer => layer.length));
  groups(rows.map(row => row.length));
  for (const key of keys) {
    out.u16(key.label);
    out.u16(key.text);
    out.u8(key.type);
    out.u8(key.layer);
    out.u8(key.span);
    out.u8(key.highlight);
  }
  return out.done();
}

/** Whether two tables hold the same bytes. */
function sameBytes(left: Uint8Array, right: Uint8Array): boolean {
  return (
    left.length === right.length &&
    left.every((byte, index) => byte === right[index])
  );
}

/**
 * Each panel's needs together: the most any panel needs of each resource.
 *
 * @param programs  Each panel's program.
 *
 * @returns The needs.
 */
function mergedNeeds(programs: Program[]): ProgramNeed[] {
  const amounts = new Map<number, number>();
  for (const need of programs.flatMap(program => program.needs)) {
    amounts.set(
      need.resource,
      Math.max(amounts.get(need.resource) ?? 0, need.amount),
    );
  }
  return [...amounts]
    .sort(([left], [right]) => left - right)
    .map(([resource, amount]) => ({resource, amount}));
}

/**
 * Writes a program section, its build_id worked out over the finished bytes. Each program is one panel's. A
 * table that is the same for every panel is listed once for all of them; any other is listed for each panel
 * that has it, and panels whose copies are the same share its bytes. A function body the same in several
 * panels is stored once.
 *
 * @param programs  Each panel's program, in the order the panel table lists them.
 *
 * @returns The section's bytes, its build_id, and each table's size.
 */
export function writeProgram(programs: Program[]): WrittenProgram {
  // Every panel's functions point into one code table and one types table.
  const bodies = new Bodies();
  const panelTables = programs.map(program => tablesOf(program, bodies));
  const types = new ByteWriter();
  bodies.types.forEach(type => types.u8(type));
  types.pad4();
  const code = new ByteWriter();
  bodies.code.forEach(word => code.u32(word));
  for (const tables of panelTables) {
    tables.set(Table.types, types.done());
    tables.set(Table.code, code.done());
  }
  const ids = [...new Set(panelTables.flatMap(tables => [...tables.keys()]))];
  ids.sort((left, right) => left - right);

  // The bytes stored, and the directory entries that name them.
  const stored: Uint8Array[] = [];
  const entries: {id: number; panel: number; data: number}[] = [];
  const sizes: TableSize[] = [];
  for (const id of ids) {
    const copies = panelTables.map(tables => tables.get(id));
    const first = copies[0];
    if (first && copies.every(copy => copy && sameBytes(copy, first))) {
      entries.push({id, panel: NONE, data: stored.push(first) - 1});
      sizes.push({id, bytes: [first.length]});
      continue;
    }
    const own: number[] = [];
    copies.forEach((copy, panel) => {
      if (!copy) {
        return;
      }
      let data = own.find(index => sameBytes(stored[index], copy));
      if (data === undefined) {
        data = stored.push(copy) - 1;
        own.push(data);
      }
      entries.push({id, panel, data});
    });
    sizes.push({id, bytes: own.map(index => stored[index].length)});
  }

  const panels = programs.map(program => program.panels[0]);
  const needs = mergedNeeds(programs);
  const out = new ByteWriter();
  out.u16(FORMAT_MAJOR);
  out.u16(FORMAT_MINOR);
  out.u16(ENGINE_ABI);
  out.u16(programs[0].flags);
  const buildIdAt = out.length;
  out.u32(0);
  out.u32(Math.max(...panels.map(panel => panel.arenaBytes)));
  out.u16(Math.max(...panels.map(panel => panel.nodeCount)));
  out.u16(panels.length);
  out.u16(needs.length);
  out.u16(entries.length);
  for (const panel of panels) {
    out.u16(panel.width);
    out.u16(panel.height);
    out.u32(panel.arenaBytes);
    out.u16(panel.nodeCount);
    out.u16(panel.rootTemplate);
    out.u16(panel.bootFunction);
    out.u16(panel.updateFunction);
  }
  for (const need of needs) {
    out.u16(need.resource);
    out.u16(0);
    out.u32(need.amount);
  }

  // The directory, then the tables, each starting 4-aligned since every one is a multiple of 4 long.
  const offsets: number[] = [];
  let offset =
    HEADER_SIZE +
    panels.length * PANEL_SIZE +
    needs.length * NEED_SIZE +
    entries.length * DIRECTORY_SIZE;
  for (const bytes of stored) {
    offsets.push(offset);
    offset += bytes.length;
  }
  for (const entry of entries) {
    out.u16(entry.id);
    out.u16(entry.panel);
    out.u32(offsets[entry.data]);
    out.u32(stored[entry.data].length);
  }
  for (const bytes of stored) {
    out.bytes(bytes);
  }

  const bytes = out.done();
  const buildId = crc32(bytes);
  new DataView(bytes.buffer).setUint32(buildIdAt, buildId, true);
  return {bytes, buildId, tables: sizes};
}
