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

import {traverse} from '@babel/core';
import {aotError, withLoc} from './diagnostics.mts';
import {evalStatic, foldScope} from './static-eval.mts';
import {floatLit} from './c-syntax.mts';
import {panGestureField} from './pan-responder.mts';
import {toCExpr} from './c/expressions.mts';
import {printText} from './c/text.mts';
import type * as t from '@babel/types';
import type {AotError} from './diagnostics.mts';
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
import type {IrText, IrTextPart} from './ir/text.mts';

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
 * A compile-time number. A whole number too big for an int is a 64-bit constant (`lit`), so whole-number
 * math keeps it exact; past 64 bits it is a float.
 *
 * @param value  The number.
 *
 * @returns Its constant.
 */
function numConst(value: number): IrNumber {
  // A float has no NaN or Infinity literal (a folded `0 / 0`, say); floatLit refuses them here, at the expression.
  if (!Number.isInteger(value)) {
    floatLit(value);
    return {kind: 'number', value, cType: 'float'};
  }

  if (value < -(2 ** 63) || value >= 2 ** 63) {
    floatLit(value);
    return {kind: 'number', value, cType: 'float'};
  }

  if (value < INT_MIN || value > INT_MAX) {
    return {kind: 'number', value, cType: 'i64', lit: true};
  }

  return {kind: 'number', value, cType: 'int'};
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
  cType: 'string',
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
  cType: 'int',
  isBool: true,
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
  cType: local.cType,
  isBool: local.isBool,
  lit: local.lit,
  wide: local.wide,
});

/**
 * `+ - *` or a unary `-`: the operators that can overflow a whole number.
 *
 * @param node  The expression.
 *
 * @returns Whether it is one of them.
 */
const isIntArith = (node: t.Node): boolean =>
  (node.type === 'BinaryExpression' &&
    Boolean(CHECKED_HELPER[node.operator])) ||
  (node.type === 'UnaryExpression' && node.operator === '-');

/**
 * Whole-number math JS would not cut to 32 bits: `+ - *`, negation, `%`, and the Math functions that keep a
 * whole number whole. Beside a 64-bit value all of it is worked out in 64 bits, however deep it sits.
 *
 * @param node  The expression.
 *
 * @returns Whether it is such math.
 */
const keepsMath64 = (node: t.Node): boolean =>
  isIntArith(node) ||
  (node.type === 'BinaryExpression' && node.operator === '%') ||
  (node.type === 'CallExpression' &&
    node.callee.type === 'MemberExpression' &&
    !node.callee.computed &&
    node.callee.object.type === 'Identifier' &&
    node.callee.object.name === 'Math' &&
    WHOLE_MATH.has((node.callee.property as t.Identifier).name));

/**
 * A 64-bit value computed at runtime from a timestamp, as opposed to a 64-bit constant (`lit`) or
 * whole-number math widened to 64 bits beside one (`wide`).
 *
 * @param expr  The lowered expression.
 *
 * @returns Whether it is a timestamp.
 */
export const isTime64 = (expr: IrTyped): boolean =>
  expr.cType === 'i64' && !expr.lit && !expr.wide;

/**
 * Besides a float, a 64-bit constant is an ordinary number that converts, as in JS; only a timestamp refuses.
 *
 * @param operands  The operands.
 *
 * @returns The operands, with any 64-bit constant converted to float when one of them is a float.
 */
const litPeers = (...operands: IrExpr[]): IrExpr[] =>
  operands.some(operand => operand.cType === 'float')
    ? operands.map(
        (operand): IrExpr =>
          operand.lit ? {kind: 'toFloat', operand, cType: 'float'} : operand,
      )
    : operands;

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

  const error = aotError(
    'AOT: a 64-bit time value can only be divided by a constant',
    'the divisor must be known at compile time — e.g. `ms % 1000`, `Math.floor(ms / ' +
      '60000)`. For a divisor from state, reduce the timestamp first: `(ms % 86400000) / ' +
      'period`.',
  );
  if (node.loc) {
    error.aotLoc = node.loc.start;
  }

  throw error;
}

/**
 * `+ - * %` with a 64-bit value on either side, kept whole. `+ - *` saturate at the int64 limits. With a
 * timestamp in it, `%` takes a constant; whole-number math widened beside one takes any divisor, as the int
 * `%` does. The result narrows back to int when the divisor or the dividend fits one, so `ms % 1000` is an
 * ordinary number again. `/` is refused: JS would give a fraction, and the whole-number division is written
 * Math.floor(a / b).
 *
 * @param node  The binary expression.
 * @param left  Its lowered left operand.
 * @param right  Its lowered right operand.
 * @param env  The expression environment.
 *
 * @returns The lowered expression.
 */
function lowerArith64(
  node: t.BinaryExpression,
  left: IrExpr,
  right: IrExpr,
  env: Env,
): IrExpr {
  if (left.cType === 'float' || right.cType === 'float') {
    throw mix64Error();
  }

  if (node.operator === '/') {
    throw aotError(
      'AOT: `/` on a 64-bit time value is not supported',
      'JS division gives a fraction, which a 64-bit integer cannot hold. Use Math.floor(a / ' +
        'b) for whole units — e.g. Math.floor(ms / 1000) for seconds.',
    );
  }

  // With no timestamp on either side, this is whole-number math widened to 64 bits.
  const isWide = !isTime64(left) && !isTime64(right);

  // `%`: zero/±1 divisors give 0, a timestamp needs a constant divisor, and the result narrows to int if it fits.
  if (node.operator === '%') {
    const staticDivisor = staticInt(node.right, env);
    // JS gives NaN for a zero divisor, which is 0 here, as the int `%` has it.
    if (staticDivisor === 0) return numConst(0);

    if (isWide && staticDivisor === null) {
      return {
        kind: 'helper',
        helper: 'mod',
        bits: 64,
        args: [left, right],
        cType: 'i64',
        wide: true,
      };
    }

    const divisor = constDivisor(node.right, env);
    // ±1 leaves no remainder, and INT64_MIN % -1 overflows in C.
    if (divisor === 1 || divisor === -1) return numConst(0);

    if (Math.abs(divisor) <= 0x7fffffff || left.cType === 'int') {
      return {kind: 'narrowRemainder', left, right, cType: 'int'};
    }

    return {
      kind: 'arithmetic',
      op: '%',
      left,
      right,
      cType: 'i64',
      wide: isWide,
    };
  }

  // `+ - *`: the saturating 64-bit helper.
  return {
    kind: 'helper',
    helper: CHECKED_HELPER[node.operator],
    bits: 64,
    args: [left, right],
    cType: 'i64',
    wide: isWide,
  };
}

/**
 * Math.floor / ceil / round / trunc over `a / b` with a 64-bit timestamp on either side, as exact integer
 * division by a constant (see constDivisor); a zero one saturates by the dividend's sign, as JS's ±Infinity
 * converts. Null when neither side is a timestamp, which leaves it to the whole-number or float path.
 *
 * @param fn  The Math function's name.
 * @param args  Its arguments.
 * @param env  The expression environment.
 *
 * @returns The lowered call, or null when it is not this case.
 */
function lowerTimeRoundDiv(
  fn: string,
  args: t.CallExpression['arguments'],
  env: Env,
): IrExpr | null {
  const division = args[0];
  if (
    !INT_ROUND_DIV.has(fn) ||
    args.length !== 1 ||
    division.type !== 'BinaryExpression' ||
    division.operator !== '/'
  ) {
    return null;
  }

  const dividend = lowerExprWide(division.left, env);
  const divisor = lowerExprWide(division.right, env);
  if (!isTime64(dividend) && !isTime64(divisor)) return null;

  if (dividend.cType === 'float' || divisor.cType === 'float') {
    throw mix64Error();
  }

  if (dividend.cType === 'string' || divisor.cType === 'string') return null;

  // Dividing by -1 is a negation, which saturates; C's `/` would overflow on INT64_MIN / -1.
  if (constDivisor(division.right, env) === -1) {
    return {
      kind: 'helper',
      helper: 'neg',
      bits: 64,
      args: [dividend],
      cType: 'i64',
    };
  }

  return {
    kind: 'helper',
    helper: INT_ROUND_DIV.get(fn)!,
    bits: 64,
    args: [dividend, divisor],
    cType: 'i64',
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
 * @param args  Its arguments.
 * @param env  The expression environment.
 *
 * @returns The lowered call, or null when it is not this case.
 */
function lowerIntRoundDiv(
  fn: string,
  args: t.CallExpression['arguments'],
  env: Env,
): IrExpr | null {
  const division = args[0];
  if (
    !INT_ROUND_DIV.has(fn) ||
    args.length !== 1 ||
    division.type !== 'BinaryExpression' ||
    division.operator !== '/'
  ) {
    return null;
  }

  // Divide in 64 bits beside a 64-bit value, when both operands are whole numbers and neither is a timestamp.
  const dividend = lowerExprWide(division.left, env);
  const divisor = lowerExprWide(division.right, env);
  if (env.math64 || dividend.cType === 'i64' || divisor.cType === 'i64') {
    const isWhole = (operand: IrExpr) =>
      operand.cType === 'int' ||
      (operand.cType === 'i64' && !isTime64(operand));
    if (!isWhole(dividend) || !isWhole(divisor)) return null;
    return {
      kind: 'helper',
      helper: INT_ROUND_DIV.get(fn)!,
      bits: 64,
      args: [dividend, divisor],
      cType: 'i64',
      wide: true,
    };
  }

  // Otherwise divide two ints in 32 bits; anything else is left to the float path.
  if (dividend.cType !== 'int' || divisor.cType !== 'int') return null;
  return {
    kind: 'helper',
    helper: INT_ROUND_DIV.get(fn)!,
    bits: 32,
    args: [dividend, divisor],
    cType: 'int',
  };
}

/**
 * Math.* over a 64-bit timestamp: only what stays exact in whole numbers.
 *
 * @param fn  The Math function's name.
 * @param argExprs  Its lowered arguments, at least one of them 64-bit.
 *
 * @returns The lowered call.
 */
function lowerMath64(fn: string, argExprs: IrExpr[]): IrExpr {
  if (argExprs.some(argExpr => argExpr.cType === 'float')) {
    throw mix64Error();
  }

  if (
    argExprs.every(
      argExpr => argExpr.cType === 'int' || argExpr.cType === 'i64',
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
        cType: 'i64',
        wide: isWide,
      };
    }

    if (fn === 'abs' && argExprs.length === 1) {
      return {
        kind: 'helper',
        helper: 'abs',
        bits: 64,
        args: [argExprs[0]],
        cType: 'i64',
        wide: isWide,
      };
    }

    if ((fn === 'min' || fn === 'max') && argExprs.length === 2) {
      return {
        kind: 'helper',
        helper: fn,
        bits: 64,
        args: [argExprs[0], argExprs[1]],
        cType: 'i64',
        wide: isWide,
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
 * Lowers one JS expression to the IR (see lowerExprWide, the located entry point).
 *
 * @param node  The expression.
 * @param env  The expression environment.
 *
 * @returns The lowered expression.
 */
function lowerExprImpl(node: t.Node, env: Env): IrExpr {
  // 64-bit int math reaches only through whole-number math (keepsMath64); other nodes are typed as usual.
  if (env.math64 && !keepsMath64(node)) {
    env = {...env, math64: false};
  }

  switch (node.type) {
    case 'NumericLiteral':
      return numConst(node.value);
    case 'StringLiteral':
      return stringConst(node.value);
    case 'BooleanLiteral':
      return boolConst(node.value);
    case 'Identifier': {
      // A name resolves to a local first, then to state, then to a compile-time constant.
      if (env.locals.has(node.name)) {
        return localRead(env.locals.get(node.name)!);
      }

      if (env.state.has(node.name)) {
        const stateRecord = env.state.get(node.name)!;
        if (stateRecord.kind === 'list') {
          throw new Error(
            `AOT: a list state ("${node.name}") can only be used via .length or .map`,
          );
        }

        return {
          kind: 'state',
          state: stateRecord,
          cType: stateRecord.cType,
          isBool: stateRecord.isBool,
        };
      }

      if (node.name in env.consts) {
        const constValue = env.consts[node.name];
        if (typeof constValue === 'number') return numConst(constValue);

        if (typeof constValue === 'string') {
          return stringConst(constValue);
        }

        if (typeof constValue === 'boolean') {
          return boolConst(constValue);
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
      if (staticOperand !== null) return numConst(-staticOperand);

      const operand = lowerExprWide(node.argument, env);
      if (
        (node.operator === '-' || node.operator === '+') &&
        operand.cType === 'string'
      ) {
        throw aotError(
          `AOT: unary "${node.operator}" on a string is not supported`,
          'JS would coerce the string to a number; C has no such coercion. Keep the operand numeric.',
        );
      }

      // Negating the minimum overflows, so a whole number saturates the way `+ - *` do.
      if (
        node.operator === '-' &&
        (operand.cType === 'int' || operand.cType === 'i64')
      ) {
        const is64Bit = operand.cType === 'i64' || env.math64;
        return is64Bit
          ? {
              kind: 'helper',
              helper: 'neg',
              bits: 64,
              args: [operand],
              cType: 'i64',
              wide: !isTime64(operand),
            }
          : {
              kind: 'helper',
              helper: 'neg',
              bits: 32,
              args: [operand],
              cType: 'int',
            };
      }

      if (
        node.operator === '-' ||
        node.operator === '+' ||
        node.operator === '!'
      ) {
        // Only `!` yields a boolean; unary +/- coerce to a number, so `+flag` drops its boolean-ness.
        return {
          kind: 'unary',
          op: node.operator,
          operand,
          cType: node.operator === '!' ? 'int' : operand.cType,
          isBool: node.operator === '!',
        };
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
        return numConst(
          node.operator === '+'
            ? leftConst! + rightConst
            : node.operator === '-'
              ? leftConst! - rightConst
              : leftConst! * rightConst,
        );
      }

      // Lower both operands; besides a 64-bit value, redo whole-number math in 64 bits, as JS never cuts it to 32.
      let left = lowerExprWide(node.left, env);
      let right = lowerExprWide(node.right, env);
      if (left.cType === 'i64' || right.cType === 'i64') {
        if (left.cType === 'int' && keepsMath64(node.left)) {
          left = lowerExprWide(node.left, {...env, math64: true});
        }

        if (right.cType === 'int' && keepsMath64(node.right)) {
          right = lowerExprWide(node.right, {...env, math64: true});
        }
      }
      [left, right] = litPeers(left, right);

      // Arithmetic: refuse strings, hand 64-bit math to lowerArith64, and saturate whole-number `+ - *`.
      if (ARITH.has(node.operator)) {
        if (left.cType === 'string' || right.cType === 'string') {
          // String `+` has no single C value; only a char-buffer destination can lower it, via emitFormat().
          if (node.operator === '+') {
            throw aotError(
              'AOT: string concatenation is not supported in this position',
              'a `+` chain over strings lowers to a printf format, which only works where the value ' +
                'lands in a text buffer: a <Text> body, a string useState setter, or a <TextInput ' +
                'value>.',
            );
          }
          // JS would coerce the string to a number; C would do pointer arithmetic or refuse to compile.
          throw aotError(
            `AOT: "${node.operator}" on a string is not supported`,
            'keep both operands numeric — a string cannot be coerced to a number here.',
          );
        }
        // `/` takes a 64-bit constant as an ordinary C number; only a runtime 64-bit value is refused it.
        if (
          isTime64(left) ||
          isTime64(right) ||
          ((left.cType === 'i64' || right.cType === 'i64') &&
            (CHECKED_HELPER[node.operator] || node.operator === '%'))
        ) {
          return lowerArith64(node, left, right, env);
        }

        if (node.operator === '/') {
          return {kind: 'floatDivide', left, right, cType: 'float'};
        }

        const cType =
          left.cType === 'float' || right.cType === 'float' ? 'float' : 'int';
        const checkedHelper = cType === 'int' && CHECKED_HELPER[node.operator];
        if (checkedHelper) {
          return env.math64
            ? {
                kind: 'helper',
                helper: checkedHelper,
                bits: 64,
                args: [left, right],
                cType: 'i64',
                wide: true,
              }
            : {
                kind: 'helper',
                helper: checkedHelper,
                bits: 32,
                args: [left, right],
                cType: 'int',
              };
        }

        // A 0 divisor (NaN in JS) or -1 (INT_MIN % -1 overflows C) gives 0; other constants need no check.
        if (cType === 'int' && node.operator === '%') {
          const staticDivisor = staticInt(node.right, env);
          if (staticDivisor === 0 || staticDivisor === -1) {
            return numConst(0);
          }
          if (staticDivisor === null) {
            return {
              kind: 'helper',
              helper: 'mod',
              bits: 32,
              args: [left, right],
              cType: 'int',
            };
          }
        }

        // C has no float `%`; fmodf matches JS's `%` exactly, zero and infinite divisors included.
        if (cType === 'float' && node.operator === '%') {
          return {
            kind: 'floatMath',
            fn: 'mod',
            args: [left, right],
            cType: 'float',
          };
        }

        return {
          kind: 'arithmetic',
          op: node.operator as IrArithmetic['op'],
          left,
          right,
          cType,
        };
      }

      // Comparisons: strings through strcmp, everything else as a plain C comparison yielding a boolean.
      if (COMPARE.has(node.operator)) {
        const cOperator =
          node.operator === '==='
            ? '=='
            : node.operator === '!=='
              ? '!='
              : node.operator;
        if (left.cType === 'string' || right.cType === 'string') {
          if (left.cType !== right.cType) {
            // A strict comparison never coerces, so a string against a number is decided statically.
            if (node.operator === '===' || node.operator === '!==') {
              return boolConst(node.operator === '!==');
            }

            throw aotError(
              'AOT: a string cannot be compared with a number',
              `\`${node.operator}\` coerces the string to a number in JS, which C cannot reproduce. ` +
                `Compare like with like, or use === / !== (which never coerce).`,
            );
          }
          // Equality is exact, but strcmp's UTF-8 order can differ from JS's UTF-16 order, so refuse ordering.
          if (cOperator !== '==' && cOperator !== '!=') {
            throw aotError(
              `AOT: ordering strings with "${node.operator}" is not supported`,
              'JS orders strings by UTF-16 code unit but the device holds UTF-8, so the order can ' +
                'differ. Use === / !==, or compare numbers.',
            );
          }

          return {
            kind: 'stringEquals',
            op: cOperator,
            left,
            right,
            cType: 'int',
            isBool: true,
          };
        }
        // A float compared with a timestamp would round it.
        if (
          (left.cType === 'i64' || right.cType === 'i64') &&
          (left.cType === 'float' || right.cType === 'float')
        ) {
          throw mix64Error();
        }

        // `true === 1` is false in JS but true in C's int slot; fold it when the other side is surely a number.
        const isSurelyNumber = (operandNode: t.Node, operand: IrExpr) =>
          !operand.isBool &&
          (operand.cType === 'float' ||
            operand.cType === 'i64' ||
            operandNode.type === 'NumericLiteral' ||
            (operandNode.type === 'UnaryExpression' &&
              operandNode.argument.type === 'NumericLiteral') ||
            (operandNode.type === 'Identifier' &&
              !env.locals.has(operandNode.name) &&
              (env.state.get(operandNode.name) as ScalarState | undefined)
                ?.isBool === false));
        if (
          (node.operator === '===' || node.operator === '!==') &&
          ((left.isBool && isSurelyNumber(node.right, right)) ||
            (right.isBool && isSurelyNumber(node.left, left)))
        ) {
          return boolConst(node.operator === '!==');
        }

        return {
          kind: 'compare',
          op: cOperator as IrCompare['op'],
          left,
          right,
          cType: 'int',
          isBool: true,
        };
      }

      throw new Error(`AOT: unsupported binary operator "${node.operator}"`);
    }
    case 'LogicalExpression': {
      // Only `&&` and `||` are supported (not `??`).
      const operator =
        node.operator === '&&' || node.operator === '||' ? node.operator : null;
      if (!operator) {
        throw new Error(`AOT: unsupported logical operator "${node.operator}"`);
      }

      // JS `&&`/`||` return an operand, so with a 64-bit side keep its value instead of collapsing it to 0/1.
      const [left, right] = litPeers(
        lowerExprWide(node.left, env),
        lowerExprWide(node.right, env),
      );
      if (left.cType === 'i64' || right.cType === 'i64') {
        if (left.cType === 'float' || right.cType === 'float') {
          throw mix64Error();
        }

        if (left.cType === 'string' || right.cType === 'string') {
          throw aotError(
            `AOT: "${operator}" cannot mix a 64-bit time value with a string`,
            'keep both sides numbers, or write the branch out: {ms ? ms : 0}.',
          );
        }

        if (left.isBool || right.isBool) {
          throw bool64Error(`"${operator}"`);
        }

        return {kind: 'pick', op: operator, left, right, cType: 'i64'};
      }

      // Otherwise the C result is the truth of the expression, a boolean only when both operands are.
      return {
        kind: 'logical',
        op: operator,
        left,
        right,
        cType: 'int',
        isBool: Boolean(left.isBool && right.isBool),
      };
    }
    case 'ConditionalExpression': {
      const test = lowerExprWide(node.test, env);
      const [consequent, alternate] = litPeers(
        lowerExprWide(node.consequent, env),
        lowerExprWide(node.alternate, env),
      );
      // A string branch beside a numeric one is ill-typed C with no single printf spec, so refuse it here.
      if ((consequent.cType === 'string') !== (alternate.cType === 'string')) {
        throw aotError(
          'AOT: a ternary cannot mix a string branch with a numeric one',
          "both branches must be the same kind — quote the number to keep it text, e.g. {ok ? '1' : 'none'}.",
        );
      }

      // Both branches share one C slot, float or i64 if either is; i64 beside a float or boolean is refused.
      const eitherIs = (kind: CType) =>
        consequent.cType === kind || alternate.cType === kind;
      if (eitherIs('i64') && eitherIs('float')) {
        throw mix64Error();
      }

      if (eitherIs('i64') && (consequent.isBool || alternate.isBool)) {
        throw bool64Error('a ternary');
      }

      const cType = eitherIs('float')
        ? 'float'
        : eitherIs('i64')
          ? 'i64'
          : consequent.cType === alternate.cType
            ? consequent.cType
            : 'int';
      return {
        kind: 'conditional',
        test,
        consequent,
        alternate,
        cType,
        isBool: Boolean(consequent.isBool && alternate.isBool),
      };
    }
    case 'MemberExpression': {
      // Static fold: member access that resolves to a compile-time constant (e.g., a .map item's `.key`).
      try {
        const constValue = evalStatic(node, foldScope(env, env.consts ?? {}));
        if (typeof constValue === 'number') return numConst(constValue);

        if (typeof constValue === 'string') {
          return stringConst(constValue);
        }

        if (typeof constValue === 'boolean') {
          return boolConst(constValue);
        }
      } catch {
        /* not static — fall through to the dynamic member forms below */
      }

      // Otherwise the object must be one of the runtime forms below; `prop` is null for a computed member.
      const object = node.object;
      const prop = node.computed ? null : (node.property as t.Identifier).name;
      // `<list>.length` → the runtime count.
      if (
        object.type === 'Identifier' &&
        env.state.get(object.name)?.kind === 'list' &&
        prop === 'length'
      ) {
        return {
          kind: 'listLength',
          list: env.state.get(object.name) as ListState,
          cType: 'int',
        };
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

        return {
          kind: 'itemField',
          item,
          field,
          cType: field.kind === 'string' ? 'string' : field.kind,
        };
      }

      // `<ref>.current` — a value ref's mutable C slot.
      if (
        object.type === 'Identifier' &&
        env.refs?.has(object.name) &&
        prop === 'current'
      ) {
        const ref = env.refs.get(object.name)!;
        ref.used = true;
        return {kind: 'ref', ref, cType: ref.cType as CType};
      }

      // `<event>.x / .y / .dx / .dy` — touch fields of the handler's EREventData.
      if (
        object.type === 'Identifier' &&
        env.event === object.name &&
        (prop === 'x' || prop === 'y' || prop === 'dx' || prop === 'dy')
      ) {
        return {kind: 'event', field: prop, cType: 'int'};
      }

      // `<event>.vx / .vy` — finger velocity (px/ms) at the last move; the engine measures it, a handler cannot.
      if (
        object.type === 'Identifier' &&
        env.event === object.name &&
        (prop === 'vx' || prop === 'vy')
      ) {
        return {kind: 'event', field: prop, cType: 'float'};
      }

      // `<gestureState>.…` — the second argument of a PanResponder callback (see emitPanResponder).
      if (object.type === 'Identifier' && env.gesture === object.name && prop) {
        return {kind: 'foreign', ...panGestureField(prop, env.pan)};
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
        return {kind: 'layout', field: prop as LayoutField, cType: 'int'};
      }

      // `Math.PI` — the only Math constant.
      if (
        object.type === 'Identifier' &&
        object.name === 'Math' &&
        prop === 'PI'
      ) {
        return {kind: 'pi', cType: 'float'};
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
          return {kind: 'clock', clock: callee.object.name, cType: 'i64'};
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

      // Math.*: exact integer division first, then 64-bit math, then whole-int abs/min/max/trunc, then floats.
      if (
        callee.type === 'MemberExpression' &&
        (callee.object as t.Identifier).name === 'Math'
      ) {
        const fn = (callee.property as t.Identifier).name;
        const timeDiv = lowerTimeRoundDiv(fn, node.arguments, env);
        if (timeDiv) return timeDiv;

        const intDiv = lowerIntRoundDiv(fn, node.arguments, env);
        if (intDiv) return intDiv;

        const argExprs = litPeers(
          ...node.arguments.map(argNode => lowerExprWide(argNode, env)),
        );
        if (argExprs.some(argExpr => argExpr.cType === 'i64')) {
          return lowerMath64(fn, argExprs);
        }

        // Keep int abs/min/max whole (a float rounds past 2^24); with math64, abs widens to keep |INT_MIN|.
        if (
          argExprs.length &&
          argExprs.every(argExpr => argExpr.cType === 'int')
        ) {
          if (fn === 'abs' && argExprs.length === 1) {
            return env.math64
              ? {
                  kind: 'helper',
                  helper: 'abs',
                  bits: 64,
                  args: [argExprs[0]],
                  cType: 'i64',
                  wide: true,
                }
              : {
                  kind: 'helper',
                  helper: 'abs',
                  bits: 32,
                  args: [argExprs[0]],
                  cType: 'int',
                };
          }

          if ((fn === 'min' || fn === 'max') && argExprs.length === 2) {
            return {
              kind: 'helper',
              helper: fn,
              bits: 32,
              args: [argExprs[0], argExprs[1]],
              cType: 'int',
            };
          }
        }

        // Math.trunc stays an int: an int as is, a float via app_f2i (toward zero, NaN as 0, saturating).
        if (fn === 'trunc' && argExprs.length === 1) {
          return argExprs[0].cType === 'int'
            ? {kind: 'wholeRound', operand: argExprs[0], cType: 'int'}
            : {kind: 'trunc', operand: argExprs[0], cType: 'int'};
        }

        if (FLOAT_MATH_UNARY.has(fn) && argExprs.length === 1) {
          // An int is already whole, so rounding leaves it as it is; a float round trip would lose digits.
          const roundsToWhole =
            fn === 'round' || fn === 'floor' || fn === 'ceil';
          if (roundsToWhole && argExprs[0].cType === 'int') {
            return {kind: 'wholeRound', operand: argExprs[0], cType: 'int'};
          }

          const floatCall: IrExpr = {
            kind: 'floatMath',
            fn: fn as FloatMath,
            args: [argExprs[0]],
            cType: 'float',
          };
          // round/floor/ceil yield a whole number, kept as an int so %d / int assignments are correct.
          return roundsToWhole
            ? {kind: 'toInt', operand: floatCall, cType: 'int'}
            : floatCall;
        }

        if (FLOAT_MATH_BINARY.has(fn) && argExprs.length === 2) {
          return {
            kind: 'floatMath',
            fn: fn as FloatMath,
            args: [argExprs[0], argExprs[1]],
            cType: 'float',
          };
        }

        throw new Error(
          `AOT: unsupported Math.${fn}(...) (arity ${argExprs.length})`,
        );
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

/** Lowers one JS expression to the IR, with the expression's location attached to any AOT error it throws. */
export const lowerExprWide = withLoc(lowerExprImpl);

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
 * Lowers an expression for a destination that holds an int, a float, or a string. C would narrow a 64-bit
 * value (a Date.now() / performance.now() timestamp, or a whole number past the int range) into one of those
 * without a warning, so it is refused here; the destinations that hold 64 bits call emitExprWide.
 *
 * The result carries its C type, so the caller can pick the right printf spec or assignment. A destination
 * that holds text lowers through emitFormat instead: one printf format plus its arguments.
 *
 * @param node  The expression.
 * @param env  The expression environment.
 *
 * @returns The lowered expression; never 64-bit.
 */
export function emitExpr(node: t.Node, env: Env): CExpr {
  const cExpr = emitExprWide(node, env);
  if (cExpr.cType !== 'i64') return cExpr;

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
  if (node.type === 'LogicalExpression' && !value.isBool) {
    throw textShapeError(
      node,
      `AOT: "${node.operator}" in text evaluates to one of its operands, not to true/false`,
      `\`a ${node.operator} b\` is a or b unless both sides are already booleans — the ` +
        `generated C only has 0/1. Write the branch out instead: {cond ? 'yes' : ''}.`,
    );
  }

  if (
    node.type === 'ConditionalExpression' &&
    Boolean(lowerExprWide(node.consequent, env).isBool) !==
      Boolean(lowerExprWide(node.alternate, env).isBool)
  ) {
    throw textShapeError(
      node,
      'AOT: a ternary in text mixes a boolean branch with a non-boolean one',
      'JS renders those differently (`cond ? true : 5` is "true" or "5") but they share one ' +
        'C slot here. Make both branches the same kind.',
    );
  }

  // A boolean prints nothing as a child, "true"/"false" as an operand; isString stays false so `on + n` adds.
  if (value.isBool) {
    return isOperand
      ? {isString: false, parts: [{kind: 'booleanWord', value}]}
      : {isString: false, parts: [{kind: 'literal', text: ''}]};
  }

  return {
    isString: value.cType === 'string',
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
