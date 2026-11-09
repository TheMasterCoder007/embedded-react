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

import type {SourcePosition} from '../diagnostics.mts';
import type {
  ItemField,
  ListState,
  Local,
  RefRecord,
  ScalarState,
} from '../types.mts';
import type {IrAnimationStart} from './animations.mts';
import type {IrExpr} from './expressions.mts';
import type {IrText} from './text.mts';
import type {IrTapeValue, IrVectorPaint} from './vectors.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** `const name = …`: a local that the statements after it read. */
export interface IrDeclareLocal {
  kind: 'declareLocal';
  /** The slot's name, which later reads of the local use. */
  name: string;
  initializer: IrExpr;
  /** A string built from parts, which it is set to instead of the initializer. */
  text?: IrText;
  /** Kept in a slot (a dep-driven effect's local its cleanup reads), so only assigned here. */
  isHoisted: boolean;
}

/** `setInterval` / `setTimeout`: a timer whose callback is a function of its own. */
export interface IrTimer {
  /** The delay, or null for none. */
  delay: IrExpr | null;
  isRepeating: boolean;
  /** The function the callback was compiled to. */
  functionName: string;
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

/** One field of an appended or updated list item: a number, or text. */
export type IrItemValue =
  {field: ItemField; value: IrExpr} | {field: ItemField; text: IrText};

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

/** Drops one row, moving the rest up: `items.filter((_, i) => i !== n)`, `items.toSpliced(n, 1)` and the like. */
export interface IrRemoveItem {
  kind: 'removeItem';
  list: ListState;
  /** The row. Past either end, nothing is removed. */
  index: IrExpr;
  /**
   * What a negative index does: nothing (`filter`), counts back from the end and no further than the first item
   * (`toSpliced`), or counts back with -1 stopping the app, since the pair of slices repeats the list there.
   */
  negative: 'none' | 'spliced' | 'sliced';
}

/**
 * Visits a list's rows and runs its body on each: `items.map(item => …)` visits every row, `items.with(n, …)`
 * just row n. The body is `if`s and `setRow`s, which read the row through a local.
 */
export interface IrUpdateItems {
  kind: 'updateItems';
  list: ListState;
  /** The local the body reads the row's index through. */
  row: Local;
  /** The one row `with` visits, a negative one counting back; null to visit every row. */
  index: IrExpr | null;
  body: IrStmt[];
  /**
   * `with` keeps fields of `items[n]`, which is undefined for a negative n, so such an n stops the app rather than
   * keep the row's.
   */
  negativeKeeps?: boolean;
}

/** Sets fields of the row an `updateItems` visits. Every value is worked out before any is stored. */
export interface IrSetRow {
  kind: 'setRow';
  list: ListState;
  values: IrItemValue[];
  /** Fields set to undefined. */
  cleared: ItemField[];
}

/** Keeps the rows a test passes, in order: `items.filter(item => …)`. The rows are tested last to first. */
export interface IrFilterItems {
  kind: 'filterItems';
  list: ListState;
  /** The local the test reads the row's index through. */
  position: Local;
  keep: IrExpr;
}

/** Sorts a list's rows in place, keeping equal ones in order: `items.toSorted((a, b) => …)`. */
export interface IrSortItems {
  kind: 'sortItems';
  list: ListState;
  /** The locals the compare function reads the two rows' indexes through. */
  first: Local;
  second: Local;
  /** Above zero when the first row goes after the second. */
  order: IrExpr;
}

/** Fills a filtered or sorted list with its source's rows, those a test passes when it has one. */
export interface IrFillOrder {
  kind: 'fillOrder';
  list: ListState;
  /** The local the test reads the source row's index through. */
  row: Local;
  keep: IrExpr | null;
}

/** `ref.current = …` or a compound assignment to it. */
export interface IrWriteRef {
  kind: 'writeRef';
  ref: RefRecord;
  operator: string;
  value: IrExpr;
}

/** `ref.current++` / `--` on a float ref. */
export interface IrStepRef {
  kind: 'stepRef';
  ref: RefRecord;
  operator: '++' | '--';
}

/** `anim.stop()`: each animated value it drives freezes where it is, and a pending completion wait ends. */
export interface IrStopAnimations {
  kind: 'stopAnimations';
  /** The animated values' handles. */
  handles: string[];
  /** The composition whose pending completion waits end, or null when it has no delay to wait on. */
  waitOwner: number | null;
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
 * `updateVector(ref, shapes, dirtyRect?)`: rewrites a vector node's op-tape in place and hands it over, the
 * imperative fast path (a drag) that bypasses the update. Its tape and paint tables are declared with it.
 */
export interface IrUpdateVector {
  kind: 'updateVector';
  /** Numbers the call's tables and its locals. */
  id: number;
  /** The node ref's slot. */
  node: string;
  locals: {name: string; value: IrTapeValue}[];
  tape: IrTapeValue[];
  /** One static paint per shape. */
  paints: IrVectorPaint[];
  /** The sub-rect that changed, x, y, width, height, or null to repaint the whole node. */
  dirty: IrExpr[] | null;
}

/** A callback lowered to statements: an event handler, a timer, or a PanResponder callback. */
export interface IrCallback {
  body: IrStmt[];
  /** A statement in the body set state, so it ends by re-applying state. */
  updatesState: boolean;
}

/** A lowered handler, effect, or timer statement, before any backend prints it, and where it is in the source. */
export type IrStmt = {location?: SourcePosition} & (
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
  | IrRemoveItem
  | IrUpdateItems
  | IrSetRow
  | IrFilterItems
  | IrSortItems
  | IrFillOrder
  | IrWriteRef
  | IrStepRef
  | IrStopAnimations
  | IrIf
  | IrReturn
  | IrArmCleanup
  | IrAnimationStart
  | IrUpdateVector
);
