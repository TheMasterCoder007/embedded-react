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

import {cstr} from '../c-syntax.mts';
import {printExpr} from './expressions.mts';
import type {FormatResult} from '../types.mts';
import type {IrExpr} from '../ir/expressions.mts';
import type {IrText} from '../ir/text.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** One printf conversion: its spec and the C argument it formats. */
interface PrintfPart {
  spec: string;
  code: string;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * A value as one printf spec and its argument. `%lld` needs a long long, and int64_t is `long` on 64-bit
 * Linux. A float goes through app_ftoa, which prints Infinity, -Infinity, NaN, and 0 where %g prints inf,
 * -inf, nan and -0, and anything else as %g, into a buffer the call site lends it: one per float, so two
 * floats in one snprintf keep their own.
 *
 * @param value  The value.
 *
 * @returns Its printf spec and argument.
 */
function printfPart(value: IrExpr): PrintfPart {
  const code = printExpr(value);
  if (value.cType === 'string') {
    return {spec: '%s', code};
  }

  if (value.cType === 'float') {
    return {spec: '%s', code: `app_ftoa((char[16]){0}, ${code})`};
  }

  if (value.cType === 'i64') {
    return {spec: '%lld', code: `(long long)(${code})`};
  }

  return {spec: '%d', code};
}

/**
 * Prints a text as one printf format and its arguments, for a char-buffer destination.
 *
 * @param text  The text.
 *
 * @returns The format, with literal `%` escaped as `%%`, and its arguments.
 */
export function printText(text: IrText): FormatResult {
  let format = '';
  const args = [];
  for (const part of text.parts) {
    // Literal text joins the format; each runtime value adds a spec and its argument.
    if (part.kind === 'literal') {
      format += part.text.replace(/%/g, '%%');
      continue;
    }

    const {spec, code} =
      part.kind === 'booleanWord'
        ? {
            spec: '%s',
            code: `((${printExpr(part.value)}) ? "true" : "false")`,
          }
        : printfPart(part.value);
    format += spec;
    args.push(code);
  }

  return {format, args};
}

/**
 * Renders a printed text as snprintf's trailing arguments (a constant string keeps its `%s` form).
 *
 * @param result  The format and its arguments.
 *
 * @returns The arguments after the buffer and its size, as C.
 */
export const formatArgs = ({format, args}: FormatResult): string =>
  args.length
    ? `${cstr(format)}, ${args.join(', ')}`
    : `"%s", ${cstr(format.replace(/%%/g, '%'))}`;
