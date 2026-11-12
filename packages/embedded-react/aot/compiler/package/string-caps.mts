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
 * String caps: each string buffer is sized from the values that can reach it. A string state's or a list
 * field's cap is the most bytes its initial value or any write can hold, and a local's is its value's. A write
 * is built in the format buffer, so it holds at most ER_TEXT_MAX bytes.
 *
 * A string built from itself, where a write reads it back and adds to it (`setLog(log + '.')`), has no bound
 * of its own, so it takes the default cap (listStrCap). A `slice` with a fixed length bounds what it cuts, which
 * is how an app sets a cap of its own: `setLog(log.slice(-39) + '.')`.
 */

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {aotError, constantText, ENGINE_STRING_MAX} from '../diagnostics.mts';
import {nodesOf} from '../ir/nodes.mts';
import {itemReadOf, itemValueAt} from '../list-items.mts';
import {TEXT_END} from '../ir/text.mts';
import type {AotError, SourcePosition} from '../diagnostics.mts';
import type {IrApp} from '../ir/app.mts';
import type {IrExpr} from '../ir/expressions.mts';
import type {IrStmt} from '../ir/statements.mts';
import type {IrText, IrTextEdit, IrTextPart} from '../ir/text.mts';
import type {ListState, ScalarState} from '../types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

// What an IR walk skips: where a value came from (which repeats its reads, untyped) and source positions.
const SKIPPED_KEYS = new Set(['source', 'location']);

// The most bytes JS prints for a number: "-2147483648", "-9223372036854775808", and a float below 1e21 in full.
const INT_TEXT_MAX = 11;
const INT64_TEXT_MAX = 20;
const FLOAT_TEXT_MAX = 22;

// The most bytes the VM prints for a float's toFixed.
const FIXED_TEXT_MAX = 63;

const BOOLEAN_TEXT_MAX = 5;

// The most UTF-8 bytes in one JS character (a 4-byte character counts as two).
const BYTES_PER_UNIT = 3;

/** The most bytes a string buffer holds. */
export const STRING_CAP_MAX = 32767;

const WRITE_MAX = ENGINE_STRING_MAX.ER_TEXT_MAX;

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** The most bytes a string can be, and whether it can only hold ASCII. */
interface Bound {
  bytes: number;
  ascii: boolean;
}

/** A write into a string buffer. */
interface Write {
  text: IrText;
  location: SourcePosition | undefined;
}

/** A string buffer: a string state, a list's string field, or a string local. */
interface StringBuffer {
  /** What it is, for errors. */
  what: string;
  initial: string[];
  initialAt: SourcePosition | undefined;
  writes: Write[];
  bound: Bound;
  /** A write reads it back and adds to it, so it takes the default cap. */
  isSelfBuilt: boolean;
}

/** One string a write reads, and whether the write adds to it. */
interface Read {
  buffer: StringBuffer;
  adds: boolean;
}

/** Each string buffer's cap: the most bytes it holds, not counting its terminator. */
export interface StringCaps {
  state(state: ScalarState): number;
  field(list: ListState, key: string): number;
  local(name: string): number;
  /** The most bytes a value can be, for a buffer it is copied into whole. */
  value(expression: IrExpr): number;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

const byteLength = (text: string): number => Buffer.byteLength(text, 'utf8');

const literalBound = (text: string): Bound => ({
  bytes: byteLength(text),
  ascii: /^[\x00-\x7f]*$/.test(text),
});

/** What a string the compiler can't follow can be: anything a write holds. */
const UNKNOWN: Bound = {bytes: WRITE_MAX, ascii: false};

const widest = (bounds: Bound[]): Bound =>
  bounds.reduce(
    (widened, bound) => ({
      bytes: Math.max(widened.bytes, bound.bytes),
      ascii: widened.ascii && bound.ascii,
    }),
    {bytes: 0, ascii: true},
  );

/**
 * The string literals in an expression, as one bound: what a boot value's string can be, since nothing else
 * reaches it at boot.
 *
 * @param expression  The value.
 *
 * @returns The bound.
 */
function literalsBound(expression: IrExpr): Bound {
  const bounds: Bound[] = [];
  const walk = (value: unknown): void => {
    if (!value || typeof value !== 'object') {
      return;
    }
    const node = value as {kind?: string; value?: unknown};
    if (node.kind === 'string' && typeof node.value === 'string') {
      bounds.push(literalBound(node.value));
      return;
    }
    Object.values(node).forEach(walk);
  };
  walk(expression);
  return widest(bounds);
}

/**
 * The most bytes a boot value's string can be: its longest literal.
 *
 * @param expression  The value.
 *
 * @returns The bytes, at least 1.
 */
export const longestLiteral = (expression: IrExpr): number =>
  Math.max(literalsBound(expression).bytes, 1);

/**
 * The most JS characters a slice keeps, whatever it cuts: null when that depends on how long the string is.
 *
 * @param edit  The slice.
 *
 * @returns The characters, or null.
 */
function sliceLength(edit: IrTextEdit & {kind: 'slice'}): number | null {
  const start = edit.start.kind === 'number' ? edit.start.value : null;
  const end =
    edit.end.kind === 'number' && edit.end.value !== TEXT_END
      ? edit.end.value
      : null;
  const lengths: number[] = [];
  if (start !== null && start < 0) {
    lengths.push(-start);
  }
  if (end !== null && end >= 0) {
    lengths.push(Math.max(0, end - Math.max(start ?? 0, 0)));
  }
  if (start !== null && end !== null && start < 0 && end < 0) {
    lengths.push(Math.max(0, end - start));
  }
  return lengths.length ? Math.min(...lengths) : null;
}

/** Whether a piece's edits cut it to a fixed length, so what it prints no longer depends on its value's. */
const isCutShort = (part: IrTextPart): boolean =>
  part.kind !== 'literal' &&
  (part.edits ?? []).some(
    edit => edit.kind === 'slice' && sliceLength(edit) !== null,
  );

/**
 * A bound after a string helper.
 *
 * @param bound  The piece's bound.
 * @param edit  The helper.
 *
 * @returns The bound.
 */
function editedBound(bound: Bound, edit: IrTextEdit): Bound {
  if (edit.kind === 'pad') {
    // A character can take several bytes, so padding non-ASCII text adds up to the whole width.
    return {
      bytes: bound.ascii
        ? Math.max(bound.bytes, edit.width)
        : bound.bytes + edit.width,
      ascii: bound.ascii,
    };
  }
  if (edit.kind === 'slice') {
    const length = sliceLength(edit);
    if (length !== null) {
      return {
        bytes: Math.min(
          bound.bytes,
          bound.ascii ? length : length * BYTES_PER_UNIT,
        ),
        ascii: bound.ascii,
      };
    }
  }
  return bound;
}

/**
 * Calls `visit` on every statement in the app, wherever it is.
 *
 * @param app  The app.
 * @param visit  Called on each statement.
 */
function eachStatement(app: IrApp, visit: (statement: IrStmt) => void): void {
  const seen = new Set<object>();
  const walk = (value: unknown): void => {
    if (!value || typeof value !== 'object' || seen.has(value)) {
      return;
    }
    seen.add(value);
    if (Array.isArray(value)) {
      value.forEach(walk);
      return;
    }

    const node = value as Record<string, unknown> & {kind?: string};
    if (
      node.kind === 'setStateText' ||
      node.kind === 'appendItem' ||
      node.kind === 'setRow' ||
      node.kind === 'splitItems' ||
      node.kind === 'declareLocal' ||
      node.kind === 'assignLocal'
    ) {
      visit(node as unknown as IrStmt);
    }
    for (const [key, child] of Object.entries(node)) {
      if (!SKIPPED_KEYS.has(key)) {
        walk(child);
      }
    }
  };

  const {output} = app;
  walk([
    nodesOf(app.root),
    output.handlers,
    output.timerFunctions,
    output.nativeEventHandlers,
    output.effectFunctions,
    output.mountEffects,
    output.dependencyEffects,
    output.animationCallbacks,
    output.panResponders,
    output.vectors,
    app.boot,
    app.state.map(state =>
      state.kind === 'list' ? state.derived : state.worked,
    ),
  ]);
}

/**
 * Pins an error to a source position, unless it has one.
 *
 * @param error  The error.
 * @param location  The position.
 *
 * @returns The error.
 */
function located(
  error: AotError,
  location: SourcePosition | undefined,
): AotError {
  if (location && !error.aotLoc) {
    error.aotLoc = location;
  }
  return error;
}

/**
 * Works out every string buffer's cap from the values that reach it, and refuses a constant too long for a
 * string built from itself.
 *
 * @param app  The app.
 * @param interned  The interned string states, each with its values.
 * @param defaultCap  The cap of a string built from itself.
 *
 * @returns The caps.
 */
export function stringCaps(
  app: IrApp,
  interned: ReadonlyMap<ScalarState, readonly string[]>,
  defaultCap: number,
): StringCaps {
  const newBuffer = (
    what: string,
    initial: string[],
    initialAt: SourcePosition | undefined,
  ): StringBuffer => ({
    what,
    initial,
    initialAt,
    writes: [],
    bound: widest(initial.map(literalBound)),
    isSelfBuilt: false,
  });

  const states = new Map<ScalarState, StringBuffer>();
  const fields = new Map<ListState, Map<string, StringBuffer>>();
  const locals = new Map<string, StringBuffer>();
  for (const state of app.state) {
    if (state.kind === 'list') {
      fields.set(
        state,
        new Map(
          state.struct.fields
            .filter(field => field.kind === 'string')
            .map(field => [
              field.key,
              newBuffer(
                `field "${field.key}" of list state "${state.name}"`,
                state.items.map(item => {
                  const {present, value} = itemValueAt(item, field);
                  return present ? String(value) : '';
                }),
                state.initialAt,
              ),
            ]),
        ),
      );
    } else if (state.slotKind === 'string' && !interned.has(state)) {
      states.set(
        state,
        newBuffer(
          `state "${state.name}"`,
          [String(state.initial)],
          state.initialAt,
        ),
      );
    }
  }

  eachStatement(app, statement => {
    const {location} = statement;
    if (statement.kind === 'setStateText') {
      states
        .get(statement.state)
        ?.writes.push({text: statement.text, location});
    } else if (statement.kind === 'appendItem' || statement.kind === 'setRow') {
      for (const itemValue of statement.values) {
        const buffer = fields.get(statement.list)?.get(itemValue.field.key);
        buffer?.writes.push({
          text:
            'text' in itemValue
              ? itemValue.text
              : {parts: [{kind: 'value', value: itemValue.value}]},
          location,
        });
      }
    } else if (statement.kind === 'splitItems') {
      // A piece is at most the whole string.
      const [field] = statement.list.struct.fields;
      fields
        .get(statement.list)
        ?.get(field.key)
        ?.writes.push({
          text: {parts: [{kind: 'value', value: statement.text}]},
          location,
        });
    } else if (
      (statement.kind === 'declareLocal' &&
        statement.initializer.type.kind === 'string') ||
      (statement.kind === 'assignLocal' &&
        statement.value.type.kind === 'string')
    ) {
      if (!locals.has(statement.name)) {
        locals.set(
          statement.name,
          newBuffer(`local "${statement.name}"`, [], undefined),
        );
      }
      const value =
        statement.kind === 'declareLocal'
          ? statement.initializer
          : statement.value;
      locals.get(statement.name)!.writes.push({
        text: statement.text ?? {parts: [{kind: 'value', value}]},
        location,
      });
    }
  });

  const bufferOf = (expression: IrExpr): StringBuffer | undefined => {
    switch (expression.kind) {
      case 'state':
        return states.get(expression.state);
      case 'local':
        return expression.local.name !== undefined
          ? locals.get(expression.local.name)
          : undefined;
      case 'itemField':
        return expression.item.list
          ? fields.get(expression.item.list)?.get(expression.field.key)
          : undefined;
      default:
        return undefined;
    }
  };

  const valueBound = (expression: IrExpr): Bound => {
    if (expression.kind === 'string') {
      return literalBound(expression.value);
    }
    if (expression.kind === 'state' && interned.has(expression.state)) {
      return widest(interned.get(expression.state)!.map(literalBound));
    }
    if (expression.kind === 'local' && expression.local.expression) {
      return valueBound(expression.local.expression);
    }
    if (expression.kind === 'boot') {
      return expression.slot.initializer.kind === 'expr'
        ? literalsBound(expression.slot.initializer.expression)
        : UNKNOWN;
    }
    if (expression.kind === 'conditional') {
      return widest([
        valueBound(expression.consequent),
        valueBound(expression.alternate),
      ]);
    }
    // A <TextInput>'s text, or a value the compiler can't follow.
    return bufferOf(expression)?.bound ?? UNKNOWN;
  };

  const partBound = (part: IrTextPart): Bound => {
    if (part.kind === 'literal') {
      return literalBound(part.text);
    }
    let bound: Bound;
    if (part.kind === 'booleanWord') {
      bound = {bytes: BOOLEAN_TEXT_MAX, ascii: true};
    } else {
      // A whole number's toFixed prints its decimals after it.
      const decimals = part.digits ? part.digits + 1 : 0;
      const type = part.value.type.kind;
      bound =
        type === 'string'
          ? valueBound(part.value)
          : {
              bytes:
                type === 'float'
                  ? Math.min(FIXED_TEXT_MAX, FLOAT_TEXT_MAX + decimals)
                  : (type === 'int64' ? INT64_TEXT_MAX : INT_TEXT_MAX) +
                    decimals,
              ascii: true,
            };
    }
    bound = (part.edits ?? []).reduce(editedBound, bound);
    const read = itemReadOf(part.value);
    return part.missing && (read?.field.optional || read?.item.guarded)
      ? widest([bound, literalBound(part.missing)])
      : bound;
  };

  const writtenBound = (text: IrText): Bound => {
    const bounds = text.parts.map(partBound);
    return {
      bytes: Math.min(
        WRITE_MAX,
        bounds.reduce((total, bound) => total + bound.bytes, 0),
      ),
      ascii: bounds.every(bound => bound.ascii),
    };
  };

  // The strings a value reads, through choices and the values locals stand for.
  const readsOf = (expression: IrExpr, found: StringBuffer[]): void => {
    if (expression.kind === 'conditional') {
      readsOf(expression.consequent, found);
      readsOf(expression.alternate, found);
    } else if (expression.kind === 'local' && expression.local.expression) {
      readsOf(expression.local.expression, found);
    } else {
      const buffer = bufferOf(expression);
      if (buffer) {
        found.push(buffer);
      }
    }
  };

  // The strings each buffer's writes read, unless a slice cuts them to a fixed length.
  const buffers = [
    ...states.values(),
    ...[...fields.values()].flatMap(byKey => [...byKey.values()]),
    ...locals.values(),
  ];
  const reads = new Map<StringBuffer, Read[]>();
  for (const buffer of buffers) {
    const found: Read[] = [];
    for (const {text} of buffer.writes) {
      text.parts.forEach((part, index) => {
        if (
          part.kind !== 'value' ||
          part.value.type.kind !== 'string' ||
          isCutShort(part)
        ) {
          return;
        }
        // A pad's width already bounds it, so only another part adds.
        const adds = text.parts.some(
          (other, otherIndex) =>
            otherIndex !== index &&
            !(other.kind === 'literal' && other.text === ''),
        );
        const read: StringBuffer[] = [];
        readsOf(part.value, read);
        found.push(...read.map(readBuffer => ({buffer: readBuffer, adds})));
      });
    }
    reads.set(buffer, found);
  }

  // A buffer is built from itself when it is in a cycle of reads that adds to it (Tarjan's strongly connected
  // components).
  const order = new Map<StringBuffer, number>();
  const lowest = new Map<StringBuffer, number>();
  const stack: StringBuffer[] = [];
  const onStack = new Set<StringBuffer>();
  const visit = (buffer: StringBuffer): void => {
    order.set(buffer, order.size);
    lowest.set(buffer, order.get(buffer)!);
    stack.push(buffer);
    onStack.add(buffer);
    for (const read of reads.get(buffer)!) {
      if (!order.has(read.buffer)) {
        visit(read.buffer);
        lowest.set(
          buffer,
          Math.min(lowest.get(buffer)!, lowest.get(read.buffer)!),
        );
      } else if (onStack.has(read.buffer)) {
        lowest.set(
          buffer,
          Math.min(lowest.get(buffer)!, order.get(read.buffer)!),
        );
      }
    }
    if (lowest.get(buffer) !== order.get(buffer)) {
      return;
    }

    const component = new Set<StringBuffer>();
    let member: StringBuffer;
    do {
      member = stack.pop()!;
      onStack.delete(member);
      component.add(member);
    } while (member !== buffer);
    const adds = [...component].some(inCycle =>
      reads.get(inCycle)!.some(read => read.adds && component.has(read.buffer)),
    );
    if (adds) {
      for (const inCycle of component) {
        inCycle.isSelfBuilt = true;
        inCycle.bound = {bytes: defaultCap, ascii: false};
      }
    }
  };
  for (const buffer of buffers) {
    if (!order.has(buffer)) {
      visit(buffer);
    }
  }

  // Each bound only grows, and a write holds at most WRITE_MAX bytes, so this settles.
  let hasChanged = true;
  while (hasChanged) {
    hasChanged = false;
    for (const buffer of buffers) {
      if (buffer.isSelfBuilt) {
        continue;
      }
      const next = widest([
        buffer.bound,
        ...buffer.writes.map(write => writtenBound(write.text)),
      ]);
      if (
        next.bytes !== buffer.bound.bytes ||
        next.ascii !== buffer.bound.ascii
      ) {
        buffer.bound = next;
        hasChanged = true;
      }
    }
  }

  checkFits(buffers, defaultCap);

  // The package verifier refuses a string cap of 0.
  const capOf = (buffer: StringBuffer | undefined): number =>
    Math.max(1, buffer ? buffer.bound.bytes : WRITE_MAX);
  return {
    state: state => capOf(states.get(state)),
    field: (list, key) => capOf(fields.get(list)?.get(key)),
    local: name => capOf(locals.get(name)),
    value: expression =>
      Math.max(1, Math.min(STRING_CAP_MAX, valueBound(expression).bytes)),
  };
}

/**
 * Refuses a constant too long for a string built from itself, and a string too long for any buffer.
 *
 * @param buffers  The string buffers, their bounds settled.
 * @param defaultCap  The cap of a string built from itself.
 */
function checkFits(buffers: StringBuffer[], defaultCap: number): void {
  const hint =
    'raise ER_AOT_LIST_STR_CAP (the listStrCap option), shorten the string, or keep it to a fixed length ' +
    "with slice, e.g. setLog(log.slice(-39) + '.').";
  for (const buffer of buffers) {
    if (!buffer.isSelfBuilt) {
      if (buffer.bound.bytes > STRING_CAP_MAX) {
        throw located(
          aotError(
            `AOT: ${buffer.what} starts ${buffer.bound.bytes} bytes long, but a string holds ${STRING_CAP_MAX} bytes`,
            'shorten the string.',
          ),
          buffer.initialAt,
        );
      }
      continue;
    }

    const initial = buffer.initial.find(text => byteLength(text) > defaultCap);
    if (initial !== undefined) {
      throw located(
        aotError(
          `AOT: ${buffer.what} starts ${byteLength(initial)} bytes long, but a string built from itself holds ` +
            `${defaultCap} bytes`,
          hint,
        ),
        buffer.initialAt,
      );
    }
    for (const write of buffer.writes) {
      const constant = constantText(write.text);
      if (constant !== null && byteLength(constant) > defaultCap) {
        throw located(
          aotError(
            `AOT: ${buffer.what} is set to a ${byteLength(constant)}-byte string, but a string built from itself ` +
              `holds ${defaultCap} bytes`,
            hint,
          ),
          write.location,
        );
      }
    }
  }
}
