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

import {cstr, floatLit, i64Lit} from '../c-syntax.mts';
import type {CExpr} from '../types.mts';
import type {
  FloatMath,
  IrExpr,
  IrNumber,
  LayoutField,
} from '../ir/expressions.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** The C function each float Math function prints as. */
const FLOAT_MATH_C: Record<FloatMath, string> = {
  sin: 'sinf',
  cos: 'cosf',
  tan: 'tanf',
  sqrt: 'sqrtf',
  abs: 'fabsf',
  round: 'app_roundf',
  floor: 'floorf',
  ceil: 'ceilf',
  min: 'fminf',
  max: 'fmaxf',
  atan2: 'atan2f',
  pow: 'powf',
  mod: 'fmodf',
};

/** The field of the engine's ERRect each onLayout field reads. */
const LAYOUT_RECT_C: Record<LayoutField, string> = {
  x: 'x',
  y: 'y',
  width: 'w',
  height: 'h',
};

/** The smallest C int. */
const INT_MIN = -(2 ** 31);

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
 * A compile-time number as a C constant.
 *
 * @param expr  The number.
 *
 * @returns Its C constant.
 */
function printNumber(expr: IrNumber): string {
  if (expr.cType === 'float') {
    return floatLit(expr.value);
  }

  if (expr.cType === 'i64') {
    return i64Lit(expr.value);
  }

  // `-2147483648` is `-` applied to 2147483648, which does not fit an int.
  return expr.value === INT_MIN ? '(-2147483647 - 1)' : String(expr.value);
}

/**
 * An expression as a C condition (see asCond).
 *
 * @param expr  The expression.
 *
 * @returns C that is true exactly when JS would find it truthy.
 */
const printCondition = (expr: IrExpr): string =>
  asCond({code: printExpr(expr), cType: expr.cType});

/**
 * Prints an expression as C.
 *
 * @param expr  The expression.
 *
 * @returns Its C.
 */
export function printExpr(expr: IrExpr): string {
  switch (expr.kind) {
    case 'number':
      return printNumber(expr);
    case 'string':
      return cstr(expr.value);
    case 'boolean':
      return expr.value ? '1' : '0';
    case 'state':
      return expr.state.cMember;
    case 'local':
      return expr.local.code;
    case 'ref':
      return expr.ref.cVar;
    case 'listLength':
      return expr.list.countMember;
    case 'itemField':
      return `${expr.item.code}.${expr.field.key}`;
    case 'event':
      return `data->${expr.field}`;
    case 'layout':
      return `data->layout_rect.${LAYOUT_RECT_C[expr.field]}`;
    case 'clock':
      return expr.clock === 'Date' ? 'app_date_now()' : 'app_perf_now()';
    case 'pi':
      return '(float)M_PI';
    case 'foreign':
      return expr.code;
    case 'toFloat':
      return `((float)${printExpr(expr.operand)})`;
    case 'unary': {
      // The operand is parenthesized so `-` on a negative operand prints `(-(-x))`, not `(--x)` (a decrement).
      const operand =
        expr.op === '!'
          ? printCondition(expr.operand)
          : printExpr(expr.operand);
      return `(${expr.op}(${operand}))`;
    }
    case 'helper':
      return `app_${expr.helper}${expr.bits === 64 ? '64' : ''}(${expr.args.map(printExpr).join(', ')})`;
    case 'arithmetic':
      return `(${printExpr(expr.left)} ${expr.op} ${printExpr(expr.right)})`;
    case 'floatDivide':
      return `((float)(${printExpr(expr.left)}) / (float)(${printExpr(expr.right)}))`;
    case 'narrowRemainder':
      return `((int)(${printExpr(expr.left)} % ${printExpr(expr.right)}))`;
    case 'floatMath': {
      const args = expr.args.map(arg => `(float)(${printExpr(arg)})`);
      return `${FLOAT_MATH_C[expr.fn]}(${args.join(', ')})`;
    }
    case 'wholeRound':
      return printExpr(expr.operand);
    case 'trunc':
      return `app_f2i((float)(${printExpr(expr.operand)}))`;
    case 'toInt':
      return `app_f2i(${printExpr(expr.operand)})`;
    case 'compare':
      return `(${printExpr(expr.left)} ${expr.op} ${printExpr(expr.right)})`;
    case 'stringEquals':
      return `(strcmp(${printExpr(expr.left)}, ${printExpr(expr.right)}) ${expr.op} 0)`;
    case 'logical':
      return `(${printCondition(expr.left)} ${expr.op} ${printCondition(expr.right)})`;
    case 'pick': {
      // Reading `left` twice is safe: expressions have no side effects, and the clock only moves in er_tick().
      const left = printExpr(expr.left);
      const right = printExpr(expr.right);
      return expr.op === '||'
        ? `(${left} ? ${left} : ${right})`
        : `(${left} ? ${right} : ${left})`;
    }
    case 'conditional':
      return `(${printCondition(expr.test)} ? ${printExpr(expr.consequent)} : ${printExpr(expr.alternate)})`;
  }
}

/**
 * An expression as the C expression the emitters build with: its C, and what is known about its value.
 *
 * @param expr  The expression.
 *
 * @returns The C expression.
 */
export function toCExpr(expr: IrExpr): CExpr {
  // A bare local is the Local itself, so a list row keeps its struct (extractProps copies it into a prop).
  if (expr.kind === 'local') {
    return expr.local;
  }

  const cExpr: CExpr = {code: printExpr(expr), cType: expr.cType};
  if (expr.isBool !== undefined) {
    cExpr.isBool = expr.isBool;
  }

  if (expr.lit !== undefined) {
    cExpr.lit = expr.lit;
  }

  if (expr.wide !== undefined) {
    cExpr.wide = expr.wide;
  }

  return cExpr;
}
