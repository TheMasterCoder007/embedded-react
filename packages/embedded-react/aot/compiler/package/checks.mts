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
 * A layout's check: the program the loader runs to learn whether a layout takes the screen. It is what the compile
 * assumed of its panel, lowered to the stack machine in erb_format.h.
 *
 * The compiler runs the same program to find the layouts: exactly for one size (runCheck), and over a range of
 * sizes (checkOverSizes), which gives a verdict for all of them at once. Both follow erb_verify.c's run_check().
 */

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {
  CHECK_ALWAYS,
  CHECK_BUDGET,
  CHECK_LOCALS,
  CHECK_STACK,
  CheckDivide,
  CheckOp,
  MathFn,
} from './format.mts';
import type {IrBootAssumption, IrBootSlot} from '../ir/boot.mts';
import type {IrExpr} from '../ir/expressions.mts';
import type {Screen} from '../options.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** One step of a check. */
export interface CheckStep {
  opcode: number;
  /** The argument of a word, or the constant of a pushk. */
  operand: number;
}

/** A layout's check. */
export interface LayoutCheck {
  /** Its steps, or null when the layout takes any screen. */
  steps: CheckStep[] | null;
  /** The dimensions it reads. */
  reads: {width: boolean; height: boolean};
  /** Why it only takes the size it was compiled for, when it does: the expression boot can't work out. */
  exactBecause: string | null;
}

/** A range of sizes, both ends included. */
export interface SizeRange {
  minWidth: number;
  maxWidth: number;
  minHeight: number;
  maxHeight: number;
}

/** A check's verdict on a range of sizes. */
export type Verdict = 'pass' | 'fail' | 'unsure';

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** The most layouts a package may hold. An app that changes at more sizes fails the build instead of growing. */
export const MAX_LAYOUTS = 16;

/** What rounding adds before it floors: a half rounds up, as the VM's does. */
const ROUND_HALF_UP = 0.5;

/** The most steps a check may take, so a full package's checks always fit the loader's budget. */
const MAX_CHECK_STEPS = CHECK_BUDGET / MAX_LAYOUTS;

/** How far a fraction may stray from the build's: as c/boot.mts's guard allows, for a libm function's last digit. */
const FLOAT_TOLERANCE = 1e-9;

/** The most a pushi holds: its argument is 24 bits, signed. */
const PUSHI_MAX = 0x7fffff;

const INT_MAX = 2147483647;
const INT_MIN = -2147483648;

/** The IR's whole-number divisions, by helper name. */
const DIVISIONS: Record<string, number> = {
  div: CheckDivide.trunc,
  floordiv: CheckDivide.floor,
  ceildiv: CheckDivide.ceil,
  rounddiv: CheckDivide.round,
  mod: CheckDivide.mod,
};

/*----------------------------------------------------------------------------------------------------------------------
 - Lowering
 ---------------------------------------------------------------------------------------------------------------------*/

/** Why a value can't be a check's: the check falls back to the exact size. */
class NotCheckable extends Error {}

/**
 * Lowers what one compile assumed to its check.
 *
 * @class CheckLowering
 */
class CheckLowering {
  readonly steps: CheckStep[] = [];
  readonly reads = {width: false, height: false};
  private readonly locals = new Map<IrBootSlot, number>();
  /** A boot string is one of the app's literals, so a check compares them as numbers. '' is false, as in JS. */
  private readonly strings = new Map<string, number>([['', 0]]);
  private depth = 0;

  /**
   * Adds a step, tracking the stack it uses.
   *
   * @param opcode  The op.
   * @param operand  Its argument.
   * @param pops  What it pops.
   * @param pushes  What it pushes.
   */
  private step(
    opcode: number,
    operand: number,
    pops: number,
    pushes: number,
  ): void {
    this.depth += pushes - pops;
    // One step stays free for the end.
    if (this.depth > CHECK_STACK || this.steps.length >= MAX_CHECK_STEPS - 1) {
      throw new NotCheckable();
    }
    this.steps.push({opcode, operand});
  }

  /**
   * Pushes a number, in one word when it fits.
   *
   * @param value  The number.
   */
  number(value: number): void {
    if (
      Number.isInteger(value) &&
      Math.abs(value) <= PUSHI_MAX &&
      !Object.is(value, -0)
    ) {
      this.step(CheckOp.pushi, value, 0, 1);
    } else {
      this.step(CheckOp.pushk, value, 0, 1);
    }
  }

  /**
   * Pushes a string, as the number it is interned as.
   *
   * @param text  The string.
   */
  string(text: string): void {
    let id = this.strings.get(text);
    if (id === undefined) {
      id = this.strings.size;
      this.strings.set(text, id);
    }
    this.number(id);
  }

  /**
   * Pushes a boot slot's value. Each is worked out once, in a local.
   *
   * @param slot  The slot.
   */
  slot(slot: IrBootSlot): void {
    const {initializer} = slot;
    if (initializer.kind === 'screen') {
      this.reads[initializer.dimension] = true;
      this.step(
        initializer.dimension === 'width' ? CheckOp.width : CheckOp.height,
        0,
        0,
        1,
      );
      return;
    }

    // Whether a native module is there isn't known until the firmware loads the package.
    if (initializer.kind === 'native') {
      throw new NotCheckable();
    }
    let local = this.locals.get(slot);
    if (local === undefined) {
      local = this.locals.size;
      if (local >= CHECK_LOCALS) {
        throw new NotCheckable();
      }
      // Taken first: the expression's own slots take locals too.
      this.locals.set(slot, local);
      this.expr(initializer.expression);
      this.step(CheckOp.store, local, 1, 0);
    }
    this.step(CheckOp.load, local, 0, 1);
  }

  /**
   * Adds a two-operand step over two expressions.
   *
   * @param opcode  The op.
   * @param left  The first operand.
   * @param right  The second.
   * @param operand  Its argument.
   */
  private binary(
    opcode: number,
    left: IrExpr,
    right: IrExpr,
    operand = 0,
  ): void {
    this.expr(left);
    this.expr(right);
    this.step(opcode, operand, 2, 1);
  }

  /**
   * Pushes an expression's value. Anything but what boot can work out is refused.
   *
   * @param expression  The expression.
   */
  expr(expression: IrExpr): void {
    switch (expression.kind) {
      case 'number':
        this.number(expression.value);
        return;
      case 'string':
        this.string(expression.value);
        return;
      case 'boolean':
        this.number(expression.value ? 1 : 0);
        return;
      case 'boot':
        this.slot(expression.slot);
        return;
      case 'pi':
        this.number(Math.PI);
        return;
      case 'toFloat':
      case 'wholeRound':
        this.expr(expression.operand);
        return;
      case 'unary':
        this.expr(expression.operand);
        if (expression.operator === '-') this.step(CheckOp.neg, 0, 1, 1);
        if (expression.operator === '!') this.step(CheckOp.not, 0, 1, 1);
        return;
      case 'helper':
        this.helper(expression);
        return;
      case 'arithmetic':
        if (expression.operator === '%') {
          this.binary(
            CheckOp.math,
            expression.left,
            expression.right,
            MathFn.mod,
          );
        } else {
          const ops = {'+': CheckOp.add, '-': CheckOp.sub, '*': CheckOp.mul};
          this.binary(
            ops[expression.operator],
            expression.left,
            expression.right,
          );
        }
        return;
      case 'floatDivide':
        this.binary(CheckOp.div, expression.left, expression.right);
        return;
      case 'floatMath':
        this.math(expression.mathFunction, expression.callArguments);
        return;
      case 'trunc':
      case 'toInt':
        this.expr(expression.operand);
        this.step(CheckOp.toint, 0, 1, 1);
        return;
      case 'compare': {
        const ops = {
          '<': CheckOp.lt,
          '>': CheckOp.gt,
          '<=': CheckOp.le,
          '>=': CheckOp.ge,
          '==': CheckOp.eq,
          '!=': CheckOp.ne,
        };
        this.binary(
          ops[expression.operator],
          expression.left,
          expression.right,
        );
        return;
      }
      case 'stringEquals':
        this.binary(
          expression.operator === '==' ? CheckOp.eq : CheckOp.ne,
          expression.left,
          expression.right,
        );
        return;
      case 'logical':
        this.binary(
          expression.operator === '&&' ? CheckOp.and : CheckOp.or,
          expression.left,
          expression.right,
        );
        return;
      case 'conditional':
        this.expr(expression.test);
        this.expr(expression.consequent);
        this.expr(expression.alternate);
        this.step(CheckOp.select, 0, 3, 1);
        return;
      default:
        throw new NotCheckable();
    }
  }

  /**
   * Pushes a Math function's value.
   *
   * @param mathFunction  The function's name.
   * @param callArguments  Its operands.
   */
  private math(
    mathFunction: keyof typeof MathFn,
    callArguments: IrExpr[],
  ): void {
    callArguments.forEach(argument => this.expr(argument));
    this.step(CheckOp.math, MathFn[mathFunction], callArguments.length, 1);
  }

  /**
   * Pushes a saturating whole-number helper's value: the sum in doubles, cut to the int range, is the same. A
   * 64-bit one is not something a check holds.
   *
   * @param expression  The helper.
   */
  private helper(expression: Extract<IrExpr, {kind: 'helper'}>): void {
    if (expression.bits !== 32) {
      throw new NotCheckable();
    }

    const [left, right] = expression.callArguments;
    const {helper} = expression;
    switch (helper) {
      case 'min':
      case 'max':
        this.math(helper, expression.callArguments);
        return;
      case 'div':
      case 'floordiv':
      case 'ceildiv':
      case 'rounddiv':
      case 'mod':
        this.binary(CheckOp.idiv, left, right, DIVISIONS[helper]);
        return;
      case 'abs':
        this.math('abs', expression.callArguments);
        break;
      case 'neg':
        this.expr(left);
        this.step(CheckOp.neg, 0, 1, 1);
        break;
      case 'add':
        this.binary(CheckOp.add, left, right);
        break;
      case 'sub':
        this.binary(CheckOp.sub, left, right);
        break;
      default:
        this.binary(CheckOp.mul, left, right);
    }
    this.step(CheckOp.toint, 0, 1, 1);
  }

  /**
   * Pushes whether one assumption holds.
   *
   * @param slot  The slot it reads.
   * @param value  What the slot had on the compile's panel.
   */
  holds(slot: IrBootSlot, value: number | string | boolean): void {
    this.slot(slot);
    if (typeof value === 'string') {
      this.string(value);
      this.step(CheckOp.eq, 0, 2, 1);
    } else if (typeof value === 'boolean') {
      if (!value) this.step(CheckOp.not, 0, 1, 1);
    } else if (slot.type.kind === 'float') {
      this.number(value);
      this.step(CheckOp.sub, 0, 2, 1);
      this.step(CheckOp.math, MathFn.abs, 1, 1);
      this.number(FLOAT_TOLERANCE * (1 + Math.abs(value)));
      this.step(CheckOp.le, 0, 2, 1);
    } else {
      this.number(value);
      this.step(CheckOp.eq, 0, 2, 1);
    }
  }

  /** Pops two truths, and pushes whether both hold. */
  both(): void {
    this.step(CheckOp.and, 0, 2, 1);
  }

  /** Ends the check. */
  end(): void {
    this.steps.push({opcode: CheckOp.end, operand: 0});
  }
}

/**
 * The check that takes one size only.
 *
 * @param screen  The size.
 * @param why  The expression that makes it so.
 *
 * @returns The check.
 */
function exactCheck(screen: Screen, why: string): LayoutCheck {
  const {pushi, width, height, eq, and, end} = CheckOp;
  return {
    steps: [
      {opcode: width, operand: 0},
      {opcode: pushi, operand: screen.width},
      {opcode: eq, operand: 0},
      {opcode: height, operand: 0},
      {opcode: pushi, operand: screen.height},
      {opcode: eq, operand: 0},
      {opcode: and, operand: 0},
      {opcode: end, operand: 0},
    ],
    reads: {width: true, height: true},
    exactBecause: why,
  };
}

/**
 * Lowers what a compile assumed of its panel to the layout's check.
 *
 * @param assumptions  What it assumed.
 * @param screen  The panel it was compiled for.
 *
 * @returns The check.
 */
export function lowerCheck(
  assumptions: IrBootAssumption[],
  screen: Screen,
): LayoutCheck {
  if (!assumptions.length) {
    return {
      steps: null,
      reads: {width: false, height: false},
      exactBecause: null,
    };
  }

  const lowering = new CheckLowering();
  for (const [index, assumption] of assumptions.entries()) {
    if (assumption.kind === 'panel') {
      return exactCheck(screen, assumption.source ?? 'a value');
    }
    try {
      lowering.holds(assumption.slot, assumption.value);
      if (index) lowering.both();
    } catch (error) {
      if (error instanceof NotCheckable) {
        return exactCheck(screen, assumption.source ?? assumption.slot.field);
      }
      throw error;
    }
  }

  lowering.end();
  return {steps: lowering.steps, reads: lowering.reads, exactBecause: null};
}

/**
 * Packs a check's steps into words.
 *
 * @param steps  The steps.
 *
 * @returns The words.
 */
export function encodeCheck(steps: CheckStep[]): number[] {
  const words: number[] = [];
  const bits = new DataView(new ArrayBuffer(8));
  for (const {opcode, operand} of steps) {
    if (opcode === CheckOp.pushk) {
      bits.setFloat64(0, operand, true);
      words.push(opcode, bits.getUint32(0, true), bits.getUint32(4, true));
    } else {
      words.push((opcode | ((operand & 0xffffff) << 8)) >>> 0);
    }
  }

  return words;
}

/**
 * Lays out the checks table: each distinct check once.
 *
 * @param checks  Each panel's check.
 *
 * @returns Each panel's first word (or CHECK_ALWAYS), and the table's words.
 */
export function checksTable(checks: LayoutCheck[]): {
  starts: number[];
  words: number[];
} {
  const words: number[] = [];
  const known = new Map<string, number>();
  const starts = checks.map(({steps}) => {
    if (!steps) return CHECK_ALWAYS;
    const encoded = encodeCheck(steps);
    const key = encoded.join();
    let start = known.get(key);
    if (start === undefined) {
      start = words.length;
      known.set(key, start);
      words.push(...encoded);
    }
    return start;
  });
  return {starts, words};
}

/*----------------------------------------------------------------------------------------------------------------------
 - Running a check for one size
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * A value's truth, as JS and the runtime read it.
 *
 * @param value  The value.
 *
 * @returns True unless it is 0 or NaN.
 */
const truth = (value: number): boolean => value === value && value !== 0;

/**
 * A double as an int, as the VM converts one: toward zero, NaN as 0, saturating.
 *
 * @param value  The value.
 *
 * @returns The int.
 */
function toInt(value: number): number {
  if (value !== value) return 0;
  if (value >= INT_MAX) return INT_MAX;
  if (value <= INT_MIN) return INT_MIN;
  return Math.trunc(value) + 0;
}

/**
 * A whole-number division, with the VM's rules: dividing by 0 saturates to the dividend's sign.
 *
 * @param mode  A CheckDivide value.
 * @param dividendValue  The dividend.
 * @param divisorValue  The divisor.
 *
 * @returns The result.
 */
function divide(
  mode: number,
  dividendValue: number,
  divisorValue: number,
): number {
  const dividend = toInt(dividendValue);
  const divisor = toInt(divisorValue);
  if (mode === CheckDivide.mod) {
    return divisor === 0 || divisor === -1 ? 0 : (dividend % divisor) + 0;
  }
  if (divisor === 0) {
    return dividend > 0 ? INT_MAX : dividend < 0 ? INT_MIN : 0;
  }

  const quotient = dividend / divisor;
  const rounded =
    mode === CheckDivide.floor
      ? Math.floor(quotient)
      : mode === CheckDivide.ceil
        ? Math.ceil(quotient)
        : mode === CheckDivide.round
          ? Math.floor(quotient + ROUND_HALF_UP)
          : Math.trunc(quotient);
  return Math.min(INT_MAX, Math.max(INT_MIN, rounded)) + 0;
}

/**
 * A Math function, as the VM's dmath() works it out.
 *
 * @param mathFunction  A MathFn value.
 * @param left  The first operand.
 * @param right  The second, if it takes one.
 *
 * @returns The result.
 */
function mathOf(mathFunction: number, left: number, right: number): number {
  switch (mathFunction) {
    case MathFn.sin:
      return Math.sin(left);
    case MathFn.cos:
      return Math.cos(left);
    case MathFn.tan:
      return Math.tan(left);
    case MathFn.sqrt:
      return Math.sqrt(left);
    case MathFn.abs:
      return Math.abs(left);
    case MathFn.round: {
      const floor = Math.floor(left);
      return left - floor >= ROUND_HALF_UP ? floor + 1 : floor;
    }
    case MathFn.floor:
      return Math.floor(left);
    case MathFn.ceil:
      return Math.ceil(left);
    case MathFn.min:
      return left !== left || right !== right
        ? NaN
        : left < right
          ? left
          : right;
    case MathFn.max:
      return left !== left || right !== right
        ? NaN
        : left > right
          ? left
          : right;
    case MathFn.atan2:
      return Math.atan2(left, right);
    case MathFn.pow:
      return Math.pow(left, right);
    default:
      return left % right;
  }
}

/** Whether a Math function takes two operands. */
const isBinaryMath = (mathFunction: number): boolean =>
  mathFunction >= MathFn.min;

/**
 * Runs a check for one size, as the loader does.
 *
 * @param check  The check.
 * @param width  The screen's width.
 * @param height  The screen's height.
 *
 * @returns Whether the layout takes the screen.
 */
export function runCheck(
  check: LayoutCheck,
  width: number,
  height: number,
): boolean {
  if (!check.steps) return true;
  const stack: number[] = [];
  const locals: number[] = [];
  for (const {opcode, operand} of check.steps) {
    switch (opcode) {
      case CheckOp.end:
        return truth(stack.pop()!);
      case CheckOp.width:
        stack.push(width);
        break;
      case CheckOp.height:
        stack.push(height);
        break;
      case CheckOp.pushi:
      case CheckOp.pushk:
        stack.push(operand);
        break;
      case CheckOp.load:
        stack.push(locals[operand] ?? 0);
        break;
      case CheckOp.store:
        locals[operand] = stack.pop()!;
        break;
      case CheckOp.neg:
        stack.push(-stack.pop()!);
        break;
      case CheckOp.toint:
        stack.push(toInt(stack.pop()!));
        break;
      case CheckOp.not:
        stack.push(truth(stack.pop()!) ? 0 : 1);
        break;
      case CheckOp.math:
        if (isBinaryMath(operand)) {
          const right = stack.pop()!;
          stack.push(mathOf(operand, stack.pop()!, right));
        } else {
          stack.push(mathOf(operand, stack.pop()!, 0));
        }
        break;
      case CheckOp.select: {
        const alternate = stack.pop()!;
        const consequent = stack.pop()!;
        stack.push(truth(stack.pop()!) ? consequent : alternate);
        break;
      }
      default: {
        const right = stack.pop()!;
        stack.push(binaryOf(opcode, operand, stack.pop()!, right));
      }
    }
  }

  throw new Error('AOT: a layout check has no end');
}

/**
 * A two-operand step's result.
 *
 * @param opcode  The op.
 * @param operand  Its argument.
 * @param left  The first operand.
 * @param right  The second.
 *
 * @returns The result.
 */
function binaryOf(
  opcode: number,
  operand: number,
  left: number,
  right: number,
): number {
  switch (opcode) {
    case CheckOp.add:
      return left + right;
    case CheckOp.sub:
      return left - right;
    case CheckOp.mul:
      return left * right;
    case CheckOp.div:
      return left / right;
    case CheckOp.idiv:
      return divide(operand, left, right);
    case CheckOp.lt:
      return left < right ? 1 : 0;
    case CheckOp.gt:
      return left > right ? 1 : 0;
    case CheckOp.le:
      return left <= right ? 1 : 0;
    case CheckOp.ge:
      return left >= right ? 1 : 0;
    case CheckOp.eq:
      return left === right ? 1 : 0;
    case CheckOp.ne:
      return left !== right ? 1 : 0;
    case CheckOp.and:
      return truth(left) && truth(right) ? 1 : 0;
    default:
      return truth(left) || truth(right) ? 1 : 0;
  }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Running a check over a range of sizes
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Every value a step can give over a range of sizes: from low to high, and maybe NaN. It may be wider than the truth
 * but never narrower. The ops are monotonic in doubles, so the ends' own rounding keeps that true.
 */
interface Range {
  low: number;
  high: number;
  includesNaN: boolean;
}

/** A range that says nothing. */
const ANYTHING: Range = {low: -Infinity, high: Infinity, includesNaN: true};
const FALSE: Range = {low: 0, high: 0, includesNaN: false};
const TRUE: Range = {low: 1, high: 1, includesNaN: false};
const EITHER: Range = {low: 0, high: 1, includesNaN: false};

/**
 * The range holding one value.
 *
 * @param value  The value.
 *
 * @returns The range.
 */
const only = (value: number): Range =>
  value !== value ? ANYTHING : {low: value, high: value, includesNaN: false};

/**
 * Whether a range holds one number, so the exact op applies.
 *
 * @param range  The range.
 *
 * @returns True when it does.
 */
const isOne = (range: Range): boolean =>
  !range.includesNaN && range.low === range.high;

/**
 * The range between the smallest and largest of some values.
 *
 * @param values  The values; a NaN says nothing is known.
 * @param includesNaN  Whether the operands may be NaN.
 *
 * @returns The range.
 */
function between(values: number[], includesNaN: boolean): Range {
  if (values.some(value => value !== value)) return ANYTHING;
  return {low: Math.min(...values), high: Math.max(...values), includesNaN};
}

/**
 * A range's truth.
 *
 * @param range  The range.
 *
 * @returns True or false when every value in it agrees, else null.
 */
function truthOf(range: Range): boolean | null {
  if (!range.includesNaN && (range.low > 0 || range.high < 0)) return true;
  if (range.low === 0 && range.high === 0) return false;
  return null;
}

/**
 * A comparison over two ranges. NaN compares false, except for `!=`.
 *
 * @param opcode  The op.
 * @param left  The first operand.
 * @param right  The second.
 *
 * @returns The range.
 */
function compareRanges(opcode: number, left: Range, right: Range): Range {
  const includesNaN = left.includesNaN || right.includesNaN;
  let always: boolean;
  let never: boolean;
  switch (opcode) {
    case CheckOp.lt:
      always = left.high < right.low;
      never = left.low >= right.high;
      break;
    case CheckOp.gt:
      always = left.low > right.high;
      never = left.high <= right.low;
      break;
    case CheckOp.le:
      always = left.high <= right.low;
      never = left.low > right.high;
      break;
    case CheckOp.ge:
      always = left.low >= right.high;
      never = left.high < right.low;
      break;
    case CheckOp.eq:
      always = isOne(left) && isOne(right) && left.low === right.low;
      never = left.high < right.low || left.low > right.high;
      break;
    default:
      // A NaN makes `!=` true, so `always` stands whatever the operands' NaN.
      if (left.high < right.low || left.low > right.high) return TRUE;
      return isOne(left) && isOne(right) && left.low === right.low
        ? FALSE
        : EITHER;
  }

  if (always && !includesNaN) return TRUE;
  return never ? FALSE : EITHER;
}

/**
 * A Math function over ranges.
 *
 * @param mathFunction  A MathFn value.
 * @param left  The first operand.
 * @param right  The second, if it takes one.
 *
 * @returns The range.
 */
function mathOverRanges(
  mathFunction: number,
  left: Range,
  right: Range,
): Range {
  if (isOne(left) && (!isBinaryMath(mathFunction) || isOne(right))) {
    return only(mathOf(mathFunction, left.low, right.low));
  }

  const includesNaN =
    left.includesNaN || (isBinaryMath(mathFunction) && right.includesNaN);
  switch (mathFunction) {
    case MathFn.sin:
    case MathFn.cos:
      return {low: -1, high: 1, includesNaN: true};
    case MathFn.sqrt:
      return left.high < 0
        ? ANYTHING
        : {
            low: Math.sqrt(Math.max(0, left.low)),
            high: Math.sqrt(left.high),
            includesNaN: includesNaN || left.low < 0,
          };
    case MathFn.abs:
      return left.low >= 0
        ? left
        : left.high <= 0
          ? {low: -left.high, high: -left.low, includesNaN: left.includesNaN}
          : {
              low: 0,
              high: Math.max(-left.low, left.high),
              includesNaN: left.includesNaN,
            };
    case MathFn.round:
    case MathFn.floor:
    case MathFn.ceil:
      return {
        low: mathOf(mathFunction, left.low, 0),
        high: mathOf(mathFunction, left.high, 0),
        includesNaN,
      };
    case MathFn.min:
      return {
        low: Math.min(left.low, right.low),
        high: Math.min(left.high, right.high),
        includesNaN,
      };
    case MathFn.max:
      return {
        low: Math.max(left.low, right.low),
        high: Math.max(left.high, right.high),
        includesNaN,
      };
    case MathFn.mod:
      return isOne(right) && right.low > 0 && left.low >= 0
        ? moduloRange(left, right.low)
        : ANYTHING;
    default:
      return ANYTHING;
  }
}

/**
 * The remainder of a range of non-negative numbers by a positive one.
 *
 * @param left  The dividends.
 * @param divisor  The divisor.
 *
 * @returns The range.
 */
function moduloRange(left: Range, divisor: number): Range {
  const samePeriod =
    Math.floor(left.low / divisor) === Math.floor(left.high / divisor);
  return samePeriod
    ? {
        low: left.low % divisor,
        high: left.high % divisor,
        includesNaN: left.includesNaN,
      }
    : {low: 0, high: divisor, includesNaN: left.includesNaN};
}

/**
 * A whole-number division over ranges: by one positive divisor it rises with the dividend.
 *
 * @param mode  A CheckDivide value.
 * @param left  The dividends.
 * @param right  The divisors.
 *
 * @returns The range.
 */
function divideOverRanges(mode: number, left: Range, right: Range): Range {
  if (isOne(left) && isOne(right)) {
    return only(divide(mode, left.low, right.low));
  }
  if (!isOne(right) || right.low <= 0) {
    return {low: INT_MIN, high: INT_MAX, includesNaN: false};
  }

  const whole = rangeToInt(left);
  if (mode === CheckDivide.mod) {
    return whole.low >= 0
      ? moduloRange(whole, right.low)
      : {low: -right.low, high: right.low, includesNaN: false};
  }
  return {
    low: divide(mode, whole.low, right.low),
    high: divide(mode, whole.high, right.low),
    includesNaN: false,
  };
}

/**
 * A range of doubles as ints.
 *
 * @param range  The range.
 *
 * @returns The range.
 */
function rangeToInt(range: Range): Range {
  const low = toInt(range.low);
  const high = toInt(range.high);
  return range.includesNaN
    ? {low: Math.min(low, 0), high: Math.max(high, 0), includesNaN: false}
    : {low, high, includesNaN: false};
}

/**
 * The product of two ranges.
 *
 * @param left  The first.
 * @param right  The second.
 *
 * @returns The range.
 */
const timesRange = (left: Range, right: Range): Range =>
  between(
    [
      left.low * right.low,
      left.low * right.high,
      left.high * right.low,
      left.high * right.high,
    ],
    left.includesNaN || right.includesNaN,
  );

/**
 * The quotient of two ranges. A divisor that may be 0 says nothing.
 *
 * @param left  The dividends.
 * @param right  The divisors.
 *
 * @returns The range.
 */
const overRange = (left: Range, right: Range): Range =>
  right.low <= 0 && right.high >= 0
    ? ANYTHING
    : between(
        [
          left.low / right.low,
          left.low / right.high,
          left.high / right.low,
          left.high / right.high,
        ],
        left.includesNaN || right.includesNaN,
      );

/**
 * A two-operand step over ranges.
 *
 * @param opcode  The op.
 * @param operand  Its argument.
 * @param left  The first operand.
 * @param right  The second.
 *
 * @returns The range.
 */
function binaryOverRanges(
  opcode: number,
  operand: number,
  left: Range,
  right: Range,
): Range {
  const includesNaN = left.includesNaN || right.includesNaN;
  switch (opcode) {
    case CheckOp.add:
      return between(
        [left.low + right.low, left.high + right.high],
        includesNaN,
      );
    case CheckOp.sub:
      return between(
        [left.low - right.high, left.high - right.low],
        includesNaN,
      );
    case CheckOp.mul:
      return timesRange(left, right);
    case CheckOp.div:
      return overRange(left, right);
    case CheckOp.idiv:
      return divideOverRanges(operand, left, right);
    case CheckOp.and: {
      const [first, second] = [truthOf(left), truthOf(right)];
      if (first === false || second === false) return FALSE;
      return first && second ? TRUE : EITHER;
    }
    case CheckOp.or: {
      const [first, second] = [truthOf(left), truthOf(right)];
      if (first || second) return TRUE;
      return first === false && second === false ? FALSE : EITHER;
    }
    default:
      return compareRanges(opcode, left, right);
  }
}

/**
 * Runs a check over a range of sizes.
 *
 * @param check  The check.
 * @param sizes  The sizes.
 *
 * @returns 'pass' when the check passes for every one, 'fail' when it fails for every one, else 'unsure'.
 */
export function checkOverSizes(check: LayoutCheck, sizes: SizeRange): Verdict {
  if (!check.steps) return 'pass';

  // One size has an exact answer.
  if (
    (sizes.minWidth === sizes.maxWidth || !check.reads.width) &&
    (sizes.minHeight === sizes.maxHeight || !check.reads.height)
  ) {
    return runCheck(check, sizes.minWidth, sizes.minHeight) ? 'pass' : 'fail';
  }

  const stack: Range[] = [];
  const locals: Range[] = [];
  for (const {opcode, operand} of check.steps) {
    switch (opcode) {
      case CheckOp.end: {
        const verdict = truthOf(stack.pop()!);
        return verdict === null ? 'unsure' : verdict ? 'pass' : 'fail';
      }
      case CheckOp.width:
        stack.push({
          low: sizes.minWidth,
          high: sizes.maxWidth,
          includesNaN: false,
        });
        break;
      case CheckOp.height:
        stack.push({
          low: sizes.minHeight,
          high: sizes.maxHeight,
          includesNaN: false,
        });
        break;
      case CheckOp.pushi:
      case CheckOp.pushk:
        stack.push(only(operand));
        break;
      case CheckOp.load:
        stack.push(locals[operand] ?? FALSE);
        break;
      case CheckOp.store:
        locals[operand] = stack.pop()!;
        break;
      case CheckOp.neg: {
        const {low, high, includesNaN} = stack.pop()!;
        stack.push({low: -high, high: -low, includesNaN});
        break;
      }
      case CheckOp.toint:
        stack.push(rangeToInt(stack.pop()!));
        break;
      case CheckOp.not: {
        const value = truthOf(stack.pop()!);
        stack.push(value === null ? EITHER : value ? FALSE : TRUE);
        break;
      }
      case CheckOp.math:
        if (isBinaryMath(operand)) {
          const right = stack.pop()!;
          stack.push(mathOverRanges(operand, stack.pop()!, right));
        } else {
          stack.push(mathOverRanges(operand, stack.pop()!, FALSE));
        }
        break;
      case CheckOp.select: {
        const alternate = stack.pop()!;
        const consequent = stack.pop()!;
        const test = truthOf(stack.pop()!);
        if (test === null) {
          stack.push({
            low: Math.min(consequent.low, alternate.low),
            high: Math.max(consequent.high, alternate.high),
            includesNaN: consequent.includesNaN || alternate.includesNaN,
          });
        } else {
          stack.push(test ? consequent : alternate);
        }
        break;
      }
      default: {
        const right = stack.pop()!;
        stack.push(binaryOverRanges(opcode, operand, stack.pop()!, right));
      }
    }
  }

  throw new Error('AOT: a layout check has no end');
}
