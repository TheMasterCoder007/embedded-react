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
import {
  ENGINE_STRING_MAX,
  aotError,
  locateError,
  withLoc,
} from './diagnostics.mts';
import {
  bootSlotOf,
  containsBoot,
  foldBoot,
  isBoot,
  wholeBootValue,
} from './boot.mts';
import {
  evalStatic,
  fixedDigitsProblem,
  foldScope,
  hasBinding,
  padFillProblem,
  padWidthProblem,
} from './static-eval.mts';
import {lowerGestureField} from './pan-responder.mts';
import type * as t from '@babel/types';
import type {BootValue} from './boot.mts';
import type {AotError, SourcePosition} from './diagnostics.mts';
import type {IrSlotStore, IrSource, IrSourceDivisor} from './ir/source.mts';
import type {
  SlotKind,
  Env,
  ListState,
  Local,
  ScalarState,
  Scope,
  SlotValue,
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
  typeOfSlot,
  typeOfSlotValue,
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
const ARITHMETIC = new Set(['+', '-', '*', '/', '%']);
const COMPARE = new Set(['<', '>', '<=', '>=', '==', '!=', '===', '!==']);

/** The saturating helper each whole-number operator lowers to in the IR. */
const CHECKED_HELPER: Record<string, WholeHelper> = {
  '+': 'add',
  '-': 'sub',
  '*': 'mul',
};

/** Expressions an app may write over values worked out at boot, which then read as one boot slot. */
const WHOLE_BOOT_TYPES = new Set([
  'BinaryExpression',
  'UnaryExpression',
  'LogicalExpression',
  'ConditionalExpression',
  'CallExpression',
]);

/** The range of an int. */
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
const INT_ROUND_DIVISION = new Map<string, WholeHelper>([
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

/** The largest 32-bit int. */
const INT32_MAX = 0x7fffffff;

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

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
 * @param environment  The expression environment.
 *
 * @returns The integer, or null when the expression is not one.
 */
export function staticInt(node: t.Node, environment: Env): number | null {
  try {
    const value = evalStatic(
      node,
      foldScope(environment, environment.constants ?? {}),
      'strict',
    );
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
 * @param location  Where it is.
 *
 * @returns Its source.
 */
function numberSource(
  value: number,
  location: SourcePosition | undefined,
): IrSource {
  if (numConst(value).type.kind === 'float') {
    checkNumber(value);
  }

  return {kind: 'number', value, location};
}

/**
 * A read of a value worked out at boot. One boot cannot work out is taken at the build's panel instead, which
 * the panel at boot must then give too.
 *
 * @param value  The value.
 * @param location  Where it is read.
 *
 * @returns Its source.
 */
function bootSource(
  value: BootValue,
  location: SourcePosition | undefined,
): IrSource {
  const slot = bootSlotOf(value);
  if (slot) {
    return {
      kind: 'fixed',
      expression: {kind: 'boot', slot, type: slot.type},
      location,
    };
  }

  const buildValue = foldBoot(value);
  if (typeof buildValue === 'number') return numberSource(buildValue, location);
  if (typeof buildValue === 'string') {
    return {kind: 'string', value: buildValue, location};
  }

  if (typeof buildValue === 'boolean') {
    return {kind: 'boolean', value: buildValue, location};
  }

  throw new Error(
    'AOT: a value derived from the panel size cannot be used in a dynamic expression',
  );
}

/**
 * Whether an expression names a binding that is, or holds, a value worked out at boot.
 *
 * @param node  The expression.
 * @param scope  The constants it reads.
 *
 * @returns True when it does.
 */
function readsBootName(node: unknown, scope: Scope): boolean {
  if (typeof node !== 'object' || node === null) return false;
  if (Array.isArray(node)) return node.some(item => readsBootName(item, scope));
  const typed = node as t.Node;
  if (typed.type === 'Identifier') {
    return hasBinding(scope, typed.name) && containsBoot(scope[typed.name]);
  }

  // A member's name and an object key are not reads.
  if (typed.type === 'MemberExpression') {
    return (
      readsBootName(typed.object, scope) ||
      (typed.computed && readsBootName(typed.property, scope))
    );
  }

  if (typed.type === 'ObjectProperty' && !typed.computed) {
    return readsBootName(typed.value, scope);
  }

  return Object.entries(typed).some(
    ([key, value]) => key !== 'loc' && readsBootName(value, scope),
  );
}

/**
 * An expression that depends only on the panel, read as a whole from its boot slot, so it is worked out once and
 * in double precision. Null for one that also reads state, or that boot cannot work out.
 *
 * @param node  The expression.
 * @param environment  The expression environment.
 * @param location  Where it is.
 *
 * @returns Its source, or null.
 */
function wholeBootSource(
  node: t.Node,
  environment: Env,
  location: SourcePosition | undefined,
): IrSource | null {
  const scope = foldScope(environment, environment.constants ?? {});
  if (!readsBootName(node, scope)) return null;
  let value: unknown;
  try {
    value = evalStatic(node, scope, 'boot');
  } catch {
    return null;
  }

  if (!isBoot(value) || !value.lowerable) return null;
  const whole = wholeBootValue(node, scope, value);
  const slot = whole && bootSlotOf(whole);
  return slot
    ? {
        kind: 'fixed',
        expression: {kind: 'boot', slot, type: slot.type},
        location,
      }
    : null;
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
  type: typeOfSlotValue(local),
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
    (Boolean(CHECKED_HELPER[source.operator]) || source.operator === '%')) ||
  (source.kind === 'unary' && source.operator === '-') ||
  (source.kind === 'math' && source.wholeMath);

/**
 * A 64-bit value computed at runtime from a timestamp, as opposed to a 64-bit constant or whole-number math
 * widened to 64 bits beside one.
 *
 * @param expression  The lowered expression.
 *
 * @returns Whether it is a timestamp.
 */
export const isTime64 = (expression: IrTyped): boolean =>
  isTime(expression.type);

/**
 * Besides a float, a 64-bit number that is not a time converts, as in JS; only a timestamp refuses.
 *
 * @param operands  The operands.
 *
 * @returns The operands, with any such number converted to float when one of them is a float.
 */
const litPeers = (...operands: IrExpr[]): IrExpr[] =>
  operands.some(operand => operand.type.kind === 'float')
    ? operands.map((operand): IrExpr =>
        isConvertible64(operand.type)
          ? {kind: 'toFloat', operand, type: FLOAT}
          : operand,
      )
    : operands;

/**
 * The error for a divisor that is not a constant where a time's has to be one.
 *
 * @param location  Where the divisor is.
 *
 * @returns The error, for the caller to throw.
 */
function nonConstantDivisorError(
  location: SourcePosition | undefined,
): AotError {
  const error = aotError(
    'AOT: a 64-bit time value can only be divided by a constant',
    'the divisor must be known at compile time — e.g. `ms % 1000`, `Math.floor(ms / ' +
      '60000)`. For a divisor from state, reduce the timestamp first: `(ms % 86400000) / ' +
      'period`.',
  );
  if (location) {
    error.aotLoc = location;
  }

  return error;
}

/**
 * The divisor of a timestamp's `%` or Math.floor / ceil / round / trunc(a / b), which has to be a constant;
 * for one from state, an app reduces the timestamp first. A constant 0 gives JS's answer as the int math
 * converts it: a remainder of 0, or a quotient saturated by the dividend's sign.
 *
 * @param node  The divisor.
 * @param environment  The expression environment.
 *
 * @returns The divisor's value.
 */
export function constDivisor(node: t.Node, environment: Env): number {
  const divisor = staticInt(node, environment);
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

  throw nonConstantDivisorError(divisor.location);
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

  if (source.operator === '/') {
    throw aotError(
      'AOT: `/` on a 64-bit time value is not supported',
      'JS division gives a fraction, which a 64-bit integer cannot hold. Use Math.floor(a / ' +
        'b) for whole units — e.g. Math.floor(ms / 1000) for seconds.',
    );
  }

  // With no timestamp on either side, this is whole-number math widened to 64 bits.
  const isWide = !isTime64(left) && !isTime64(right);

  // `%`: zero/±1 divisors give 0, a timestamp needs a constant divisor, and the result narrows to int if it fits.
  if (source.operator === '%') {
    const staticDivisor = source.divisor.constant;
    // JS gives NaN for a zero divisor, which is 0 here, as the int `%` has it.
    if (staticDivisor === 0) return numConst(0);

    if (isWide && staticDivisor === null) {
      return {
        kind: 'helper',
        helper: 'mod',
        bits: 64,
        callArguments: [left, right],
        type: INT64_MATH,
      };
    }

    const divisor = sourceDivisor(source.divisor);
    // ±1 leaves no remainder, and INT64_MIN % -1 overflows.
    if (divisor === 1 || divisor === -1) return numConst(0);

    if (Math.abs(divisor) <= INT32_MAX || isIntSlot(left.type)) {
      return {kind: 'narrowRemainder', left, right, type: INT};
    }

    return {
      kind: 'arithmetic',
      operator: '%',
      left,
      right,
      type: isWide ? INT64_MATH : TIME,
    };
  }

  // `+ - *`: the saturating 64-bit helper.
  return {
    kind: 'helper',
    helper: CHECKED_HELPER[source.operator],
    bits: 64,
    callArguments: [left, right],
    type: isWide ? INT64_MATH : TIME,
  };
}

/**
 * Math.floor / ceil / round / trunc over `a / b` with a 64-bit timestamp on either side, as exact integer
 * division by a constant (see constDivisor); a zero one saturates by the dividend's sign, as JS's ±Infinity
 * converts. Null when neither side is a timestamp, which leaves it to the whole-number or float path.
 *
 * @param mathFunction  The Math function's name.
 * @param division  Its one argument, the division.
 * @param math64  Whole-number math is worked out in 64 bits here.
 *
 * @returns The typed call, or null when it is not this case.
 */
function typeTimeRoundDiv(
  mathFunction: string,
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

  // Dividing by -1 is a negation, which saturates; a plain `/` would overflow on INT64_MIN / -1.
  if (sourceDivisor(division.divisor) === -1) {
    return {
      kind: 'helper',
      helper: 'neg',
      bits: 64,
      callArguments: [dividend],
      type: TIME,
    };
  }

  return {
    kind: 'helper',
    helper: INT_ROUND_DIVISION.get(mathFunction)!,
    bits: 64,
    callArguments: [dividend, divisor],
    type: TIME,
  };
}

/**
 * Math.floor / ceil / round / trunc over `a / b` with two whole numbers, as exact integer division: the float
 * path rounds an operand past 2^24, so Math.floor(16777217 / 1) would come out 16777216. A zero divisor keeps
 * JS's answer as an int (±Infinity saturates by the dividend's sign, 0 / 0 is 0), and
 * INT_MIN / -1 saturates too. Beside a 64-bit value, or with a 64-bit operand that is not a timestamp (a
 * constant past the int range, or math already widened), it divides in 64 bits, since JS would not cut the
 * operands to 32. Null for anything else, which leaves it to the float path.
 *
 * @param mathFunction  The Math function's name.
 * @param division  Its one argument, the division.
 * @param math64  Whole-number math is worked out in 64 bits here.
 *
 * @returns The typed call, or null when it is not this case.
 */
function typeIntRoundDiv(
  mathFunction: string,
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
      helper: INT_ROUND_DIVISION.get(mathFunction)!,
      bits: 64,
      callArguments: [dividend, divisor],
      type: INT64_MATH,
    };
  }

  // Otherwise divide two ints in 32 bits; anything else is left to the float path.
  if (!isIntSlot(dividend.type) || !isIntSlot(divisor.type)) return null;
  return {
    kind: 'helper',
    helper: INT_ROUND_DIVISION.get(mathFunction)!,
    bits: 32,
    callArguments: [dividend, divisor],
    type: INT,
  };
}

/**
 * Math.* over a 64-bit timestamp: only what stays exact in whole numbers.
 *
 * @param mathFunction  The Math function's name.
 * @param argumentExpressions  Its typed arguments, at least one of them 64-bit.
 *
 * @returns The typed call.
 */
function typeMath64(
  mathFunction: string,
  argumentExpressions: IrExpr[],
): IrExpr {
  if (
    argumentExpressions.some(
      argumentExpression => argumentExpression.type.kind === 'float',
    )
  ) {
    throw mix64Error();
  }

  if (
    argumentExpressions.every(
      argumentExpression =>
        isIntSlot(argumentExpression.type) ||
        argumentExpression.type.kind === 'int64',
    )
  ) {
    // Whole-number math with no timestamp in it stays whole-number math.
    const isWide = !argumentExpressions.some(isTime64);
    // A timestamp is already whole, so rounding leaves it as it is.
    if (
      (mathFunction === 'floor' ||
        mathFunction === 'round' ||
        mathFunction === 'ceil' ||
        mathFunction === 'trunc') &&
      argumentExpressions.length === 1
    ) {
      return {
        kind: 'wholeRound',
        operand: argumentExpressions[0],
        type: isWide ? INT64_MATH : TIME,
      };
    }

    if (mathFunction === 'abs' && argumentExpressions.length === 1) {
      return {
        kind: 'helper',
        helper: 'abs',
        bits: 64,
        callArguments: [argumentExpressions[0]],
        type: isWide ? INT64_MATH : TIME,
      };
    }

    if (
      (mathFunction === 'min' || mathFunction === 'max') &&
      argumentExpressions.length === 2
    ) {
      return {
        kind: 'helper',
        helper: mathFunction,
        bits: 64,
        callArguments: [argumentExpressions[0], argumentExpressions[1]],
        type: isWide ? INT64_MATH : TIME,
      };
    }
  }

  throw aotError(
    `AOT: Math.${mathFunction}(...) on a 64-bit time value is not supported`,
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
    (source.operator === '-' || source.operator === '+') &&
    operand.type.kind === 'string'
  ) {
    throw aotError(
      `AOT: unary "${source.operator}" on a string is not supported`,
      "JS would coerce the string to a number; the AOT doesn't. Keep the operand numeric.",
    );
  }

  // Negating the minimum overflows, so a whole number saturates the way `+ - *` do.
  if (
    source.operator === '-' &&
    (isIntSlot(operand.type) || operand.type.kind === 'int64')
  ) {
    const is64Bit = operand.type.kind === 'int64' || math64;
    return is64Bit
      ? {
          kind: 'helper',
          helper: 'neg',
          bits: 64,
          callArguments: [operand],
          type: operand.type.kind === 'int64' ? operand.type : INT64_MATH,
        }
      : {
          kind: 'helper',
          helper: 'neg',
          bits: 32,
          callArguments: [operand],
          type: INT,
        };
  }

  // Only `!` yields a boolean; unary +/- coerce to a number, so `+flag` drops its boolean-ness.
  return {
    kind: 'unary',
    operator: source.operator,
    operand,
    type:
      source.operator === '!'
        ? BOOL
        : isBoolean(operand.type)
          ? INT
          : operand.type,
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
  if (ARITHMETIC.has(source.operator)) {
    if (left.type.kind === 'string' || right.type.kind === 'string') {
      // String `+` has no single value; only a destination that holds text can lower it, via lowerText().
      if (source.operator === '+') {
        throw aotError(
          'AOT: string concatenation is not supported in this position',
          'a `+` chain over strings only works where the value lands in text: a <Text> body, a string ' +
            'useState setter, or a <TextInput value>.',
        );
      }
      // JS would coerce the string to a number; the AOT doesn't.
      throw aotError(
        `AOT: "${source.operator}" on a string is not supported`,
        'keep both operands numeric — a string cannot be coerced to a number here.',
      );
    }
    // `/` takes a 64-bit constant as an ordinary number; only a runtime 64-bit value is refused it.
    if (
      isTime64(left) ||
      isTime64(right) ||
      ((left.type.kind === 'int64' || right.type.kind === 'int64') &&
        (CHECKED_HELPER[source.operator] || source.operator === '%'))
    ) {
      return typeArith64(source, left, right);
    }

    if (source.operator === '/') {
      return {kind: 'floatDivide', left, right, type: FLOAT};
    }

    const isFloat = left.type.kind === 'float' || right.type.kind === 'float';
    const checkedHelper = !isFloat && CHECKED_HELPER[source.operator];
    if (checkedHelper) {
      return math64
        ? {
            kind: 'helper',
            helper: checkedHelper,
            bits: 64,
            callArguments: [left, right],
            type: INT64_MATH,
          }
        : {
            kind: 'helper',
            helper: checkedHelper,
            bits: 32,
            callArguments: [left, right],
            type: INT,
          };
    }

    // A 0 divisor (NaN in JS) or -1 (INT_MIN % -1 overflows) gives 0; other constants need no check.
    if (!isFloat && source.operator === '%') {
      const staticDivisor = source.divisor.constant;
      if (staticDivisor === 0 || staticDivisor === -1) {
        return numConst(0);
      }
      if (staticDivisor === null) {
        return {
          kind: 'helper',
          helper: 'mod',
          bits: 32,
          callArguments: [left, right],
          type: INT,
        };
      }
    }

    // fmod matches JS's `%` exactly, zero and infinite divisors included.
    if (isFloat && source.operator === '%') {
      return {
        kind: 'floatMath',
        mathFunction: 'mod',
        callArguments: [left, right],
        type: FLOAT,
      };
    }

    return {
      kind: 'arithmetic',
      operator: source.operator as IrArithmetic['operator'],
      left,
      right,
      type: isFloat ? FLOAT : INT,
    };
  }

  // Comparisons: strings by their text, everything else as a plain comparison yielding a boolean.
  if (COMPARE.has(source.operator)) {
    const comparison =
      source.operator === '==='
        ? '=='
        : source.operator === '!=='
          ? '!='
          : source.operator;
    if (left.type.kind === 'string' || right.type.kind === 'string') {
      if (slotOf(left.type) !== slotOf(right.type)) {
        // A strict comparison never coerces, so a string against a number is decided statically.
        if (source.operator === '===' || source.operator === '!==') {
          return boolConst(source.operator === '!==');
        }

        throw aotError(
          'AOT: a string cannot be compared with a number',
          `\`${source.operator}\` coerces the string to a number in JS, which the AOT doesn't reproduce. ` +
            `Compare like with like, or use === / !== (which never coerce).`,
        );
      }
      // Equality is exact, but strcmp's UTF-8 order can differ from JS's UTF-16 order, so refuse ordering.
      if (comparison !== '==' && comparison !== '!=') {
        throw aotError(
          `AOT: ordering strings with "${source.operator}" is not supported`,
          'JS orders strings by UTF-16 code unit but the device holds UTF-8, so the order can ' +
            'differ. Use === / !==, or compare numbers.',
        );
      }

      return {
        kind: 'stringEquals',
        operator: comparison,
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

    // `true === 1` is false in JS but true in an int slot; fold it when the other side is surely a number.
    const isSurelyNumber = (isWrittenAsNumber: boolean, operand: IrExpr) =>
      !isBoolean(operand.type) &&
      (operand.type.kind === 'float' ||
        operand.type.kind === 'int64' ||
        isWrittenAsNumber);
    if (
      (source.operator === '===' || source.operator === '!==') &&
      ((isBoolean(left.type) && isSurelyNumber(source.rightIsNumber, right)) ||
        (isBoolean(right.type) && isSurelyNumber(source.leftIsNumber, left)))
    ) {
      return boolConst(source.operator === '!==');
    }

    return {
      kind: 'compare',
      operator: comparison as IrCompare['operator'],
      left,
      right,
      type: BOOL,
    };
  }

  throw new Error(`AOT: unsupported binary operator "${source.operator}"`);
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
        `AOT: "${source.operator}" cannot mix a 64-bit time value with a string`,
        'keep both sides numbers, or write the branch out: {ms ? ms : 0}.',
      );
    }

    if (isBoolean(left.type) || isBoolean(right.type)) {
      throw bool64Error(`"${source.operator}"`);
    }

    return {
      kind: 'pick',
      operator: source.operator,
      left,
      right,
      type: int64Join(left.type, right.type),
    };
  }

  // Otherwise the result is the truth of the expression, a boolean only when both operands are.
  return {
    kind: 'logical',
    operator: source.operator,
    left,
    right,
    type: isBoolean(left.type) && isBoolean(right.type) ? BOOL : INT,
  };
}

/**
 * Types `test ? consequent : alternate`. Both branches share one slot: float or 64-bit if either is, and a
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
  // A string branch beside a numeric one has no single type, so refuse it here.
  if (
    (consequent.type.kind === 'string') !==
    (alternate.type.kind === 'string')
  ) {
    throw aotError(
      'AOT: a ternary cannot mix a string branch with a numeric one',
      "both branches must be the same kind — quote the number to keep it text, e.g. {ok ? '1' : 'none'}.",
    );
  }

  const eitherIs = (kind: SlotKind) =>
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
  const {mathFunction} = source;

  // Math.min and Math.max take any number of values, the IR's two: the rest go pairwise, left to right.
  if (
    (mathFunction === 'min' || mathFunction === 'max') &&
    source.callArguments.length > 2
  ) {
    const [first, second, ...rest] = source.callArguments;
    return typeMath(
      rest.reduce<typeof source>(
        (pairs, argument) => ({...source, callArguments: [pairs, argument]}),
        {...source, callArguments: [first, second]},
      ),
      math64,
    );
  }

  if (source.exactDivision) {
    const division = source.callArguments[0] as Extract<
      IrSource,
      {kind: 'binary'}
    >;
    const timeDivision = typeTimeRoundDiv(mathFunction, division, math64);
    if (timeDivision) return timeDivision;

    const intDivision = typeIntRoundDiv(mathFunction, division, math64);
    if (intDivision) return intDivision;
  }

  let argumentExpressions = litPeers(
    ...source.callArguments.map(argument => typeSource(argument, math64)),
  );
  if (
    argumentExpressions.some(
      argumentExpression => argumentExpression.type.kind === 'int64',
    )
  ) {
    // Whole-number Math keeps a 64-bit value exact, and a time takes nothing else; the rest works on floats.
    if (WHOLE_MATH.has(mathFunction) || argumentExpressions.some(isTime64)) {
      return typeMath64(mathFunction, argumentExpressions);
    }

    argumentExpressions = argumentExpressions.map(
      (argumentExpression): IrExpr =>
        argumentExpression.type.kind === 'int64'
          ? {kind: 'toFloat', operand: argumentExpression, type: FLOAT}
          : argumentExpression,
    );
  }

  // Keep int abs/min/max whole (a float rounds past 2^24); with math64, abs widens to keep |INT_MIN|.
  if (
    argumentExpressions.length &&
    argumentExpressions.every(argumentExpression =>
      isIntSlot(argumentExpression.type),
    )
  ) {
    if (mathFunction === 'abs' && argumentExpressions.length === 1) {
      return math64
        ? {
            kind: 'helper',
            helper: 'abs',
            bits: 64,
            callArguments: [argumentExpressions[0]],
            type: INT64_MATH,
          }
        : {
            kind: 'helper',
            helper: 'abs',
            bits: 32,
            callArguments: [argumentExpressions[0]],
            type: INT,
          };
    }

    if (
      (mathFunction === 'min' || mathFunction === 'max') &&
      argumentExpressions.length === 2
    ) {
      return {
        kind: 'helper',
        helper: mathFunction,
        bits: 32,
        callArguments: [argumentExpressions[0], argumentExpressions[1]],
        type: INT,
      };
    }
  }

  // Math.trunc stays an int: an int as is, a float truncated (toward zero, NaN as 0, saturating).
  if (mathFunction === 'trunc' && argumentExpressions.length === 1) {
    return isIntSlot(argumentExpressions[0].type)
      ? {kind: 'wholeRound', operand: argumentExpressions[0], type: INT}
      : {kind: 'trunc', operand: argumentExpressions[0], type: INT};
  }

  if (FLOAT_MATH_UNARY.has(mathFunction) && argumentExpressions.length === 1) {
    // An int is already whole, so rounding leaves it as it is; a float round trip would lose digits.
    const roundsToWhole =
      mathFunction === 'round' ||
      mathFunction === 'floor' ||
      mathFunction === 'ceil';
    if (roundsToWhole && isIntSlot(argumentExpressions[0].type)) {
      return {kind: 'wholeRound', operand: argumentExpressions[0], type: INT};
    }

    const floatCall: IrExpr = {
      kind: 'floatMath',
      mathFunction: mathFunction as FloatMath,
      callArguments: [argumentExpressions[0]],
      type: FLOAT,
    };
    // round/floor/ceil yield a whole number, kept as an int so %d / int assignments are correct.
    return roundsToWhole
      ? {kind: 'toInt', operand: floatCall, type: INT}
      : floatCall;
  }

  if (FLOAT_MATH_BINARY.has(mathFunction) && argumentExpressions.length === 2) {
    return {
      kind: 'floatMath',
      mathFunction: mathFunction as FloatMath,
      callArguments: [argumentExpressions[0], argumentExpressions[1]],
      type: FLOAT,
    };
  }

  throw new Error(
    `AOT: unsupported Math.${mathFunction}(...) (arity ${argumentExpressions.length})`,
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
          (state.slotKind === 'int' || state.slotKind === 'i64') &&
          !state.isBoolean
            ? assumedWide?.get(state.slotName)
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
          ref.kind === 'value' &&
          (ref.slotKind === 'int' || ref.slotKind === 'i64')
            ? assumedWide?.get(ref.slotName)
            : undefined;
        return {
          kind: 'ref',
          ref,
          type: assumed ? int64Of(assumed) : refType(ref),
        };
      }
      case 'fixed':
        return source.expression;
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
    locateError(error, source.location);
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
 * @param left  One set.
 * @param right  The other.
 *
 * @returns True when every slot holds the same in both.
 */
export const sameSlotWidths = (left: SlotWidths, right: SlotWidths): boolean =>
  left.size === right.size &&
  [...left].every(([slot, holds]) => right.get(slot) === holds);

/**
 * Whether an operand is written as a number, so a boolean on the other side of `===` never equals it: a
 * numeric literal, a negated one, or a non-boolean state read by name.
 *
 * @param node  The operand.
 * @param environment  The expression environment.
 *
 * @returns True when it is surely a number.
 */
const isWrittenAsNumber = (node: t.Node, environment: Env): boolean =>
  node.type === 'NumericLiteral' ||
  (node.type === 'UnaryExpression' &&
    node.argument.type === 'NumericLiteral') ||
  (node.type === 'Identifier' &&
    !environment.locals.has(node.name) &&
    (environment.state.get(node.name) as ScalarState | undefined)?.isBoolean ===
      false);

/**
 * A binary expression's source, from its operands' sources.
 *
 * @param node  The expression.
 * @param left  Its left operand's source.
 * @param right  Its right operand's source.
 * @param environment  The expression environment.
 *
 * @returns The source.
 */
const binarySource = (
  node: t.BinaryExpression,
  left: IrSource,
  right: IrSource,
  environment: Env,
): IrSource => ({
  kind: 'binary',
  operator: node.operator,
  left,
  right,
  divisor: {
    constant: staticInt(node.right, environment),
    location: node.right.loc?.start,
  },
  leftIsNumber: isWrittenAsNumber(node.left, environment),
  rightIsNumber: isWrittenAsNumber(node.right, environment),
  location: node.loc?.start,
});

/**
 * Whether `node` is the handler's event, or its `nativeEvent` as React Native code reads it.
 *
 * @param node  The expression.
 * @param event  The event parameter's name, if the handler has one.
 *
 * @returns True for `<event>` or `<event>.nativeEvent`.
 */
function isEventRoot(node: t.Node, event: string | null | undefined): boolean {
  if (!event) return false;
  if (node.type === 'Identifier') return node.name === event;
  return (
    node.type === 'MemberExpression' &&
    !node.computed &&
    node.object.type === 'Identifier' &&
    node.object.name === event &&
    (node.property as t.Identifier).name === 'nativeEvent'
  );
}

/**
 * Lowers one JS expression to its source: names resolved, constants folded, and each operand lowered and typed
 * in turn, so an error in it is reported before anything after it.
 *
 * @param node  The expression.
 * @param environment  The expression environment.
 *
 * @returns The source.
 */
function sourceOf(node: t.Node, environment: Env): IrSource {
  const location = node.loc?.start;
  // An expression that depends only on the panel is read from a slot worked out at boot, in doubles as JS.
  const whole = WHOLE_BOOT_TYPES.has(node.type)
    ? wholeBootSource(node, environment, location)
    : null;
  if (whole) return whole;

  const operand = (operandNode: t.Node): IrSource =>
    lowerSource(operandNode, environment).source;
  switch (node.type) {
    case 'NumericLiteral':
      return numberSource(node.value, location);
    case 'StringLiteral':
      return {kind: 'string', value: node.value, location};
    case 'BooleanLiteral':
      return {kind: 'boolean', value: node.value, location};
    case 'Identifier': {
      // A name resolves to a local first, then to state, then to a compile-time constant.
      if (environment.locals.has(node.name)) {
        return {
          kind: 'local',
          local: environment.locals.get(node.name)!,
          location,
        };
      }

      if (environment.state.has(node.name)) {
        const stateRecord = environment.state.get(node.name)!;
        if (stateRecord.kind === 'list') {
          throw new Error(
            `AOT: a list state ("${node.name}") can only be used via .length or .map`,
          );
        }

        return {kind: 'state', state: stateRecord, location};
      }

      if (hasBinding(environment.constants, node.name)) {
        const constantValue = environment.constants[node.name];
        if (isBoot(constantValue)) return bootSource(constantValue, location);
        if (typeof constantValue === 'number')
          return numberSource(constantValue, location);

        if (typeof constantValue === 'string') {
          return {kind: 'string', value: constantValue, location};
        }

        if (typeof constantValue === 'boolean') {
          return {kind: 'boolean', value: constantValue, location};
        }
      }
      throw new Error(
        `AOT: cannot resolve identifier "${node.name}" in a dynamic expression`,
      );
    }
    case 'UnaryExpression': {
      // A constant is negated here, so `-A` is its value, and -2147483648 is not an int overflow.
      const staticOperand =
        node.operator === '-' ? staticInt(node.argument, environment) : null;
      if (staticOperand !== null) return numberSource(-staticOperand, location);

      const operandSource = operand(node.argument);
      if (
        node.operator === '-' ||
        node.operator === '+' ||
        node.operator === '!'
      ) {
        return {
          kind: 'unary',
          operator: node.operator,
          operand: operandSource,
          location,
        };
      }

      throw new Error(`AOT: unsupported unary operator "${node.operator}"`);
    }
    case 'BinaryExpression': {
      // Fold `+ - *` over two whole-number constants as JS would, so a product past int range stays exact.
      const leftConstant = CHECKED_HELPER[node.operator]
        ? staticInt(node.left, environment)
        : null;
      const rightConstant =
        leftConstant === null ? null : staticInt(node.right, environment);
      if (rightConstant !== null) {
        return numberSource(
          node.operator === '+'
            ? leftConstant! + rightConstant
            : node.operator === '-'
              ? leftConstant! - rightConstant
              : leftConstant! * rightConstant,
          location,
        );
      }

      return binarySource(
        node,
        operand(node.left),
        operand(node.right),
        environment,
      );
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
        operator: operator,
        left: operand(node.left),
        right: operand(node.right),
        location,
      };
    }
    case 'ConditionalExpression':
      return {
        kind: 'conditional',
        test: operand(node.test),
        consequent: operand(node.consequent),
        alternate: operand(node.alternate),
        location,
      };
    case 'MemberExpression': {
      // Static fold: member access that resolves to a compile-time constant (e.g., a .map item's `.key`).
      try {
        const constantValue = evalStatic(
          node,
          foldScope(environment, environment.constants ?? {}),
          'boot',
        );
        if (isBoot(constantValue)) return bootSource(constantValue, location);
        // A number with no float form falls through too, so its check runs now, not in the check pass.
        if (typeof constantValue === 'number') {
          return attemptLowering(() => numberSource(constantValue, location));
        }

        if (typeof constantValue === 'string') {
          return {kind: 'string', value: constantValue, location};
        }

        if (typeof constantValue === 'boolean') {
          return {kind: 'boolean', value: constantValue, location};
        }
      } catch {
        /* not static — fall through to the dynamic member forms below */
      }

      // Otherwise the object must be one of the runtime forms below; `prop` is null for a computed member.
      const object = node.object;
      const prop = node.computed ? null : (node.property as t.Identifier).name;
      const fixed = (expression: IrExpr): IrSource => ({
        kind: 'fixed',
        expression,
        location,
      });
      // `<list>.length` → the runtime count.
      if (
        object.type === 'Identifier' &&
        environment.state.get(object.name)?.kind === 'list' &&
        prop === 'length'
      ) {
        return fixed({
          kind: 'listLength',
          list: environment.state.get(object.name) as ListState,
          type: INT,
        });
      }

      // `<item>.field` where item is a struct local (a list row's bound element).
      if (
        object.type === 'Identifier' &&
        environment.locals.get(object.name)?.struct &&
        prop
      ) {
        const item = environment.locals.get(object.name)!;
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

      // `<ref>.current` — a value ref's mutable slot.
      if (
        object.type === 'Identifier' &&
        environment.refs?.has(object.name) &&
        prop === 'current'
      ) {
        const ref = environment.refs.get(object.name)!;
        ref.used = true;
        return {kind: 'ref', ref, location};
      }

      // `<event>.x / .y / .dx / .dy` — touch fields of the handler's EREventData.
      if (
        object.type === 'Identifier' &&
        environment.event === object.name &&
        (prop === 'x' || prop === 'y' || prop === 'dx' || prop === 'dy')
      ) {
        return fixed({kind: 'event', field: prop, type: INT});
      }

      // `<event>.vx / .vy` — finger velocity (px/ms) at the last move; the engine measures it, a handler cannot.
      if (
        object.type === 'Identifier' &&
        environment.event === object.name &&
        (prop === 'vx' || prop === 'vy')
      ) {
        return fixed({kind: 'event', field: prop, type: FLOAT});
      }

      // `<gestureState>.…` — the second argument of a PanResponder callback (see emitPanResponder).
      if (
        object.type === 'Identifier' &&
        environment.gesture === object.name &&
        prop
      ) {
        return fixed(lowerGestureField(prop, environment.pan));
      }

      // `<event>.layout.x / .y / .width / .height` — the onLayout rect, also read as
      // `<event>.nativeEvent.layout.…` as in React Native.
      if (
        object.type === 'MemberExpression' &&
        !object.computed &&
        (object.property as t.Identifier).name === 'layout' &&
        isEventRoot(object.object, environment.event)
      ) {
        if (!environment.layout) {
          throw aotError(
            'AOT: only an onLayout event carries a layout rect',
            'read e.layout (or e.nativeEvent.layout) inside an onLayout handler.',
          );
        }
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
        !environment.shadowedClock?.has(node)
      ) {
        if ((callee.property as t.Identifier).name === 'now') {
          return {
            kind: 'fixed',
            expression: {kind: 'clock', clock: callee.object.name, type: TIME},
            location,
          };
        }
        if (callee.object.name === 'Date') {
          throw dateObjectError();
        }
      }

      if (
        callee.type === 'Identifier' &&
        callee.name === 'Date' &&
        !environment.shadowedClock?.has(node)
      ) {
        throw dateObjectError();
      }

      // Math.*: a division under floor / ceil / round / trunc stays apart, for the typing pass to divide exactly.
      if (
        callee.type === 'MemberExpression' &&
        (callee.object as t.Identifier).name === 'Math'
      ) {
        const mathFunction = (callee.property as t.Identifier).name;
        const division = node.arguments[0];
        const exactDivision =
          INT_ROUND_DIVISION.has(mathFunction) &&
          node.arguments.length === 1 &&
          division.type === 'BinaryExpression' &&
          division.operator === '/';
        return {
          kind: 'math',
          mathFunction,
          callArguments: exactDivision
            ? [
                binarySource(
                  division as t.BinaryExpression,
                  operand((division as t.BinaryExpression).left),
                  operand((division as t.BinaryExpression).right),
                  environment,
                ),
              ]
            : node.arguments.map(operand),
          exactDivision,
          wholeMath: keepsMath64Node(node),
          location,
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
        !environment.shadowedClock?.has(node)
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
  (node: t.Node, environment: Env): {source: IrSource; expression: IrExpr} => {
    // Below a node that is not whole-number math, its operands are typed as usual (see keepsMath64Node).
    if (environment.math64 && !keepsMath64Node(node)) {
      environment = {...environment, math64: false};
    }

    const source = sourceOf(node, environment);
    return {
      source,
      expression: typeSource(source, environment.math64 ?? false),
    };
  },
);

/**
 * Lowers one JS expression to the IR: its source, typed by the typing pass.
 *
 * @param node  The expression.
 * @param environment  The expression environment.
 *
 * @returns The typed expression.
 */
export const lowerExprWide = (node: t.Node, environment: Env): IrExpr =>
  lowerSource(node, environment).expression;

/**
 * What a slot holding an expression's value records about it. A bare local is the Local itself, so a list row keeps
 * its struct (extractProps copies it into a prop).
 *
 * @param expression  The expression.
 *
 * @returns The slot value.
 */
export function slotValueOf(expression: IrExpr): SlotValue {
  if (expression.kind === 'local') {
    return expression.local;
  }

  const value: SlotValue = {slotKind: slotOf(expression.type)!};
  if (expression.type.kind === 'bool') {
    value.isBoolean = true;
  } else if (
    expression.type.kind === 'int64' &&
    expression.type.source !== 'time'
  ) {
    value[expression.type.source === 'number' ? 'isLiteral' : 'wide'] = true;
  }

  return value;
}

/**
 * lowerExprWide, for a local that holds the value: with the source it was lowered from, which the typing pass
 * reads when it infers slot widths.
 *
 * @param node  The expression.
 * @param environment  The expression environment.
 *
 * @returns The slot value, its source, and the typed expression.
 */
export function lowerSourcedWide(
  node: t.Node,
  environment: Env,
): SlotValue & {source: IrSource; expression: IrExpr} {
  const lowered = lowerSource(node, environment);
  return {
    ...slotValueOf(lowered.expression),
    source: lowered.source,
    expression: lowered.expression,
  };
}

/**
 * Lowers an expression for a destination that holds an int, a float, or a string. A 64-bit value (a Date.now() /
 * performance.now() timestamp, or a whole number past the int range) would be cut narrowing into one of those, so it
 * is refused here; the destinations that hold 64 bits call lowerExprWide.
 *
 * The result carries its type. A destination that holds text lowers through lowerText instead.
 *
 * @param node  The expression.
 * @param environment  The expression environment.
 *
 * @returns The lowered expression; never 64-bit.
 */
export function lowerExpr(node: t.Node, environment: Env): IrExpr {
  const expression = lowerExprWide(node, environment);
  if (expression.type.kind !== 'int64') return expression;

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
 * Whether an expression is a compile-time constant.
 *
 * @param node  The expression.
 * @param scope  The constants a fold may read.
 *
 * @returns True when it folds.
 */
function foldsStatically(node: t.Node, scope: Scope): boolean {
  try {
    evalStatic(node, scope, 'strict');
    return true;
  } catch {
    return false;
  }
}

/**
 * Whether an expression builds a string at run time: a template literal, a string helper, or a string `+`. Only a
 * destination that holds text can take one, through lowerText.
 *
 * @param node  The expression.
 * @param environment  The expression environment.
 *
 * @returns True when it builds a string.
 */
export function buildsText(node: t.Node, environment: Env): boolean {
  const scope = foldScope(environment, environment.constants ?? {});
  if (foldsStatically(node, scope)) return false;
  if (node.type === 'TemplateLiteral' || textCallOf(node, scope) !== null) {
    return true;
  }

  return (
    node.type === 'BinaryExpression' &&
    node.operator === '+' &&
    lowerTextParts(node, environment, scope, true).isString
  );
}

/**
 * The text helper a call is: `String(x)`, `x.toFixed(n)` or `x.padStart(n, fill)`.
 *
 * @param node  The expression.
 * @param scope  The constants a fold may read, which may shadow `String`.
 *
 * @returns The helper's name, or null for anything else.
 */
function textCallOf(
  node: t.Node,
  scope: Scope,
): 'String' | 'toFixed' | 'padStart' | null {
  if (node.type !== 'CallExpression') return null;
  const {callee} = node;
  if (
    callee.type === 'Identifier' &&
    callee.name === 'String' &&
    !hasBinding(scope, 'String')
  ) {
    return 'String';
  }

  if (
    callee.type === 'MemberExpression' &&
    !callee.computed &&
    callee.property.type === 'Identifier' &&
    (callee.property.name === 'toFixed' || callee.property.name === 'padStart')
  ) {
    return callee.property.name;
  }

  return null;
}

/**
 * A compile-time whole number a text helper takes.
 *
 * @param node  The argument, or undefined when it is left out.
 * @param fallback  The value JS uses when it is left out.
 * @param what  What the argument is, for errors.
 * @param environment  The expression environment.
 *
 * @returns The number.
 */
function helperInt(
  node: t.Node | undefined,
  fallback: number,
  what: string,
  environment: Env,
): number {
  if (!node) return fallback;
  const value = staticInt(node, environment);
  if (value === null) {
    throw textShapeError(
      node,
      `AOT: ${what} must be a compile-time whole number`,
      'the package bakes it into the instruction, so write a literal or a module constant.',
    );
  }

  return value;
}

/**
 * Lowers `x.toFixed(digits)` on a runtime number.
 *
 * @param call  The call.
 * @param environment  The expression environment.
 *
 * @returns The text: the number, printed with that many decimals.
 */
function lowerToFixed(call: t.CallExpression, environment: Env): TextParts {
  const receiver = (call.callee as t.MemberExpression).object;
  const value = lowerExprWide(receiver, environment);
  if (!['int', 'float', 'int64'].includes(value.type.kind)) {
    throw textShapeError(
      call,
      'AOT: toFixed is only supported on a number',
      'JS has toFixed on numbers only; call it on the number itself.',
    );
  }

  const digits = helperInt(
    call.arguments[0],
    0,
    "toFixed's digits",
    environment,
  );
  const problem = fixedDigitsProblem(digits);
  if (problem) {
    throw textShapeError(call, ...problem);
  }

  return {isString: true, parts: [{kind: 'value', value, digits}]};
}

/**
 * Lowers `x.padStart(width, fill)` on a runtime string.
 *
 * @param call  The call.
 * @param environment  The expression environment.
 * @param scope  The constants a fold may read.
 *
 * @returns The text: the string, padded.
 */
function lowerPadStart(
  call: t.CallExpression,
  environment: Env,
  scope: Scope,
): TextParts {
  const receiver = lowerTextParts(
    (call.callee as t.MemberExpression).object,
    environment,
    scope,
    true,
  );
  if (!receiver.isString) {
    throw textShapeError(
      call,
      'AOT: padStart is only supported on a string',
      'JS has padStart on strings only; convert first: String(n).padStart(2, "0").',
    );
  }

  // The same limits as a constant fold, checked before the receiver so a constant one gets the same error.
  const width = helperInt(
    call.arguments[0],
    0,
    "padStart's length",
    environment,
  );
  const widthProblem = padWidthProblem(width);
  if (widthProblem) {
    throw textShapeError(call, ...widthProblem);
  }

  const fillNode = call.arguments[1];
  let fill: unknown = ' ';
  if (fillNode) {
    try {
      fill = evalStatic(fillNode, scope, 'strict');
    } catch {
      fill = null;
    }
  }
  const fillProblem = padFillProblem(fill);
  if (fillProblem) {
    throw textShapeError(fillNode ?? call, ...fillProblem);
  }

  const pieces = receiver.parts.filter(
    part => !(part.kind === 'literal' && part.text === ''),
  );
  const [piece] = pieces;
  if (pieces.length !== 1 || piece.kind === 'literal') {
    throw textShapeError(
      call,
      'AOT: padStart can only pad one value',
      'pad each value on its own and join them after: `${String(h).padStart(2, "0")}:${String(m).padStart(2, "0")}`.',
    );
  }

  if (fill === '' || width <= 0) {
    return {isString: true, parts: [piece]};
  }

  return {
    isString: true,
    parts: [
      {...piece, pads: [...(piece.pads ?? []), {width, fill: fill as string}]},
    ],
  };
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
 * @param environment  The expression environment.
 * @param scope  The constants a fold may read (already filtered by foldScope).
 * @param isOperand  The expression is an operand of `+`, where null / undefined / booleans print as words.
 *
 * @returns The parts, and whether JS would treat the expression as a string.
 */
function lowerTextParts(
  node: t.Node,
  environment: Env,
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

  // A compile-time constant becomes literal text; a value worked out at boot is a runtime value.
  try {
    const constantValue = evalStatic(node, scope, 'strict');
    return {
      isString: typeof constantValue === 'string',
      parts: [
        {
          kind: 'literal',
          text: isOperand ? String(constantValue) : jsxChildText(constantValue),
        },
      ],
    };
  } catch {
    /* not a compile-time constant — split it below */
  }

  // A `+` with a string on either side is a concatenation: its parts are both sides' parts in order.
  if (node.type === 'BinaryExpression' && node.operator === '+') {
    const leftParts = lowerTextParts(node.left, environment, scope, true);
    const rightParts = lowerTextParts(node.right, environment, scope, true);
    if (leftParts.isString || rightParts.isString) {
      return {isString: true, parts: [...leftParts.parts, ...rightParts.parts]};
    }
  }

  // A template literal is its strings and values in order, each value printed as String() prints it.
  if (node.type === 'TemplateLiteral') {
    const parts: IrTextPart[] = [];
    node.quasis.forEach((quasi, index) => {
      parts.push({kind: 'literal', text: quasi.value.cooked ?? ''});
      if (index < node.expressions.length) {
        parts.push(
          ...lowerTextParts(node.expressions[index], environment, scope, true)
            .parts,
        );
      }
    });
    return {isString: true, parts};
  }

  // String(x) prints x as an operand of a string `+` would.
  if (textCallOf(node, scope) === 'String') {
    const [argument] = (node as t.CallExpression).arguments;
    if (argument?.type === 'SpreadElement') {
      throw textShapeError(
        node,
        'AOT: String(...) cannot take a spread',
        'pass the value itself: String(n).',
      );
    }

    return {
      isString: true,
      parts: argument
        ? lowerTextParts(argument, environment, scope, true).parts
        : [],
    };
  }

  if (textCallOf(node, scope) === 'toFixed') {
    return lowerToFixed(node as t.CallExpression, environment);
  }

  if (textCallOf(node, scope) === 'padStart') {
    return lowerPadStart(node as t.CallExpression, environment, scope);
  }

  // A branch or operand that builds text needs its own format; refuse before lowerExpr's misleading error.
  const concatenates = (subExpression: t.Node) =>
    (subExpression.type === 'BinaryExpression' &&
      subExpression.operator === '+' &&
      lowerTextParts(subExpression, environment, scope, true).isString) ||
    ((subExpression.type === 'TemplateLiteral' ||
      textCallOf(subExpression, scope) !== null) &&
      !foldsStatically(subExpression, scope));
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
  const value = lowerExprWide(node, environment);

  // `&&`/`||` collapse to 0/1 and a ternary to one slot, so text matches JS only when operands agree in kind.
  if (node.type === 'LogicalExpression' && !isBoolean(value.type)) {
    throw textShapeError(
      node,
      `AOT: "${node.operator}" in text evaluates to one of its operands, not to true/false`,
      `\`a ${node.operator} b\` is a or b unless both sides are already booleans — the ` +
        `package only has 0/1. Write the branch out instead: {cond ? 'yes' : ''}.`,
    );
  }

  if (
    node.type === 'ConditionalExpression' &&
    isBoolean(lowerExprWide(node.consequent, environment).type) !==
      isBoolean(lowerExprWide(node.alternate, environment).type)
  ) {
    throw textShapeError(
      node,
      'AOT: a ternary in text mixes a boolean branch with a non-boolean one',
      'JS renders those differently (`cond ? true : 5` is "true" or "5") but they share one ' +
        'slot here. Make both branches the same kind.',
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
 * @param environment  The expression environment.
 * @param scope  The constants a fold may read; by default the env's.
 *
 * @returns The text.
 */
export function lowerText(
  node: t.Node,
  environment: Env,
  scope: Scope = environment.constants ?? {},
): IrText {
  return {
    parts: lowerTextParts(node, environment, foldScope(environment, scope))
      .parts,
  };
}
