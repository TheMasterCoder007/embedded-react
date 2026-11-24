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

// What the simulator's debugger reads from the running app: the runtime's descriptor, the app's state, lists, rows
// and locals, and the places a value is set at. It reads the same descriptor (erb_debug.h) and debug file as
// gdb/erb.py does on a board, and shows values the same way.

import type {
  DebugField,
  DebugFile,
  DebugLine,
  DebugLocal,
  DebugPanel,
  DebugState,
} from '../aot/compiler/package/debug-file.mts';

export const ERB_DEBUG_MAGIC = 0x44425245;
export const ERB_DEBUG_VERSION = 3;
export const DEBUG_FILE_FORMAT = 3;

export const NO_FUNCTION = 0xffff;
const LIST_HEADER_BYTES = 4;
const MOST_FRAMES = 64;
const MOST_ROWS_SHOWN = 8;
const LONGEST_STRING = 256;
const POINTER_BYTES = 4;

const ERROR_TOO_BIG = 7;
const ERROR_ENGINE_ABI = 5;
const ERROR_NO_PANEL = 6;
const ERROR_STACK = 21;

const RESOURCE_NAMES: Record<number, string> = {
  0: 'arena',
  0x100: 'nodes',
  0x101: 'package slot',
  1: 'vector nodes',
  2: 'animated values',
  3: 'animations',
  4: 'animation groups',
  5: 'images',
  6: 'fonts',
};

const STRING_ARENA = 0x80000000;
const STRING_NATIVE = 0x40000000;

const VALUE_BYTES: Record<string, number> = {
  int: 4,
  bool: 4,
  float: 4,
  enum: 4,
  int64: 8,
  double: 8,
};

const KIND_NAMES: Record<string, string> = {
  int: 'a whole number',
  int64: 'a whole number',
  float: 'a number',
  double: 'a number',
  bool: 'true or false',
  enum: 'one of a set of strings',
  list: 'a list',
};

/** A list's row field, with an interned string's values. */
type Field = DebugField & {values?: string[]};

/** A source line: a file by its index in the debug file's files, and a line in it. */
export interface Line {
  file: number;
  line: number;
}

/** A request's failure, said to the developer as it is. */
export class DebugError extends Error {}

/** The simulator's memory. Its buffer is replaced when the memory grows, so it is asked for at each read. */
export interface Memory {
  bytes(): Uint8Array;
}

/** A value the debugger can't read, and what it shows in its place. */
export class Unreadable {
  readonly text: string;

  constructor(text: string) {
    this.text = text;
  }
}

/** What an expression works on: a JavaScript value, or a row, a list or an object of the app's. */
export type Value =
  | number
  | boolean
  | string
  | null
  | undefined
  | Unreadable
  | Row
  | Rows
  | Fields;

/** What a name, a member or an index finds: a place a value is kept, or a value kept nowhere. */
export type Found = Place | Value;

// --------------------------------------------------------------------------------------------------------------------
// Reading the simulator
// --------------------------------------------------------------------------------------------------------------------

/** Reads and writes the simulator's memory, little-endian with 4-byte pointers. */
export class Target {
  private readonly memory: Memory;

  constructor(memory: Memory) {
    this.memory = memory;
  }

  private view(address: number, length: number): DataView {
    const bytes = this.memory.bytes();
    if (address <= 0 || address + length > bytes.length) {
      throw new DebugError(
        `can't read 0x${address.toString(16)} (${length} bytes)`,
      );
    }
    return new DataView(bytes.buffer, bytes.byteOffset + address, length);
  }

  /** A copy of some bytes. */
  read(address: number, length: number): Uint8Array {
    const view = this.view(address, length);
    return new Uint8Array(view.buffer, view.byteOffset, length).slice();
  }

  write(address: number, data: Uint8Array): void {
    const view = this.view(address, data.length);
    new Uint8Array(view.buffer, view.byteOffset, data.length).set(data);
  }

  u8(address: number): number {
    return this.view(address, 1).getUint8(0);
  }

  u16(address: number): number {
    return this.view(address, 2).getUint16(0, true);
  }

  u32(address: number): number {
    return this.view(address, 4).getUint32(0, true);
  }

  i32(address: number): number {
    return this.view(address, 4).getInt32(0, true);
  }

  setU8(address: number, value: number): void {
    this.view(address, 1).setUint8(0, value);
  }

  setU16(address: number, value: number): void {
    this.view(address, 2).setUint16(0, value, true);
  }

  setU32(address: number, value: number): void {
    this.view(address, 4).setUint32(0, value, true);
  }

  /** A NUL-terminated string, or null for no address. */
  string(address: number, limit = LONGEST_STRING): string | null {
    if (!address) {
      return null;
    }
    const bytes = this.memory.bytes();
    const end = Math.min(address + limit, bytes.length);
    let stop = address;
    while (stop < end && bytes[stop] !== 0) {
      stop++;
    }
    return decodeText(bytes.subarray(address, stop));
  }
}

const decoder = new TextDecoder('utf-8');
const encoder = new TextEncoder();

function decodeText(bytes: Uint8Array): string {
  return decoder.decode(bytes);
}

type FieldKind = 'u8' | 'u16' | 'u32' | 'ptr';

/** A C struct's field offsets, worked out with natural alignment. */
class Layout {
  readonly offsets: Record<string, number> = {};
  readonly size: number;

  constructor(fields: [string, FieldKind, number][]) {
    const sizes = {u8: 1, u16: 2, u32: 4, ptr: POINTER_BYTES};
    let offset = 0;
    let largest = 1;
    for (const [name, kind, count] of fields) {
      const size = sizes[kind];
      offset = Math.ceil(offset / size) * size;
      this.offsets[name] = offset;
      offset += size * count;
      largest = Math.max(largest, size);
    }
    this.size = Math.ceil(offset / largest) * largest;
  }
}

const RUNTIME_LAYOUT = new Layout([
  ['magic', 'u32', 1],
  ['version', 'u16', 1],
  ['frame_cells', 'u16', 1],
  ['engine', 'ptr', 1],
  ['reasons', 'ptr', 1],
  ['reason_count', 'u16', 1],
  ['reserved', 'u16', 1],
  ['error', 'ptr', 1],
  ['error_fields', 'u16', 10],
  ['loaded', 'ptr', 1],
  ['faulted', 'ptr', 1],
  ['in_vm', 'ptr', 1],
  ['build_id', 'ptr', 1],
  ['panel', 'ptr', 1],
  ['arena', 'ptr', 1],
  ['stack_offset', 'ptr', 1],
  ['function', 'ptr', 1],
  ['program_counter', 'ptr', 1],
  ['frame_base', 'ptr', 1],
  ['update', 'ptr', 1],
  ['halted', 'ptr', 1],
  ['control', 'ptr', 1],
  ['stop_capacity', 'u16', 1],
]);

const ERROR_FIELDS = [
  'status_at',
  'status_bytes',
  'detail_at',
  'needed_at',
  'available_at',
  'resource_at',
  'function_at',
  'offset_at',
  'name_at',
  'build_id_at',
] as const;

/** A flag, a number or a pointer the descriptor points at. */
type RuntimeField =
  | 'loaded'
  | 'faulted'
  | 'in_vm'
  | 'build_id'
  | 'panel'
  | 'arena'
  | 'stack_offset'
  | 'function'
  | 'program_counter'
  | 'frame_base'
  | 'update'
  | 'halted';

/** The runtime's descriptor (ErbDebug), read from the simulator. */
export class Runtime {
  readonly target: Target;
  private readonly address: number;
  readonly frameCells: number;
  readonly control: number;
  readonly stopCapacity: number;
  private readonly reasons: number;
  private readonly reasonCount: number;
  private readonly errorAddress: number;
  private readonly errorFields: Record<(typeof ERROR_FIELDS)[number], number>;

  constructor(target: Target, address: number) {
    this.target = target;
    this.address = address;
    const field = (name: string) => address + RUNTIME_LAYOUT.offsets[name];
    if (target.u32(field('magic')) !== ERB_DEBUG_MAGIC) {
      throw new DebugError(
        "the simulator's runtime has no debug descriptor: its build doesn't match this embedded-react",
      );
    }
    const version = target.u16(field('version'));
    if (version !== ERB_DEBUG_VERSION) {
      throw new DebugError(
        `the simulator's runtime descriptor is version ${version}, but this debugger reads version ${ERB_DEBUG_VERSION}: the prebuilt simulator doesn't match this embedded-react`,
      );
    }
    this.frameCells = target.u16(field('frame_cells'));
    this.reasons = target.u32(field('reasons'));
    this.reasonCount = target.u16(field('reason_count'));
    this.errorAddress = target.u32(field('error'));
    this.errorFields = Object.fromEntries(
      ERROR_FIELDS.map((name, index) => [
        name,
        target.u16(field('error_fields') + 2 * index),
      ]),
    ) as Record<(typeof ERROR_FIELDS)[number], number>;
    this.control = target.u32(field('control'));
    this.stopCapacity = target.u16(field('stop_capacity'));
  }

  private pointer(name: RuntimeField): number {
    return this.target.u32(this.address + RUNTIME_LAYOUT.offsets[name]);
  }

  flag(name: 'loaded' | 'faulted' | 'in_vm'): boolean {
    return this.target.u8(this.pointer(name)) !== 0;
  }

  u16(name: 'panel' | 'function'): number {
    return this.target.u16(this.pointer(name));
  }

  u32(
    name: 'build_id' | 'stack_offset' | 'program_counter' | 'frame_base',
  ): number {
    return this.target.u32(this.pointer(name));
  }

  get arena(): number {
    return this.target.u32(this.pointer('arena'));
  }

  private reason(status: number): string {
    if (status >= 0 && status < this.reasonCount) {
      return (
        this.target.string(
          this.target.u32(this.reasons + status * POINTER_BYTES),
        ) ?? ''
      );
    }
    return `unknown status ${status}`;
  }

  /** Has the app run its update at its next tick, after the debugger wrote state. */
  requestUpdate(): void {
    this.target.setU8(this.pointer('update'), 1);
  }

  /**
   * The last fault or refused load.
   *
   * @param place  Names a function and offset of a build, when the debug file is that build's.
   *
   * @returns Its reason and the lines that say it in full, or null for none.
   */
  lastError(
    place: (
      buildId: number,
      functionIndex: number,
      offset: number,
    ) => string | null,
  ): {reason: string; lines: string[]} | null {
    const fields = this.errorFields;
    const target = this.target;
    const at = (name: (typeof ERROR_FIELDS)[number]) =>
      this.errorAddress + fields[name];
    const status =
      fields.status_bytes === 4
        ? target.u32(at('status_at'))
        : fields.status_bytes === 1
          ? target.u8(at('status_at'))
          : target.u16(at('status_at'));
    if (status === 0) {
      return null;
    }
    const name = target.string(target.u32(at('name_at')));
    const detail = target.string(target.u32(at('detail_at')));
    const needed = target.u32(at('needed_at'));
    const available = target.u32(at('available_at'));
    const resource = target.u16(at('resource_at'));
    const functionIndex = target.u16(at('function_at'));
    const offset = target.u32(at('offset_at'));
    const buildId = target.u32(at('build_id_at'));

    const reason = this.reason(status) + (name ? `: ${name}` : '');
    const lines = [reason];
    if (status === ERROR_TOO_BIG) {
      lines.push(
        `  ${RESOURCE_NAMES[resource] ?? `resource ${resource}`}: needs ${needed}, the simulator reserves ${available}`,
      );
    } else if (status === ERROR_ENGINE_ABI) {
      lines.push(`  built for engine ABI ${needed}`);
    } else if (status === ERROR_NO_PANEL) {
      lines.push(`  this screen is ${needed} x ${available}`);
    } else if (status === ERROR_STACK) {
      lines.push(`  needs ${needed} cells, the arena has ${available}`);
    }
    if (detail) {
      lines.push(`  check: ${detail}`);
    }
    if (functionIndex !== NO_FUNCTION) {
      lines.push(
        `  at ${place(buildId, functionIndex, offset) ?? `function ${functionIndex} at ${offset}`}`,
      );
    }
    if (buildId) {
      lines.push(`  build ${buildHex(buildId)}`);
    }
    return {reason, lines};
  }
}

export function buildHex(buildId: number): string {
  return (buildId >>> 0).toString(16).padStart(8, '0');
}

// --------------------------------------------------------------------------------------------------------------------
// The debug file and the running app
// --------------------------------------------------------------------------------------------------------------------

/** The (file, line) a debug file's entry names. */
export function lineOf(entry: {file?: number; line: number}): Line {
  return {file: entry.file ?? 0, line: entry.line};
}

export function sameLine(left: Line | null, right: Line | null): boolean {
  return (
    left !== null &&
    right !== null &&
    left.file === right.file &&
    left.line === right.line
  );
}

/** A function's statements, in order: the offset each starts at and its line. */
export function statements(
  panel: DebugPanel,
  functionIndex: number,
): {at: number; line: Line}[] {
  const entry = panel.functions[functionIndex];
  if (!entry) {
    return [];
  }
  const found: {at: number; line: Line}[] = [];
  for (const line of entry.lines as DebugLine[]) {
    const place = lineOf(line);
    // An entry that carries on the line before it is part of its statement.
    if (!found.length || !sameLine(found[found.length - 1].line, place)) {
      found.push({at: line.offset, line: place});
    }
  }
  return found;
}

/** The start and line of the statement that holds an offset, or null for both before the first. */
export function statementLine(
  panel: DebugPanel,
  functionIndex: number,
  offset: number,
): {start: number | null; line: Line | null} {
  let start: number | null = null;
  let line: Line | null = null;
  for (const statement of statements(panel, functionIndex)) {
    if (statement.at > offset) {
      break;
    }
    start = statement.at;
    line = statement.line;
  }
  return {start, line};
}

const labelCache = new WeakMap<DebugPanel, Map<string, string>>();

/**
 * Each state's label, by storage name: its name, or `Component.name` for an inlined component's. When one component
 * is inlined more than once, the label adds the instance's line (`Counter:16.taps`), and a number when instances
 * share a line (`Counter:16[2].taps`). A label that is still shared falls back to the storage name.
 */
export function stateLabels(panel: DebugPanel): Map<string, string> {
  const cached = labelCache.get(panel);
  if (cached) {
    return cached;
  }
  const instance = (state: DebugState): number | null => {
    const match = /^c(\d+)_/.exec(state.storage);
    return match ? Number(match[1]) : null;
  };
  const byComponent = new Map<string, Set<number>>();
  const byLine = new Map<string, Set<number>>();
  const lineKey = (component: NonNullable<DebugState['component']>) =>
    `${component.name}\n${component.file ?? 0}\n${component.line}`;
  for (const state of panel.states) {
    const number = instance(state);
    if (state.component && number !== null) {
      const name = state.component.name;
      byComponent.set(name, (byComponent.get(name) ?? new Set()).add(number));
      const key = lineKey(state.component);
      byLine.set(key, (byLine.get(key) ?? new Set()).add(number));
    }
  }
  const label = (state: DebugState): string => {
    const component = state.component;
    if (!component) {
      return state.name;
    }
    if ((byComponent.get(component.name)?.size ?? 0) <= 1) {
      return `${component.name}.${state.name}`;
    }
    const onLine = [...(byLine.get(lineKey(component)) ?? [])].sort(
      (left, right) => left - right,
    );
    const number = instance(state);
    const numbered =
      onLine.length > 1 && number !== null && onLine.includes(number)
        ? `[${onLine.indexOf(number) + 1}]`
        : '';
    return `${component.name}:${component.line}${numbered}.${state.name}`;
  };
  const labels = new Map(
    panel.states.map(state => [state.storage, label(state)]),
  );
  const shared = new Map<string, number>();
  for (const value of labels.values()) {
    shared.set(value, (shared.get(value) ?? 0) + 1);
  }
  for (const [storage, value] of labels) {
    if ((shared.get(value) ?? 0) > 1) {
      labels.set(storage, storage);
    }
  }
  labelCache.set(panel, labels);
  return labels;
}

/** A frame of the app's call chain: its function, the offset it is at, and its base on the stack in cells. */
export interface Frame {
  function: number;
  offset: number;
  base: number;
}

/** The running app: the descriptor, and the debug file's panel for the layout it runs. */
export class App {
  readonly runtime: Runtime;
  readonly target: Target;
  readonly debugFile: DebugFile;
  readonly panel: DebugPanel;
  readonly arena: number;
  readonly loaded: boolean;

  constructor(
    runtime: Runtime,
    debugFile: DebugFile | null,
    needRunning = true,
  ) {
    this.runtime = runtime;
    this.target = runtime.target;
    if (debugFile === null) {
      throw new DebugError('no app has been built yet');
    }
    this.debugFile = debugFile;
    this.loaded = runtime.flag('loaded');
    if (needRunning && !this.loaded) {
      throw new DebugError('no app is running');
    }
    const buildId = runtime.u32('build_id');
    if (this.loaded && buildId !== debugFile.buildId >>> 0) {
      throw new DebugError(
        `the simulator runs build ${buildHex(buildId)}, but its debug file is build ${buildHex(debugFile.buildId)}`,
      );
    }
    const panel = this.loaded ? runtime.u16('panel') : 0;
    if (panel >= debugFile.panels.length) {
      throw new DebugError(
        `the simulator runs layout ${panel}, but the debug file has ${debugFile.panels.length}`,
      );
    }
    this.panel = debugFile.panels[panel];
    this.arena = this.loaded ? runtime.arena : 0;
  }

  get inVm(): boolean {
    return this.runtime.flag('in_vm');
  }

  /** `App.jsx:41  onPress` for a function and offset. */
  place(functionIndex: number, offset: number): string {
    const entry = this.panel.functions[functionIndex];
    if (!entry) {
      return `function ${functionIndex} at ${offset}`;
    }
    let line: DebugLine | null = null;
    for (const candidate of entry.lines) {
      if (candidate.offset > offset) {
        break;
      }
      line = candidate;
    }
    const name = entry.name || `function ${functionIndex}`;
    if (line === null) {
      return `${name} (function ${functionIndex} at ${offset})`;
    }
    return `${this.shown(lineOf(line))}  ${name}`;
  }

  /** A line as it is printed: Card.tsx:12. */
  shown(line: Line): string {
    return `${this.debugFile.files[line.file]}:${line.line}`;
  }

  functionName(functionIndex: number): string {
    return (
      this.panel.functions[functionIndex]?.name || `function ${functionIndex}`
    );
  }

  /** The app's call chain, innermost first. */
  frameChain(): Frame[] {
    const runtime = this.runtime;
    let base = runtime.u32('frame_base');
    const frames: Frame[] = [
      {
        function: runtime.u16('function'),
        offset: runtime.u32('program_counter'),
        base,
      },
    ];
    const stack = this.stack;
    for (let depth = 0; depth < MOST_FRAMES; depth++) {
      const caller = this.target.i32(stack + 4 * base);
      if (caller < 0) {
        break;
      }
      const resumeAt = this.target.u32(stack + 4 * base + 4);
      base = this.target.u32(stack + 4 * base + 8);
      // The caller resumes after its CALL, which is one word.
      frames.push({function: caller, offset: resumeAt - 1, base});
    }
    return frames;
  }

  get stack(): number {
    return this.arena + this.runtime.u32('stack_offset');
  }

  /** The parameters and locals that hold a value at an offset in a function. */
  localsAt(functionIndex: number, offset: number): DebugLocal[] {
    const entry = this.panel.functions[functionIndex];
    if (!entry) {
      return [];
    }
    return entry.locals.filter(
      local => local.from <= offset && offset < local.to,
    );
  }

  stateAt(slot: number | undefined): DebugState | null {
    return this.panel.states.find(state => state.slot === slot) ?? null;
  }

  label(state: DebugState): string {
    return stateLabels(this.panel).get(state.storage) ?? state.storage;
  }

  /** A state by the label it is shown with, its storage name, or its own name when only one has it. */
  state(name: string): DebugState {
    const states = this.panel.states;
    let found = states.filter(
      state => this.label(state) === name || state.storage === name,
    );
    if (!found.length) {
      found = states.filter(state => state.name === name);
    }
    if (found.length === 1) {
      return found[0];
    }
    if (found.length) {
      throw new DebugError(
        `${name} names ${found.length} states: ${found.map(state => this.label(state)).join(', ')}`,
      );
    }
    const names = states
      .filter(state => !state.hidden)
      .map(state => this.label(state))
      .join(', ');
    throw new DebugError(
      `no state named ${name}; the app's state: ${names || 'none'}`,
    );
  }

  /** Has the app run its update for a state the debugger wrote, as after a setter. */
  markChanged(state: DebugState): void {
    if (state.dirtyBit === undefined) {
      return;
    }
    const bit = state.dirtyBit;
    const word =
      this.arena + this.panel.regions.dirty.offset + 4 * Math.floor(bit / 32);
    this.target.setU32(word, (this.target.u32(word) | (1 << (bit % 32))) >>> 0);
    this.runtime.requestUpdate();
  }
}

// --------------------------------------------------------------------------------------------------------------------
// Values and where they are kept
// --------------------------------------------------------------------------------------------------------------------

/** Where a value is kept: it is read from there, and set there. */
export abstract class Place {
  kind = '';
  values: string[] | undefined = undefined;

  abstract read(): Value;

  /** Sets the value from its text. */
  abstract write(text: string): void;
}

/** An object: its fields by name, each a place, or an object within it. */
export class Fields {
  readonly members = new Map<string, Place | Fields>();

  /** Puts a place at a path of field names. A name already taken by a value keeps the place by its whole path. */
  add(path: string[], place: Place): void {
    let fields: Fields = this;
    for (let depth = 0; depth < path.length - 1; depth++) {
      let inner = fields.members.get(path[depth]);
      if (inner === undefined) {
        inner = new Fields();
        fields.members.set(path[depth], inner);
      }
      if (!(inner instanceof Fields)) {
        fields.members.set(path.slice(depth).join('.'), place);
        return;
      }
      fields = inner;
    }
    fields.members.set(path[path.length - 1], place);
  }

  text(): string {
    const shown: string[] = [];
    for (const [name, member] of this.members) {
      const value = settled(member);
      // A field a row leaves out.
      if (value !== undefined) {
        shown.push(`${name}: ${show(value)}`);
      }
    }
    return `{${shown.join(', ')}}`;
  }
}

/** The value of what a name, a member or an index finds: a place's is read. */
export function settled(found: Found): Value {
  return found instanceof Place ? found.read() : found;
}

/** A list's row: its bytes, and its number in the list. */
export class Row {
  readonly app: App;
  readonly state: DebugState;
  data: Uint8Array;
  at: number;
  readonly index: number | null;
  private fields: Fields | null = null;

  constructor(
    app: App,
    state: DebugState,
    data: Uint8Array,
    at = 0,
    index: number | null = null,
  ) {
    this.app = app;
    this.state = state;
    this.data = data;
    this.at = at;
    this.index = index;
  }

  get address(): number {
    return (
      this.app.arena +
      this.state.offset +
      LIST_HEADER_BYTES +
      (this.index ?? 0) * (this.state.rowBytes ?? 0)
    );
  }

  /** An optional field's flag, which says whether the row holds it, or null for a field every row holds. */
  flag(field: Field): Field | null {
    return (
      (this.state.fields ?? []).find(flag => flag.name === `${field.name}?`) ??
      null
    );
  }

  has(field: Field): boolean {
    const flag = this.flag(field);
    return (
      flag === null ||
      readTyped('bool', this.data, this.at + flag.offset) === true
    );
  }

  /** The row as an object: a field named `pos.x` is `x` of its `pos`. */
  tree(): Fields {
    if (this.fields === null) {
      this.fields = new Fields();
      for (const field of this.state.fields ?? []) {
        if (!field.name.endsWith('?')) {
          this.fields.add(field.name.split('.'), new FieldPlace(this, field));
        }
      }
    }
    return this.fields;
  }

  child(name: string): Place | Fields {
    const members = this.tree().members;
    const found = members.get(name);
    if (found === undefined) {
      throw new DebugError(
        `a row of ${this.state.name} has no ${name}; it has ${[...members.keys()].join(', ')}`,
      );
    }
    return found;
  }

  text(): string {
    return this.tree().text();
  }

  /** Reads the row again, after a write to it. */
  reread(): void {
    this.data = this.app.target.read(this.address, this.state.rowBytes ?? 0);
    this.at = 0;
  }
}

/** A field of a list's row. A list of plain values has one unnamed field, which is the row. */
export class FieldPlace extends Place {
  private readonly row: Row;
  private readonly field: Field;

  constructor(row: Row, field: Field) {
    super();
    this.row = row;
    this.field = field;
    this.kind = field.type;
    this.values = field.values;
  }

  read(): Value {
    const {row, field} = this;
    if (!row.has(field)) {
      return undefined;
    }
    return readTyped(
      field.type,
      row.data,
      row.at + field.offset,
      field.cap,
      field.values,
    );
  }

  write(text: string): void {
    const {row, field} = this;
    const name = `${row.state.name}[${row.index}]${field.name ? `.${field.name}` : ''}`;
    if (row.index === null) {
      throw new DebugError(`${name} is in no list, so it can't be set`);
    }
    const target = row.app.target;
    const flag = row.flag(field);
    if (text.trim() === 'undefined') {
      if (flag === null) {
        throw new DebugError(
          `every row of ${row.state.name} holds ${field.name || 'a value'}`,
        );
      }
      target.setU32(row.address + flag.offset, 0);
    } else {
      target.write(
        row.address + field.offset,
        encodeValue(field.type, text, name, field.cap, field.values),
      );
      if (flag !== null) {
        target.setU32(row.address + flag.offset, 1);
      }
    }
    row.app.markChanged(row.state);
    row.reread();
  }
}

/** A list state: its rows are read as they are asked for. */
export class Rows {
  readonly app: App;
  readonly state: DebugState;
  readonly count: number;
  private readonly address: number;
  private readonly plain: boolean;

  constructor(app: App, state: DebugState) {
    this.app = app;
    this.state = state;
    this.address = app.arena + state.offset;
    this.count = app.target.u32(this.address);
    const fields = state.fields ?? [];
    this.plain = fields.length === 1 && !fields[0].name;
  }

  /** Rows from a start, read at once: each a row, or a plain value's place. */
  children(start: number, count: number): (Row | FieldPlace)[] {
    const first = Math.max(start, 0);
    const wanted = Math.max(Math.min(count, this.count - first), 0);
    const size = this.state.rowBytes ?? 0;
    const data = wanted
      ? this.app.target.read(
          this.address + LIST_HEADER_BYTES + first * size,
          wanted * size,
        )
      : new Uint8Array(0);
    const found: (Row | FieldPlace)[] = [];
    for (let number = 0; number < wanted; number++) {
      const row = new Row(
        this.app,
        this.state,
        data,
        number * size,
        first + number,
      );
      found.push(
        this.plain ? new FieldPlace(row, (this.state.fields ?? [])[0]) : row,
      );
    }
    return found;
  }

  child(index: number): Row | FieldPlace | undefined {
    return index >= 0 ? this.children(index, 1)[0] : undefined;
  }

  row(index: number): Value {
    return settled(this.child(index));
  }

  text(): string {
    const shown = Math.min(this.count, MOST_ROWS_SHOWN);
    const rows = this.children(0, shown).map(row => show(settled(row)));
    const more = this.count > shown ? ', …' : '';
    return `[${rows.join(', ')}${more}]  (${this.count} of ${this.state.cap} rows)`;
  }
}

/** A 32-bit float as the shortest number that reads back as it, so 0.1 isn't 0.10000000149. */
function readFloat(view: DataView, at: number): number {
  const value = view.getFloat32(at, true);
  if (!Number.isFinite(value)) {
    return value;
  }
  for (let digits = 1; digits < 10; digits++) {
    const short = Number(value.toPrecision(digits));
    if (Math.fround(short) === value) {
      return short;
    }
  }
  return value;
}

/** A value from its bytes, as the expressions hold it. */
export function readTyped(
  kind: string,
  data: Uint8Array,
  at: number,
  cap?: number,
  values?: string[],
): Value {
  const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
  switch (kind) {
    case 'int':
      return view.getInt32(at, true);
    case 'bool':
      return view.getInt32(at, true) !== 0;
    case 'float':
      return readFloat(view, at);
    case 'int64':
      return Number(view.getBigInt64(at, true));
    case 'double':
      return view.getFloat64(at, true);
    case 'enum': {
      const index = view.getInt32(at, true);
      return values && index >= 0 && index < values.length
        ? values[index]
        : new Unreadable(`<value ${index}>`);
    }
    case 'string': {
      const text = data.subarray(at, at + (cap ?? 0) + 1);
      const end = text.indexOf(0);
      return decodeText(text.subarray(0, end >= 0 ? end : text.length));
    }
    default:
      return new Unreadable(`<${kind}>`);
  }
}

/** A value as the debugger shows it. */
export function show(value: Value): string {
  if (value === null) {
    return 'null';
  }
  if (value === undefined) {
    return 'undefined';
  }
  if (typeof value === 'boolean' || typeof value === 'number') {
    return String(value);
  }
  if (typeof value === 'string') {
    return JSON.stringify(value);
  }
  return value instanceof Unreadable ? value.text : value.text();
}

export function isStructured(value: unknown): value is Row | Rows | Fields {
  return (
    value instanceof Row || value instanceof Rows || value instanceof Fields
  );
}

/** A value's type, as an editor shows it beside the value. */
export function typeName(value: Value): string {
  if (typeof value === 'boolean') {
    return 'boolean';
  }
  if (typeof value === 'number' || typeof value === 'string') {
    return typeof value;
  }
  if (value instanceof Fields) {
    return 'object';
  }
  return value instanceof Row ? 'row' : value instanceof Rows ? 'list' : '';
}

/** A state's value. */
export function stateRead(app: App, state: DebugState): Value {
  const kind = state.type;
  const address = app.arena + state.offset;
  if (kind === 'string') {
    const cap = state.cap ?? 0;
    return readTyped(kind, app.target.read(address, cap + 1), 0, cap);
  }
  if (kind === 'list') {
    return new Rows(app, state);
  }
  return readTyped(
    kind,
    app.target.read(address, VALUE_BYTES[kind] ?? 4),
    0,
    undefined,
    state.values,
  );
}

export function stateValue(app: App, state: DebugState): string {
  return show(stateRead(app, state));
}

/** A string register's text: empty, in the arena, a native event's, or a package constant. */
function registerString(app: App, reference: number): Value {
  if (reference === 0) {
    return '';
  }
  if (reference & STRING_ARENA) {
    return app.target.string(app.arena + (reference & ~STRING_ARENA)) ?? '';
  }
  if (reference & STRING_NATIVE) {
    return new Unreadable("<a native event's string>");
  }
  return new Unreadable(`<string constant ${reference - 1}>`);
}

/** A row local's value: its list's row, through a filtered or sorted list when it counts in one. */
function listRow(app: App, local: DebugLocal, row: number): Value {
  const counted = app.stateAt(local.through ?? local.list);
  const state = app.stateAt(local.list);
  if (counted === null || state === null) {
    return new Unreadable(`<row ${row}>`);
  }
  const address = app.arena + counted.offset;
  const count = app.target.u32(address);
  if (row < 0 || row >= count) {
    return new Unreadable(
      `<no row ${row}: ${app.label(counted)} has ${count}>`,
    );
  }
  let index = row;
  if (local.through !== undefined) {
    const first = (counted.fields ?? [])[0];
    index = app.target.i32(
      address +
        LIST_HEADER_BYTES +
        row * (counted.rowBytes ?? 0) +
        (first?.offset ?? 0),
    );
  }
  return new Rows(app, state).row(index);
}

/** A parameter's or a local's value, in the frame at a base. */
function localRead(app: App, local: DebugLocal, base: number): Value {
  const kind = local.type;
  if (kind === undefined) {
    // A constant has its value in the debug file.
    return local.value !== undefined
      ? local.value
      : new Unreadable('<optimised out>');
  }
  if (kind === 'list') {
    const state = app.stateAt(local.list);
    return state ? new Rows(app, state) : new Unreadable('<list>');
  }
  const target = app.target;
  if (local.register !== undefined) {
    const address =
      app.stack + 4 * (base + app.runtime.frameCells + local.register);
    if (kind === 'row') {
      return listRow(app, local, target.i32(address));
    }
    if (kind === 'string') {
      return registerString(app, target.u32(address));
    }
    return readTyped(kind, target.read(address, VALUE_BYTES[kind] ?? 4), 0);
  }
  const address = app.arena + (local.offset ?? 0);
  if (kind === 'string') {
    const cap = local.cap ?? 0;
    return readTyped(kind, target.read(address, cap + 1), 0, cap);
  }
  return readTyped(kind, target.read(address, VALUE_BYTES[kind] ?? 4), 0);
}

/** A value's bytes from its text, for a state, a row's field or a local of a kind. */
function encodeValue(
  kind: string,
  written: string,
  name: string,
  cap?: number,
  values?: string[],
): Uint8Array {
  let text = written.trim();
  const refuse = () =>
    new DebugError(`${name} holds ${KIND_NAMES[kind] ?? kind}, not ${text}`);
  const bytes = new Uint8Array(VALUE_BYTES[kind] ?? 0);
  const view = new DataView(bytes.buffer);
  const whole = /^[+-]?(0[xX][0-9a-fA-F]+|0[bB][01]+|0[oO][0-7]+|\d+)$/;
  const number =
    /^[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$|^[+-]?Infinity$|^NaN$/;
  switch (kind) {
    case 'int': {
      if (!whole.test(text)) throw refuse();
      const value = parseWhole(text);
      if (value < -0x80000000n || value > 0x7fffffffn) throw refuse();
      view.setInt32(0, Number(value), true);
      return bytes;
    }
    case 'int64': {
      if (!whole.test(text)) throw refuse();
      const value = parseWhole(text);
      if (BigInt.asIntN(64, value) !== value) throw refuse();
      view.setBigInt64(0, value, true);
      return bytes;
    }
    case 'float':
    case 'double':
      if (!number.test(text)) throw refuse();
      if (kind === 'float') view.setFloat32(0, Number(text), true);
      else view.setFloat64(0, Number(text), true);
      return bytes;
    case 'bool':
      if (!['true', 'false', '1', '0'].includes(text)) throw refuse();
      view.setInt32(0, text === 'true' || text === '1' ? 1 : 0, true);
      return bytes;
  }
  if (
    text.length >= 2 &&
    text[0] === text[text.length - 1] &&
    `"'`.includes(text[0])
  ) {
    text = text.slice(1, -1);
  }
  if (kind === 'enum' && values) {
    if (!values.includes(text)) {
      throw new DebugError(
        `${name} is one of ${values.map(value => JSON.stringify(value)).join(', ')}`,
      );
    }
    view.setInt32(0, values.indexOf(text), true);
    return bytes;
  }
  if (kind === 'string') {
    const encoded = encoder.encode(text);
    if (encoded.length > (cap ?? 0)) {
      throw new DebugError(`${name} holds at most ${cap} bytes`);
    }
    const terminated = new Uint8Array(encoded.length + 1);
    terminated.set(encoded);
    return terminated;
  }
  throw new DebugError(
    `${name} can't be set from the debugger: it holds ${KIND_NAMES[kind] ?? kind}`,
  );
}

function parseWhole(text: string): bigint {
  const negative = text.startsWith('-');
  const digits = text.replace(/^[+-]/, '');
  return negative ? -BigInt(digits) : BigInt(digits);
}

/** A state, which a write also has the app update for. */
export class StatePlace extends Place {
  readonly app: App;
  readonly state: DebugState;
  /** Called after the debugger writes the state. */
  private readonly written: (state: DebugState) => void;

  constructor(
    app: App,
    state: DebugState,
    written: (state: DebugState) => void,
  ) {
    super();
    this.app = app;
    this.state = state;
    this.kind = state.type;
    this.values = state.values;
    this.written = written;
  }

  read(): Value {
    return stateRead(this.app, this.state);
  }

  write(text: string): void {
    const {app, state} = this;
    app.target.write(
      app.arena + state.offset,
      encodeValue(state.type, text, state.name, state.cap, state.values),
    );
    app.markChanged(state);
    this.written(state);
  }
}

/** A parameter or a local, in the frame at a base. */
export class LocalPlace extends Place {
  private readonly app: App;
  private readonly local: DebugLocal;
  private readonly base: number;

  constructor(app: App, local: DebugLocal, base: number) {
    super();
    this.app = app;
    this.local = local;
    this.base = base;
    this.kind = local.type ?? (local.value !== undefined ? 'constant' : '');
  }

  read(): Value {
    return localRead(this.app, this.local, this.base);
  }

  write(text: string): void {
    const {app, local} = this;
    const name = local.name;
    const kind = local.type;
    if (kind === undefined) {
      throw new DebugError(
        `the compiler folded ${name} into the code, so it has no value to set`,
      );
    }
    if (kind === 'row' || kind === 'list') {
      throw new DebugError(
        `${name} is a ${kind}: set one of its ${kind === 'row' ? 'fields' : 'rows'}`,
      );
    }
    if (local.register === undefined) {
      app.target.write(
        app.arena + (local.offset ?? 0),
        encodeValue(kind, text, name, local.cap),
      );
    } else if (kind === 'string') {
      throw new DebugError(
        `${name} reads a string kept elsewhere, so it has no text of its own to set`,
      );
    } else {
      const address =
        app.stack + 4 * (this.base + app.runtime.frameCells + local.register);
      app.target.write(address, encodeValue(kind, text, name));
    }
  }
}

/** Where a state sits among the app's state: its label, or for an object's field the object's label and the names. */
function statePath(app: App, state: DebugState): string[] {
  const label = app.label(state);
  const names = state.name.split('.');
  if (names.length > 1 && label.endsWith(state.name)) {
    return [
      label.slice(0, label.length - state.name.length) + names[0],
      ...names.slice(1),
    ];
  }
  return [label];
}

/** The app's state, an object state's fields under its name. */
export function stateTree(
  app: App,
  written: (state: DebugState) => void,
  hidden = false,
): Fields {
  const tree = new Fields();
  for (const state of app.panel.states) {
    if (hidden || !state.hidden) {
      tree.add(statePath(app, state), new StatePlace(app, state, written));
    }
  }
  return tree;
}

/** The parameters and locals a frame shows, parameters first. */
export function frameLocals(app: App, frame: Frame): DebugLocal[] {
  // A name declared again in an inner block hides the outer one, an object's fields too.
  const shown = new Map<string, DebugLocal>();
  for (const local of app.localsAt(frame.function, frame.offset)) {
    const name = local.name;
    const root = name.split('.')[0];
    for (const other of [...shown.keys()]) {
      if (
        other !== name &&
        other.split('.')[0] === root &&
        (other === root || name === root)
      ) {
        shown.delete(other);
      }
    }
    shown.set(name, local);
  }
  const locals = [...shown.values()];
  return [
    ...locals.filter(local => local.parameter),
    ...locals.filter(local => !local.parameter),
  ];
}

/** A frame's parameters and locals, an object's fields under its name. */
export function localsTree(app: App, frame: Frame): Fields {
  const tree = new Fields();
  for (const local of frameLocals(app, frame)) {
    tree.add(local.name.split('.'), new LocalPlace(app, local, frame.base));
  }
  return tree;
}
