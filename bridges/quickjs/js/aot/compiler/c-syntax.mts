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

import {aotError} from './diagnostics.mts';
import type {CType} from './types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * A whole number in the int64 range as a C literal. `String(value)` is JS's shortest spelling that reads back
 * as the same double, which past 2^53 is not the number itself: 4611686018427387904 prints as
 * 4611686018427388000.
 *
 * @param value  A whole number in the int64 range.
 *
 * @returns Its C literal.
 */
export function i64Lit(value: number): string {
  // `-9223372036854775808` is `-` applied to 9223372036854775808, which fits no signed type.
  return value === -(2 ** 63)
    ? '(-9223372036854775807 - 1)'
    : BigInt(value).toString();
}

/**
 * The C type a numeric state, ref, or local slot of `cType` is declared with.
 *
 * @param cType  The slot's kind; a string slot has no scalar type.
 *
 * @returns The C type name.
 */
export function cScalarType(cType: CType): string {
  if (cType === 'int') return 'int';
  if (cType === 'float') return 'float';
  if (cType === 'i64') return 'int64_t';
  throw new Error(`AOT internal: no C scalar type for "${cType}"`);
}

/**
 * C source with its string and char literals and its comments blanked, so a scan for a call sees only code.
 *
 * @param cSource  C source.
 *
 * @returns The source with every literal and comment replaced by a space.
 */
export const stripCLiterals = (cSource: string): string =>
  cSource.replace(
    /"(?:[^"\\\n]|\\.)*"|'(?:[^'\\\n]|\\.)*'|\/\*[\s\S]*?\*\/|\/\/[^\n]*/g,
    ' ',
  );

/**
 * The kind of slot a constant is stored in: string, float (a fractional number), or int (anything else).
 *
 * @param value  A compile-time value.
 *
 * @returns Its slot kind.
 */
export const cTypeOfValue = (value: unknown): 'string' | 'float' | 'int' =>
  typeof value === 'string'
    ? 'string'
    : typeof value === 'number' && !Number.isInteger(value)
      ? 'float'
      : 'int';

/**
 * Formats a number as a valid C float literal (`1` → `1.0f`, not `1f` which doesn't compile).
 *
 * @param rawValue  The value; anything `Number()` converts to a finite float.
 *
 * @returns Its C float literal.
 */
export function floatLit(rawValue: unknown): string {
  // Non-finite values have no C form; every literal funnels here, so they fail now, not in the host compiler.
  const value = Number(rawValue);
  if (!Number.isFinite(value)) {
    throw aotError(
      `AOT: a numeric constant folded to ` +
        `${rawValue === undefined ? 'undefined' : String(rawValue)}, which has no C form`,
      'the value must be a finite number.',
    );
  }

  // Refuse values past the float range and write float underflow as 0, since C compilers warn about both.
  const float32 = Math.fround(value);
  if (!Number.isFinite(float32)) {
    throw aotError(
      `AOT: the numeric constant ${value} is past the float range`,
      'a float holds up to about 3.4e38.',
    );
  }

  if (float32 === 0 && value !== 0) return value < 0 ? '-0.0f' : '0.0f';

  // An exponent (`1e+21`) already makes a float literal; `.0` after one would not be C.
  const spelling = String(value);
  return /[.e]/.test(spelling) ? `${spelling}f` : `${spelling}.0f`;
}

/**
 * A JS string as a C string literal, with backslashes, quotes, newlines, and tabs escaped.
 *
 * @param text  The string.
 *
 * @returns Its C literal, quotes included.
 */
export const cstr = (text: string): string => {
  // Backslashes first, so the ones added for the other escapes are not doubled.
  const escaped = text
    .replace(/\\/g, '\\\\')
    .replace(/"/g, '\\"')
    .replace(/\n/g, '\\n')
    .replace(/\t/g, '\\t');
  return `"${escaped}"`;
};
