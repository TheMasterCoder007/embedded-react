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
 * The debug file (app.erdbg) a build writes beside its package: what the package's debug section holds, and what
 * the package leaves out, each state's name and each node's source line, for host tools such as `embedded-react
 * where` and a debugger.
 */

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {
  ENGINE_VALUES,
  NONE,
  Region,
  SlotFlag,
  TemplateKind,
  ValueType,
} from './format.mts';
import type {Program, ProgramLocal} from './writer.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** Where a run of a function's code starts in the source. */
export interface DebugLine {
  /** The word the run starts at. */
  offset: number;
  line: number;
  /** 1-based. */
  column: number;
}

/**
 * A name the app gives a value in a function: a parameter, or a local. Where it is, a register, a place in the arena,
 * or a list's row, is only good in the words from `from` up to `to`; one with none of those the compiler folded away.
 */
export interface DebugLocal {
  name: string;
  /** A value type, `row` for a list's row, or `list`; none for a name the compiler folded away. */
  type?: string;
  /** What the function is entered with. */
  parameter?: boolean;
  /** Its register in the function's frame: for a row, the row's number. */
  register?: number;
  /** Its byte offset in the arena, for one kept in a slot. */
  offset?: number;
  /** A string's most bytes, without its NUL. */
  cap?: number;
  /** A row's or a list's list, by its slot in the states. */
  list?: number;
  /** For a row of a filtered or sorted list: that list, by its slot, which holds the row of `list`. */
  through?: number;
  from: number;
  to: number;
}

export interface DebugFunction {
  name: string;
  lines: DebugLine[];
  /** In the order the code names them. */
  locals: DebugLocal[];
}

/** A list's row field. */
export interface DebugField {
  name: string;
  type: string;
  /** Its byte offset in the row. */
  offset: number;
  /** A string's most bytes, without its NUL. */
  cap?: number;
}

export interface DebugState {
  /** The name the app gives it. */
  name: string;
  /** Its storage name: the name, prefixed for an inlined component's instance. */
  storage: string;
  /** The component it belongs to, and the line of that instance's element, for one inlined from another. */
  component?: {name: string; line: number};
  /** Worked out by the compiler, not written by the app: a filtered list, a memo's loop, an alert. */
  hidden: boolean;
  /** Its index in the slot table. */
  slot: number;
  type: string;
  /** Its byte offset in the arena. */
  offset: number;
  /** Its bit in the dirty words, for state a node or an effect reads. */
  dirtyBit?: number;
  /** A string's most bytes, without its NUL; a list's most rows. */
  cap?: number;
  /** An interned string's values, by the number its slot holds. */
  values?: string[];
  /** A list's row size and fields. */
  rowBytes?: number;
  fields?: DebugField[];
}

export interface DebugNode {
  /** The engine node type, an ER_NODE_* name. */
  type: string;
  /** Its children, which follow it in the template, each after the one before and everything under that. */
  children: number;
  /** Where its element is, or null for one no element made: the root template's own node. */
  line: number | null;
  column: number | null;
  /** Where its handle is, for a node code reaches: one handle, or one per row for a row's node. */
  handle?: DebugHandle;
}

/** A node handle's slot: `count` 8-byte handles from `offset` in the arena, each the tag + 1 and the serial. */
export interface DebugHandle {
  slot: number;
  offset: number;
  count: number;
}

/** An arena region. */
export interface DebugRegion {
  offset: number;
  bytes: number;
  /** What it holds: timers, running animations, queued events, or 0. */
  count: number;
}

export interface DebugTemplate {
  kind: 'root' | 'branch' | 'row';
  nodes: DebugNode[];
}

/** One layout of the package. */
export interface DebugPanel {
  /** The size it was compiled at. */
  width: number;
  height: number;
  arenaBytes: number;
  /** The arena's regions, by name. */
  regions: Record<string, DebugRegion>;
  /** In the functions table's order. */
  functions: DebugFunction[];
  states: DebugState[];
  /** In the templates table's order. */
  templates: DebugTemplate[];
}

export interface DebugFile {
  format: number;
  /** The program section's build_id, which the board's error screen shows. */
  buildId: number;
  /** The source file the lines are in. */
  file: string;
  /** The engine's node types, by the number a node holds. */
  nodeTypes: string[];
  /** In the package's panel order. */
  panels: DebugPanel[];
}

/** Where a function's code came from, in one or more panels. */
export interface DebugPlace {
  /** The panels it is for, by index. */
  panels: number[];
  function: string;
  line: number;
  column: number;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** The debug file's version, raised when a field changes its meaning. */
export const DEBUG_FILE_FORMAT = 2;

const TYPE_NAMES = new Map<number, string>(
  Object.entries(ValueType).map(([name, type]) => [type, name]),
);

const NODE_TYPES = new Map<number, string>(
  [...ENGINE_VALUES]
    .filter(([name]) => name.startsWith('ER_NODE_'))
    .map(([name, type]) => [type, name]),
);

const REGION_NAMES = new Map<number, string>(
  Object.entries(Region).map(([name, kind]) => [kind, name]),
);

const TEMPLATE_KINDS = new Map<number, DebugTemplate['kind']>(
  Object.entries(TemplateKind).map(([name, kind]) => [
    kind,
    name as DebugTemplate['kind'],
  ]),
);

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * A local as the debug file names it: a slot's place in the arena, and a list by its slot.
 *
 * @param program  The panel's program.
 * @param local  The local.
 *
 * @returns The local.
 */
function debugLocal(program: Program, local: ProgramLocal): DebugLocal {
  const {name, type, register, parameter, from, to} = local;
  const described: DebugLocal = {
    name,
    ...(type !== undefined && {type}),
    ...(parameter && {parameter}),
    ...(register !== undefined && {register}),
    from,
    to,
  };
  if (type === 'list') {
    described.list = local.slot;
  } else if (local.slot !== undefined) {
    const slot = program.slots[local.slot];
    described.offset = slot.offset;
    if (type === 'string') {
      described.cap = slot.cap;
    }
  }
  if (type === 'row') {
    described.list = local.list;
    if (local.through !== undefined) {
      described.through = local.through;
    }
  }
  return described;
}

/**
 * One panel's part of the debug file.
 *
 * @param program  The panel's program.
 *
 * @returns The panel.
 */
function debugPanel(program: Program): DebugPanel {
  const {width, height, arenaBytes} = program.panels[0];
  const typeName = (type: number): string => TYPE_NAMES.get(type) ?? `${type}`;
  const states = (program.states ?? []).map(state => {
    const slot = program.slots[state.slot];
    const described: DebugState = {
      name: state.name,
      storage: state.storage,
      ...(state.component && {component: state.component}),
      hidden: state.hidden,
      slot: state.slot,
      type: typeName(slot.type),
      offset: slot.offset,
      ...(slot.flags & SlotFlag.tracked && {dirtyBit: slot.dirtyBit}),
    };
    if (slot.type === ValueType.string || slot.type === ValueType.list) {
      described.cap = slot.cap;
    }
    if (slot.type === ValueType.enum) {
      described.values = program.enums[slot.aux].map(
        string => program.strings[string],
      );
    }
    if (slot.type === ValueType.list) {
      const row = program.rows[slot.aux];
      described.rowBytes = row.size;
      described.fields = row.fields.map((field, index) => ({
        name: state.fields?.[index] ?? `${index}`,
        type: typeName(field.type),
        offset: field.offset,
        ...(field.type === ValueType.string && {cap: field.cap}),
      }));
    }
    return described;
  });
  const regions = Object.fromEntries(
    program.regions.map(region => [
      REGION_NAMES.get(region.kind) ?? `${region.kind}`,
      {offset: region.offset, bytes: region.bytes, count: region.count},
    ]),
  );
  return {
    width,
    height,
    arenaBytes,
    regions,
    functions: program.functions.map(programFunction => ({
      name: programFunction.debug?.name ?? '',
      lines: (programFunction.debug?.lines ?? []).map(line => ({
        offset: line.offset,
        line: line.line,
        column: line.column + 1,
      })),
      locals: (programFunction.debug?.locals ?? []).map(local =>
        debugLocal(program, local),
      ),
    })),
    states,
    templates: program.templates.map(template => ({
      kind: TEMPLATE_KINDS.get(template.kind)!,
      nodes: template.nodes.map(node => ({
        type: NODE_TYPES.get(node.type) ?? `${node.type}`,
        children: node.children,
        line: node.debug?.line ?? null,
        column: node.debug?.column ?? null,
        ...(node.handle !== NONE && {
          handle: {
            slot: node.handle,
            offset: program.slots[node.handle].offset,
            count: Math.max(program.slots[node.handle].cap, 1),
          },
        }),
      })),
    })),
  };
}

/**
 * Writes the debug file.
 *
 * @param programs  Each panel's program, in the order the package lists them.
 * @param buildId  The program section's build_id.
 * @param file  The source file, as the error screen names it.
 *
 * @returns app.erdbg's text.
 */
export function writeDebugFile(
  programs: Program[],
  buildId: number,
  file: string,
): string {
  const debugFile: DebugFile = {
    format: DEBUG_FILE_FORMAT,
    buildId,
    file,
    nodeTypes: Array.from(
      {length: Math.max(...NODE_TYPES.keys()) + 1},
      (_, type) => NODE_TYPES.get(type) ?? '',
    ),
    panels: programs.map(debugPanel),
  };
  return `${JSON.stringify(debugFile)}\n`;
}

/**
 * What is wrong with a parsed debug file's shape, so a tool can say so instead of failing as it reads it.
 *
 * @param debugFile  The parsed file, its format already checked.
 *
 * @returns The first fault found, or null for none.
 */
function shapeFault(debugFile: DebugFile): string | null {
  const isNumber = (value: unknown): boolean =>
    typeof value === 'number' && Number.isFinite(value);
  const isString = (value: unknown): boolean => typeof value === 'string';
  const isList = (value: unknown): value is unknown[] => Array.isArray(value);
  const isRecord = (value: unknown): value is Record<string, unknown> =>
    typeof value === 'object' && value !== null && !Array.isArray(value);

  if (!isNumber(debugFile.buildId)) return 'no buildId';
  if (!isString(debugFile.file)) return 'no file';
  if (!isList(debugFile.nodeTypes)) return 'no nodeTypes';
  for (const [panelIndex, panel] of debugFile.panels.entries()) {
    const where = `panel ${panelIndex}`;
    if (!isRecord(panel) || !isNumber(panel.width) || !isNumber(panel.height)) {
      return `${where} has no size`;
    }
    if (!isList(panel.functions) || !isList(panel.states)) {
      return `${where} has no functions or states`;
    }
    if (!isList(panel.templates)) return `${where} has no templates`;
    if (!isRecord(panel.regions) || !isNumber(panel.arenaBytes)) {
      return `${where} has no arena`;
    }
    for (const [functionIndex, programFunction] of panel.functions.entries()) {
      if (
        !isRecord(programFunction) ||
        !isString(programFunction.name) ||
        !isList(programFunction.lines) ||
        !programFunction.lines.every(
          line =>
            isRecord(line) &&
            isNumber(line.offset) &&
            isNumber(line.line) &&
            isNumber(line.column),
        ) ||
        !isList(programFunction.locals) ||
        !programFunction.locals.every(
          local =>
            isRecord(local) &&
            isString(local.name) &&
            isNumber(local.from) &&
            isNumber(local.to),
        )
      ) {
        return `${where}, function ${functionIndex}`;
      }
    }
    for (const [stateIndex, state] of panel.states.entries()) {
      if (
        !isRecord(state) ||
        !isString(state.name) ||
        !isString(state.storage) ||
        !isString(state.type) ||
        !isNumber(state.slot) ||
        !isNumber(state.offset)
      ) {
        return `${where}, state ${stateIndex}`;
      }
    }
    for (const [templateIndex, template] of panel.templates.entries()) {
      if (
        !isRecord(template) ||
        !isString(template.kind) ||
        !isList(template.nodes) ||
        !template.nodes.every(
          node =>
            isRecord(node) &&
            isString(node.type) &&
            isNumber(node.children) &&
            (node.line === null || isNumber(node.line)),
        )
      ) {
        return `${where}, template ${templateIndex}`;
      }
    }
  }
  return null;
}

/**
 * Reads a debug file, refusing one this version can't read or whose shape is wrong.
 *
 * @param text  app.erdbg's text.
 *
 * @returns The file.
 */
export function readDebugFile(text: string): DebugFile {
  let parsed: DebugFile;
  try {
    parsed = JSON.parse(text);
  } catch {
    throw new Error(
      'not a debug file: build the app again to write a new app.erdbg',
    );
  }
  if (parsed?.format !== DEBUG_FILE_FORMAT || !Array.isArray(parsed.panels)) {
    throw new Error(
      `a debug file of format ${parsed?.format ?? 'unknown'}, but this version reads format ${DEBUG_FILE_FORMAT}: ` +
        'build the app again with this version',
    );
  }
  const fault = shapeFault(parsed);
  if (fault) {
    throw new Error(
      `a malformed debug file (${fault}): build the app again to write a new app.erdbg`,
    );
  }
  return parsed;
}

/**
 * Where a place in a function came from: the last line at or before the offset, as the error screen finds it. The
 * panels can number their functions apart, so each panel is looked up, and panels that agree share a place.
 *
 * @param debugFile  The debug file.
 * @param functionIndex  The function's index.
 * @param offset  The word offset in the function.
 *
 * @returns Each place, or none when no panel has a line there.
 */
export function placeOf(
  debugFile: DebugFile,
  functionIndex: number,
  offset: number,
): DebugPlace[] {
  const places: DebugPlace[] = [];
  debugFile.panels.forEach((panel, panelIndex) => {
    const programFunction = panel.functions[functionIndex];
    const line = programFunction?.lines.findLast(
      entry => entry.offset <= offset,
    );
    if (!line) {
      return;
    }
    const same = places.find(
      place =>
        place.function === programFunction.name &&
        place.line === line.line &&
        place.column === line.column,
    );
    if (same) {
      same.panels.push(panelIndex);
    } else {
      places.push({
        panels: [panelIndex],
        function: programFunction.name,
        line: line.line,
        column: line.column,
      });
    }
  });
  return places;
}
