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

import type {CExpr, CType, RefRecord, ScalarState} from '../types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** Where a 64-bit whole number comes from, which decides how it mixes with other numbers. */
export type Int64Source =
  /** A time: a clock read, or a value computed from one. Only whole-number math keeps it exact. */
  | 'time'
  /**
   * A 64-bit number that is not a time: a constant past the int range, or a slot that only ever holds such
   * numbers. It converts like any other number.
   */
  | 'number'
  /** Whole-number math worked out in 64 bits beside a time, so it cannot overflow. */
  | 'math';

/** What an int slot widened to 64 bits holds: a time, or only 64-bit numbers that are not times. */
export type WideSlot = 'time' | 'number';

/** The slots widened to 64 bits, by C name. */
export type SlotWidths = Map<string, WideSlot>;

/** The type of a value in the IR: what JS holds, whatever slot a backend stores it in. */
export type IrType =
  | {kind: 'bool'}
  | {kind: 'int'}
  | {kind: 'float'}
  | {kind: 'string'}
  | {kind: 'int64'; source: Int64Source}
  /** A list row's item, read only through its fields. */
  | {kind: 'struct'}
  /** A node ref's handle. */
  | {kind: 'node'};

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

export const BOOL: IrType = {kind: 'bool'};
export const INT: IrType = {kind: 'int'};
export const FLOAT: IrType = {kind: 'float'};
export const STRING: IrType = {kind: 'string'};
export const TIME: IrType = {kind: 'int64', source: 'time'};
export const INT64_NUMBER: IrType = {kind: 'int64', source: 'number'};
export const INT64_MATH: IrType = {kind: 'int64', source: 'math'};
export const STRUCT: IrType = {kind: 'struct'};
export const NODE: IrType = {kind: 'node'};

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * The slot a value of a type is stored in: a boolean in an int, every 64-bit number in an int64. A list row's
 * item has none: it is read through its fields.
 *
 * @param type  The type.
 *
 * @returns The slot's kind, or undefined for a list row's item.
 */
export function slotOf(type: IrType): CType | undefined {
  switch (type.kind) {
    case 'bool':
      return 'int';
    case 'int64':
      return 'i64';
    case 'struct':
      return undefined;
    case 'node':
      return 'ERNode*' as CType; // a handle, which no number or text slot holds
    default:
      return type.kind;
  }
}

/**
 * The type of a 64-bit number from a source.
 *
 * @param source  Where it comes from.
 *
 * @returns The type.
 */
export const int64Of = (source: Int64Source): IrType =>
  source === 'time' ? TIME : source === 'number' ? INT64_NUMBER : INT64_MATH;

/**
 * The type of a value whose slot is known: a boolean when it holds one, a time when it is a 64-bit slot.
 *
 * @param slot  The slot's kind.
 * @param isBool  The value is a JS boolean.
 *
 * @returns The type.
 */
export function typeOfSlot(slot: CType | undefined, isBool = false): IrType {
  if (isBool) {
    if (slot !== 'int') {
      throw new Error(`AOT internal: a boolean in a "${slot}" slot`);
    }
    return BOOL;
  }

  switch (slot) {
    case 'int':
      return INT;
    case 'float':
      return FLOAT;
    case 'string':
      return STRING;
    case 'i64':
      return TIME;
    case undefined:
      return STRUCT;
  }

  // Only a node ref's handle, cast to a slot, reaches here.
  return NODE;
}

/**
 * The type of a value lowered to C: its slot, and what the C form records about it.
 *
 * @param value  The lowered value.
 *
 * @returns The type.
 */
export function typeOfCExpr(value: Omit<CExpr, 'code'>): IrType {
  if (value.lit || value.wide) {
    if (value.cType !== 'i64' || (value.lit && value.wide)) {
      throw new Error(
        'AOT internal: a 64-bit source on a value that is not one 64-bit number',
      );
    }
    return value.lit ? INT64_NUMBER : INT64_MATH;
  }

  return typeOfSlot(value.cType, value.isBool);
}

/**
 * Whether a value is a time: a 64-bit number computed at runtime from a clock.
 *
 * @param type  The value's type.
 *
 * @returns True for a time.
 */
export const isTime = (type: IrType): boolean =>
  type.kind === 'int64' && type.source === 'time';

/**
 * Whether a value is a JS boolean.
 *
 * @param type  The value's type.
 *
 * @returns True for a boolean.
 */
export const isBoolean = (type: IrType): boolean => type.kind === 'bool';

/**
 * Whether a value is a 64-bit number that is not a time (a number past the int range, or whole-number math),
 * which converts to float like any other number.
 *
 * @param type  The value's type.
 *
 * @returns True for such a number.
 */
export const isConvertible64 = (type: IrType): boolean =>
  type.kind === 'int64' && type.source !== 'time';

/**
 * Whether a value is stored in an int: an int, or a boolean.
 *
 * @param type  The value's type.
 *
 * @returns True for an int slot.
 */
export const isIntSlot = (type: IrType): boolean =>
  type.kind === 'int' || type.kind === 'bool';

/**
 * The type of a 64-bit value picked from several (a ternary's branches, the operands of `&&` / `||`): a time
 * when any of them is one, a number when every 64-bit one is, whole-number math otherwise.
 *
 * @param types  The types it is picked from.
 *
 * @returns The picked value's type.
 */
export function int64Join(...types: IrType[]): IrType {
  const sources = types.flatMap(type =>
    type.kind === 'int64' ? [type.source] : [],
  );
  if (sources.includes('time')) return TIME;
  return sources.every(source => source === 'number')
    ? INT64_NUMBER
    : INT64_MATH;
}

/**
 * The type of a 64-bit slot: a time, unless it only ever holds 64-bit numbers that are not times.
 *
 * @param widened  What the slot was widened to hold, if it was.
 *
 * @returns The type of a read of the slot.
 */
const wideSlotType = (widened: WideSlot | undefined): IrType =>
  widened === 'number' ? INT64_NUMBER : TIME;

/**
 * The type of a scalar state's value.
 *
 * @param state  The state.
 *
 * @returns Its type.
 */
export const stateType = (state: ScalarState): IrType =>
  state.cType === 'i64'
    ? wideSlotType(state.widened)
    : typeOfSlot(state.cType, state.isBool);

/**
 * The type of a ref's current value.
 *
 * @param ref  The ref.
 *
 * @returns A node handle for a node ref, the value's type otherwise.
 */
export const refType = (ref: RefRecord): IrType =>
  ref.kind === 'node'
    ? NODE
    : ref.cType === 'i64'
      ? wideSlotType(ref.widened)
      : typeOfSlot(ref.cType);
