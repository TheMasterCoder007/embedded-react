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

import type {ItemField, ListState, RefRecord, ScalarState} from '../types.mts';
import type {IrExpr} from './expressions.mts';
import type {IrText} from './text.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** `const name = …`: a local that the statements after it read. */
export interface IrDeclareLocal {
  kind: 'declareLocal';
  /** The slot's name, which later reads of the local use. */
  name: string;
  init: IrExpr;
  /** Declared at file scope (a dep-driven effect's local its cleanup reads), so only assigned here. */
  isHoisted: boolean;
  /** The buffer size a string local is copied into. */
  stringCap: number;
}

/** `setInterval` / `setTimeout`: a timer whose callback is a function of its own. */
export interface IrTimer {
  /** The delay, or null for none. */
  delay: IrExpr | null;
  isRepeating: boolean;
  /** The function the callback was compiled to. */
  fn: string;
}

/** Starts a timer, keeping its id in a local when the code goes on to clear it. */
export interface IrStartTimer {
  kind: 'startTimer';
  timer: IrTimer;
  idLocal: {name: string; isHoisted: boolean} | null;
}

/** `clearInterval` / `clearTimeout`. */
export interface IrClearTimer {
  kind: 'clearTimer';
  id: IrExpr;
}

/** A setter call on a numeric scalar state. */
export interface IrSetState {
  kind: 'setState';
  state: ScalarState;
  value: IrExpr;
}

/** A setter call on a string state. */
export interface IrSetStateText {
  kind: 'setStateText';
  state: ScalarState;
  text: IrText;
}

/** One field of an appended list item: a number, or text. */
export type IrItemValue =
  | {field: ItemField; value: IrExpr}
  | {field: ItemField; text: IrText};

/** `setItems([])`. */
export interface IrClearList {
  kind: 'clearList';
  list: ListState;
}

/** `setItems([...items, item])`: one appended item, dropped when the list is full. */
export interface IrAppendItem {
  kind: 'appendItem';
  list: ListState;
  /** The fields the item sets, in struct order. */
  values: IrItemValue[];
}

/** `items.slice(0, -1)`. */
export interface IrDropLastItem {
  kind: 'dropLastItem';
  list: ListState;
}

/** `items.slice(0, count)` with a constant count. */
export interface IrKeepFirstItems {
  kind: 'keepFirstItems';
  list: ListState;
  count: number;
}

/** `items.slice(0, end)` worked out by JS's rules at runtime (a negative end counts back). */
export interface IrSliceItems {
  kind: 'sliceItems';
  list: ListState;
  end: IrExpr;
}

/** `ref.current = …` or a compound assignment to it. */
export interface IrWriteRef {
  kind: 'writeRef';
  ref: RefRecord;
  op: string;
  value: IrExpr;
}

/** `ref.current++` / `--` on a float ref. */
export interface IrStepRef {
  kind: 'stepRef';
  ref: RefRecord;
  op: '++' | '--';
}

/** `anim.stop()`: each animated value it drives freezes where it is. */
export interface IrStopAnimations {
  kind: 'stopAnimations';
  /** The animated values' handles. */
  handles: string[];
}

/** `if (test) { … } else { … }`. */
export interface IrIf {
  kind: 'if';
  test: IrExpr;
  consequent: IrStmt[];
  alternate: IrStmt[] | null;
}

/** An early `return` from an effect body. */
export interface IrReturn {
  kind: 'return';
}

/** Marks a dep-driven effect's cleanup as due before its next run. */
export interface IrArmCleanup {
  kind: 'armCleanup';
  flag: string;
}

/**
 * C from a module that does not lower to the IR yet: an `Animated` start (whose lines carry their own indent)
 * or an updateVector call (whose lines take the statement's indent).
 */
export interface IrForeign {
  kind: 'foreign';
  lines: string[];
  isIndented: boolean;
}

/** A lowered handler, effect or timer statement, before any backend prints it. */
export type IrStmt =
  | IrDeclareLocal
  | IrStartTimer
  | IrClearTimer
  | IrSetState
  | IrSetStateText
  | IrClearList
  | IrAppendItem
  | IrDropLastItem
  | IrKeepFirstItems
  | IrSliceItems
  | IrWriteRef
  | IrStepRef
  | IrStopAnimations
  | IrIf
  | IrReturn
  | IrArmCleanup
  | IrForeign;
