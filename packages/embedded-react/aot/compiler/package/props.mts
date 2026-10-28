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
 * Constant props as package records. Lowering spells a constant prop the way the C output writes it
 * (`0xFF0D0D0Fu`, `ER_ALIGN_CENTER`, `0.5f`), so this reads that spelling back into the prop's value.
 */

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {aotError} from '../diagnostics.mts';
import {ABI_PROPS, ENGINE_VALUES, PropKind} from './format.mts';
import type {AbiProp} from './format.mts';
import type {ProgramPropEntry} from './writer.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

// A cast the C output puts in front of a constant, which doesn't change its value here.
const CAST = /^\((?:u?int(?:8|16|32)_t|float|double)\)\s*/;

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * The ABI prop an ERProps field is.
 *
 * @param field  The field, as lowering names it (`flex_grow`, `gradient_stops[0].color`).
 *
 * @returns The prop.
 */
export function abiProp(field: string): AbiProp {
  const prop = ABI_PROPS.get(field);
  if (!prop) {
    throw aotError(
      `AOT: the package output has no engine prop for \`${field}\` yet`,
    );
  }

  return prop;
}

/**
 * A float's 32 bits, as C's `float` holds it.
 *
 * @param value  The number.
 *
 * @returns The bits.
 */
export function floatBits(value: number): number {
  const view = new DataView(new ArrayBuffer(4));
  view.setFloat32(0, value, true);
  return view.getUint32(0, true);
}

/**
 * Reads a constant as the C output spells it.
 *
 * @param code  The C: a number, a hex color, an er_scene.h name, or one of those negated or cast.
 *
 * @returns The number, or null when it is none of those.
 */
export function readConstant(code: string): number | null {
  let text = code.trim();
  while (CAST.test(text) || /^\(.*\)$/.test(text)) {
    text = CAST.test(text) ? text.replace(CAST, '') : text.slice(1, -1).trim();
  }

  if (text.startsWith('-')) {
    const negated = readConstant(text.slice(1));
    return negated === null ? null : -negated;
  }

  if (/^0x[0-9a-f]+u?$/i.test(text)) {
    return Number.parseInt(text.replace(/u$/i, ''), 16);
  }

  if (/^\d+(\.\d*)?(e[+-]?\d+)?f?$/i.test(text)) {
    return Number(text.replace(/f$/i, ''));
  }

  return ENGINE_VALUES.get(text) ?? null;
}

/**
 * A constant prop as a prop set entry.
 *
 * @param field  The ERProps field.
 * @param code  Its value, as C.
 *
 * @returns The entry.
 */
export function constantPropEntry(
  field: string,
  code: string,
): ProgramPropEntry {
  const prop = abiProp(field);
  const value = readConstant(code);
  if (value === null) {
    throw aotError(
      `AOT: the package output can't read the constant \`${code}\` for \`${field}\` yet`,
    );
  }

  return {
    prop: prop.id,
    kind: PropKind[prop.kind],
    value:
      prop.kind === 'float'
        ? floatBits(value)
        : prop.kind === 'color'
          ? value >>> 0
          : Math.trunc(value) | 0,
  };
}

/**
 * A string or image prop as a prop set entry.
 *
 * @param field  `text`, `placeholder` or `image_name`.
 * @param stringIndex  The string's index in the strings table.
 *
 * @returns The entry.
 */
export function stringPropEntry(
  field: string,
  stringIndex: number,
): ProgramPropEntry {
  const prop = abiProp(field);
  return {prop: prop.id, kind: PropKind[prop.kind], value: stringIndex};
}
