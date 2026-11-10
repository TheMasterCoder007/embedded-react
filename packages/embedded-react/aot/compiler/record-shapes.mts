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

// record-shapes.mts — the shapes an object state takes: its initial value, and the objects its setter is given.

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {aotError} from './diagnostics.mts';
import {flattenTree, isObjectTree, propertyKey} from './records.mts';
import {evalStatic, rethrowFoldLimit} from './static-eval.mts';
import {returnedValues} from './updaters.mts';
import type * as t from '@babel/types';
import type {FunctionNode, Scope} from './types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** A field an object is written with: its value when it folds, and what it was written as. */
export interface WrittenField {
  known: boolean;
  value?: unknown;
  node?: t.Node;
}

/** An object a setter is given, as written: its fields by their paths joined with dots. */
export interface WrittenObject {
  fields: Map<string, WrittenField>;
  /** The object as written, for errors. */
  node: t.ObjectExpression;
  /** It spreads something, so its other fields aren't known here. */
  spread: boolean;
}

/** The shapes of a tagged object state, the initial value's first. */
export interface RecordShapes {
  /** The field whose constant names the shape. */
  key: string;
  values: (string | number | boolean)[];
  /** Each shape's fields, the tag left out. */
  shapes: Set<string>[];
}

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

// Keys that never hold child AST nodes.
const SKIP_KEYS = new Set([
  'loc',
  'start',
  'end',
  'range',
  'extra',
  'leadingComments',
  'trailingComments',
  'innerComments',
]);

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Whether a value can name a shape: a string, a finite number or a boolean.
 *
 * @param value  The value.
 *
 * @returns Whether it can.
 */
const isTagValue = (value: unknown): value is string | number | boolean =>
  typeof value === 'string' ||
  typeof value === 'boolean' ||
  (typeof value === 'number' && Number.isFinite(value));

/**
 * The fields of an object literal, nested ones flattened; a field that folds holds its value.
 *
 * @param node  The object.
 * @param scope  Compile-time constants in reach.
 *
 * @returns The object.
 */
function writtenObjectOf(
  node: t.ObjectExpression,
  scope: Scope,
): WrittenObject {
  const written: WrittenObject = {fields: new Map(), spread: false, node};
  const visit = (object: t.ObjectExpression, path: string[]): void => {
    for (const property of object.properties) {
      if (property.type === 'SpreadElement') {
        written.spread = true;
        continue;
      }
      if (property.type !== 'ObjectProperty') continue;
      let key: string;
      try {
        key = propertyKey(property);
      } catch {
        continue; // refused where the write is lowered
      }
      const fieldPath = [...path, key];
      const {value: valueNode} = property;
      if (valueNode.type === 'ObjectExpression') {
        visit(valueNode, fieldPath);
        continue;
      }
      let value: unknown;
      let known = true;
      try {
        value = evalStatic(valueNode, scope);
      } catch (error) {
        rethrowFoldLimit(error);
        known = false;
      }
      if (known && isObjectTree(value)) {
        for (const [nestedKey, nested] of flattenTree(value)) {
          written.fields.set([...fieldPath, nestedKey].join('.'), {
            known: true,
            value: nested,
          });
        }
        continue;
      }
      written.fields.set(
        fieldPath.join('.'),
        known ? {known, value, node: valueNode} : {known, node: valueNode},
      );
    }
  };
  visit(node, []);
  return written;
}

/**
 * The objects a value can be: itself, a side of a ternary, or what a function returns.
 *
 * @param node  The value.
 * @param scope  Compile-time constants in reach.
 * @param objects  Takes the objects.
 */
function collectObjects(
  node: t.Node,
  scope: Scope,
  objects: WrittenObject[],
): void {
  if (node.type === 'ObjectExpression') {
    objects.push(writtenObjectOf(node, scope));
  } else if (node.type === 'ConditionalExpression') {
    collectObjects(node.consequent, scope, objects);
    collectObjects(node.alternate, scope, objects);
  } else if (
    node.type === 'ArrowFunctionExpression' ||
    node.type === 'FunctionExpression' ||
    node.type === 'FunctionDeclaration'
  ) {
    const values =
      node.body.type === 'BlockStatement'
        ? returnedValues(node.body)
        : [node.body];
    values.forEach(value => collectObjects(value, scope, objects));
  }
}

/**
 * The objects a reducer returns.
 *
 * @param reducer  The reducer.
 * @param scope  Compile-time constants in reach.
 *
 * @returns The objects.
 */
export function returnedObjects(
  reducer: FunctionNode,
  scope: Scope,
): WrittenObject[] {
  const objects: WrittenObject[] = [];
  collectObjects(reducer, scope, objects);
  return objects;
}

/**
 * The objects a setter is called with anywhere in a component: given directly, as a side of a ternary, or by an
 * updater.
 *
 * @param body  The component's body.
 * @param setter  The setter's name.
 * @param scope  Compile-time constants in reach.
 *
 * @returns The objects.
 */
export function writtenObjects(
  body: t.Node,
  setter: string,
  scope: Scope,
): WrittenObject[] {
  const objects: WrittenObject[] = [];
  const walk = (node: unknown): void => {
    if (Array.isArray(node)) {
      node.forEach(walk);
      return;
    }
    if (typeof node !== 'object' || node === null) return;
    const astNode = node as t.Node;
    if (
      astNode.type === 'CallExpression' &&
      astNode.callee.type === 'Identifier' &&
      astNode.callee.name === setter &&
      astNode.arguments[0]
    ) {
      collectObjects(astNode.arguments[0], scope, objects);
    }
    for (const [key, child] of Object.entries(astNode)) {
      if (!SKIP_KEYS.has(key)) walk(child);
    }
  };
  walk(body);
  return objects;
}

/**
 * The shapes an object state takes, when it takes more than one: the initial value and each object written out in
 * full, told apart by a field that holds a constant in each, the first such field of the initial value.
 *
 * @param name  The state's name.
 * @param setter  The setter's name.
 * @param initial  The initial value's fields.
 * @param written  The objects the setter is given.
 * @param write  How a write of an object is written, for errors: by default a call to the setter.
 *
 * @returns The shapes, or null when the state has one shape.
 */
export function recordShapesOf(
  name: string,
  setter: string,
  initial: Map<string, unknown>,
  written: WrittenObject[],
  write: (object: string) => string = object => `${setter}(${object})`,
): RecordShapes | null {
  // A write that spreads the old value keeps its shape, so one that changes a constant of the initial value and adds
  // a field no shape has is a new shape written as a spread.
  const full = written.filter(object => !object.spread);
  const known = new Set([
    ...initial.keys(),
    ...full.flatMap(object => [...object.fields.keys()]),
  ]);
  for (const object of written) {
    if (!object.spread) continue;
    const added = [...object.fields.keys()].find(key => !known.has(key));
    const retagged = [...object.fields].find(
      ([key, field]) =>
        field.known &&
        isTagValue(initial.get(key)) &&
        field.value !== initial.get(key),
    );
    if (!added || !retagged) continue;
    const [tagKey, {value}] = retagged;
    const error = aotError(
      `AOT: a write that spreads "${name}" keeps its shape, so it can't add "${added}"`,
      `to give ${name} another shape, write the new shape out in full: ` +
        `${write(`{${tagKey}: ${typeof value === 'string' ? `'${value}'` : String(value)}, ${added}: …}`)}.`,
    );
    if (object.node.loc) error.aotLoc = object.node.loc.start;
    throw error;
  }
  const keysDiffer = (object: WrittenObject): boolean =>
    [...object.fields.keys()].some(key => !initial.has(key)) ||
    (!object.spread && object.fields.size !== initial.size);
  if (!full.length || !written.some(keysDiffer)) return null;

  for (const [key, initialValue] of initial) {
    if (key.includes('.') || !isTagValue(initialValue)) continue;
    const values: (string | number | boolean)[] = [initialValue];
    const shapes = [
      new Set([...initial.keys()].filter(field => field !== key)),
    ];
    const agrees = full.every(object => {
      const tag = object.fields.get(key);
      if (!tag?.known || typeof tag.value !== typeof initialValue) return false;
      if (!isTagValue(tag.value)) return false;
      const fields = new Set(
        [...object.fields.keys()].filter(field => field !== key),
      );
      const shape = values.indexOf(tag.value);
      if (shape < 0) {
        values.push(tag.value);
        shapes.push(fields);
        return true;
      }
      return (
        shapes[shape].size === fields.size &&
        [...fields].every(field => shapes[shape].has(field))
      );
    });
    if (agrees && values.length > 1) return {key, values, shapes};
  }

  throw aotError(
    `AOT: state "${name}" is written with objects of different fields`,
    `an object of several shapes needs a field naming the shape, with a constant written in each: ` +
      `${write("{kind: 'busy', pct: 0}")}. Write each new shape out in full; a write that keeps the shape ` +
      `spreads the old value: ${write(`{...${name}, pct: 1}`)}.`,
  );
}
