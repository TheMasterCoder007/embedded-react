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
import {i64Lit, floatLit, cstr} from './c-syntax.mts';
import {panGestureField} from './pan-responder.mts';
import type * as t from '@babel/types';
import type {AotError} from './diagnostics.mts';
import type {
  CExpr,
  CType,
  Env,
  FormatResult,
  ListState,
  ScalarState,
  Scope,
} from './types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** One piece of a printf format: literal text, or a spec with the C argument it formats. */
type TextPart =
  | {literal: string; spec?: undefined; code?: undefined}
  | {literal?: undefined; spec: string; code: string};

/** A JS expression split into printf parts, and whether JS would treat it as a string. */
interface TextParts {
  isString: boolean;
  parts: TextPart[];
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

/** The helper that rounds `a / b` for two ints the way each Math function does. */
const INT_ROUND_DIV = new Map([
  ['floor', 'app_floordiv'],
  ['ceil', 'app_ceildiv'],
  ['round', 'app_rounddiv'],
  ['trunc', 'app_div'],
]);

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * `expr` used as a C condition. JS treats a string as truthy when it is non-empty; a bare char[] in C tests
 * its ADDRESS, which is always true — and real GCC refuses that under -Werror=address.
 *
 * @param expr  The lowered expression.
 *
 * @returns C that is true exactly when JS would find `expr` truthy.
 */
export const asCond = (expr: CExpr): string =>
  expr.cType === 'string' ? `(${expr.code}[0] != '\\0')` : expr.code;

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
 * A compile-time number as C. A whole number too big for an int is a 64-bit constant (`lit`), so whole-number
 * math keeps it exact; past 64 bits it is a float.
 *
 * @param value  The number.
 *
 * @returns Its C constant.
 */
function numConst(value: number): CExpr {
  // floatLit refuses NaN and Infinity (a folded `0 / 0`, say), which have no C literal.
  if (!Number.isInteger(value)) return {code: floatLit(value), cType: 'float'};

  if (value < -(2 ** 63) || value >= 2 ** 63) {
    return {code: floatLit(value), cType: 'float'};
  }

  if (value < INT_MIN || value > INT_MAX) {
    return {code: i64Lit(value), cType: 'i64', lit: true};
  }

  // `-2147483648` is `-` applied to 2147483648, which does not fit an int.
  return {
    code: value === INT_MIN ? '(-2147483647 - 1)' : String(value),
    cType: 'int',
  };
}

/**
 * `+ - *` or a unary `-`: the operators that can overflow a whole number.
 *
 * @param node  The expression.
 *
 * @returns Whether it is one of them.
 */
const isIntArith = (node: t.Node): boolean =>
  (node.type === 'BinaryExpression' && Boolean(CHECKED_OP[node.operator])) ||
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
export const isTime64 = (expr: CExpr): boolean =>
  expr.cType === 'i64' && !expr.lit && !expr.wide;

/**
 * Besides a float, a 64-bit constant is an ordinary number that converts, as in JS; only a timestamp refuses.
 *
 * @param operands  The operands.
 *
 * @returns The operands, with any 64-bit constant cast to float when one of them is a float.
 */
const litPeers = (...operands: CExpr[]): CExpr[] =>
  operands.some(operand => operand.cType === 'float')
    ? operands.map(operand =>
        operand.lit
          ? {code: `((float)${operand.code})`, cType: 'float'}
          : operand,
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
function emitArith64(
  node: t.BinaryExpression,
  left: CExpr,
  right: CExpr,
  env: Env,
): CExpr {
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
    if (staticDivisor === 0) return {code: '0', cType: 'int'};

    if (isWide && staticDivisor === null) {
      return {
        code: `app_mod64(${left.code}, ${right.code})`,
        cType: 'i64',
        wide: true,
      };
    }

    const divisor = constDivisor(node.right, env);
    // ±1 leaves no remainder, and INT64_MIN % -1 overflows in C.
    if (divisor === 1 || divisor === -1) return {code: '0', cType: 'int'};

    if (Math.abs(divisor) <= 0x7fffffff || left.cType === 'int') {
      return {code: `((int)(${left.code} % ${right.code}))`, cType: 'int'};
    }

    return {code: `(${left.code} % ${right.code})`, cType: 'i64', wide: isWide};
  }

  // `+ - *`: the saturating 64-bit helper.
  return {
    code: `${CHECKED_OP[node.operator]}64(${left.code}, ${right.code})`,
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
function emitTimeRoundDiv(
  fn: string,
  args: t.CallExpression['arguments'],
  env: Env,
): CExpr | null {
  const division = args[0];
  if (
    !INT_ROUND_DIV.has(fn) ||
    args.length !== 1 ||
    division.type !== 'BinaryExpression' ||
    division.operator !== '/'
  ) {
    return null;
  }

  const dividend = emitExprWide(division.left, env);
  const divisor = emitExprWide(division.right, env);
  if (!isTime64(dividend) && !isTime64(divisor)) return null;

  if (dividend.cType === 'float' || divisor.cType === 'float') {
    throw mix64Error();
  }

  if (dividend.cType === 'string' || divisor.cType === 'string') return null;

  // Dividing by -1 is a negation, which saturates; C's `/` would overflow on INT64_MIN / -1.
  if (constDivisor(division.right, env) === -1) {
    return {code: `app_neg64(${dividend.code})`, cType: 'i64'};
  }

  return {
    code: `${INT_ROUND_DIV.get(fn)}64(${dividend.code}, ${divisor.code})`,
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
function emitIntRoundDiv(
  fn: string,
  args: t.CallExpression['arguments'],
  env: Env,
): CExpr | null {
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
  const dividend = emitExprWide(division.left, env);
  const divisor = emitExprWide(division.right, env);
  if (env.math64 || dividend.cType === 'i64' || divisor.cType === 'i64') {
    const isWhole = (operand: CExpr) =>
      operand.cType === 'int' ||
      (operand.cType === 'i64' && !isTime64(operand));
    if (!isWhole(dividend) || !isWhole(divisor)) return null;
    return {
      code: `${INT_ROUND_DIV.get(fn)}64(${dividend.code}, ${divisor.code})`,
      cType: 'i64',
      wide: true,
    };
  }

  // Otherwise divide two ints in 32 bits; anything else is left to the float path.
  if (dividend.cType !== 'int' || divisor.cType !== 'int') return null;
  return {
    code: `${INT_ROUND_DIV.get(fn)}(${dividend.code}, ${divisor.code})`,
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
function emitMath64(fn: string, argExprs: CExpr[]): CExpr {
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
      return {code: argExprs[0].code, cType: 'i64', wide: isWide};
    }

    if (fn === 'abs' && argExprs.length === 1) {
      return {
        code: `app_abs64(${argExprs[0].code})`,
        cType: 'i64',
        wide: isWide,
      };
    }

    if ((fn === 'min' || fn === 'max') && argExprs.length === 2) {
      return {
        code: `app_${fn}64(${argExprs[0].code}, ${argExprs[1].code})`,
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
 * Lowers one JS expression to C (see emitExprWide, the located entry point).
 *
 * @param node  The expression.
 * @param env  The expression environment.
 *
 * @returns The lowered expression.
 */
function emitExprImpl(node: t.Node, env: Env): CExpr {
  // 64-bit int math reaches only through whole-number math (keepsMath64); other nodes are typed as usual.
  if (env.math64 && !keepsMath64(node)) {
    env = {...env, math64: false};
  }

  switch (node.type) {
    case 'NumericLiteral':
      return numConst(node.value);
    case 'StringLiteral':
      return {code: cstr(node.value), cType: 'string'};
    case 'BooleanLiteral':
      return {code: node.value ? '1' : '0', cType: 'int', isBool: true};
    case 'Identifier': {
      // A name resolves to a local first, then to state, then to a compile-time constant.
      if (env.locals.has(node.name)) return env.locals.get(node.name)!;

      if (env.state.has(node.name)) {
        const stateRecord = env.state.get(node.name)!;
        if (stateRecord.kind === 'list') {
          throw new Error(
            `AOT: a list state ("${node.name}") can only be used via .length or .map`,
          );
        }

        return {
          code: stateRecord.cMember,
          cType: stateRecord.cType,
          isBool: stateRecord.isBool,
        };
      }

      if (node.name in env.consts) {
        const constValue = env.consts[node.name];
        if (typeof constValue === 'number') return numConst(constValue);

        if (typeof constValue === 'string') {
          return {code: cstr(constValue), cType: 'string'};
        }

        if (typeof constValue === 'boolean') {
          return {code: constValue ? '1' : '0', cType: 'int', isBool: true};
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

      const operand = emitExprWide(node.argument, env);
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
              code: `app_neg64(${operand.code})`,
              cType: 'i64',
              wide: !isTime64(operand),
            }
          : {code: `app_neg(${operand.code})`, cType: 'int'};
      }

      // Parenthesize the operand so `-` on a negative operand emits `(-(-x))`, not `(--x)` (a decrement).
      if (
        node.operator === '-' ||
        node.operator === '+' ||
        node.operator === '!'
      ) {
        // Only `!` yields a boolean; unary +/- coerce to a number, so `+flag` drops its boolean-ness.
        return {
          code: `(${node.operator}(${node.operator === '!' ? asCond(operand) : operand.code}))`,
          cType: node.operator === '!' ? 'int' : operand.cType,
          isBool: node.operator === '!',
        };
      }

      throw new Error(`AOT: unsupported unary operator "${node.operator}"`);
    }
    case 'BinaryExpression': {
      // Fold `+ - *` over two whole-number constants as JS would, so a product past int range stays exact.
      const leftConst = CHECKED_OP[node.operator]
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
      let left = emitExprWide(node.left, env);
      let right = emitExprWide(node.right, env);
      if (left.cType === 'i64' || right.cType === 'i64') {
        if (left.cType === 'int' && keepsMath64(node.left)) {
          left = emitExprWide(node.left, {...env, math64: true});
        }

        if (right.cType === 'int' && keepsMath64(node.right)) {
          right = emitExprWide(node.right, {...env, math64: true});
        }
      }
      [left, right] = litPeers(left, right);

      // Arithmetic: refuse strings, hand 64-bit math to emitArith64, and saturate whole-number `+ - *`.
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
            (CHECKED_OP[node.operator] || node.operator === '%'))
        ) {
          return emitArith64(node, left, right, env);
        }

        if (node.operator === '/') {
          return {
            code: `((float)(${left.code}) / (float)(${right.code}))`,
            cType: 'float',
          };
        }

        const cType =
          left.cType === 'float' || right.cType === 'float' ? 'float' : 'int';
        const checkedHelper = cType === 'int' && CHECKED_OP[node.operator];
        if (checkedHelper) {
          return env.math64
            ? {
                code: `${checkedHelper}64(${left.code}, ${right.code})`,
                cType: 'i64',
                wide: true,
              }
            : {
                code: `${checkedHelper}(${left.code}, ${right.code})`,
                cType: 'int',
              };
        }

        // A 0 divisor (NaN in JS) or -1 (INT_MIN % -1 overflows C) gives 0; other constants need no check.
        if (cType === 'int' && node.operator === '%') {
          const staticDivisor = staticInt(node.right, env);
          if (staticDivisor === 0 || staticDivisor === -1) {
            return {code: '0', cType: 'int'};
          }
          if (staticDivisor === null) {
            return {code: `app_mod(${left.code}, ${right.code})`, cType: 'int'};
          }
        }

        // C has no float `%`; fmodf matches JS's `%` exactly, zero and infinite divisors included.
        if (cType === 'float' && node.operator === '%') {
          return {
            code: `fmodf((float)(${left.code}), (float)(${right.code}))`,
            cType: 'float',
          };
        }

        return {code: `(${left.code} ${node.operator} ${right.code})`, cType};
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
              return {
                code: node.operator === '===' ? '0' : '1',
                cType: 'int',
                isBool: true,
              };
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
            code: `(strcmp(${left.code}, ${right.code}) ${cOperator} 0)`,
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
        const isSurelyNumber = (operandNode: t.Node, operand: CExpr) =>
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
          return {
            code: node.operator === '===' ? '0' : '1',
            cType: 'int',
            isBool: true,
          };
        }

        return {
          code: `(${left.code} ${cOperator} ${right.code})`,
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
        emitExprWide(node.left, env),
        emitExprWide(node.right, env),
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

        // Reading `left` twice is safe: expressions have no side effects, and the clock only moves in er_tick().
        return {
          code:
            operator === '||'
              ? `(${left.code} ? ${left.code} : ${right.code})`
              : `(${left.code} ? ${right.code} : ${left.code})`,
          cType: 'i64',
        };
      }

      // Otherwise the C result is the truth of the expression, a boolean only when both operands are.
      return {
        code: `(${asCond(left)} ${operator} ${asCond(right)})`,
        cType: 'int',
        isBool: Boolean(left.isBool && right.isBool),
      };
    }
    case 'ConditionalExpression': {
      const test = emitExprWide(node.test, env);
      const [consequent, alternate] = litPeers(
        emitExprWide(node.consequent, env),
        emitExprWide(node.alternate, env),
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
        code: `(${asCond(test)} ? ${consequent.code} : ${alternate.code})`,
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
          return {code: cstr(constValue), cType: 'string'};
        }

        if (typeof constValue === 'boolean') {
          return {code: constValue ? '1' : '0', cType: 'int', isBool: true};
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
          code: (env.state.get(object.name) as ListState).countMember,
          cType: 'int',
        };
      }

      // `<item>.field` where item is a struct local (a list row's bound element).
      if (
        object.type === 'Identifier' &&
        env.locals.get(object.name)?.struct &&
        prop
      ) {
        const field = env.locals
          .get(object.name)!
          .struct!.fields.find(structField => structField.key === prop);
        if (!field) {
          throw new Error(`AOT: unknown field "${prop}" on a list item`);
        }

        return {
          code: `${env.locals.get(object.name)!.code}.${field.key}`,
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
        return {code: ref.cVar, cType: ref.cType as CType};
      }

      // `<event>.x / .y / .dx / .dy` — touch fields of the handler's EREventData.
      if (
        object.type === 'Identifier' &&
        env.event === object.name &&
        (prop === 'x' || prop === 'y' || prop === 'dx' || prop === 'dy')
      ) {
        return {code: `data->${prop}`, cType: 'int'};
      }

      // `<event>.vx / .vy` — finger velocity (px/ms) at the last move; the engine measures it, a handler cannot.
      if (
        object.type === 'Identifier' &&
        env.event === object.name &&
        (prop === 'vx' || prop === 'vy')
      ) {
        return {code: `data->${prop}`, cType: 'float'};
      }

      // `<gestureState>.…` — the second argument of a PanResponder callback (see emitPanResponder).
      if (object.type === 'Identifier' && env.gesture === object.name && prop) {
        return panGestureField(prop, env.pan);
      }

      // `<event>.layout.x / .y / .width / .height` — the onLayout rect (EREventData.layout_rect; ERRect uses w/h).
      if (
        object.type === 'MemberExpression' &&
        !object.computed &&
        object.object.type === 'Identifier' &&
        env.event === object.object.name &&
        (object.property as t.Identifier).name === 'layout'
      ) {
        const RECT: Record<string, string> = {
          x: 'x',
          y: 'y',
          width: 'w',
          height: 'h',
        };
        const rectField = RECT[prop as string];
        if (!rectField) {
          throw new Error(
            `AOT: unknown onLayout rect field "${prop}" (use x / y / width / height)`,
          );
        }
        return {code: `data->layout_rect.${rectField}`, cType: 'int'};
      }

      // `Math.PI` — the only Math constant.
      if (
        object.type === 'Identifier' &&
        object.name === 'Math' &&
        prop === 'PI'
      ) {
        return {code: '(float)M_PI', cType: 'float'};
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
            code:
              callee.object.name === 'Date'
                ? 'app_date_now()'
                : 'app_perf_now()',
            cType: 'i64',
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

      // Math.*: exact integer division first, then 64-bit math, then whole-int abs/min/max/trunc, then libm.
      if (
        callee.type === 'MemberExpression' &&
        (callee.object as t.Identifier).name === 'Math'
      ) {
        const fn = (callee.property as t.Identifier).name;
        const timeDiv = emitTimeRoundDiv(fn, node.arguments, env);
        if (timeDiv) return timeDiv;

        const intDiv = emitIntRoundDiv(fn, node.arguments, env);
        if (intDiv) return intDiv;

        const argExprs = litPeers(
          ...node.arguments.map(argNode => emitExprWide(argNode, env)),
        );
        if (argExprs.some(argExpr => argExpr.cType === 'i64')) {
          return emitMath64(fn, argExprs);
        }

        // Keep int abs/min/max whole (a float rounds past 2^24); with math64, abs widens to keep |INT_MIN|.
        if (
          argExprs.length &&
          argExprs.every(argExpr => argExpr.cType === 'int')
        ) {
          if (fn === 'abs' && argExprs.length === 1) {
            return env.math64
              ? {
                  code: `app_abs64(${argExprs[0].code})`,
                  cType: 'i64',
                  wide: true,
                }
              : {code: `app_abs(${argExprs[0].code})`, cType: 'int'};
          }

          if ((fn === 'min' || fn === 'max') && argExprs.length === 2) {
            return {
              code: `app_${fn}(${argExprs[0].code}, ${argExprs[1].code})`,
              cType: 'int',
            };
          }
        }

        // Math.trunc stays an int: an int as is, a float via app_f2i (toward zero, NaN as 0, saturating).
        if (fn === 'trunc' && argExprs.length === 1) {
          return argExprs[0].cType === 'int'
            ? {code: argExprs[0].code, cType: 'int'}
            : {code: `app_f2i((float)(${argExprs[0].code}))`, cType: 'int'};
        }

        const UNARY: Record<string, string> = {
          sin: 'sinf',
          cos: 'cosf',
          tan: 'tanf',
          sqrt: 'sqrtf',
          abs: 'fabsf',
          round: 'app_roundf',
          floor: 'floorf',
          ceil: 'ceilf',
        };

        if (UNARY[fn] && argExprs.length === 1) {
          // An int is already whole, so rounding leaves it as it is; a float round trip would lose digits.
          const roundsToWhole =
            fn === 'round' || fn === 'floor' || fn === 'ceil';
          if (roundsToWhole && argExprs[0].cType === 'int') {
            return {code: argExprs[0].code, cType: 'int'};
          }

          const libmCall = `${UNARY[fn]}((float)(${argExprs[0].code}))`;
          // round/floor/ceil yield a whole number, kept as an int so %d / int assignments are correct.
          return roundsToWhole
            ? {code: `app_f2i(${libmCall})`, cType: 'int'}
            : {code: libmCall, cType: 'float'};
        }

        const BINARY: Record<string, string> = {
          min: 'fminf',
          max: 'fmaxf',
          atan2: 'atan2f',
          pow: 'powf',
        };
        if (BINARY[fn] && argExprs.length === 2) {
          return {
            code: `${BINARY[fn]}((float)(${argExprs[0].code}), (float)(${argExprs[1].code}))`,
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

/** emitExpr for the destinations that can hold a 64-bit timestamp: state, refs, locals, text, conditions. */
export const emitExprWide = withLoc(emitExprImpl);

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
 * An expression as one printf spec and its argument. `%lld` needs a long long, and int64_t is `long` on
 * 64-bit Linux. A float goes through app_ftoa, which prints Infinity, -Infinity, NaN, and 0 where %g prints
 * inf, -inf, nan and -0, and anything else as %g, into a buffer the call site lends it: one per float, so two
 * floats in one snprintf keep their own.
 *
 * @param expr  The lowered expression.
 *
 * @returns Its printf spec and argument.
 */
const printfPart = (expr: CExpr): {spec: string; code: string} =>
  expr.cType === 'string'
    ? {spec: '%s', code: expr.code}
    : expr.cType === 'float'
      ? {spec: '%s', code: `app_ftoa((char[16]){0}, ${expr.code})`}
      : expr.cType === 'i64'
        ? {spec: '%lld', code: `(long long)(${expr.code})`}
        : {spec: '%d', code: expr.code};

/**
 * An aotError pinned to the expression that cannot be lowered to text (concatParts is not withLoc-wrapped).
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
 * Splits a string-building `+` chain into printf parts, following JS's own left-to-right typing: a `+`
 * is a concatenation only once one of its sides is a string, so `n + 1 + 'ms'` still adds before it
 * appends. Parts are either a `literal` (folded into the format) or a `{spec, code}` pair (a runtime arg).
 *
 * @param node  The expression.
 * @param env  The expression environment.
 * @param scope  The constants a fold may read (already filtered by foldScope).
 * @param isOperand  The expression is an operand of `+`, where null / undefined / booleans print as words.
 *
 * @returns The parts, and whether JS would treat the expression as a string.
 */
function concatParts(
  node: t.Node,
  env: Env,
  scope: Scope,
  isOperand = false,
): TextParts {
  // `undefined` is "" as a child but "undefined" in a `+`; handled here, not in the fold every prop reader shares.
  if (node.type === 'Identifier' && node.name === 'undefined') {
    return {isString: false, parts: [{literal: isOperand ? 'undefined' : ''}]};
  }

  // A compile-time constant becomes literal text.
  try {
    const constValue = evalStatic(node, scope);
    return {
      isString: typeof constValue === 'string',
      parts: [
        {literal: isOperand ? String(constValue) : jsxChildText(constValue)},
      ],
    };
  } catch {
    /* not a compile-time constant — split it below */
  }

  // A `+` with a string on either side is a concatenation: its parts are both sides' parts in order.
  if (node.type === 'BinaryExpression' && node.operator === '+') {
    const leftParts = concatParts(node.left, env, scope, true);
    const rightParts = concatParts(node.right, env, scope, true);
    if (leftParts.isString || rightParts.isString) {
      return {isString: true, parts: [...leftParts.parts, ...rightParts.parts]};
    }
  }

  // A branch or operand that concatenates needs its own format; refuse before emitExpr's misleading error.
  const concatenates = (subExpr: t.Node) =>
    subExpr.type === 'BinaryExpression' &&
    subExpr.operator === '+' &&
    concatParts(subExpr, env, scope, true).isString;
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
  const cExpr = emitExprWide(node, env);

  // C collapses `&&`/`||` to 0/1 and a ternary to one slot, so text matches JS only when operands agree in kind.
  if (node.type === 'LogicalExpression' && !cExpr.isBool) {
    throw textShapeError(
      node,
      `AOT: "${node.operator}" in text evaluates to one of its operands, not to true/false`,
      `\`a ${node.operator} b\` is a or b unless both sides are already booleans — the ` +
        `generated C only has 0/1. Write the branch out instead: {cond ? 'yes' : ''}.`,
    );
  }

  if (
    node.type === 'ConditionalExpression' &&
    Boolean(emitExprWide(node.consequent, env).isBool) !==
      Boolean(emitExprWide(node.alternate, env).isBool)
  ) {
    throw textShapeError(
      node,
      'AOT: a ternary in text mixes a boolean branch with a non-boolean one',
      'JS renders those differently (`cond ? true : 5` is "true" or "5") but they share one ' +
        'C slot here. Make both branches the same kind.',
    );
  }

  // A boolean prints nothing as a child, "true"/"false" as an operand; isString stays false so `on + n` adds.
  if (cExpr.isBool) {
    return isOperand
      ? {
          isString: false,
          parts: [{spec: '%s', code: `((${cExpr.code}) ? "true" : "false")`}],
        }
      : {isString: false, parts: [{literal: ''}]};
  }

  return {
    isString: cExpr.cType === 'string',
    parts: [printfPart(cExpr)],
  };
}

/**
 * Lowers an expression to a printf format + args for a char-buffer destination (a <Text> body, a string
 * state slot, a <TextInput value>). A string-building `+` chain becomes one spec per dynamic part with
 * the literals folded into the format; anything else is a single spec over its own value.
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
  // Fold literal parts into the format (escaping `%`) and turn each runtime part into a spec and an argument.
  const visibleScope = foldScope(env, scope);
  let format = '';
  const args = [];
  for (const part of concatParts(node, env, visibleScope).parts) {
    if (part.literal !== undefined) {
      format += part.literal.replace(/%/g, '%%');
    } else {
      format += part.spec;
      args.push(part.code);
    }
  }

  return {format, args};
}

/**
 * Renders an emitFormat() result as snprintf's trailing arguments (a constant string keeps its `%s` form).
 *
 * @param result  The format and its arguments.
 *
 * @returns The arguments after the buffer and its size, as C.
 */
export const formatArgs = ({format, args}: FormatResult): string =>
  args.length
    ? `${cstr(format)}, ${args.join(', ')}`
    : `"%s", ${cstr(format.replace(/%%/g, '%'))}`;
