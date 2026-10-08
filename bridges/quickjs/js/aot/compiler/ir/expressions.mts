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

import type {
  CExpr,
  ItemField,
  ListState,
  Local,
  RefRecord,
  ScalarState,
} from '../types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * What every expression node knows about its value: the slot it needs (int, i64, float, or string) and how JS
 * would treat it (a boolean, a 64-bit constant, or whole-number math widened to 64 bits).
 */
export type IrTyped = Omit<CExpr, 'code'>;

/** A whole-number helper that saturates where C's own arithmetic would overflow. */
export type WholeHelper =
  | 'add'
  | 'sub'
  | 'mul'
  | 'neg'
  | 'mod'
  | 'abs'
  | 'min'
  | 'max'
  | 'floordiv'
  | 'ceildiv'
  | 'rounddiv'
  | 'div';

/** A Math function worked out on floats (`mod` is JS's `%` on a float). */
export type FloatMath =
  | 'sin'
  | 'cos'
  | 'tan'
  | 'sqrt'
  | 'abs'
  | 'round'
  | 'floor'
  | 'ceil'
  | 'min'
  | 'max'
  | 'atan2'
  | 'pow'
  | 'mod';

/** A touch field of the handler's event. */
export type EventField = 'x' | 'y' | 'dx' | 'dy' | 'vx' | 'vy';

/** A field of the onLayout rect. */
export type LayoutField = 'x' | 'y' | 'width' | 'height';

/** A number known at compile time. */
export interface IrNumber extends IrTyped {
  kind: 'number';
  value: number;
}

/** A string known at compile time. */
export interface IrString extends IrTyped {
  kind: 'string';
  value: string;
}

/** A boolean known at compile time. */
export interface IrBoolean extends IrTyped {
  kind: 'boolean';
  value: boolean;
}

/** A read of a scalar useState / useHostValue. */
export interface IrState extends IrTyped {
  kind: 'state';
  state: ScalarState;
}

/** A read of a local: a handler `const`, a memo, a dynamic prop, an updater's parameter, or a list row. */
export interface IrLocal extends IrTyped {
  kind: 'local';
  local: Local;
}

/** `ref.current`. */
export interface IrRef extends IrTyped {
  kind: 'ref';
  ref: RefRecord;
}

/** `list.length` on a list state. */
export interface IrListLength extends IrTyped {
  kind: 'listLength';
  list: ListState;
}

/** `item.field` on a list row. */
export interface IrItemField extends IrTyped {
  kind: 'itemField';
  item: Local;
  field: ItemField;
}

/** A touch field of the handler's event (`e.x`, `e.vx` …). */
export interface IrEvent extends IrTyped {
  kind: 'event';
  field: EventField;
}

/** A field of the onLayout rect (`e.layout.width` …). */
export interface IrLayout extends IrTyped {
  kind: 'layout';
  field: LayoutField;
}

/** `Date.now()` or `performance.now()`: the engine clock in 64-bit whole milliseconds. */
export interface IrClock extends IrTyped {
  kind: 'clock';
  clock: 'Date' | 'performance';
}

/** `Math.PI`. */
export interface IrPi extends IrTyped {
  kind: 'pi';
}

/** C produced by a module that does not lower to the IR yet (a PanResponder gesture field). */
export interface IrForeign extends IrTyped {
  kind: 'foreign';
  code: string;
}

/** A 64-bit constant converted to float beside a float, as JS converts any number. */
export interface IrToFloat extends IrTyped {
  kind: 'toFloat';
  operand: IrExpr;
}

/** Unary `-` / `+` on a float, or `!` on any value. */
export interface IrUnary extends IrTyped {
  kind: 'unary';
  op: '-' | '+' | '!';
  operand: IrExpr;
}

/** A saturating whole-number helper, in 32 or 64 bits. */
export interface IrHelper extends IrTyped {
  kind: 'helper';
  helper: WholeHelper;
  bits: 32 | 64;
  args: IrExpr[];
}

/** Plain arithmetic that cannot overflow: a float `+ - *`, or `%` by a constant that is safe. */
export interface IrArithmetic extends IrTyped {
  kind: 'arithmetic';
  op: '+' | '-' | '*' | '%';
  left: IrExpr;
  right: IrExpr;
}

/** JS division: always a float. */
export interface IrFloatDivide extends IrTyped {
  kind: 'floatDivide';
  left: IrExpr;
  right: IrExpr;
}

/** A 64-bit remainder by a constant that fits an int, which fits an int itself. */
export interface IrNarrowRemainder extends IrTyped {
  kind: 'narrowRemainder';
  left: IrExpr;
  right: IrExpr;
}

/** A Math function worked out on floats. */
export interface IrFloatMath extends IrTyped {
  kind: 'floatMath';
  fn: FloatMath;
  args: IrExpr[];
}

/** Math.round / floor / ceil / trunc of a value that is already whole: the value itself. */
export interface IrWholeRound extends IrTyped {
  kind: 'wholeRound';
  operand: IrExpr;
}

/** Math.trunc of a float: toward zero, NaN as 0, saturating at the int range. */
export interface IrTrunc extends IrTyped {
  kind: 'trunc';
  operand: IrExpr;
}

/** A float that is already whole (a rounded result) converted to an int, saturating at the int range. */
export interface IrToInt extends IrTyped {
  kind: 'toInt';
  operand: IrExpr;
}

/** A numeric comparison or equality, yielding a boolean. */
export interface IrCompare extends IrTyped {
  kind: 'compare';
  op: '<' | '>' | '<=' | '>=' | '==' | '!=';
  left: IrExpr;
  right: IrExpr;
}

/** String equality, yielding a boolean. */
export interface IrStringEquals extends IrTyped {
  kind: 'stringEquals';
  op: '==' | '!=';
  left: IrExpr;
  right: IrExpr;
}

/** `&&` / `||` over truth values, yielding their truth. */
export interface IrLogical extends IrTyped {
  kind: 'logical';
  op: '&&' | '||';
  left: IrExpr;
  right: IrExpr;
}

/** `&&` / `||` over 64-bit values, yielding the operand JS would yield. */
export interface IrPick extends IrTyped {
  kind: 'pick';
  op: '&&' | '||';
  left: IrExpr;
  right: IrExpr;
}

/** `test ? consequent : alternate`. */
export interface IrConditional extends IrTyped {
  kind: 'conditional';
  test: IrExpr;
  consequent: IrExpr;
  alternate: IrExpr;
}

/** A lowered JS expression, before any backend prints it. */
export type IrExpr =
  | IrNumber
  | IrString
  | IrBoolean
  | IrState
  | IrLocal
  | IrRef
  | IrListLength
  | IrItemField
  | IrEvent
  | IrLayout
  | IrClock
  | IrPi
  | IrForeign
  | IrToFloat
  | IrUnary
  | IrHelper
  | IrArithmetic
  | IrFloatDivide
  | IrNarrowRemainder
  | IrFloatMath
  | IrWholeRound
  | IrTrunc
  | IrToInt
  | IrCompare
  | IrStringEquals
  | IrLogical
  | IrPick
  | IrConditional;
