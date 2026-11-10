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

// tagged-records.mts — an object state of several shapes: a hidden tag, the union of the fields, and the tests on
// the tag that narrow which fields can be read.

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {aotError} from './diagnostics.mts';
import {
  HiddenField,
  RuntimeLeaf,
  evalStatic,
  foldScope,
  hasBinding,
  rethrowFoldLimit,
  setBinding,
} from './static-eval.mts';
import {BOOL, FLOAT, INT, STRING, stateType} from './ir/types.mts';
import type * as t from '@babel/types';
import type {IrExpr} from './ir/expressions.mts';
import type {
  Env,
  Local,
  RecordState,
  RecordTag,
  ScalarState,
  Scope,
} from './types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** A record that takes one of several shapes. */
export type TaggedRecord = RecordState & {tag: RecordTag};

/** A tagged object as code reads it: the record, and the shapes it can have there. */
interface TaggedView {
  record: TaggedRecord;
  shapes: number[];
}

/** A test of a tagged object's tag against a constant, `s.kind === 'busy'`: the shapes each outcome leaves. */
export interface TagTest {
  name: string;
  view: TaggedView;
  whenTrue: number[];
  whenFalse: number[];
}

/** The tagged objects a test narrows, by the names that read them, when it is true and when it is false. */
export interface Narrowing {
  whenTrue: Map<string, Record<string, unknown>>;
  whenFalse: Map<string, Record<string, unknown>>;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/** Each tagged object code can read, and what it is. */
const views = new WeakMap<object, TaggedView>();

/** Each read of a tag worked out at run time, and the record whose tag it is. */
const tagLeaves = new WeakMap<RuntimeLeaf, RecordState>();

/**
 * A read of a scalar state, as a local that stands for it.
 *
 * @param state  The state.
 *
 * @returns The local.
 */
export const stateLocal = (state: ScalarState): Local => ({
  slotKind: state.slotKind,
  isBoolean: state.isBoolean,
  ...(state.widened === 'number' ? {isLiteral: true} : {}),
  source: {kind: 'state', state, location: undefined},
  expression: {kind: 'state', state, type: stateType(state)},
});

/**
 * A tag value as code writes it.
 *
 * @param value  The value.
 *
 * @returns `'busy'`, `2` or `true`.
 */
export const tagText = (value: unknown): string =>
  typeof value === 'string' ? `'${value}'` : String(value);

/**
 * The shapes' tag values, as a list for a message.
 *
 * @param values  The values.
 *
 * @returns `'idle' or 'busy'`.
 */
const tagList = (values: unknown[]): string => values.map(tagText).join(' or ');

/**
 * Whether a shape's tag equals a value, as `===` or `==` compares them.
 *
 * @param record  The record.
 * @param value  The value it is compared with.
 * @param strict  The test is `===` or `!==`.
 * @param shape  The shape.
 *
 * @returns Whether the tag equals the value.
 */
const tagEquals = (
  record: TaggedRecord,
  value: unknown,
  strict: boolean,
  shape: number,
): boolean =>
  strict
    ? record.tag.values[shape] === value
    : record.tag.values[shape] == value;

/**
 * A comparison of the hidden tag with a shape's index.
 *
 * @param record  The record.
 * @param shape  The shape.
 * @param operator  `==` or `!=`.
 *
 * @returns The comparison.
 */
function tagIs(
  record: TaggedRecord,
  shape: number,
  operator: '==' | '!=' = '==',
): IrExpr {
  const {state} = record.tag;
  return {
    kind: 'compare',
    operator,
    left: {kind: 'state', state, type: stateType(state)},
    right: {kind: 'number', value: shape, type: INT},
    type: BOOL,
  };
}

/**
 * A read of the tag where the shape isn't known: the tag value of the shape the hidden tag names.
 *
 * @param record  The record.
 * @param shapes  The shapes it can have there.
 *
 * @returns The read.
 */
function tagLeaf(record: TaggedRecord, shapes: number[]): RuntimeLeaf {
  const values = shapes.map(shape => record.tag.values[shape]);
  const isString = typeof values[0] === 'string';
  const isBoolean = typeof values[0] === 'boolean';
  const isFloat = values.some(
    value => typeof value === 'number' && !Number.isInteger(value),
  );
  const type = isString ? STRING : isBoolean ? BOOL : isFloat ? FLOAT : INT;
  const constant = (value: string | number | boolean): IrExpr =>
    typeof value === 'string'
      ? {kind: 'string', value, type}
      : typeof value === 'boolean'
        ? {kind: 'boolean', value, type}
        : {kind: 'number', value, type};
  let expression = constant(values.at(-1)!);
  for (let position = shapes.length - 2; position >= 0; position--) {
    expression = {
      kind: 'conditional',
      test: tagIs(record, shapes[position]),
      consequent: constant(values[position]),
      alternate: expression,
      type,
    };
  }
  const leaf = new RuntimeLeaf({
    slotKind: isString ? 'string' : isFloat ? 'float' : 'int',
    isBoolean,
    source: {kind: 'fixed', expression, location: undefined},
    expression,
  });
  tagLeaves.set(leaf, record);
  return leaf;
}

/**
 * A field only some of the shapes have, where the shape isn't known.
 *
 * @param record  The record.
 * @param key  The field's path, joined with dots.
 * @param field  Its slot.
 *
 * @returns The field.
 */
function hiddenField(
  record: TaggedRecord,
  key: string,
  field: ScalarState,
): HiddenField {
  const {tag, name} = record;
  const owners = tag.values.filter((_, shape) => tag.shapes[shape].has(key));
  const first = tagText(owners[0]);
  return new HiddenField(
    field,
    `AOT: "${name}.${key}" is only there when ${name}.${tag.key} is ${tagList(owners)}`,
    `test the tag first: ${name}.${tag.key} === ${first} && …, or if (${name}.${tag.key} === ${first}) { … }.`,
  );
}

/**
 * A tagged object as code reads it where it can have only some shapes: the tag a constant when there is one shape,
 * the fields every shape has, and the others hidden.
 *
 * @param record  The record.
 * @param shapes  The shapes.
 *
 * @returns The object.
 */
export function taggedTreeOf(
  record: TaggedRecord,
  shapes: number[],
): Record<string, unknown> {
  const tree: Record<string, unknown> = {};
  const place = (key: string, value: unknown): void => {
    const path = key.split('.');
    let branch = tree;
    for (const part of path.slice(0, -1)) {
      branch = (branch[part] ??= {}) as Record<string, unknown>;
    }
    branch[path.at(-1)!] = value;
  };
  const {tag} = record;
  place(
    tag.key,
    shapes.length === 1 ? tag.values[shapes[0]] : tagLeaf(record, shapes),
  );
  for (const [key, field] of record.fields) {
    const visible = shapes.every(shape => tag.shapes[shape].has(key));
    place(
      key,
      visible
        ? new RuntimeLeaf(stateLocal(field))
        : hiddenField(record, key, field),
    );
  }
  views.set(tree, {record, shapes});
  return tree;
}

/**
 * The shapes a tagged object can have where it is read.
 *
 * @param value  The object.
 *
 * @returns The shapes, or null when it isn't a tagged object.
 */
export const shapesOf = (value: unknown): number[] | null =>
  (typeof value === 'object' && value !== null && views.get(value)?.shapes) ||
  null;

/**
 * Whether a value is a read of a record's own tag where its shape isn't known.
 *
 * @param value  The value.
 * @param record  The record.
 *
 * @returns Whether it is.
 */
export const isTagOf = (value: unknown, record: RecordState): boolean =>
  value instanceof RuntimeLeaf && tagLeaves.get(value) === record;

/**
 * The tagged object a name reads, if it reads one.
 *
 * @param node  The expression.
 * @param environment  The expression environment.
 *
 * @returns The name and the object, or null.
 */
function viewOf(
  node: t.Node,
  environment: Env,
): {name: string; view: TaggedView} | null {
  if (
    node.type !== 'Identifier' ||
    environment.locals.has(node.name) ||
    !hasBinding(environment.constants, node.name)
  ) {
    return null;
  }
  const value = environment.constants[node.name];
  const view =
    typeof value === 'object' && value !== null ? views.get(value) : undefined;
  return view ? {name: node.name, view} : null;
}

/**
 * The tagged object whose tag an expression reads: `s.kind`.
 *
 * @param node  The expression.
 * @param environment  The expression environment.
 *
 * @returns The name and the object, or null.
 */
function tagReadOf(
  node: t.Node,
  environment: Env,
): {name: string; view: TaggedView} | null {
  if (node.type !== 'MemberExpression') return null;
  const key = !node.computed
    ? (node.property as t.Identifier).name
    : node.property.type === 'StringLiteral'
      ? node.property.value
      : null;
  const found = viewOf(node.object, environment);
  return found && key === found.view.record.tag.key ? found : null;
}

/**
 * Reads a test of a tagged object's tag against a constant: `s.kind === 'busy'`, `'idle' !== s.kind`.
 *
 * @param node  The test.
 * @param environment  The expression environment.
 *
 * @returns The test, or null when it is none.
 */
export function tagTestOf(node: t.Node, environment: Env): TagTest | null {
  if (
    node.type !== 'BinaryExpression' ||
    !['===', '==', '!==', '!='].includes(node.operator)
  ) {
    return null;
  }
  for (const [member, other] of [
    [node.left, node.right],
    [node.right, node.left],
  ]) {
    const found = tagReadOf(member, environment);
    if (!found) continue;
    const key = found.view.record.tag.key;

    let value: unknown;
    try {
      value = evalStatic(
        other,
        foldScope(environment, environment.constants ?? {}),
      );
    } catch (error) {
      rethrowFoldLimit(error);
      continue;
    }
    const {name, view} = found;
    const {record} = view;
    const strict = node.operator.length === 3;
    const all = record.tag.values.map((_, shape) => shape);
    if (!all.some(shape => tagEquals(record, value, strict, shape))) {
      throw aotError(
        `AOT: ${name}.${key} is never ${tagText(value)}`,
        `its shapes have ${key} ${tagList(record.tag.values)}.`,
      );
    }
    const matching = view.shapes.filter(shape =>
      tagEquals(record, value, strict, shape),
    );
    const rest = view.shapes.filter(shape => !matching.includes(shape));
    const negated = node.operator.startsWith('!');
    return {
      name,
      view,
      whenTrue: negated ? rest : matching,
      whenFalse: negated ? matching : rest,
    };
  }
  return null;
}

/**
 * Lowers a test of a tagged object's tag: a comparison of the hidden tag, or a constant when the shapes it can
 * have there decide it.
 *
 * @param test  The test.
 *
 * @returns The test's value.
 */
export function lowerTagTest(test: TagTest): IrExpr {
  if (!test.whenTrue.length || !test.whenFalse.length) {
    return {kind: 'boolean', value: !test.whenFalse.length, type: BOOL};
  }
  const {record} = test.view;
  if (test.whenTrue.length === 1) return tagIs(record, test.whenTrue[0]);
  if (test.whenFalse.length === 1) {
    return tagIs(record, test.whenFalse[0], '!=');
  }
  return test.whenTrue
    .map(shape => tagIs(record, shape))
    .reduce((left, right) => ({
      kind: 'logical',
      operator: '||',
      left,
      right,
      type: BOOL,
    }));
}

/**
 * The tagged objects a test narrows: a test on a tag, `!`, `&&` and `||` of them.
 *
 * @param test  The test.
 * @param environment  The expression environment.
 *
 * @returns What each outcome narrows.
 */
export function narrowOn(test: t.Node, environment: Env): Narrowing {
  if (test.type === 'UnaryExpression' && test.operator === '!') {
    const inner = narrowOn(test.argument, environment);
    return {whenTrue: inner.whenFalse, whenFalse: inner.whenTrue};
  }
  if (
    test.type === 'LogicalExpression' &&
    (test.operator === '&&' || test.operator === '||')
  ) {
    const isAnd = test.operator === '&&';
    const left = narrowOn(test.left, environment);
    const leftSide = isAnd ? left.whenTrue : left.whenFalse;
    const right = narrowOn(test.right, narrowed(environment, leftSide));
    const both = new Map([
      ...leftSide,
      ...(isAnd ? right.whenTrue : right.whenFalse),
    ]);
    return isAnd
      ? {whenTrue: both, whenFalse: new Map()}
      : {whenTrue: new Map(), whenFalse: both};
  }

  // `r.ok` as a test: the shapes whose tag is truthy.
  const read = tagReadOf(test, environment);
  const truthy = read?.view.shapes.filter(
    shape => read.view.record.tag.values[shape],
  );
  const tagTest: TagTest | null = read
    ? {
        ...read,
        whenTrue: truthy!,
        whenFalse: read.view.shapes.filter(shape => !truthy!.includes(shape)),
      }
    : tagTestOf(test, environment);
  const side = (shapes: number[]): Map<string, Record<string, unknown>> =>
    tagTest && shapes.length && shapes.length !== tagTest.view.shapes.length
      ? new Map([[tagTest.name, taggedTreeOf(tagTest.view.record, shapes)]])
      : new Map();
  return {
    whenTrue: side(tagTest?.whenTrue ?? []),
    whenFalse: side(tagTest?.whenFalse ?? []),
  };
}

/**
 * Constants with names bound to narrowed objects.
 *
 * @param scope  The constants.
 * @param bindings  The objects, by name.
 *
 * @returns The constants.
 */
export function narrowedScope(
  scope: Scope,
  bindings: Map<string, Record<string, unknown>>,
): Scope {
  if (!bindings.size) return scope;
  const constants = {...scope};
  for (const [name, tree] of bindings) setBinding(constants, name, tree);
  return constants;
}

/**
 * An environment with names bound to narrowed objects.
 *
 * @param environment  The expression environment.
 * @param bindings  The objects, by name.
 *
 * @returns The environment.
 */
export function narrowed(
  environment: Env,
  bindings: Map<string, Record<string, unknown>>,
): Env {
  if (!bindings.size) return environment;
  return {
    ...environment,
    constants: narrowedScope(environment.constants, bindings),
  };
}
