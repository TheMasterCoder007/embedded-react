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
import {checksTable} from './checks.mts';
import {
  DEBUG_ENTRY_SIZE,
  DEBUG_HEADER_SIZE,
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
import type {LayoutCheck} from './checks.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

export interface ProgramPanel {
  /** The size the layout was compiled at; any size its check takes runs it. */
  width: number;
  height: number;
  check: LayoutCheck;
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

export interface ProgramImport {
  /** Its name's string index. */
  name: number;
  version: number;
  /** Its signature's string index. */
  signature: number;
}

/** A native event's handler: the event as the firmware registers it, and the function it runs. */
export interface ProgramNativeEvent {
  /** Its name's string index. */
  name: number;
  version: number;
  /** Its signature's string index. */
  signature: number;
  function: number;
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
  parameters: [number, number, number, number];
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
  gridColumns: number;
  keyGap: number;
  keyRadius: number;
  fontSize: number;
  panelColor: number;
  keyColor: number;
  keyActiveColor: number;
  labelColor: number;
}

/** Where a run of a function's code starts in the source. */
export interface ProgramLine {
  /** The word the run starts at. */
  offset: number;
  /** 1-based. */
  line: number;
  /** 0-based, as Babel counts it. */
  column: number;
}

export interface ProgramFunction {
  kind: number;
  parameters: number;
  result: number;
  /** One type per register cell. */
  types: number[];
  /** The instruction words. */
  code: number[];
  /** What the debug table says of it. */
  debug?: {name: string; lines: ProgramLine[]};
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
  animations: ProgramAnim[];
  /** Native methods the code calls. */
  imports: ProgramImport[];
  /** The handlers of the native events the app subscribes to. */
  nativeEvents: ProgramNativeEvent[];
  /** Host-fed values, sorted by name. */
  inputs: ProgramInput[];
  interpolations: ProgramInterp[];
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

/** Bytes a writer starts with; it doubles as it fills. */
const INITIAL_BUFFER_SIZE = 256;

/*----------------------------------------------------------------------------------------------------------------------
 - Classes
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Little-endian bytes, grown as they are written.
 *
 * @class ByteWriter
 */
export class ByteWriter {
  private buffer = new Uint8Array(INITIAL_BUFFER_SIZE);
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
  place(programFunction: ProgramFunction): {code: number; types: number} {
    return {
      code: Bodies.store(this.codeAt, this.code, programFunction.code),
      types: Bodies.store(this.typesAt, this.types, programFunction.types),
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
  writeRecord: (writer: ByteWriter, record: Record) => void,
): Uint8Array {
  const writer = new ByteWriter();
  writer.u32(groups.length);
  let first = 0;
  for (const group of groups) {
    writer.u16(first);
    writer.u16(group.length);
    first += group.length;
  }
  for (const group of groups) {
    for (const record of group) {
      writeRecord(writer, record);
    }
  }
  return writer.done();
}

/**
 * Bytes padded with zeros to a multiple of 4.
 *
 * @param bytes  The bytes.
 *
 * @returns The padded bytes.
 */
function padded(bytes: Uint8Array): Uint8Array {
  const writer = new ByteWriter();
  writer.bytes(bytes);
  writer.pad4();
  return writer.done();
}

/**
 * The strings table: the count, each string's byte offset from the table's start, then the NUL-terminated UTF-8.
 *
 * @param strings  The strings.
 *
 * @returns The table's bytes.
 */
export function stringsTable(strings: string[]): Uint8Array {
  const writer = new ByteWriter();
  writer.u32(strings.length);
  const encoded = strings.map(text => new TextEncoder().encode(`${text}\0`));
  let offset = 4 + strings.length * 4;
  for (const bytes of encoded) {
    writer.u32(offset);
    offset += bytes.length;
  }
  for (const bytes of encoded) {
    writer.bytes(bytes);
  }
  writer.pad4();
  return writer.done();
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
  const table = (id: number, write: (writer: ByteWriter) => void): void => {
    const writer = new ByteWriter();
    write(writer);
    tables.set(id, writer.done());
  };

  if (program.strings.length) {
    tables.set(Table.strings, stringsTable(program.strings));
  }
  if (program.const32.length) {
    table(Table.const32, writer =>
      program.const32.forEach(bits => writer.u32(bits)),
    );
  }
  if (program.const64.length) {
    table(Table.const64, writer => {
      for (const value of program.const64) {
        const bits = BigInt.asUintN(64, value);
        writer.u32(Number(bits & 0xffffffffn));
        writer.u32(Number(bits >> 32n));
      }
    });
  }
  table(Table.arena, writer => {
    for (const region of program.regions) {
      writer.u16(region.kind);
      writer.u16(region.count);
      writer.u32(region.offset);
      writer.u32(region.bytes);
    }
  });
  table(Table.slots, writer => {
    for (const slot of program.slots) {
      writer.u8(slot.type);
      writer.u8(slot.flags);
      writer.u16(slot.dirtyBit);
      writer.u32(slot.offset);
      writer.u32(slot.init);
      writer.u16(slot.cap);
      writer.u16(slot.aux);
    }
  });
  if (program.enums.length) {
    const enums = headedTable(program.enums, (writer, value) =>
      writer.u16(value),
    );
    tables.set(Table.enums, padded(enums));
  }
  if (program.rows.length) {
    table(Table.rows, writer => {
      writer.u32(program.rows.length);
      let first = 0;
      for (const row of program.rows) {
        writer.u16(row.size);
        writer.u16(first);
        writer.u16(row.fields.length);
        writer.u16(0);
        first += row.fields.length;
      }
      for (const field of program.rows.flatMap(row => row.fields)) {
        writer.u8(field.type);
        writer.u8(0);
        writer.u16(field.cap);
        writer.u32(field.offset);
      }
    });
  }
  if (program.propSets.length) {
    tables.set(
      Table.props,
      headedTable(program.propSets, (writer, entry) => {
        writer.u16(entry.prop);
        writer.u8(entry.kind);
        writer.u8(0);
        writer.u32(entry.value);
      }),
    );
  }
  table(Table.templates, writer => {
    writer.u32(program.templates.length);
    let firstNode = 0;
    for (const template of program.templates) {
      writer.u16(firstNode);
      writer.u16(template.nodes.length);
      writer.u16(template.kind);
      writer.u16(0);
      firstNode += template.nodes.length;
    }
    for (const node of program.templates.flatMap(template => template.nodes)) {
      writer.u8(node.type);
      writer.u8(0);
      writer.u16(node.children);
      writer.u16(node.props);
      writer.u16(node.handle);
      writer.u16(node.bindings);
      writer.u8(node.bindingCount);
      writer.u8(0);
    }
  });
  if (program.bindings.length) {
    table(Table.bindings, writer => {
      for (const binding of program.bindings) {
        writer.u8(binding.kind);
        writer.u8(binding.event);
        writer.u16(binding.target);
      }
    });
  }
  table(Table.functions, writer => {
    for (const programFunction of program.functions) {
      const {code, types} = bodies.place(programFunction);
      writer.u32(code);
      writer.u32(programFunction.code.length);
      writer.u32(types);
      writer.u8(programFunction.types.length);
      writer.u8(programFunction.parameters);
      writer.u8(programFunction.result);
      writer.u8(programFunction.kind);
    }
  });
  if (program.animations.length) {
    table(Table.animations, writer => {
      for (const animation of program.animations) {
        writer.u8(animation.kind);
        writer.u8(animation.easing);
        writer.u8(animation.flags);
        writer.u8(0);
        writer.u16(animation.target);
        writer.u16(animation.prop);
        writer.u16(animation.first);
        writer.u16(animation.count);
        writer.u32(animation.amount);
        writer.u32(animation.delayMs);
        writer.u32(animation.to);
        animation.parameters.forEach(bits => writer.u32(bits));
      }
    });
  }
  if (program.imports.length) {
    table(Table.imports, writer => {
      for (const nativeImport of program.imports) {
        writer.u16(nativeImport.name);
        writer.u16(nativeImport.version);
        writer.u16(nativeImport.signature);
        writer.u16(0);
      }
    });
  }
  if (program.nativeEvents.length) {
    table(Table.nativeEvents, writer => {
      for (const nativeEvent of program.nativeEvents) {
        writer.u16(nativeEvent.name);
        writer.u16(nativeEvent.version);
        writer.u16(nativeEvent.signature);
        writer.u16(nativeEvent.function);
      }
    });
  }
  if (program.inputs.length) {
    table(Table.inputs, writer => {
      for (const input of program.inputs) {
        writer.u16(input.name);
        writer.u16(input.slot);
      }
    });
  }
  if (program.interpolations.length) {
    table(Table.interpolations, writer => {
      for (const interpolation of program.interpolations) {
        writer.u16(interpolation.slot);
        writer.u8(interpolation.input.length);
        writer.u8(interpolation.flags);
        writer.u8(interpolation.extrapolateLeft);
        writer.u8(interpolation.extrapolateRight);
        writer.u16(0);
        for (const values of [interpolation.input, interpolation.output]) {
          for (let point = 0; point < MAX_POINTS; point++) {
            writer.u32(values[point] ?? 0);
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
  const writer = new ByteWriter();
  writer.u32(tapes.length);
  writer.u32(paintsAt);
  writer.u32(gradientsAt);
  writer.u32(gradientsAt + gradients.length * GRADIENT_SIZE);
  let floatFirst = 0;
  let paintFirst = 0;
  let gradientFirst = 0;
  for (const tape of tapes) {
    writer.u32(floatsAt + floatFirst * 4);
    writer.u16(tape.floats.length);
    writer.u16(paintFirst);
    writer.u16(tape.paints.length);
    writer.u16(gradientFirst);
    writer.u16(tape.gradients.length);
    writer.u16(0);
    floatFirst += tape.floats.length;
    paintFirst += tape.paints.length;
    gradientFirst += tape.gradients.length;
  }
  tapes.forEach(tape => tape.floats.forEach(value => writer.f32(value)));
  for (const paint of paints) {
    writer.u32(paint.fill);
    writer.u32(paint.stroke);
    writer.f32(paint.strokeWidth);
    writer.f32(paint.miter);
    writer.u8(paint.cap);
    writer.u8(paint.join);
    writer.u8(paint.fillRule);
    writer.u8(0);
    writer.u16(paint.fillGradient);
    writer.u16(paint.strokeGradient);
  }
  let stopFirst = 0;
  for (const gradient of gradients) {
    writer.u8(gradient.type);
    writer.u8(gradient.stops.length);
    writer.u16(stopFirst);
    for (const value of [
      gradient.ax,
      gradient.ay,
      gradient.bx,
      gradient.by,
      gradient.radius,
    ]) {
      writer.f32(value);
    }
    stopFirst += gradient.stops.length;
  }
  for (const stop of gradients.flatMap(gradient => gradient.stops)) {
    writer.u32(stop.color);
    writer.f32(stop.position);
  }
  return writer.done();
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
  const writer = new ByteWriter();
  writer.u16(keyboard.layers.length);
  writer.u16(rows.length);
  writer.u16(keys.length);
  writer.u16(keyboard.rowHeight);
  writer.u8(keyboard.gridColumns);
  writer.u8(keyboard.keyGap);
  writer.u8(keyboard.keyRadius);
  writer.u8(keyboard.fontSize);
  writer.u32(keyboard.panelColor);
  writer.u32(keyboard.keyColor);
  writer.u32(keyboard.keyActiveColor);
  writer.u32(keyboard.labelColor);
  const groups = (sizes: number[]): void => {
    let first = 0;
    for (const size of sizes) {
      writer.u16(first);
      writer.u16(size);
      first += size;
    }
  };
  groups(keyboard.layers.map(layer => layer.length));
  groups(rows.map(row => row.length));
  for (const key of keys) {
    writer.u16(key.label);
    writer.u16(key.text);
    writer.u8(key.type);
    writer.u8(key.layer);
    writer.u8(key.span);
    writer.u8(key.highlight);
  }
  return writer.done();
}

/** Whether two tables hold the same bytes. */
function sameBytes(left: Uint8Array, right: Uint8Array): boolean {
  return (
    left.length === right.length &&
    left.every((byte, index) => byte === right[index])
  );
}

/**
 * Each panel's needs: one for every panel where they agree, else one for each panel that has it, since only the
 * chosen layout's have to fit.
 *
 * @param programs  Each panel's program.
 *
 * @returns The needs, with the panel each is for.
 */
function panelNeeds(programs: Program[]): (ProgramNeed & {panel: number})[] {
  const resources = [
    ...new Set(
      programs.flatMap(program => program.needs.map(need => need.resource)),
    ),
  ].sort((left, right) => left - right);
  return resources.flatMap(resource => {
    const amounts = programs.map(
      program =>
        program.needs.find(need => need.resource === resource)?.amount ?? 0,
    );
    return amounts.every(amount => amount === amounts[0])
      ? [{resource, amount: amounts[0], panel: NONE}]
      : amounts.flatMap((amount, panel) =>
          amount ? [{resource, amount, panel}] : [],
        );
  });
}

/**
 * Writes a program section, its build_id worked out over the finished bytes. Each program is one layout's. A
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
  const checks = checksTable(programs.map(program => program.panels[0].check));
  const types = new ByteWriter();
  bodies.types.forEach(type => types.u8(type));
  types.pad4();
  const code = new ByteWriter();
  bodies.code.forEach(word => code.u32(word));
  for (const tables of panelTables) {
    tables.set(Table.types, types.done());
    tables.set(Table.code, code.done());
    if (checks.words.length) {
      const words = new ByteWriter();
      checks.words.forEach(word => words.u32(word));
      tables.set(Table.checks, words.done());
    }
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
    const ownCopies: number[] = [];
    copies.forEach((copy, panel) => {
      if (!copy) {
        return;
      }
      let data = ownCopies.find(index => sameBytes(stored[index], copy));
      if (data === undefined) {
        data = stored.push(copy) - 1;
        ownCopies.push(data);
      }
      entries.push({id, panel, data});
    });
    sizes.push({id, bytes: ownCopies.map(index => stored[index].length)});
  }

  const panels = programs.map(program => program.panels[0]);
  const needs = panelNeeds(programs);
  const writer = new ByteWriter();
  writer.u16(FORMAT_MAJOR);
  writer.u16(FORMAT_MINOR);
  writer.u16(ENGINE_ABI);
  writer.u16(programs[0].flags);
  const buildIdAt = writer.length;
  writer.u32(0);
  writer.u32(Math.max(...panels.map(panel => panel.arenaBytes)));
  writer.u16(Math.max(...panels.map(panel => panel.nodeCount)));
  writer.u16(panels.length);
  writer.u16(needs.length);
  writer.u16(entries.length);
  panels.forEach((panel, index) => {
    writer.u32(checks.starts[index]);
    writer.u32(panel.arenaBytes);
    writer.u16(panel.nodeCount);
    writer.u16(panel.rootTemplate);
    writer.u16(panel.bootFunction);
    writer.u16(panel.updateFunction);
  });
  for (const need of needs) {
    writer.u16(need.resource);
    writer.u16(need.panel);
    writer.u32(need.amount);
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
    writer.u16(entry.id);
    writer.u16(entry.panel);
    writer.u32(offsets[entry.data]);
    writer.u32(stored[entry.data].length);
  }
  for (const bytes of stored) {
    writer.bytes(bytes);
  }

  const bytes = writer.done();
  const buildId = crc32(bytes);
  new DataView(bytes.buffer).setUint32(buildIdAt, buildId, true);
  return {bytes, buildId, tables: sizes};
}

/**
 * One panel's debug table: where each function's code is in the source, and each function's name.
 *
 * @param functions  The panel's functions.
 * @param buildId  The program section's build_id.
 * @param file  The source file, as the error screen names it.
 * @param panel  The panel it is for, or NONE for every panel.
 *
 * @returns The table's bytes.
 */
function debugTable(
  functions: ProgramFunction[],
  buildId: number,
  file: string,
  panel: number,
): Uint8Array {
  const strings = [file];
  const string = (text: string): number => {
    const at = strings.indexOf(text);
    return at >= 0 ? at : strings.push(text) - 1;
  };
  const names = functions.map(programFunction =>
    string(programFunction.debug?.name ?? ''),
  );
  const entries = functions.flatMap((programFunction, index) =>
    (programFunction.debug?.lines ?? []).map(line => ({
      ...line,
      function: index,
    })),
  );
  const stringBytes = stringsTable(strings);
  const stringsAt =
    DEBUG_HEADER_SIZE +
    entries.length * DEBUG_ENTRY_SIZE +
    Math.ceil(names.length / 2) * 4;

  const writer = new ByteWriter();
  writer.u32(buildId);
  writer.u32(entries.length);
  writer.u16(functions.length);
  writer.u16(panel);
  writer.u32(stringsAt);
  writer.u32(stringsAt + stringBytes.length);
  for (const entry of entries) {
    writer.u16(entry.function);
    writer.u16(0);
    writer.u32(entry.offset);
    writer.u16(0);
    // 1-based, as the compiler's errors show it.
    writer.u16(Math.min(entry.column + 1, NONE));
    writer.u32(entry.line);
  }
  names.forEach(name => writer.u16(name));
  writer.pad4();
  writer.bytes(stringBytes);
  return writer.done();
}

/**
 * Writes the debug section: a table for every panel when the panels' tables are the same, else one for each.
 *
 * @param programs  Each panel's program, in the order the panel table lists them.
 * @param buildId  The program section's build_id.
 * @param file  The source file, as the error screen names it.
 *
 * @returns The section's bytes.
 */
export function writeDebug(
  programs: Program[],
  buildId: number,
  file: string,
): Uint8Array {
  const shared = programs.map(program =>
    debugTable(program.functions, buildId, file, NONE),
  );
  if (shared.every(table => sameBytes(table, shared[0]))) {
    return shared[0];
  }
  const writer = new ByteWriter();
  programs.forEach((program, panel) =>
    writer.bytes(debugTable(program.functions, buildId, file, panel)),
  );
  return writer.done();
}
