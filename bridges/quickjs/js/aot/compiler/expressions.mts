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

import {attemptLowering, checkNumber} from './check.mts';
import {traverse} from '@babel/core';
import {aotError, locateError, withLoc} from './diagnostics.mts';
import {evalStatic, foldScope} from './static-eval.mts';
import {lowerGestureField} from './pan-responder.mts';
import {toCExpr} from './c/expressions.mts';
import {printText} from './c/text.mts';
import type * as t from '@babel/types';
import type {AotError, SourcePosition} from './diagnostics.mts';
import type {IrSlotStore, IrSource, IrSourceDivisor} from './ir/source.mts';
import type {
  CExpr,
  CType,
  Env,
  FormatResult,
  ListState,
  Local,
  ScalarState,
  Scope,
} from './types.mts';
import type {
  FloatMath,
  IrArithmetic,
  IrBoolean,
  IrCompare,
  IrExpr,
  IrLocal,
  IrNumber,
  IrString,
  IrTyped,
  LayoutField,
  WholeHelper,
} from './ir/expressions.mts';
import type {SlotWidths} from './ir/types.mts';
import type {IrText, IrTextPart} from './ir/text.mts';
import {
  BOOL,
  FLOAT,
  INT,
  INT64_NUMBER,
  INT64_MATH,
  STRING,
  TIME,
  isBoolean,
  isConvertible64,
  isIntSlot,
  isTime,
  int64Join,
  int64Of,
  stateType,
  refType,
  slotOf,
  typeOfCExpr,
  typeOfSlot,
} from './ir/types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** A JS expression split into text parts, and whether JS would treat it as a string. */
interface TextParts {
  isString: boolean;
  parts: IrTextPart[];
}

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** The binary operators lowered as arithmetic, and those lowered as comparisons. */
const ARITH = new Set(['+', '-', '*', '/', '%']);
const COMPARE = new Set(['<', '>', '<=', '>=', '==', '!=', '===', '!==']);

/**
 * The C helper each whole-number operator lowers to. It saturates at the limit of its type, where C's own
 * signed overflow is undefined behavior; the 64-bit variant appends `64`.
 */
export const CHECKED_OP: Record<string, string> = {
  '+': 'app_add',
  '-': 'app_sub',
  '*': 'app_mul',
};

/** The saturating helper each whole-number operator lowers to in the IR. */
const CHECKED_HELPER: Record<string, WholeHelper> = {
  '+': 'add',
  '-': 'sub',
  '*': 'mul',
};

/** The range of a C int. */
const INT_MIN = -(2 ** 31);
export const INT_MAX = 2 ** 31 - 1;

/** The Math functions that keep a whole number whole. */
const WHOLE_MATH = new Set([
  'floor',
  'ceil',
  'round',
  'trunc',
  'abs',
  'min',
  'max',
]);

/** The helper that rounds `a / b` for two whole numbers the way each Math function does. */
const INT_ROUND_DIV = new Map<string, WholeHelper>([
  ['floor', 'floordiv'],
  ['ceil', 'ceildiv'],
  ['round', 'rounddiv'],
  ['trunc', 'div'],
]);

/** The Math functions of one argument, and of two, that are worked out on floats. */
const FLOAT_MATH_UNARY = new Set([
  'sin',
  'cos',
  'tan',
  'sqrt',
  'abs',
  'round',
  'floor',
  'ceil',
]);
const FLOAT_MATH_BINARY = new Set(['min', 'max', 'atan2', 'pow']);

/** While the typing pass infers slot widths: the slots it takes to hold 64 bits, and what each holds. */
let assumedWide: SlotWidths | null = null;

/** The fields of an onLayout rect. */
const LAYOUT_FIELDS = new Set(['x', 'y', 'width', 'height']);

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

// The modules that still build C from a lowered expression or text import asCond and formatArgs from here.
export {asCond} from './c/expressions.mts';
export {formatArgs} from './c/text.mts';

/**
 * A 64-bit timestamp met a float, which would round it.
 *
 * @returns The error, for the caller to throw.
 */
export const mix64Error = (): AotError =>
  aotError(
    'AOT: a 64-bit time value cannot be mixed with a float',
    'Date.now() and performance.now() are whole milliseconds in a 64-bit integer, and a ' +
      'float would round them. Keep the arithmetic whole — e.g. Math.floor(ms / 1000), not ' +
      'ms * 0.001.',
  );

/**
 * A 64-bit timestamp met a boolean in `&&` / `||` / `?:`, where JS would hand back the boolean itself.
 *
 * @param what  The construct, for the message.
 *
 * @returns The error, for the caller to throw.
 */
const bool64Error = (what: string): AotError =>
  aotError(
    `AOT: ${what} cannot mix a boolean with a 64-bit time value`,
    'JS would give back the boolean itself (and false renders as no text), which a 64-bit ' +
      'integer cannot hold. Use a number on both sides, e.g. `on ? Date.now() : 0`.',
  );

/**
 * Flow A's lite profile has Date.now() and no Date objects, and so does the AOT.
 *
 * @returns The error, for the caller to throw.
 */
const dateObjectError = (): AotError =>
  aotError(
    'AOT: Date objects are not supported — only Date.now()',
    'keep time as milliseconds from Date.now() and do the calendar math on the number.',
  );

/**
 * The `Date` / `performance` calls whose name is the app's own binding — a const, a param, an import —
 * rather than the global, so they are not the engine clock. Babel's scopes decide, as JS would.
 *
 * @param ast  The parsed app.
 *
 * @returns The shadowed calls.
 */
export function findShadowedClockCalls(ast: t.File): WeakSet<t.Node> {
  const calls = new WeakSet<t.Node>();
  traverse(ast, {
    'CallExpression|NewExpression'(path) {
      const callee = (path.node as t.CallExpression | t.NewExpression).callee;
      const id = callee.type === 'MemberExpression' ? callee.object : callee;
      if (
        id.type === 'Identifier' &&
        (id.name === 'Date' || id.name === 'performance') &&
        path.scope.getBinding(id.name)
      ) {
        calls.add(path.node);
      }
    },
  });

  return calls;
}

/**
 * `node` as an integer compile-time constant, or null.
 *
 * @param node  The expression.
 * @param env  The expression environment.
 *
 * @returns The integer, or null when the expression is not one.
 */
export function staticInt(node: t.Node, env: Env): number | null {
  try {
    const value = evalStatic(node, foldScope(env, env.consts ?? {}));
    return Number.isInteger(value) ? (value as number) : null;
  } catch {
    return null;
  }
}

/**
 * A compile-time number, typed. A whole number too big for an int is a 64-bit constant, so whole-number math
 * keeps it exact; past 64 bits it is a float.
 *
 * @param value  The number.
 *
 * @returns Its constant.
 */
function numConst(value: number): IrNumber {
  if (!Number.isInteger(value) || value < -(2 ** 63) || value >= 2 ** 63) {
    return {kind: 'number', value, type: FLOAT};
  }

  if (value < INT_MIN || value > INT_MAX) {
    return {kind: 'number', value, type: INT64_NUMBER};
  }

  return {kind: 'number', value, type: INT};
}

/**
 * A compile-time number as written. One that ends up a float (a fraction, or past 64 bits) is checked by the
 * check pass, at the expression: a float holds no NaN or Infinity, a folded `0 / 0` say.
 *
 * @param value  The number.
 * @param loc  Where it is.
 *
 * @returns Its source.
 */
function numberSource(
  value: number,
  loc: SourcePosition | undefined,
): IrSource {
  if (numConst(value).type.kind === 'float') {
    checkNumber(value);
  }

  return {kind: 'number', value, loc};
}

/**
 * A compile-time string.
 *
 * @param value  The string.
 *
 * @returns Its constant.
 */
const stringConst = (value: string): IrString => ({
  kind: 'string',
  value,
  type: STRING,
});

/**
 * A compile-time boolean, held in an int slot.
 *
 * @param value  The boolean.
 *
 * @returns Its constant.
 */
const boolConst = (value: boolean): IrBoolean => ({
  kind: 'boolean',
  value,
  type: BOOL,
});

/**
 * A read of a local, typed as the local is.
 *
 * @param local  The local.
 *
 * @returns The read.
 */
const localRead = (local: Local): IrLocal => ({
  kind: 'local',
  local,
  type: typeOfCExpr(local),
});

/**
 * Whole-number math JS would not cut to 32 bits: `+ - *`, negation, `%`, and the Math functions that keep a
 * whole number whole. Beside a 64-bit value all of it is worked out in 64 bits, however deep it sits.
 *
 * @param node  The expression.
 *
 * @returns Whether it is such math.
 */
const keepsMath64Node = (node: t.Node): boolean =>
  (node.type === 'BinaryExpression' &&
    (Boolean(CHECKED_HELPER[node.operator]) || node.operator === '%')) ||
  (node.type === 'UnaryExpression' && node.operator === '-') ||
  (node.type === 'CallExpression' &&
    node.callee.type === 'MemberExpression' &&
    !node.callee.computed &&
    node.callee.object.type === 'Identifier' &&
    node.callee.object.name === 'Math' &&
    WHOLE_MATH.has((node.callee.property as t.Identifier).name));

/**
 * keepsMath64Node, for a source expression.
 *
 * @param source  The expression.
 *
 * @returns Whether it is whole-number math JS would not cut to 32 bits.
 */
const keepsMath64 = (source: IrSource): boolean =>
  (source.kind === 'binary' &&
    (Boolean(CHECKED_HELPER[source.op]) || source.op === '%')) ||
  (source.kind === 'unary' && source.op === '-') ||
  (source.kind === 'math' && source.wholeMath);

/**
 * A 64-bit value computed at runtime from a timestamp, as opposed to a 64-bit constant or whole-number math
 * widened to 64 bits beside one.
 *
 * @param expr  The lowered expression.
 *
 * @returns Whether it is a timestamp.
 */
export const isTime64 = (expr: IrTyped): boolean => isTime(expr.type);

/**
 * Besides a float, a 64-bit number that is not a time converts, as in JS; only a timestamp refuses.
 *
 * @param operands  The operands.
 *
 * @returns The operands, with any such number converted to float when one of them is a float.
 */
const litPeers = (...operands: IrExpr[]): IrExpr[] =>
  operands.some(operand => operand.type.kind === 'float')
    ? operands.map(
        (operand): IrExpr =>
          isConvertible64(operand.type)
            ? {kind: 'toFloat', operand, type: FLOAT}
            : operand,
      )
    : operands;

/**
 * The error for a divisor that is not a constant where a time's has to be one.
 *
 * @param loc  Where the divisor is.
 *
 * @returns The error, for the caller to throw.
 */
function nonConstantDivisorError(loc: SourcePosition | undefined): AotError {
  const error = aotError(
    'AOT: a 64-bit time value can only be divided by a constant',
    'the divisor must be known at compile time — e.g. `ms % 1000`, `Math.floor(ms / ' +
      '60000)`. For a divisor from state, reduce the timestamp first: `(ms % 86400000) / ' +
      'period`.',
  );
  if (loc) {
    error.aotLoc = loc;
  }

  return error;
}

/**
 * The divisor of a timestamp's `%` or Math.floor / ceil / round / trunc(a / b), which has to be a constant;
 * for one from state, an app reduces the timestamp first. A constant 0 gives JS's answer as the int math
 * converts it: a remainder of 0, or a quotient saturated by the dividend's sign.
 *
 * @param node  The divisor.
 * @param env  The expression environment.
 *
 * @returns The divisor's value.
 */
export function constDivisor(node: t.Node, env: Env): number {
  const divisor = staticInt(node, env);
  if (divisor !== null) return divisor;

  throw nonConstantDivisorError(node.loc?.start);
}

/**
 * The divisor a source expression records, which has to be a constant (see constDivisor).
 *
 * @param divisor  The divisor.
 *
 * @returns Its value.
 */
function sourceDivisor(divisor: IrSourceDivisor): number {
  if (divisor.constant !== null) return divisor.constant;

  throw nonConstantDivisorError(divisor.loc);
}

/**
 * `+ - * %` with a 64-bit value on either side, kept whole. `+ - *` saturate at the int64 limits. With a
 * timestamp in it, `%` takes a constant; whole-number math widened beside one takes any divisor, as the int
 * `%` does. The result narrows back to int when the divisor or the dividend fits one, so `ms % 1000` is an
 * ordinary number again. `/` is refused: JS would give a fraction, and the whole-number division is written
 * Math.floor(a / b).
 *
 * @param source  The binary expression.
 * @param left  Its typed left operand.
 * @param right  Its typed right operand.
 *
 * @returns The typed expression.
 */
function typeArith64(
  source: Extract<IrSource, {kind: 'binary'}>,
  left: IrExpr,
  right: IrExpr,
): IrExpr {
  if (left.type.kind === 'float' || right.type.kind === 'float') {
    throw mix64Error();
  }

  if (source.op === '/') {
    throw aotError(
      'AOT: `/` on a 64-bit time value is not supported',
      'JS division gives a fraction, which a 64-bit integer cannot hold. Use Math.floor(a / ' +
        'b) for whole units — e.g. Math.floor(ms / 1000) for seconds.',
    );
  }

  // With no timestamp on either side, this is whole-number math widened to 64 bits.
  const isWide = !isTime64(left) && !isTime64(right);

  // `%`: zero/±1 divisors give 0, a timestamp needs a constant divisor, and the result narrows to int if it fits.
  if (source.op === '%') {
    const staticDivisor = source.divisor.constant;
    // JS gives NaN for a zero divisor, which is 0 here, as the int `%` has it.
    if (staticDivisor === 0) return numConst(0);

    if (isWide && staticDivisor === null) {
      return {
        kind: 'helper',
        helper: 'mod',
        bits: 64,
        args: [left, right],
        type: INT64_MATH,
      };
    }

    const divisor = sourceDivisor(source.divisor);
    // ±1 leaves no remainder, and INT64_MIN % -1 overflows in C.
    if (divisor === 1 || divisor === -1) return numConst(0);

    if (Math.abs(divisor) <= 0x7fffffff || isIntSlot(left.type)) {
      return {kind: 'narrowRemainder', left, right, type: INT};
    }

    return {
      kind: 'arithmetic',
      op: '%',
      left,
      right,
      type: isWide ? INT64_MATH : TIME,
    };
  }

  // `+ - *`: the saturating 64-bit helper.
  return {
    kind: 'helper',
    helper: CHECKED_HELPER[source.op],
    bits: 64,
    args: [left, right],
    type: isWide ? INT64_MATH : TIME,
  };
}

/**
 * Math.floor / ceil / round / trunc over `a / b` with a 64-bit timestamp on either side, as exact integer
 * division by a constant (see constDivisor); a zero one saturates by the dividend's sign, as JS's ±Infinity
 * converts. Null when neither side is a timestamp, which leaves it to the whole-number or float path.
 *
 * @param fn  The Math function's name.
 * @param division  Its one argument, the division.
 * @param math64  Whole-number math is worked out in 64 bits here.
 *
 * @returns The typed call, or null when it is not this case.
 */
function typeTimeRoundDiv(
  fn: string,
  division: Extract<IrSource, {kind: 'binary'}>,
  math64: boolean,
): IrExpr | null {
  const dividend = typeSource(division.left, math64);
  const divisor = typeSource(division.right, math64);
  if (!isTime64(dividend) && !isTime64(divisor)) return null;

  if (dividend.type.kind === 'float' || divisor.type.kind === 'float') {
    throw mix64Error();
  }

  if (dividend.type.kind === 'string' || divisor.type.kind === 'string')
    return null;

  // Dividing by -1 is a negation, which saturates; C's `/` would overflow on INT64_MIN / -1.
  if (sourceDivisor(division.divisor) === -1) {
    return {
      kind: 'helper',
      helper: 'neg',
      bits: 64,
      args: [dividend],
      type: TIME,
    };
  }

  return {
    kind: 'helper',
    helper: INT_ROUND_DIV.get(fn)!,
    bits: 64,
    args: [dividend, divisor],
    type: TIME,
  };
}

/**
 * Math.floor / ceil / round / trunc over `a / b` with two whole numbers, as exact integer division: the float
 * path rounds an operand past 2^24, so Math.floor(16777217 / 1) would come out 16777216. A zero divisor keeps
 * JS's answer as app_f2i converts it (±Infinity saturates by the dividend's sign, 0 / 0 is 0), and
 * INT_MIN / -1 saturates too. Beside a 64-bit value, or with a 64-bit operand that is not a timestamp (a
 * constant past the int range, or math already widened), it divides in 64 bits, since JS would not cut the
 * operands to 32. Null for anything else, which leaves it to the float path.
 *
 * @param fn  The Math function's name.
 * @param division  Its one argument, the division.
 * @param math64  Whole-number math is worked out in 64 bits here.
 *
 * @returns The typed call, or null when it is not this case.
 */
function typeIntRoundDiv(
  fn: string,
  division: Extract<IrSource, {kind: 'binary'}>,
  math64: boolean,
): IrExpr | null {
  // Divide in 64 bits beside a 64-bit value, when both operands are whole numbers and neither is a timestamp.
  const dividend = typeSource(division.left, math64);
  const divisor = typeSource(division.right, math64);
  if (
    math64 ||
    dividend.type.kind === 'int64' ||
    divisor.type.kind === 'int64'
  ) {
    const isWhole = (operand: IrExpr) =>
      isIntSlot(operand.type) ||
      (operand.type.kind === 'int64' && !isTime64(operand));
    if (!isWhole(dividend) || !isWhole(divisor)) return null;
    return {
      kind: 'helper',
      helper: INT_ROUND_DIV.get(fn)!,
      bits: 64,
      args: [dividend, divisor],
      type: INT64_MATH,
    };
  }

  // Otherwise divide two ints in 32 bits; anything else is left to the float path.
  if (!isIntSlot(dividend.type) || !isIntSlot(divisor.type)) return null;
  return {
    kind: 'helper',
    helper: INT_ROUND_DIV.get(fn)!,
    bits: 32,
    args: [dividend, divisor],
    type: INT,
  };
}

/**
 * Math.* over a 64-bit timestamp: only what stays exact in whole numbers.
 *
 * @param fn  The Math function's name.
 * @param argExprs  Its typed arguments, at least one of them 64-bit.
 *
 * @returns The typed call.
 */
function typeMath64(fn: string, argExprs: IrExpr[]): IrExpr {
  if (argExprs.some(argExpr => argExpr.type.kind === 'float')) {
    throw mix64Error();
  }

  if (
    argExprs.every(
      argExpr => isIntSlot(argExpr.type) || argExpr.type.kind === 'int64',
    )
  ) {
    // Whole-number math with no timestamp in it stays whole-number math.
    const isWide = !argExprs.some(isTime64);
    // A timestamp is already whole, so rounding leaves it as it is.
    if (
      (fn === 'floor' || fn === 'round' || fn === 'ceil' || fn === 'trunc') &&
      argExprs.length === 1
    ) {
      return {
        kind: 'wholeRound',
        operand: argExprs[0],
        type: isWide ? INT64_MATH : TIME,
      };
    }

    if (fn === 'abs' && argExprs.length === 1) {
      return {
        kind: 'helper',
        helper: 'abs',
        bits: 64,
        args: [argExprs[0]],
        type: isWide ? INT64_MATH : TIME,
      };
    }

    if ((fn === 'min' || fn === 'max') && argExprs.length === 2) {
      return {
        kind: 'helper',
        helper: fn,
        bits: 64,
        args: [argExprs[0], argExprs[1]],
        type: isWide ? INT64_MATH : TIME,
      };
    }
  }

  throw aotError(
    `AOT: Math.${fn}(...) on a 64-bit time value is not supported`,
    'Math.floor / round / ceil / trunc / abs / min / max keep a timestamp exact; for ' +
      'anything else, reduce it to a small number first, e.g. `ms % 1000`.',
  );
}

/**
 * Types a unary expression: a whole number negates through the saturating helper, anything else as itself.
 *
 * @param source  The expression.
 * @param math64  Whole-number math is worked out in 64 bits here.
 *
 * @returns The typed expression.
 */
function typeUnary(
  source: Extract<IrSource, {kind: 'unary'}>,
  math64: boolean,
): IrExpr {
  const operand = typeSource(source.operand, math64);
  if (
    (source.op === '-' || source.op === '+') &&
    operand.type.kind === 'string'
  ) {
    throw aotError(
      `AOT: unary "${source.op}" on a string is not supported`,
      'JS would coerce the string to a number; C has no such coercion. Keep the operand numeric.',
    );
  }

  // Negating the minimum overflows, so a whole number saturates the way `+ - *` do.
  if (
    source.op === '-' &&
    (isIntSlot(operand.type) || operand.type.kind === 'int64')
  ) {
    const is64Bit = operand.type.kind === 'int64' || math64;
    return is64Bit
      ? {
          kind: 'helper',
          helper: 'neg',
          bits: 64,
          args: [operand],
          type: operand.type.kind === 'int64' ? operand.type : INT64_MATH,
        }
      : {
          kind: 'helper',
          helper: 'neg',
          bits: 32,
          args: [operand],
          type: INT,
        };
  }

  // Only `!` yields a boolean; unary +/- coerce to a number, so `+flag` drops its boolean-ness.
  return {
    kind: 'unary',
    op: source.op,
    operand,
    type:
      source.op === '!' ? BOOL : isBoolean(operand.type) ? INT : operand.type,
  };
}

/**
 * Types a binary expression: arithmetic (whole numbers saturate, 64-bit beside a time, floats as floats) or a
 * comparison. Beside a 64-bit value, whole-number math on either side is typed again in 64 bits, as JS never
 * cuts it to 32.
 *
 * @param source  The expression.
 * @param math64  Whole-number math is worked out in 64 bits here.
 *
 * @returns The typed expression.
 */
function typeBinary(
  source: Extract<IrSource, {kind: 'binary'}>,
  math64: boolean,
): IrExpr {
  let left = typeSource(source.left, math64);
  let right = typeSource(source.right, math64);
  if (left.type.kind === 'int64' || right.type.kind === 'int64') {
    if (isIntSlot(left.type) && keepsMath64(source.left)) {
      left = typeSource(source.left, true);
    }

    if (isIntSlot(right.type) && keepsMath64(source.right)) {
      right = typeSource(source.right, true);
    }
  }
  [left, right] = litPeers(left, right);

  // Arithmetic: refuse strings, hand 64-bit math to typeArith64, and saturate whole-number `+ - *`.
  if (ARITH.has(source.op)) {
    if (left.type.kind === 'string' || right.type.kind === 'string') {
      // String `+` has no single C value; only a char-buffer destination can lower it, via emitFormat().
      if (source.op === '+') {
        throw aotError(
          'AOT: string concatenation is not supported in this position',
          'a `+` chain over strings lowers to a printf format, which only works where the value ' +
            'lands in a text buffer: a <Text> body, a string useState setter, or a <TextInput ' +
            'value>.',
        );
      }
      // JS would coerce the string to a number; C would do pointer arithmetic or refuse to compile.
      throw aotError(
        `AOT: "${source.op}" on a string is not supported`,
        'keep both operands numeric — a string cannot be coerced to a number here.',
      );
    }
    // `/` takes a 64-bit constant as an ordinary C number; only a runtime 64-bit value is refused it.
    if (
      isTime64(left) ||
      isTime64(right) ||
      ((left.type.kind === 'int64' || right.type.kind === 'int64') &&
        (CHECKED_HELPER[source.op] || source.op === '%'))
    ) {
      return typeArith64(source, left, right);
    }

    if (source.op === '/') {
      return {kind: 'floatDivide', left, right, type: FLOAT};
    }

    const isFloat = left.type.kind === 'float' || right.type.kind === 'float';
    const checkedHelper = !isFloat && CHECKED_HELPER[source.op];
    if (checkedHelper) {
      return math64
        ? {
            kind: 'helper',
            helper: checkedHelper,
            bits: 64,
            args: [left, right],
            type: INT64_MATH,
          }
        : {
            kind: 'helper',
            helper: checkedHelper,
            bits: 32,
            args: [left, right],
            type: INT,
          };
    }

    // A 0 divisor (NaN in JS) or -1 (INT_MIN % -1 overflows C) gives 0; other constants need no check.
    if (!isFloat && source.op === '%') {
      const staticDivisor = source.divisor.constant;
      if (staticDivisor === 0 || staticDivisor === -1) {
        return numConst(0);
      }
      if (staticDivisor === null) {
        return {
          kind: 'helper',
          helper: 'mod',
          bits: 32,
          args: [left, right],
          type: INT,
        };
      }
    }

    // C has no float `%`; fmodf matches JS's `%` exactly, zero and infinite divisors included.
    if (isFloat && source.op === '%') {
      return {
        kind: 'floatMath',
        fn: 'mod',
        args: [left, right],
        type: FLOAT,
      };
    }

    return {
      kind: 'arithmetic',
      op: source.op as IrArithmetic['op'],
      left,
      right,
      type: isFloat ? FLOAT : INT,
    };
  }

  // Comparisons: strings through strcmp, everything else as a plain C comparison yielding a boolean.
  if (COMPARE.has(source.op)) {
    const cOperator =
      source.op === '===' ? '==' : source.op === '!==' ? '!=' : source.op;
    if (left.type.kind === 'string' || right.type.kind === 'string') {
      if (slotOf(left.type) !== slotOf(right.type)) {
        // A strict comparison never coerces, so a string against a number is decided statically.
        if (source.op === '===' || source.op === '!==') {
          return boolConst(source.op === '!==');
        }

        throw aotError(
          'AOT: a string cannot be compared with a number',
          `\`${source.op}\` coerces the string to a number in JS, which C cannot reproduce. ` +
            `Compare like with like, or use === / !== (which never coerce).`,
        );
      }
      // Equality is exact, but strcmp's UTF-8 order can differ from JS's UTF-16 order, so refuse ordering.
      if (cOperator !== '==' && cOperator !== '!=') {
        throw aotError(
          `AOT: ordering strings with "${source.op}" is not supported`,
          'JS orders strings by UTF-16 code unit but the device holds UTF-8, so the order can ' +
            'differ. Use === / !==, or compare numbers.',
        );
      }

      return {
        kind: 'stringEquals',
        op: cOperator,
        left,
        right,
        type: BOOL,
      };
    }
    // A float compared with a timestamp would round it.
    if (
      (left.type.kind === 'int64' || right.type.kind === 'int64') &&
      (left.type.kind === 'float' || right.type.kind === 'float')
    ) {
      throw mix64Error();
    }

    // `true === 1` is false in JS but true in C's int slot; fold it when the other side is surely a number.
    const isSurelyNumber = (isWrittenAsNumber: boolean, operand: IrExpr) =>
      !isBoolean(operand.type) &&
      (operand.type.kind === 'float' ||
        operand.type.kind === 'int64' ||
        isWrittenAsNumber);
    if (
      (source.op === '===' || source.op === '!==') &&
      ((isBoolean(left.type) && isSurelyNumber(source.rightIsNumber, right)) ||
        (isBoolean(right.type) && isSurelyNumber(source.leftIsNumber, left)))
    ) {
      return boolConst(source.op === '!==');
    }

    return {
      kind: 'compare',
      op: cOperator as IrCompare['op'],
      left,
      right,
      type: BOOL,
    };
  }

  throw new Error(`AOT: unsupported binary operator "${source.op}"`);
}

/**
 * Types `&&` / `||`. JS returns an operand, so with a 64-bit side the value is kept instead of collapsing it
 * to 0/1.
 *
 * @param source  The expression.
 * @param math64  Whole-number math is worked out in 64 bits here.
 *
 * @returns The typed expression.
 */
function typeLogical(
  source: Extract<IrSource, {kind: 'logical'}>,
  math64: boolean,
): IrExpr {
  const [left, right] = litPeers(
    typeSource(source.left, math64),
    typeSource(source.right, math64),
  );
  if (left.type.kind === 'int64' || right.type.kind === 'int64') {
    if (left.type.kind === 'float' || right.type.kind === 'float') {
      throw mix64Error();
    }

    if (left.type.kind === 'string' || right.type.kind === 'string') {
      throw aotError(
        `AOT: "${source.op}" cannot mix a 64-bit time value with a string`,
        'keep both sides numbers, or write the branch out: {ms ? ms : 0}.',
      );
    }

    if (isBoolean(left.type) || isBoolean(right.type)) {
      throw bool64Error(`"${source.op}"`);
    }

    return {
      kind: 'pick',
      op: source.op,
      left,
      right,
      type: int64Join(left.type, right.type),
    };
  }

  // Otherwise the C result is the truth of the expression, a boolean only when both operands are.
  return {
    kind: 'logical',
    op: source.op,
    left,
    right,
    type: isBoolean(left.type) && isBoolean(right.type) ? BOOL : INT,
  };
}

/**
 * Types `test ? consequent : alternate`. Both branches share one C slot: float or 64-bit if either is, and a
 * 64-bit branch beside a float or a boolean one is refused.
 *
 * @param source  The expression.
 * @param math64  Whole-number math is worked out in 64 bits here.
 *
 * @returns The typed expression.
 */
function typeConditional(
  source: Extract<IrSource, {kind: 'conditional'}>,
  math64: boolean,
): IrExpr {
  const test = typeSource(source.test, math64);
  const [consequent, alternate] = litPeers(
    typeSource(source.consequent, math64),
    typeSource(source.alternate, math64),
  );
  // A string branch beside a numeric one is ill-typed C with no single printf spec, so refuse it here.
  if (
    (consequent.type.kind === 'string') !==
    (alternate.type.kind === 'string')
  ) {
    throw aotError(
      'AOT: a ternary cannot mix a string branch with a numeric one',
      "both branches must be the same kind — quote the number to keep it text, e.g. {ok ? '1' : 'none'}.",
    );
  }

  const eitherIs = (kind: CType) =>
    slotOf(consequent.type) === kind || slotOf(alternate.type) === kind;
  if (eitherIs('i64') && eitherIs('float')) {
    throw mix64Error();
  }

  if (
    eitherIs('i64') &&
    (isBoolean(consequent.type) || isBoolean(alternate.type))
  ) {
    throw bool64Error('a ternary');
  }

  const slot = eitherIs('float')
    ? 'float'
    : eitherIs('i64')
      ? 'i64'
      : slotOf(consequent.type) === slotOf(alternate.type)
        ? slotOf(consequent.type)
        : 'int';
  return {
    kind: 'conditional',
    test,
    consequent,
    alternate,
    type:
      slot === 'i64'
        ? int64Join(consequent.type, alternate.type)
        : typeOfSlot(
            slot,
            isBoolean(consequent.type) && isBoolean(alternate.type),
          ),
  };
}

/**
 * Types a Math call: exact integer division first, then 64-bit math, then whole-int abs/min/max/trunc, then
 * floats.
 *
 * @param source  The call.
 * @param math64  Whole-number math is worked out in 64 bits here.
 *
 * @returns The typed call.
 */
function typeMath(
  source: Extract<IrSource, {kind: 'math'}>,
  math64: boolean,
): IrExpr {
  const {fn} = source;
  if (source.exactDivision) {
    const division = source.args[0] as Extract<IrSource, {kind: 'binary'}>;
    const timeDiv = typeTimeRoundDiv(fn, division, math64);
    if (timeDiv) return timeDiv;

    const intDiv = typeIntRoundDiv(fn, division, math64);
    if (intDiv) return intDiv;
  }

  let argExprs = litPeers(...source.args.map(arg => typeSource(arg, math64)));
  if (argExprs.some(argExpr => argExpr.type.kind === 'int64')) {
    // Whole-number Math keeps a 64-bit value exact, and a time takes nothing else; the rest works on floats.
    if (WHOLE_MATH.has(fn) || argExprs.some(isTime64)) {
      return typeMath64(fn, argExprs);
    }

    argExprs = argExprs.map(
      (argExpr): IrExpr =>
        argExpr.type.kind === 'int64'
          ? {kind: 'toFloat', operand: argExpr, type: FLOAT}
          : argExpr,
    );
  }

  // Keep int abs/min/max whole (a float rounds past 2^24); with math64, abs widens to keep |INT_MIN|.
  if (argExprs.length && argExprs.every(argExpr => isIntSlot(argExpr.type))) {
    if (fn === 'abs' && argExprs.length === 1) {
      return math64
        ? {
            kind: 'helper',
            helper: 'abs',
            bits: 64,
            args: [argExprs[0]],
            type: INT64_MATH,
          }
        : {
            kind: 'helper',
            helper: 'abs',
            bits: 32,
            args: [argExprs[0]],
            type: INT,
          };
    }

    if ((fn === 'min' || fn === 'max') && argExprs.length === 2) {
      return {
        kind: 'helper',
        helper: fn,
        bits: 32,
        args: [argExprs[0], argExprs[1]],
        type: INT,
      };
    }
  }

  // Math.trunc stays an int: an int as is, a float via app_f2i (toward zero, NaN as 0, saturating).
  if (fn === 'trunc' && argExprs.length === 1) {
    return isIntSlot(argExprs[0].type)
      ? {kind: 'wholeRound', operand: argExprs[0], type: INT}
      : {kind: 'trunc', operand: argExprs[0], type: INT};
  }

  if (FLOAT_MATH_UNARY.has(fn) && argExprs.length === 1) {
    // An int is already whole, so rounding leaves it as it is; a float round trip would lose digits.
    const roundsToWhole = fn === 'round' || fn === 'floor' || fn === 'ceil';
    if (roundsToWhole && isIntSlot(argExprs[0].type)) {
      return {kind: 'wholeRound', operand: argExprs[0], type: INT};
    }

    const floatCall: IrExpr = {
      kind: 'floatMath',
      fn: fn as FloatMath,
      args: [argExprs[0]],
      type: FLOAT,
    };
    // round/floor/ceil yield a whole number, kept as an int so %d / int assignments are correct.
    return roundsToWhole
      ? {kind: 'toInt', operand: floatCall, type: INT}
      : floatCall;
  }

  if (FLOAT_MATH_BINARY.has(fn) && argExprs.length === 2) {
    return {
      kind: 'floatMath',
      fn: fn as FloatMath,
      args: [argExprs[0], argExprs[1]],
      type: FLOAT,
    };
  }

  throw new Error(
    `AOT: unsupported Math.${fn}(...) (arity ${argExprs.length})`,
  );
}

/**
 * The typing pass over one source expression: gives it its type and decides how it is computed. Pure, so a
 * subexpression can be typed again, as whole-number math beside a 64-bit value is. An AOT error is pinned to
 * the expression it is about, as lowering it would have been.
 *
 * @param source  The expression.
 * @param math64  Whole-number math is worked out in 64 bits around it.
 *
 * @returns The typed expression.
 */
export function typeSource(source: IrSource, math64: boolean): IrExpr {
  // 64-bit int math reaches only through whole-number math (keepsMath64); other expressions are typed as usual.
  const wide = math64 && keepsMath64(source);
  try {
    switch (source.kind) {
      case 'number':
        return numConst(source.value);
      case 'string':
        return stringConst(source.value);
      case 'boolean':
        return boolConst(source.value);
      case 'state': {
        const {state} = source;
        const assumed =
          (state.cType === 'int' || state.cType === 'i64') && !state.isBool
            ? assumedWide?.get(state.cField)
            : undefined;
        return {
          kind: 'state',
          state,
          type: assumed ? int64Of(assumed) : stateType(state),
        };
      }
      case 'local':
        // While slot widths are inferred, a local is what it was set from, typed with the slots assumed so far.
        return assumedWide && source.local.source
          ? typeSource(source.local.source, false)
          : localRead(source.local);
      case 'ref': {
        const {ref} = source;
        const assumed =
          ref.kind === 'value' && (ref.cType === 'int' || ref.cType === 'i64')
            ? assumedWide?.get(ref.cVar)
            : undefined;
        return {
          kind: 'ref',
          ref,
          type: assumed ? int64Of(assumed) : refType(ref),
        };
      }
      case 'fixed':
        return source.expr;
      case 'unary':
        return typeUnary(source, wide);
      case 'binary':
        return typeBinary(source, wide);
      case 'logical':
        return typeLogical(source, wide);
      case 'conditional':
        return typeConditional(source, wide);
      case 'math':
        return typeMath(source, wide);
    }
  } catch (error) {
    locateError(error, source.loc);
    throw error;
  }
}

/**
 * Infers the slots that have to hold 64 bits: an int slot a value stored into is a 64-bit number, once the
 * slots found so far are taken to hold 64 bits too, until no more are found. Each one holds a time when a time
 * is stored into it, and only 64-bit numbers otherwise. A value whose typing fails leaves its slot as it is;
 * lowering it again reports the error.
 *
 * @param stores  Every value lowering stored into an int slot, or into a 64-bit one that holds no time yet.
 * @param wide  The slots already 64-bit.
 *
 * @returns Those slots, and every one inferred.
 */
export function inferWideSlots(
  stores: IrSlotStore[],
  wide: SlotWidths,
): SlotWidths {
  const inferred = new Map(wide);
  let grew = true;
  while (grew) {
    grew = false;
    for (const store of stores) {
      if (inferred.get(store.slot) === 'time') continue;
      assumedWide = inferred;
      try {
        const {type} = typeSource(store.value, store.math64);
        if (type.kind !== 'int64') continue;
        const holds = type.source === 'time' ? 'time' : 'number';
        if (holds === 'time' || !inferred.has(store.slot)) {
          inferred.set(store.slot, holds);
          grew = true;
        }
      } catch {
        // Lowering the value again, with the widths inferred, reports its error.
      } finally {
        assumedWide = null;
      }
    }
  }

  return inferred;
}

/**
 * Whether two sets of slot widths are the same.
 *
 * @param a  One set.
 * @param b  The other.
 *
 * @returns True when every slot holds the same in both.
 */
export const sameSlotWidths = (a: SlotWidths, b: SlotWidths): boolean =>
  a.size === b.size && [...a].every(([slot, holds]) => b.get(slot) === holds);

/**
 * Whether an operand is written as a number, so a boolean on the other side of `===` never equals it: a
 * numeric literal, a negated one, or a non-boolean state read by name.
 *
 * @param node  The operand.
 * @param env  The expression environment.
 *
 * @returns True when it is surely a number.
 */
const isWrittenAsNumber = (node: t.Node, env: Env): boolean =>
  node.type === 'NumericLiteral' ||
  (node.type === 'UnaryExpression' &&
    node.argument.type === 'NumericLiteral') ||
  (node.type === 'Identifier' &&
    !env.locals.has(node.name) &&
    (env.state.get(node.name) as ScalarState | undefined)?.isBool === false);

/**
 * A binary expression's source, from its operands' sources.
 *
 * @param node  The expression.
 * @param left  Its left operand's source.
 * @param right  Its right operand's source.
 * @param env  The expression environment.
 *
 * @returns The source.
 */
const binarySource = (
  node: t.BinaryExpression,
  left: IrSource,
  right: IrSource,
  env: Env,
): IrSource => ({
  kind: 'binary',
  op: node.operator,
  left,
  right,
  divisor: {constant: staticInt(node.right, env), loc: node.right.loc?.start},
  leftIsNumber: isWrittenAsNumber(node.left, env),
  rightIsNumber: isWrittenAsNumber(node.right, env),
  loc: node.loc?.start,
});

/**
 * Lowers one JS expression to its source: names resolved, constants folded, and each operand lowered and typed
 * in turn, so an error in it is reported before anything after it.
 *
 * @param node  The expression.
 * @param env  The expression environment.
 *
 * @returns The source.
 */
function sourceOf(node: t.Node, env: Env): IrSource {
  const loc = node.loc?.start;
  const operand = (operandNode: t.Node): IrSource =>
    lowerSource(operandNode, env).source;
  switch (node.type) {
    case 'NumericLiteral':
      return numberSource(node.value, loc);
    case 'StringLiteral':
      return {kind: 'string', value: node.value, loc};
    case 'BooleanLiteral':
      return {kind: 'boolean', value: node.value, loc};
    case 'Identifier': {
      // A name resolves to a local first, then to state, then to a compile-time constant.
      if (env.locals.has(node.name)) {
        return {kind: 'local', local: env.locals.get(node.name)!, loc};
      }

      if (env.state.has(node.name)) {
        const stateRecord = env.state.get(node.name)!;
        if (stateRecord.kind === 'list') {
          throw new Error(
            `AOT: a list state ("${node.name}") can only be used via .length or .map`,
          );
        }

        return {kind: 'state', state: stateRecord, loc};
      }

      if (node.name in env.consts) {
        const constValue = env.consts[node.name];
        if (typeof constValue === 'number')
          return numberSource(constValue, loc);

        if (typeof constValue === 'string') {
          return {kind: 'string', value: constValue, loc};
        }

        if (typeof constValue === 'boolean') {
          return {kind: 'boolean', value: constValue, loc};
        }
      }
      throw new Error(
        `AOT: cannot resolve identifier "${node.name}" in a dynamic expression`,
      );
    }
    case 'UnaryExpression': {
      // A constant is negated here, so `-A` is its value, and -2147483648 is not an int overflow.
      const staticOperand =
        node.operator === '-' ? staticInt(node.argument, env) : null;
      if (staticOperand !== null) return numberSource(-staticOperand, loc);

      const operandSource = operand(node.argument);
      if (
        node.operator === '-' ||
        node.operator === '+' ||
        node.operator === '!'
      ) {
        return {kind: 'unary', op: node.operator, operand: operandSource, loc};
      }

      throw new Error(`AOT: unsupported unary operator "${node.operator}"`);
    }
    case 'BinaryExpression': {
      // Fold `+ - *` over two whole-number constants as JS would, so a product past int range stays exact.
      const leftConst = CHECKED_HELPER[node.operator]
        ? staticInt(node.left, env)
        : null;
      const rightConst = leftConst === null ? null : staticInt(node.right, env);
      if (rightConst !== null) {
        return numberSource(
          node.operator === '+'
            ? leftConst! + rightConst
            : node.operator === '-'
              ? leftConst! - rightConst
              : leftConst! * rightConst,
          loc,
        );
      }

      return binarySource(node, operand(node.left), operand(node.right), env);
    }
    case 'LogicalExpression': {
      // Only `&&` and `||` are supported (not `??`).
      const operator =
        node.operator === '&&' || node.operator === '||' ? node.operator : null;
      if (!operator) {
        throw new Error(`AOT: unsupported logical operator "${node.operator}"`);
      }

      return {
        kind: 'logical',
        op: operator,
        left: operand(node.left),
        right: operand(node.right),
        loc,
      };
    }
    case 'ConditionalExpression':
      return {
        kind: 'conditional',
        test: operand(node.test),
        consequent: operand(node.consequent),
        alternate: operand(node.alternate),
        loc,
      };
    case 'MemberExpression': {
      // Static fold: member access that resolves to a compile-time constant (e.g., a .map item's `.key`).
      try {
        const constValue = evalStatic(node, foldScope(env, env.consts ?? {}));
        // A number with no float form falls through too, so its check runs now, not in the check pass.
        if (typeof constValue === 'number') {
          return attemptLowering(() => numberSource(constValue, loc));
        }

        if (typeof constValue === 'string') {
          return {kind: 'string', value: constValue, loc};
        }

        if (typeof constValue === 'boolean') {
          return {kind: 'boolean', value: constValue, loc};
        }
      } catch {
        /* not static — fall through to the dynamic member forms below */
      }

      // Otherwise the object must be one of the runtime forms below; `prop` is null for a computed member.
      const object = node.object;
      const prop = node.computed ? null : (node.property as t.Identifier).name;
      const fixed = (expr: IrExpr): IrSource => ({kind: 'fixed', expr, loc});
      // `<list>.length` → the runtime count.
      if (
        object.type === 'Identifier' &&
        env.state.get(object.name)?.kind === 'list' &&
        prop === 'length'
      ) {
        return fixed({
          kind: 'listLength',
          list: env.state.get(object.name) as ListState,
          type: INT,
        });
      }

      // `<item>.field` where item is a struct local (a list row's bound element).
      if (
        object.type === 'Identifier' &&
        env.locals.get(object.name)?.struct &&
        prop
      ) {
        const item = env.locals.get(object.name)!;
        const field = item.struct!.fields.find(
          structField => structField.key === prop,
        );
        if (!field) {
          throw new Error(`AOT: unknown field "${prop}" on a list item`);
        }

        return fixed({
          kind: 'itemField',
          item,
          field,
          type: typeOfSlot(field.kind),
        });
      }

      // `<ref>.current` — a value ref's mutable C slot.
      if (
        object.type === 'Identifier' &&
        env.refs?.has(object.name) &&
        prop === 'current'
      ) {
        const ref = env.refs.get(object.name)!;
        ref.used = true;
        return {kind: 'ref', ref, loc};
      }

      // `<event>.x / .y / .dx / .dy` — touch fields of the handler's EREventData.
      if (
        object.type === 'Identifier' &&
        env.event === object.name &&
        (prop === 'x' || prop === 'y' || prop === 'dx' || prop === 'dy')
      ) {
        return fixed({kind: 'event', field: prop, type: INT});
      }

      // `<event>.vx / .vy` — finger velocity (px/ms) at the last move; the engine measures it, a handler cannot.
      if (
        object.type === 'Identifier' &&
        env.event === object.name &&
        (prop === 'vx' || prop === 'vy')
      ) {
        return fixed({kind: 'event', field: prop, type: FLOAT});
      }

      // `<gestureState>.…` — the second argument of a PanResponder callback (see emitPanResponder).
      if (object.type === 'Identifier' && env.gesture === object.name && prop) {
        return fixed(lowerGestureField(prop, env.pan));
      }

      // `<event>.layout.x / .y / .width / .height` — the onLayout rect.
      if (
        object.type === 'MemberExpression' &&
        !object.computed &&
        object.object.type === 'Identifier' &&
        env.event === object.object.name &&
        (object.property as t.Identifier).name === 'layout'
      ) {
        if (!LAYOUT_FIELDS.has(prop as string)) {
          throw new Error(
            `AOT: unknown onLayout rect field "${prop}" (use x / y / width / height)`,
          );
        }
        return fixed({kind: 'layout', field: prop as LayoutField, type: INT});
      }

      // `Math.PI` — the only Math constant.
      if (
        object.type === 'Identifier' &&
        object.name === 'Math' &&
        prop === 'PI'
      ) {
        return fixed({kind: 'pi', type: FLOAT});
      }
      throw aotError(
        'AOT: unsupported member expression in a dynamic context',
        'in a handler or dynamic expression you can read state, `ref.current`, a `.map` item ' +
          'field, event fields (e.x / e.y / e.dx / e.dy / e.vx / e.vy / e.layout.*), and ' +
          'Math.PI — other member access must be a compile-time constant.',
      );
    }
    case 'CallExpression': {
      const callee = node.callee;
      // Date.now() / performance.now() → the engine clock, as 64-bit whole milliseconds.
      if (
        callee.type === 'MemberExpression' &&
        !callee.computed &&
        callee.object.type === 'Identifier' &&
        (callee.object.name === 'Date' ||
          callee.object.name === 'performance') &&
        !env.shadowedClock?.has(node)
      ) {
        if ((callee.property as t.Identifier).name === 'now') {
          return {
            kind: 'fixed',
            expr: {kind: 'clock', clock: callee.object.name, type: TIME},
            loc,
          };
        }
        if (callee.object.name === 'Date') {
          throw dateObjectError();
        }
      }

      if (
        callee.type === 'Identifier' &&
        callee.name === 'Date' &&
        !env.shadowedClock?.has(node)
      ) {
        throw dateObjectError();
      }

      // Math.*: a division under floor / ceil / round / trunc stays apart, for the typing pass to divide exactly.
      if (
        callee.type === 'MemberExpression' &&
        (callee.object as t.Identifier).name === 'Math'
      ) {
        const fn = (callee.property as t.Identifier).name;
        const division = node.arguments[0];
        const exactDivision =
          INT_ROUND_DIV.has(fn) &&
          node.arguments.length === 1 &&
          division.type === 'BinaryExpression' &&
          division.operator === '/';
        return {
          kind: 'math',
          fn,
          args: exactDivision
            ? [
                binarySource(
                  division as t.BinaryExpression,
                  operand((division as t.BinaryExpression).left),
                  operand((division as t.BinaryExpression).right),
                  env,
                ),
              ]
            : node.arguments.map(operand),
          exactDivision,
          wholeMath: keepsMath64Node(node),
          loc,
        };
      }

      throw new Error(
        'AOT: unsupported call expression in a dynamic expression',
      );
    }
    case 'NewExpression':
      // `new Date(…)` gets the Date-specific error; any other constructor gets the generic one below.
      if (
        node.callee.type === 'Identifier' &&
        node.callee.name === 'Date' &&
        !env.shadowedClock?.has(node)
      ) {
        throw dateObjectError();
      }
      break;
  }

  throw new Error(
    `AOT: unsupported expression "${node.type}" in a dynamic context`,
  );
}

/**
 * Lowers one JS expression to its source and types it, with the expression's location attached to any AOT
 * error either step throws.
 */
export const lowerSource = withLoc(
  (node: t.Node, env: Env): {source: IrSource; expr: IrExpr} => {
    // Below a node that is not whole-number math, its operands are typed as usual (see keepsMath64Node).
    if (env.math64 && !keepsMath64Node(node)) {
      env = {...env, math64: false};
    }

    const source = sourceOf(node, env);
    return {source, expr: typeSource(source, env.math64 ?? false)};
  },
);

/**
 * Lowers one JS expression to the IR: its source, typed by the typing pass.
 *
 * @param node  The expression.
 * @param env  The expression environment.
 *
 * @returns The typed expression.
 */
export const lowerExprWide = (node: t.Node, env: Env): IrExpr =>
  lowerSource(node, env).expr;

/**
 * emitExpr for the destinations that can hold a 64-bit timestamp: state, refs, locals, text, conditions.
 *
 * @param node  The expression.
 * @param env  The expression environment.
 *
 * @returns The expression as C.
 */
export const emitExprWide = (node: t.Node, env: Env): CExpr =>
  toCExpr(lowerExprWide(node, env));

/**
 * emitExprWide, for a local that holds the value: with the source it was lowered from, which the typing pass
 * reads when it infers slot widths.
 *
 * @param node  The expression.
 * @param env  The expression environment.
 *
 * @returns The expression as C, and its source.
 */
export function emitSourcedWide(
  node: t.Node,
  env: Env,
): CExpr & {source: IrSource} {
  const lowered = lowerSource(node, env);
  return {...toCExpr(lowered.expr), source: lowered.source};
}

/**
 * Lowers an expression for a destination that holds an int, a float, or a string. C would narrow a 64-bit
 * value (a Date.now() / performance.now() timestamp, or a whole number past the int range) into one of those
 * without a warning, so it is refused here; the destinations that hold 64 bits call lowerExprWide.
 *
 * The result carries its type, so the caller can pick the right printf spec or assignment. A destination
 * that holds text lowers through lowerText instead.
 *
 * @param node  The expression.
 * @param env  The expression environment.
 *
 * @returns The lowered expression; never 64-bit.
 */
export function lowerExpr(node: t.Node, env: Env): IrExpr {
  const expr = lowerExprWide(node, env);
  if (expr.type.kind !== 'int64') return expr;

  const error = aotError(
    'AOT: a 64-bit value (Date.now() / performance.now(), or a whole number past ±2^31) cannot be used here',
    'keep it in state, a ref or a local, compare it, or show it in text. Anywhere else, ' +
      'reduce it to a small number first — e.g. `ms % 1000`, or `Math.floor(ms / 1000) % ' +
      '60`.',
  );
  if (node.loc) {
    error.aotLoc = node.loc.start;
  }

  throw error;
}

/**
 * lowerExpr, as C: what the emitters that do not build the IR yet use.
 *
 * @param node  The expression.
 * @param env  The expression environment.
 *
 * @returns The expression as C; never 64-bit.
 */
export function emitExpr(node: t.Node, env: Env): CExpr {
  return toCExpr(lowerExpr(node, env));
}

/**
 * An aotError pinned to the expression that cannot be lowered to text (lowerTextParts is not withLoc-wrapped).
 *
 * @param node  The expression.
 * @param message  What is not supported.
 * @param hint  How to rewrite it.
 *
 * @returns The error, for the caller to throw.
 */
function textShapeError(node: t.Node, message: string, hint: string): AotError {
  const error = aotError(message, hint);
  if (node.loc) {
    error.aotLoc = node.loc.start;
  }

  return error;
}

/**
 * Text a constant renders as a standalone JSX child. React draws nothing for null, undefined, or a
 * boolean (see flattenTextChildren in Flow A), which is not how `+` treats the same values — a
 * concatenation operand goes through String() instead.
 *
 * @param value  The constant.
 *
 * @returns The text React would render for it.
 */
export const jsxChildText = (value: unknown): string =>
  value === undefined || value === null || typeof value === 'boolean'
    ? ''
    : String(value);

/**
 * Splits a string-building `+` chain into text parts, following JS's own left-to-right typing: a `+`
 * is a concatenation only once one of its sides is a string, so `n + 1 + 'ms'` still adds before it
 * appends. Each part is literal text, a runtime value, or a boolean printed as a word.
 *
 * @param node  The expression.
 * @param env  The expression environment.
 * @param scope  The constants a fold may read (already filtered by foldScope).
 * @param isOperand  The expression is an operand of `+`, where null / undefined / booleans print as words.
 *
 * @returns The parts, and whether JS would treat the expression as a string.
 */
function lowerTextParts(
  node: t.Node,
  env: Env,
  scope: Scope,
  isOperand = false,
): TextParts {
  // `undefined` is "" as a child but "undefined" in a `+`; handled here, not in the fold every prop reader shares.
  if (node.type === 'Identifier' && node.name === 'undefined') {
    return {
      isString: false,
      parts: [{kind: 'literal', text: isOperand ? 'undefined' : ''}],
    };
  }

  // A compile-time constant becomes literal text.
  try {
    const constValue = evalStatic(node, scope);
    return {
      isString: typeof constValue === 'string',
      parts: [
        {
          kind: 'literal',
          text: isOperand ? String(constValue) : jsxChildText(constValue),
        },
      ],
    };
  } catch {
    /* not a compile-time constant — split it below */
  }

  // A `+` with a string on either side is a concatenation: its parts are both sides' parts in order.
  if (node.type === 'BinaryExpression' && node.operator === '+') {
    const leftParts = lowerTextParts(node.left, env, scope, true);
    const rightParts = lowerTextParts(node.right, env, scope, true);
    if (leftParts.isString || rightParts.isString) {
      return {isString: true, parts: [...leftParts.parts, ...rightParts.parts]};
    }
  }

  // A branch or operand that concatenates needs its own format; refuse before lowerExpr's misleading error.
  const concatenates = (subExpr: t.Node) =>
    subExpr.type === 'BinaryExpression' &&
    subExpr.operator === '+' &&
    lowerTextParts(subExpr, env, scope, true).isString;
  if (
    node.type === 'ConditionalExpression' &&
    (concatenates(node.test) ||
      concatenates(node.consequent) ||
      concatenates(node.alternate))
  ) {
    throw textShapeError(
      node,
      'AOT: a ternary in text cannot concatenate inside its test or a branch',
      'each branch must be a single value — a literal, a state, or a number. Build the ' +
        "joined string first (a string useState set from a handler), or split it: {on ? 'a' : " +
        "'b'}{on ? s : ''}.",
    );
  }

  if (
    node.type === 'LogicalExpression' &&
    (concatenates(node.left) || concatenates(node.right))
  ) {
    throw textShapeError(
      node,
      `AOT: "${node.operator}" in text cannot concatenate inside an operand`,
      "write it with plain values — {ok ? 'n=' : ''}{ok ? n : ''} — or build the joined " +
        'string first in a string useState.',
    );
  }

  // Anything else is one runtime value.
  const value = lowerExprWide(node, env);

  // C collapses `&&`/`||` to 0/1 and a ternary to one slot, so text matches JS only when operands agree in kind.
  if (node.type === 'LogicalExpression' && !isBoolean(value.type)) {
    throw textShapeError(
      node,
      `AOT: "${node.operator}" in text evaluates to one of its operands, not to true/false`,
      `\`a ${node.operator} b\` is a or b unless both sides are already booleans — the ` +
        `generated C only has 0/1. Write the branch out instead: {cond ? 'yes' : ''}.`,
    );
  }

  if (
    node.type === 'ConditionalExpression' &&
    isBoolean(lowerExprWide(node.consequent, env).type) !==
      isBoolean(lowerExprWide(node.alternate, env).type)
  ) {
    throw textShapeError(
      node,
      'AOT: a ternary in text mixes a boolean branch with a non-boolean one',
      'JS renders those differently (`cond ? true : 5` is "true" or "5") but they share one ' +
        'C slot here. Make both branches the same kind.',
    );
  }

  // A boolean prints nothing as a child, "true"/"false" as an operand; isString stays false so `on + n` adds.
  if (isBoolean(value.type)) {
    return isOperand
      ? {isString: false, parts: [{kind: 'booleanWord', value}]}
      : {isString: false, parts: [{kind: 'literal', text: ''}]};
  }

  return {
    isString: value.type.kind === 'string',
    parts: [{kind: 'value', value}],
  };
}

/**
 * Lowers an expression to text for a char-buffer destination (a <Text> body, a string state slot, a
 * <TextInput value>). A string-building `+` chain becomes its parts in order; anything else is one value.
 *
 * @param node  The expression.
 * @param env  The expression environment.
 * @param scope  The constants a fold may read; by default the env's.
 *
 * @returns The text.
 */
export function lowerText(
  node: t.Node,
  env: Env,
  scope: Scope = env.consts ?? {},
): IrText {
  return {parts: lowerTextParts(node, env, foldScope(env, scope)).parts};
}

/**
 * lowerText, printed as one printf format and its arguments (one spec per runtime value, literals folded in).
 *
 * @param node  The expression.
 * @param env  The expression environment.
 * @param scope  The constants a fold may read; by default the env's.
 *
 * @returns The format, with literal `%` already escaped as `%%`, and its arguments.
 */
export function emitFormat(
  node: t.Node,
  env: Env,
  scope: Scope = env.consts ?? {},
): FormatResult {
  return printText(lowerText(node, env, scope));
}
