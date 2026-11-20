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

import {ENGINE_VALUES, NONE, TemplateKind, ValueType} from './format.mts';
import type {Program} from './writer.mts';

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

export interface DebugFunction {
  name: string;
  lines: DebugLine[];
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
  /** Where its element is, or null for one no element made: the root template's own node. */
  line: number | null;
  column: number | null;
  /** The slot that holds its handle, for a node code reaches. */
  handle?: number;
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
export const DEBUG_FILE_FORMAT = 1;

const TYPE_NAMES = new Map<number, string>(
  Object.entries(ValueType).map(([name, type]) => [type, name]),
);

const NODE_TYPES = new Map<number, string>(
  [...ENGINE_VALUES]
    .filter(([name]) => name.startsWith('ER_NODE_'))
    .map(([name, type]) => [type, name]),
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
 * One panel's part of the debug file.
 *
 * @param program  The panel's program.
 *
 * @returns The panel.
 */
function debugPanel(program: Program): DebugPanel {
  const {width, height} = program.panels[0];
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
  return {
    width,
    height,
    functions: program.functions.map(programFunction => ({
      name: programFunction.debug?.name ?? '',
      lines: (programFunction.debug?.lines ?? []).map(line => ({
        offset: line.offset,
        line: line.line,
        column: line.column + 1,
      })),
    })),
    states,
    templates: program.templates.map(template => ({
      kind: TEMPLATE_KINDS.get(template.kind)!,
      nodes: template.nodes.map(node => ({
        type: NODE_TYPES.get(node.type) ?? `${node.type}`,
        line: node.debug?.line ?? null,
        column: node.debug?.column ?? null,
        ...(node.handle !== NONE && {handle: node.handle}),
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
    panels: programs.map(debugPanel),
  };
  return `${JSON.stringify(debugFile)}\n`;
}

/**
 * Reads a debug file, refusing one this version can't read.
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
