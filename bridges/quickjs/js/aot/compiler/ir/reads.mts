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

import type {Local, StateRecord} from '../types.mts';
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
 * flag (a ref, the clock), so app_update() sets them again every time it runs, as a React re-render would.
 */
export type StateReads = Set<StateRecord> | null;

/** What a walk has found so far. */
interface ReadsFound {
  states: Set<StateRecord>;
  untracked: boolean;
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
  if (local.list) {
    found.states.add(local.list);
  } else if (local.source) {
    readSource(local.source, found);
  } else {
    found.untracked = true;
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
      found.states.add(source.state);
      return;
    case 'local':
      readLocal(source.local, found);
      return;
    case 'ref':
      found.untracked = true;
      return;
    case 'fixed':
      readExpr(source.expr, found);
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
      source.args.forEach(arg => readSource(arg, found));
      return;
  }
}

/**
 * Records what an expression reads.
 *
 * @param expr  The expression.
 * @param found  What the walk has found.
 */
function readExpr(expr: IrExpr, found: ReadsFound): void {
  switch (expr.kind) {
    case 'number':
    case 'string':
    case 'boolean':
    case 'pi':
      return;
    case 'state':
      found.states.add(expr.state);
      return;
    case 'local':
      readLocal(expr.local, found);
      return;
    case 'listLength':
      found.states.add(expr.list);
      return;
    case 'itemField':
      readLocal(expr.item, found);
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
      readExpr(expr.operand, found);
      return;
    case 'helper':
    case 'floatMath':
      expr.args.forEach(arg => readExpr(arg, found));
      return;
    case 'arithmetic':
    case 'floatDivide':
    case 'narrowRemainder':
    case 'compare':
    case 'stringEquals':
    case 'logical':
    case 'pick':
      readExpr(expr.left, found);
      readExpr(expr.right, found);
      return;
    case 'conditional':
      readExpr(expr.test, found);
      readExpr(expr.consequent, found);
      readExpr(expr.alternate, found);
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
    found.states.add(show.list);
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
 * The state a node's props, text and image read. app_update() sets them all again together, so this is
 * everything any of them reads.
 *
 * @param node  The node.
 *
 * @returns Its reads.
 */
export function nodeReads(node: IrNode): StateReads {
  const found: ReadsFound = {states: new Set(), untracked: false};
  node.stateProps.forEach(prop => readPropValue(prop.value, found));
  if (node.text) {
    readText(node.text, found);
  }

  if (node.image?.kind === 'value') {
    readExpr(node.image.value, found);
  }

  return readsOf(found);
}

/**
 * The state a drawing reads: its locals and tape, and its paints and gradients.
 *
 * @param vector  The drawing.
 *
 * @returns Its reads.
 */
export function vectorReads(vector: IrStateDrivenVector): StateReads {
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

  return readsOf(found);
}
