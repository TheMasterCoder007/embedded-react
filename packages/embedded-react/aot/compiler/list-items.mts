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

// list-items.mts — the fields of a list's items: worked out from its initial items, and laid out in a row.

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {aotError} from './diagnostics.mts';
import type {IrExpr, IrItemField} from './ir/expressions.mts';
import type {ItemField, ItemStruct} from './types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** One field of a row: a value, or the flag saying whether an optional value is there. */
export interface RowEntry {
  field: ItemField;
  presence: boolean;
}

/** A field's value in an item, when the item has it. */
export interface ItemValue {
  present: boolean;
  value: unknown;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Whether a value is a plain object, as an item or a nested field is.
 *
 * @param value  The value.
 *
 * @returns Whether it is one.
 */
export const isPlainObject = (
  value: unknown,
): value is Record<string, unknown> =>
  typeof value === 'object' && value !== null && !Array.isArray(value);

/**
 * The kind of field a value is stored in, or null when it is none.
 *
 * @param value  The value.
 *
 * @returns The kind.
 */
function kindOf(value: unknown): ItemField['kind'] | null {
  if (typeof value === 'string') return 'string';
  if (typeof value === 'boolean') return 'bool';
  if (typeof value === 'number') {
    return Number.isInteger(value) ? 'int' : 'float';
  }

  return null;
}

/**
 * Whether two kinds can share a field: a whole number and a fraction can, as a fraction.
 *
 * @param first  The kind the field has.
 * @param other  Another item's kind.
 *
 * @returns Whether they agree.
 */
const kindsAgree = (
  first: ItemField['kind'],
  other: ItemField['kind'],
): boolean =>
  first === other ||
  ((first === 'int' || first === 'float') &&
    (other === 'int' || other === 'float'));

/**
 * Works out the fields of a list's items from its initial items. Objects take the union of their fields, nested
 * objects flattened into dotted paths; a field some item leaves out, or sets to null, is optional. Plain values
 * (numbers, strings or booleans) are one field. A number field is a fraction when any item's value is one.
 *
 * @param items  The initial items.
 * @param what  The list, for errors: `list state "items"`.
 *
 * @returns The struct.
 */
export function inferItemStruct(items: unknown, what: string): ItemStruct {
  const shapeHint =
    'a list holds objects with string, number or boolean fields (nested objects too), or plain numbers, strings ' +
    'or booleans, e.g. useState([{title: "A", n: 1}, {title: "B", n: 2}]).';
  if (!Array.isArray(items) || !items.length) {
    throw aotError(
      `AOT: ${what} needs ≥1 initial element to infer its item shape`,
      shapeHint,
    );
  }

  // Plain values: one field, of the values' kind.
  if (!items.some(isPlainObject)) {
    const kind = kindOf(items[0]);
    if (
      !kind ||
      items.some(item => !kindOf(item) || !kindsAgree(kind, kindOf(item)!))
    ) {
      throw aotError(
        `AOT: ${what} must hold objects, or values of one kind (numbers, strings or booleans)`,
        shapeHint,
      );
    }

    const widened = items.some(item => kindOf(item) === 'float')
      ? 'float'
      : kind;
    return {fields: [{key: '', path: [], kind: widened}], plain: true};
  }

  if (!items.every(isPlainObject)) {
    throw aotError(`AOT: ${what} elements must all be objects`, shapeHint);
  }

  // Each field in the order it first appears, typed by the items' values.
  const fields = new Map<string, ItemField>();
  const objects = new Set<string>();
  const nulls = new Set<string>();
  const visit = (object: Record<string, unknown>, path: string[]): void => {
    const keys = Object.keys(object);
    if (path.length && !keys.length) {
      throw aotError(
        `AOT: field "${path.join('.')}" of ${what} is an empty object`,
        'give it a field, or leave it out.',
      );
    }

    for (const key of keys) {
      const fieldPath = [...path, key];
      const name = fieldPath.join('.');
      if (key.includes('.') || key === '') {
        throw aotError(
          `AOT: ${what} has a field named "${key}"`,
          'name each field with letters, digits or `_`.',
        );
      }

      const value = object[key];
      if (isPlainObject(value)) {
        if (fields.has(name)) {
          throw aotError(
            `AOT: field "${name}" of ${what} is a value in one item and an object in another`,
            shapeHint,
          );
        }
        objects.add(name);
        visit(value, fieldPath);
        continue;
      }

      if (value == null) {
        nulls.add(name);
        continue;
      }

      const kind = kindOf(value);
      if (!kind || objects.has(name)) {
        throw aotError(
          `AOT: field "${name}" of ${what} must be a string, a number, a boolean or an object of those`,
          shapeHint,
        );
      }

      const field = fields.get(name);
      if (!field) {
        fields.set(name, {key: name, path: fieldPath, kind});
      } else if (!kindsAgree(field.kind, kind)) {
        throw aotError(
          `AOT: field "${name}" of ${what} is a ${field.kind === 'bool' ? 'boolean' : field.kind === 'string' ? 'string' : 'number'} in one item and not in another`,
          shapeHint,
        );
      } else if (kind === 'float') {
        field.kind = 'float';
      }
    }
  };
  items.forEach(item => visit(item as Record<string, unknown>, []));

  // A field that is null in every item has no kind.
  for (const name of nulls) {
    if (!fields.has(name) && !objects.has(name)) {
      throw aotError(
        `AOT: field "${name}" of ${what} is null in every item`,
        'give one item a value for it, so the field has a kind.',
      );
    }
  }

  const struct: ItemStruct = {fields: [...fields.values()]};
  for (const field of struct.fields) {
    if (items.some(item => !itemValueAt(item, field).present)) {
      field.optional = true;
    }
  }

  return struct;
}

/**
 * A field's value in an item: missing when the item, or an object on the way, leaves it out or holds null.
 *
 * @param item  The item.
 * @param field  The field.
 *
 * @returns The value, and whether it is there.
 */
export function itemValueAt(item: unknown, field: ItemField): ItemValue {
  let value = item;
  for (const key of field.path) {
    if (!isPlainObject(value) || !Object.hasOwn(value, key)) {
      return {present: false, value: undefined};
    }
    value = value[key];
  }

  return value == null
    ? {present: false, value: undefined}
    : {present: true, value};
}

/**
 * The fields of a row in order: every value, then a flag for each optional one.
 *
 * @param struct  The items' fields.
 *
 * @returns The row's fields.
 */
export const rowEntries = (struct: ItemStruct): RowEntry[] => [
  ...struct.fields.map(field => ({field, presence: false})),
  ...struct.fields
    .filter(field => field.optional)
    .map(field => ({field, presence: true})),
];

/**
 * Where a field, or its flag, is in a row.
 *
 * @param struct  The items' fields.
 * @param field  The field.
 * @param presence  The flag rather than the value.
 *
 * @returns Its index.
 */
export function rowIndexOf(
  struct: ItemStruct,
  field: ItemField,
  presence = false,
): number {
  const index = rowEntries(struct).findIndex(
    entry => entry.field === field && entry.presence === presence,
  );
  if (index < 0) {
    throw new Error(`AOT internal: no row field for "${field.key}"`);
  }

  return index;
}

/**
 * The fields at or under a path: the field itself, or every field of the object there.
 *
 * @param struct  The items' fields.
 * @param path  The path.
 *
 * @returns The fields, none when nothing is there.
 */
export function fieldsUnder(struct: ItemStruct, path: string[]): ItemField[] {
  return struct.fields.filter(
    field =>
      field.path.length >= path.length &&
      path.every((key, index) => field.path[index] === key),
  );
}

/**
 * The list item field an expression reads, directly or through a local that stands for it (a prop given one).
 *
 * @param expression  The expression.
 *
 * @returns The read, or null.
 */
export function itemReadOf(expression: IrExpr): IrItemField | null {
  if (expression.kind === 'itemField') {
    return expression;
  }
  if (expression.kind === 'local' && expression.local.expression) {
    return itemReadOf(expression.local.expression);
  }
  return null;
}
