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

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import type {ListState, Local, StateRecord} from '../types.mts';
import type {IrStmt} from './statements.mts';
import type {IrExpr} from './expressions.mts';
import type {IrNode, IrPropValue, IrShow} from './nodes.mts';
import type {IrSource} from './source.mts';
import type {IrText} from './text.mts';
import type {
  IrPaintColor,
  IrStateDrivenVector,
  IrTapeValue,
} from './vectors.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * The state a node's props or a drawing read, or null when they also read something a state write does not
 * flag (a ref, the clock), so the update sets them again every time it runs, as a React re-render would.
 */
export type StateReads = Set<StateRecord> | null;

/** What a walk has found so far. */
interface ReadsFound {
  states: Set<StateRecord>;
  untracked: boolean;
  /** A value worked out at boot, which never changes after it. */
  boot?: boolean;
  /** The filtered and sorted lists walked so far, whose rebuilds read their own rows. */
  derived?: Set<ListState>;
  /** The values worked out with a loop walked so far. */
  worked?: Set<StateRecord>;
  /** The locals a worked value's statements declare, which read only what they are set from. */
  internal?: Set<string>;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Records what a local reads: a list row's item reads its list, and any other local reads what it was set from.
 * A local with neither is untracked.
 *
 * @param local  The local.
 * @param found  What the walk has found.
 */
function readLocal(local: Local, found: ReadsFound): void {
  if (local.visit || (local.name && found.internal?.has(local.name))) return;
  if (local.list) {
    readList(local.list, found);
    if (local.through) {
      readList(local.through, found);
    }
    if (local.index) {
      readExpr(local.index, found);
    }
  } else if (local.source) {
    readSource(local.source, found);
  } else {
    found.untracked = true;
  }
}

/**
 * Records what reading a list depends on. A constant array's rows never change; a list worked out in render is
 * rebuilt from what its source, if it has one, and its tests read, which a state write flags.
 *
 * @param list  The list.
 * @param found  What the walk has found.
 */
function readList(list: ListState, found: ReadsFound): void {
  if (list.derived) {
    found.derived ??= new Set();
    if (found.derived.has(list)) return;
    found.derived.add(list);
    if (list.derived.source) {
      readList(list.derived.source, found);
    }
    list.derived.tests.forEach(test => readExpr(test, found));
    return;
  }
  if (!list.constant) {
    found.states.add(list);
  }
}

/**
 * Records what reading a state depends on: a value worked out with a loop is worked out again from what its
 * statements read, which a state write flags.
 *
 * @param state  The state.
 * @param found  What the walk has found.
 */
function readState(state: StateRecord, found: ReadsFound): void {
  if (state.kind === 'list' || !state.worked) {
    found.states.add(state);
    return;
  }
  found.worked ??= new Set();
  if (found.worked.has(state)) return;
  found.worked.add(state);
  const {rebuild} = state.worked;
  found.internal ??= new Set();
  declaredLocals(rebuild, found.internal);
  readStatements(rebuild, found);
}

/**
 * Collects the names of the locals statements declare, at any depth, loops' visit numbers included.
 *
 * @param statements  The statements.
 * @param names  Collects the names.
 */
function declaredLocals(statements: IrStmt[], names: Set<string>): void {
  for (const statement of statements) {
    if (statement.kind === 'declareLocal') {
      names.add(statement.name);
    } else if (statement.kind === 'if') {
      declaredLocals(statement.consequent, names);
      declaredLocals(statement.alternate ?? [], names);
    } else if (statement.kind === 'block') {
      declaredLocals(statement.body, names);
    } else if (statement.kind === 'loop') {
      if (statement.visit?.name) names.add(statement.visit.name);
      declaredLocals(statement.body, names);
      declaredLocals(statement.step, names);
    }
  }
}

/**
 * Records what the statements a value is worked out with read.
 *
 * @param statements  The statements.
 * @param found  What the walk has found.
 */
function readStatements(statements: IrStmt[], found: ReadsFound): void {
  for (const statement of statements) {
    switch (statement.kind) {
      case 'declareLocal':
        readExpr(statement.initializer, found);
        if (statement.text) readText(statement.text, found);
        break;
      case 'assignLocal':
        readExpr(statement.value, found);
        if (statement.text) readText(statement.text, found);
        break;
      case 'setState':
        readExpr(statement.value, found);
        break;
      case 'setStateText':
        readText(statement.text, found);
        break;
      case 'if':
        readExpr(statement.test, found);
        readStatements(statement.consequent, found);
        readStatements(statement.alternate ?? [], found);
        break;
      case 'block':
        readStatements(statement.body, found);
        break;
      case 'loop':
        if (statement.test) readExpr(statement.test, found);
        readStatements(statement.body, found);
        readStatements(statement.step, found);
        break;
    }
  }
}

/**
 * Records what an expression, before typing, reads.
 *
 * @param source  The expression.
 * @param found  What the walk has found.
 */
function readSource(source: IrSource, found: ReadsFound): void {
  switch (source.kind) {
    case 'number':
    case 'string':
    case 'boolean':
      return;
    case 'state':
      readState(source.state, found);
      return;
    case 'local':
      readLocal(source.local, found);
      return;
    case 'ref':
      found.untracked = true;
      return;
    case 'fixed':
      readExpr(source.expression, found);
      return;
    case 'unary':
      readSource(source.operand, found);
      return;
    case 'binary':
    case 'logical':
      readSource(source.left, found);
      readSource(source.right, found);
      return;
    case 'conditional':
      readSource(source.test, found);
      readSource(source.consequent, found);
      readSource(source.alternate, found);
      return;
    case 'math':
      source.callArguments.forEach(argument => readSource(argument, found));
      return;
  }
}

/**
 * Records what an expression reads.
 *
 * @param expression  The expression.
 * @param found  What the walk has found.
 */
function readExpr(expression: IrExpr, found: ReadsFound): void {
  switch (expression.kind) {
    case 'number':
    case 'string':
    case 'boolean':
    case 'pi':
      return;
    case 'state':
      readState(expression.state, found);
      return;
    case 'boot':
      found.boot = true;
      return;
    case 'local':
      readLocal(expression.local, found);
      return;
    case 'listLength':
      readList(expression.list, found);
      return;
    case 'itemField':
    case 'itemPresent':
      readLocal(expression.item, found);
      return;
    case 'ref':
    case 'clock':
    case 'event':
    case 'layout':
    case 'gesture':
      found.untracked = true;
      return;
    case 'toFloat':
    case 'unary':
    case 'wholeRound':
    case 'trunc':
    case 'toInt':
      readExpr(expression.operand, found);
      return;
    case 'helper':
    case 'floatMath':
      expression.callArguments.forEach(argument => readExpr(argument, found));
      return;
    case 'arithmetic':
    case 'floatDivide':
    case 'narrowRemainder':
    case 'compare':
    case 'stringEquals':
    case 'logical':
    case 'pick':
      readExpr(expression.left, found);
      readExpr(expression.right, found);
      return;
    case 'stringOrder':
      readExpr(expression.left, found);
      readExpr(expression.right, found);
      return;
    case 'stringIncludes':
      readExpr(expression.haystack, found);
      readExpr(expression.needle, found);
      return;
    case 'conditional':
      readExpr(expression.test, found);
      readExpr(expression.consequent, found);
      readExpr(expression.alternate, found);
      return;
  }
}

/**
 * Records what a conditional's show condition reads, the conditions it sits within included.
 *
 * @param show  The condition.
 * @param found  What the walk has found.
 */
function readShow(show: IrShow, found: ReadsFound): void {
  if (show.kind === 'rowInUse') {
    readList(show.list, found);
    return;
  }

  readExpr(show.test, found);
  if (show.within) {
    readShow(show.within, found);
  }
}

/**
 * Records what a state-driven prop value reads.
 *
 * @param value  The value.
 * @param found  What the walk has found.
 */
function readPropValue(value: IrPropValue, found: ReadsFound): void {
  switch (value.kind) {
    case 'constant':
      return;
    case 'select':
      readExpr(value.test, found);
      readPropValue(value.whenTrue, found);
      readPropValue(value.whenFalse, found);
      return;
    case 'opacity':
    case 'dimension':
    case 'visible':
    case 'float':
      readExpr(value.value, found);
      return;
    case 'shown':
      readShow(value.show, found);
      return;
    case 'whileShown':
      readShow(value.show, found);
      readPropValue(value.value, found);
      return;
    case 'flag':
      readExpr(value.test, found);
      return;
    case 'byte':
      readPropValue(value.value, found);
      return;
  }
}

/**
 * Records what a text reads.
 *
 * @param text  The text.
 * @param found  What the walk has found.
 */
function readText(text: IrText, found: ReadsFound): void {
  for (const part of text.parts) {
    if (part.kind !== 'literal') {
      readExpr(part.value, found);
      for (const edit of part.edits ?? []) {
        if (edit.kind === 'slice') {
          readExpr(edit.start, found);
          readExpr(edit.end, found);
        }
      }
    }
  }
}

/**
 * Records what one slot of a drawing's op-tape reads.
 *
 * @param value  The slot.
 * @param found  What the walk has found.
 */
function readTapeValue(value: IrTapeValue, found: ReadsFound): void {
  switch (value.kind) {
    case 'op':
    case 'number':
    case 'local':
    case 'fullTurn':
      return;
    case 'float':
      readExpr(value.value, found);
      return;
    case 'chain':
      readTapeValue(value.first, found);
      value.rest.forEach(step => readTapeValue(step.value, found));
      return;
    case 'radians':
      readTapeValue(value.degrees, found);
      return;
    case 'clampRadius':
      readTapeValue(value.radius, found);
      readTapeValue(value.side, found);
      return;
  }
}

/**
 * Records what a paint color reads.
 *
 * @param color  The color.
 * @param found  What the walk has found.
 */
function readPaintColor(color: IrPaintColor, found: ReadsFound): void {
  if (color.kind === 'state') {
    readPropValue(color.value, found);
  }
}

/**
 * What a walk found, as reads.
 *
 * @param found  What the walk found.
 *
 * @returns The state read, or null when something untracked is read too.
 */
const readsOf = (found: ReadsFound): StateReads =>
  found.untracked ? null : found.states;

/**
 * The state a node's props, text and image read. the update sets them all again together, so this is
 * everything any of them reads.
 *
 * @param node  The node.
 *
 * @returns Its reads.
 */
export const nodeReads = (node: IrNode): StateReads => readsOf(nodeFound(node));

/**
 * The state an expression reads.
 *
 * @param expression  The expression.
 *
 * @returns Its reads.
 */
export function expressionReads(expression: IrExpr): StateReads {
  const found: ReadsFound = {states: new Set(), untracked: false};
  readExpr(expression, found);
  return readsOf(found);
}

/**
 * What a filtered or sorted list's rebuild reads.
 *
 * @param list  The list.
 *
 * @returns Its reads.
 */
export function listReads(list: ListState): StateReads {
  const found: ReadsFound = {states: new Set(), untracked: false};
  readList(list, found);
  return readsOf(found);
}

/**
 * The state a filtered or sorted list's rebuild reads, even beside something untracked.
 *
 * @param list  The list.
 *
 * @returns The state.
 */
export function listStates(list: ListState): Set<StateRecord> {
  const found: ReadsFound = {states: new Set(), untracked: false};
  readList(list, found);
  return found.states;
}

/**
 * What the update reads to work out a value a memo or a helper loops for in render.
 *
 * @param state  The value's state.
 *
 * @returns Its reads.
 */
export function workedReads(state: StateRecord): StateReads {
  const found: ReadsFound = {states: new Set(), untracked: false};
  readState(state, found);
  return readsOf(found);
}

/**
 * The state the update reads to work out a value a memo or a helper loops for in render, even beside something
 * untracked.
 *
 * @param state  The value's state.
 *
 * @returns The state.
 */
export function workedStates(state: StateRecord): Set<StateRecord> {
  const found: ReadsFound = {states: new Set(), untracked: false};
  readState(state, found);
  return found.states;
}

/**
 * Whether a node's runtime props read only values worked out at boot, so the build sets them once.
 *
 * @param node  The node.
 *
 * @returns True when they do.
 */
export const isBootOnlyNode = (node: IrNode): boolean =>
  isBootOnly(nodeFound(node));

/**
 * Whether a drawing reads only values worked out at boot, so the build builds it once.
 *
 * @param vector  The drawing.
 *
 * @returns True when it does.
 */
export const isBootOnlyVector = (vector: IrStateDrivenVector): boolean =>
  isBootOnly(vectorFound(vector));

/**
 * Whether a walk found boot values and nothing that changes.
 *
 * @param found  What the walk found.
 *
 * @returns True when it did.
 */
const isBootOnly = (found: ReadsFound): boolean =>
  !!found.boot && !found.untracked && found.states.size === 0;

/**
 * Walks everything a node's props, text and image read.
 *
 * @param node  The node.
 *
 * @returns What the walk found.
 */
function nodeFound(node: IrNode): ReadsFound {
  const found: ReadsFound = {states: new Set(), untracked: false};
  node.stateProps.forEach(prop => readPropValue(prop.value, found));
  if (node.text) {
    readText(node.text, found);
  }

  if (node.image?.kind === 'value') {
    readExpr(node.image.value, found);
  }

  return found;
}

/**
 * The state a drawing reads: its locals and tape, and its paints and gradients.
 *
 * @param vector  The drawing.
 *
 * @returns Its reads.
 */
export const vectorReads = (vector: IrStateDrivenVector): StateReads =>
  readsOf(vectorFound(vector));

/**
 * Walks everything a drawing reads.
 *
 * @param vector  The drawing.
 *
 * @returns What the walk found.
 */
function vectorFound(vector: IrStateDrivenVector): ReadsFound {
  const found: ReadsFound = {states: new Set(), untracked: false};
  vector.locals.forEach(local => readTapeValue(local.value, found));
  vector.tape.forEach(value => readTapeValue(value, found));
  for (const paint of vector.paints) {
    readPaintColor(paint.fill, found);
    readPaintColor(paint.stroke, found);
    readTapeValue(paint.strokeWidth, found);
    for (const gradient of [paint.fillGradient, paint.strokeGradient]) {
      if (gradient.when) {
        readExpr(gradient.when.test, found);
      }
    }
  }
  for (const gradient of vector.gradients) {
    for (const stop of gradient.stops) {
      readPaintColor(stop.color, found);
      readTapeValue(stop.offset, found);
    }
    for (const value of [
      gradient.ax,
      gradient.ay,
      gradient.bx,
      gradient.by,
      gradient.r,
    ]) {
      readTapeValue(value, found);
    }
  }

  return found;
}
