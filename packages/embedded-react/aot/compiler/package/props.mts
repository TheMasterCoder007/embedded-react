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
import type {AbiProp, PropKindName} from './format.mts';
import type {ProgramPropEntry} from './writer.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

// A cast the C output puts in front of a constant, which doesn't change its value here.
const CAST = /^\((?:u?int(?:8|16|32)_t|float|double)\)\s*/;

// The values each whole-number kind holds, as the runtime checks them at load. A float holds any, and an
// enum's largest value is its prop's own.
const RANGES: Partial<Record<PropKindName, [min: number, max: number]>> = {
  i16: [-32768, 32767],
  u8: [0, 255],
  opacity: [0, 255],
  u16: [0, 65535],
  bool: [0, 1],
  color: [0, 0xffffffff],
};

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

  if (prop.kind === 'float') {
    return {prop: prop.id, kind: PropKind.float, value: floatBits(value)};
  }

  // The C output would wrap a value its field can't hold; the runtime refuses the package instead.
  const whole = Math.trunc(value);
  const [min, max] = prop.kind === 'enum' ? [0, prop.max!] : RANGES[prop.kind]!;
  if (whole < min || whole > max) {
    throw aotError(
      `AOT: \`${field}\` is ${whole}, but the engine holds ${min} to ${max} there`,
      'use a value in that range.',
    );
  }

  return {
    prop: prop.id,
    kind: PropKind[prop.kind],
    value: prop.kind === 'color' ? whole >>> 0 : whole | 0,
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
