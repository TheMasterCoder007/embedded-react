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
import type {Local, RefRecord, ScalarState} from '../types.mts';
import type {IrExpr} from './expressions.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** Where a source expression is in the app, for the errors its typing reports. */
interface Located {
  loc: SourcePosition | undefined;
}

/** A divisor that may have to be a constant (a time's), with where its error points. */
export interface IrSourceDivisor {
  /** Its value, when it folds to a whole number. */
  constant: number | null;
  loc: SourcePosition | undefined;
}

/**
 * A value stored into an int slot, kept so the typing pass can tell whether the slot has to hold 64 bits.
 */
export interface IrSlotStore {
  /** The slot's C name: a state's field, or a ref's variable. */
  slot: string;
  value: IrSource;
  /** The value was typed with whole-number math in 64 bits. */
  math64: boolean;
}

/**
 * An expression as the app wrote it: names resolved and constants folded, but not yet typed. The typing pass
 * decides how each operation is computed (whole numbers that saturate, 64-bit math beside a time, float math)
 * and gives every value its type.
 */
export type IrSource = Located &
  (
    | {kind: 'number'; value: number}
    | {kind: 'string'; value: string}
    | {kind: 'boolean'; value: boolean}
    | {kind: 'state'; state: ScalarState}
    | {kind: 'local'; local: Local}
    | {kind: 'ref'; ref: RefRecord}
    /** A value whose source fixes its type: an event or layout field, a clock, Math.PI, a list's count … */
    | {kind: 'fixed'; expr: IrExpr}
    | {kind: 'unary'; op: '-' | '+' | '!'; operand: IrSource}
    /** Any binary operator; one the typing pass cannot compute is refused there, after its operands. */
    | {
        kind: 'binary';
        op: string;
        left: IrSource;
        right: IrSource;
        /** The right operand as a divisor, for `%` and a time's `/`. */
        divisor: IrSourceDivisor;
        /** Each side is written as a number, which a boolean never equals in JS. */
        leftIsNumber: boolean;
        rightIsNumber: boolean;
      }
    | {kind: 'logical'; op: '&&' | '||'; left: IrSource; right: IrSource}
    | {
        kind: 'conditional';
        test: IrSource;
        consequent: IrSource;
        alternate: IrSource;
      }
    /**
     * A Math call. Math.floor / ceil / round / trunc(a / b) marks its one argument, a division, as one the
     * typing pass may compute exactly in whole numbers.
     */
    | {
        kind: 'math';
        fn: string;
        args: IrSource[];
        exactDivision: boolean;
        /** It keeps a whole number whole (floor, abs, min …), so beside a 64-bit value it is 64-bit math. */
        wholeMath: boolean;
      }
  );
