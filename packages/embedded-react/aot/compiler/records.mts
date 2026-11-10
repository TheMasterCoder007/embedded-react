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
import {
  HiddenField,
  RuntimeLeaf,
  evalStatic,
  foldScope,
  rethrowFoldLimit,
} from './static-eval.mts';
import {
  isTagOf,
  narrowOn,
  narrowed,
  shapesOf,
  tagText,
} from './tagged-records.mts';
import {STRING, typeOfSlotValue} from './ir/types.mts';
import type * as t from '@babel/types';
import type {IrExpr} from './ir/expressions.mts';
import type {IrStmt} from './ir/statements.mts';
import type {Env, Local, RecordState, ScalarState} from './types.mts';
import type {TaggedRecord} from './tagged-records.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Classes
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * A field of an object written in code, not yet lowered.
 *
 * @class AstLeaf
 */
export class AstLeaf {
  readonly node: t.Node;
  /** Locals the node reads that the environment lacks: runtime values an object held. */
  readonly locals: Map<string, Local>;

  /**
   * @param node  The field's value.
   * @param locals  Locals it reads that the environment lacks.
   */
  constructor(node: t.Node, locals: Map<string, Local> = new Map()) {
    this.node = node;
    this.locals = locals;
  }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** An object's fields: constants, RuntimeLeafs and AstLeafs, nested as the object is. */
export type ObjectTree = Record<string, unknown>;

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/** Counts the names lowering makes up, which a `$` keeps apart from any JS name. */
let syntheticCount = 0;

/**
 * Whether a value is an object of fields, not a leaf such as a RuntimeLeaf.
 *
 * @param value  The value.
 *
 * @returns Whether it is.
 */
export function isObjectTree(value: unknown): value is ObjectTree {
  if (typeof value !== 'object' || value === null) return false;
  const prototype = Object.getPrototypeOf(value);
  return prototype === Object.prototype || prototype === null;
}

/**
 * The key an object property is written with.
 *
 * @param property  The property.
 *
 * @returns The key.
 */
export function propertyKey(property: t.ObjectProperty): string {
  const {key} = property;
  if (!property.computed && key.type === 'Identifier') return key.name;
  if (key.type === 'StringLiteral') return key.value;
  if (key.type === 'NumericLiteral') return String(key.value);
  throw aotError(
    'AOT: an object field needs a fixed name',
    'write the key out: {label: …}. Computed keys are not lowered.',
  );
}

/**
 * The fields of an object an expression names: an object literal (spreads too), or a name or member that folds to
 * an object (an object state, an object built in render or in the handler, a constant).
 *
 * @param node  The expression.
 * @param environment  The expression environment.
 *
 * @returns Its fields, or null when it names no object.
 */
export function objectTreeOf(
  node: t.Node,
  environment: Env,
): ObjectTree | null {
  if (
    node.type === 'TSAsExpression' ||
    node.type === 'TSSatisfiesExpression' ||
    node.type === 'TSNonNullExpression'
  ) {
    return objectTreeOf(node.expression, environment);
  }

  if (node.type === 'ObjectExpression') {
    const tree: ObjectTree = {};
    for (const property of node.properties) {
      if (property.type === 'SpreadElement') {
        const spread = objectTreeOf(property.argument, environment);
        if (!spread) {
          throw aotError(
            'AOT: only an object can be spread into an object',
            'spread an object state or an object built from fields: {...p, x: 1}.',
          );
        }
        Object.assign(tree, spread);
        continue;
      }
      if (property.type !== 'ObjectProperty') {
        throw aotError(
          'AOT: an object can only hold plain fields and spreads',
          'write each field out: {label: "A", value: n}. Methods are not lowered.',
        );
      }
      tree[propertyKey(property)] =
        objectTreeOf(property.value, environment) ??
        new AstLeaf(property.value);
    }
    return tree;
  }

  // `cond ? a : b` of two objects with the same fields: each field picks its side.
  if (node.type === 'ConditionalExpression') {
    const narrowing = narrowOn(node.test, environment);
    const consequent = objectTreeOf(
      node.consequent,
      narrowed(environment, narrowing.whenTrue),
    );
    const alternate = objectTreeOf(
      node.alternate,
      narrowed(environment, narrowing.whenFalse),
    );
    if (!consequent || !alternate) return null;
    const sides = [flattenTree(consequent), flattenTree(alternate)];
    const keys = [...sides[0].keys()];
    if (keys.length !== sides[1].size || keys.some(key => !sides[1].has(key))) {
      throw aotError(
        'AOT: both sides of a ternary must be objects with the same fields',
        'give each side every field: cond ? {...p, x: 1} : {...p, x: 2}.',
      );
    }
    const tree: ObjectTree = {};
    for (const key of keys) {
      const [consequentLeaf, alternateLeaf] = sides.map(side =>
        leafNode(side.get(key), environment),
      );
      const added = new Map(
        [
          ...consequentLeaf.environment.locals,
          ...alternateLeaf.environment.locals,
        ].filter(([name]) => !environment.locals.has(name)),
      );
      let branch = tree;
      const path = key.split('.');
      for (const part of path.slice(0, -1)) {
        branch = (branch[part] ??= {}) as ObjectTree;
      }
      branch[path.at(-1)!] = new AstLeaf(
        {
          type: 'ConditionalExpression',
          test: node.test,
          consequent: consequentLeaf.node,
          alternate: alternateLeaf.node,
        } as t.ConditionalExpression,
        added,
      );
    }
    return tree;
  }

  if (
    node.type !== 'Identifier' &&
    node.type !== 'MemberExpression' &&
    node.type !== 'OptionalMemberExpression'
  ) {
    return null;
  }

  try {
    const value = evalStatic(
      node,
      foldScope(environment, environment.constants ?? {}),
      'boot',
    );
    return isObjectTree(value) ? value : null;
  } catch (error) {
    rethrowFoldLimit(error);
    return null;
  }
}

/**
 * An object's fields by their paths joined with dots.
 *
 * @param tree  The object.
 *
 * @returns Its fields.
 */
export function flattenTree(tree: ObjectTree): Map<string, unknown> {
  const fields = new Map<string, unknown>();
  const walk = (object: ObjectTree, path: string[]): void => {
    for (const [key, value] of Object.entries(object)) {
      if (key === '' || key.includes('.')) {
        throw aotError(
          `AOT: an object has a field named "${key}"`,
          'name each field with letters, digits or `_`.',
        );
      }
      if (isObjectTree(value)) {
        walk(value, [...path, key]);
      } else {
        fields.set([...path, key].join('.'), value);
      }
    }
  };
  walk(tree, []);
  return fields;
}

/**
 * An expression for one field of an object, and the environment it lowers in: what was written, a local that stands
 * for a runtime value, or a literal for a constant.
 *
 * @param value  The field.
 * @param environment  The expression environment.
 *
 * @returns The expression and its environment.
 */
export function leafNode(
  value: unknown,
  environment: Env,
): {node: t.Node; environment: Env} {
  if (value instanceof AstLeaf) {
    return {
      node: value.node,
      environment: value.locals.size
        ? {
            ...environment,
            locals: new Map([...environment.locals, ...value.locals]),
          }
        : environment,
    };
  }
  if (value instanceof HiddenField) throw value.error();
  if (value instanceof RuntimeLeaf) {
    const name = `field$${syntheticCount++}`;
    return {
      node: {type: 'Identifier', name} as t.Identifier,
      environment: {
        ...environment,
        locals: new Map(environment.locals).set(name, value.local),
      },
    };
  }
  if (typeof value === 'number') {
    return {
      node:
        value < 0 || Object.is(value, -0)
          ? ({
              type: 'UnaryExpression',
              operator: '-',
              prefix: true,
              argument: {type: 'NumericLiteral', value: -value},
            } as t.UnaryExpression)
          : ({type: 'NumericLiteral', value} as t.NumericLiteral),
      environment,
    };
  }
  if (typeof value === 'string') {
    return {
      node: {type: 'StringLiteral', value} as t.StringLiteral,
      environment,
    };
  }
  if (typeof value === 'boolean') {
    return {
      node: {type: 'BooleanLiteral', value} as t.BooleanLiteral,
      environment,
    };
  }
  throw aotError(
    `AOT: a field holding ${value === undefined ? 'undefined' : value === null ? 'null' : 'a ' + typeof value} cannot be stored`,
    "give each field a number, a string or a boolean: 0, '' or false.",
  );
}

/**
 * The field a member of an object names, `p.pos.x`, as the object holds it.
 *
 * @param node  The expression.
 * @param environment  The expression environment.
 *
 * @returns The field, or undefined when it names none.
 */
function fieldRead(node: t.Node, environment: Env): unknown {
  if (node.type !== 'MemberExpression') return undefined;
  const key = !node.computed
    ? (node.property as t.Identifier).name
    : node.property.type === 'StringLiteral'
      ? node.property.value
      : null;
  const object = objectTreeOf(node.object, environment);
  return object && key !== null ? object[key] : undefined;
}

/**
 * Whether a lowered value reads any of a set of states.
 *
 * @param value  The value: IR, at any depth.
 * @param states  The states.
 *
 * @returns Whether it does.
 */
function readsAny(value: unknown, states: Set<ScalarState>): boolean {
  const seen = new Set<object>();
  const walk = (part: unknown): boolean => {
    if (typeof part !== 'object' || part === null || seen.has(part)) {
      return false;
    }
    seen.add(part);
    const record = part as Record<string, unknown>;
    if (record.kind === 'state' && states.has(record.state as ScalarState)) {
      return true;
    }
    return Object.values(record).some(walk);
  };
  return walk(value);
}

/** How a record's writes are lowered. */
export interface RecordLowering {
  /** Lowers a write of an expression to one field. */
  field: (field: ScalarState, node: t.Node, environment: Env) => IrStmt;
  /** Lowers a test. */
  test: (node: t.Node, environment: Env) => IrExpr;
}

/**
 * The fields of the object a setter is given.
 *
 * @param record  The object state.
 * @param valueNode  The new object.
 * @param environment  The expression environment.
 *
 * @returns Its fields.
 */
function givenLeaves(
  record: RecordState,
  valueNode: t.Node | undefined,
  environment: Env,
): Map<string, unknown> {
  const setter = record.setter ?? `set ${record.name}`;
  const tree = valueNode ? objectTreeOf(valueNode, environment) : null;
  if (!tree) {
    throw aotError(
      `AOT: ${setter} needs an object with the fields of "${record.name}"`,
      `pass an object: ${setter}({...${record.name}, field: value}).`,
    );
  }
  const leaves = flattenTree(tree);
  for (const key of leaves.keys()) {
    if (!record.fields.has(key) && key !== record.tag?.key) {
      throw aotError(
        `AOT: state "${record.name}" has no field "${key}"`,
        'an object state holds the fields it starts with; give the field a starting value in useState.',
      );
    }
  }
  return leaves;
}

/**
 * Lowers a write to an object state: every field set from the new object, a field left as it is skipped. A value
 * that reads a field written before it is worked out first, so `{x: p.y, y: p.x}` swaps.
 *
 * @param record  The object state.
 * @param valueNode  The new object.
 * @param environment  The expression environment.
 * @param lowering  How its writes are lowered.
 *
 * @returns The writes.
 */
export function lowerRecordSet(
  record: RecordState,
  valueNode: t.Node | undefined,
  environment: Env,
  lowering: RecordLowering,
): IrStmt[] {
  if (record.tag) {
    return lowerTaggedSet(
      record as TaggedRecord,
      valueNode,
      environment,
      lowering,
    );
  }

  const setter = record.setter ?? `set ${record.name}`;
  const leaves = givenLeaves(record, valueNode, environment);
  for (const key of record.fields.keys()) {
    if (!leaves.has(key)) {
      throw aotError(
        `AOT: ${setter} leaves out "${record.name}.${key}"`,
        `an object state keeps every field it starts with; spread the old value: ${setter}({...${record.name}, …}).`,
      );
    }
  }
  return writeFields(
    [...record.fields].map(([key, field]) => [field, leaves.get(key)]),
    environment,
    lowering.field,
  );
}

/**
 * A field's value when it folds to a constant.
 *
 * @param value  The field.
 * @param environment  The expression environment.
 *
 * @returns The constant, or the field as it is.
 */
function constantOf(value: unknown, environment: Env): unknown {
  if (!(value instanceof AstLeaf) || value.locals.size) return value;
  try {
    return evalStatic(
      value.node,
      foldScope(environment, environment.constants ?? {}),
    );
  } catch (error) {
    rethrowFoldLimit(error);
    return value;
  }
}

/**
 * Lowers a write to a tagged object state. An object with its tag written out sets that shape: the hidden tag, and
 * each of the shape's fields. One that keeps the tag (a spread of the state) keeps the shape. A ternary sets the
 * shape its test picks.
 *
 * @param record  The object state.
 * @param valueNode  The new object.
 * @param environment  The expression environment.
 * @param lowering  How its writes are lowered.
 *
 * @returns The writes.
 */
function lowerTaggedSet(
  record: TaggedRecord,
  valueNode: t.Node | undefined,
  environment: Env,
  lowering: RecordLowering,
): IrStmt[] {
  const {name, tag} = record;
  const setter = record.setter ?? `set ${name}`;

  if (valueNode?.type === 'ConditionalExpression') {
    const narrowing = narrowOn(valueNode.test, environment);
    const consequent = lowerTaggedSet(
      record,
      valueNode.consequent,
      narrowed(environment, narrowing.whenTrue),
      lowering,
    );
    const alternate = lowerTaggedSet(
      record,
      valueNode.alternate,
      narrowed(environment, narrowing.whenFalse),
      lowering,
    );
    if (!consequent.length && !alternate.length) return [];
    return [
      {
        kind: 'if',
        test: lowering.test(valueNode.test, environment),
        consequent,
        alternate: alternate.length ? alternate : null,
      },
    ];
  }

  const leaves = givenLeaves(record, valueNode, environment);
  const leavesOut = (key: string, hint: string): Error =>
    aotError(`AOT: ${setter} leaves out "${name}.${key}"`, hint);
  const tagValue = constantOf(leaves.get(tag.key), environment);

  // A spread of the state keeps its shape.
  if (isTagOf(tagValue, record)) {
    for (const key of record.fields.keys()) {
      if (!leaves.has(key)) {
        throw leavesOut(
          key,
          `spread the old value to keep its shape: ${setter}({...${name}, …}).`,
        );
      }
    }
    return writeFields(
      [...record.fields].map(([key, field]) => [field, leaves.get(key)]),
      environment,
      lowering.field,
    );
  }

  if (
    typeof tagValue !== 'string' &&
    typeof tagValue !== 'number' &&
    typeof tagValue !== 'boolean'
  ) {
    throw aotError(
      `AOT: ${setter} needs to know which shape "${name}" takes`,
      `write the tag out, ${setter}({${tag.key}: ${tagText(tag.values[0])}, …}), or test ${name}.${tag.key} first.`,
    );
  }
  const shape = tag.values.indexOf(tagValue);
  if (shape < 0) {
    throw aotError(
      `AOT: state "${name}" has no shape with ${tag.key} ${tagText(tagValue)}`,
      `its shapes are its initial value and the objects ${setter} is given in full; ` +
        `write this one out in full once: ${setter}({${tag.key}: ${tagText(tagValue)}, …}).`,
    );
  }

  const fields = tag.shapes[shape];
  for (const key of fields) {
    if (!leaves.has(key) || leaves.get(key) instanceof HiddenField) {
      throw leavesOut(
        key,
        `give every field of the ${tagText(tagValue)} shape a value.`,
      );
    }
  }
  for (const [key, value] of leaves) {
    if (key === tag.key || fields.has(key) || value instanceof HiddenField) {
      continue;
    }
    throw aotError(
      `AOT: the ${tagText(tagValue)} shape of "${name}" has no field "${key}"`,
      `write the shape out with its own fields: ${setter}({${tag.key}: ${tagText(tagValue)}, …}).`,
    );
  }

  // The tag is written unless the state can only have that shape here.
  const here = environment.locals.has(name)
    ? null
    : shapesOf(environment.constants?.[name]);
  const entries: [ScalarState, unknown][] =
    here?.length === 1 && here[0] === shape ? [] : [[tag.state, shape]];
  for (const [key, field] of record.fields) {
    if (fields.has(key)) entries.push([field, leaves.get(key)]);
  }
  return writeFields(entries, environment, lowering.field);
}

/**
 * Lowers the writes of values to fields: a field set to itself is skipped, and a value that reads a field written
 * before it is worked out first.
 *
 * @param entries  Each field and its value.
 * @param environment  The expression environment.
 * @param lowerField  Lowers a write of an expression to one field.
 *
 * @returns The writes.
 */
function writeFields(
  entries: [ScalarState, unknown][],
  environment: Env,
  lowerField: RecordLowering['field'],
): IrStmt[] {
  // A field set to itself keeps its value.
  const writes: {field: ScalarState; statement: IrStmt}[] = [];
  for (const [field, value] of entries) {
    if (value instanceof HiddenField && value.field === field) continue;
    const read =
      value instanceof AstLeaf && !value.locals.size
        ? fieldRead(value.node, environment)
        : value;
    const expression =
      read instanceof RuntimeLeaf ? read.local.expression : undefined;
    if (expression?.kind === 'state' && expression.state === field) continue;
    const leaf = leafNode(value, environment);
    writes.push({
      field,
      statement: lowerField(field, leaf.node, leaf.environment),
    });
  }

  // Stage each value that reads a field written before it.
  const staged: IrStmt[] = [];
  const written = new Set<ScalarState>();
  for (const write of writes) {
    const {statement, field} = write;
    if (written.size && readsAny(statement, written)) {
      const name = `local_${field.slotName}`;
      if (statement.kind === 'setState') {
        staged.push({
          kind: 'declareLocal',
          name,
          initializer: statement.value,
          isHoisted: false,
        });
        const local: Local = {slotKind: field.slotKind, name};
        write.statement = {
          ...statement,
          value: {
            kind: 'local',
            local,
            type: statement.value.type,
          } as IrExpr,
        };
      } else if (statement.kind === 'setStateText') {
        staged.push({
          kind: 'declareLocal',
          name,
          initializer: {kind: 'string', value: '', type: STRING} as IrExpr,
          text: statement.text,
          isHoisted: false,
        });
        const local: Local = {slotKind: 'string', name};
        write.statement = {
          ...statement,
          text: {
            parts: [
              {
                kind: 'value',
                value: {kind: 'local', local, type: typeOfSlotValue(local)},
              },
            ],
          },
        };
      }
    }
    written.add(field);
  }

  return [...staged, ...writes.map(write => write.statement)];
}

/** Binds a name to an object once its fields' locals are declared. */
export interface ObjectBinding {
  kind: 'bindObject';
  name: string;
  tree: ObjectTree;
}

/** What a handler declaration of an object expands to: a local per field, then the object bound over them. */
export interface ObjectDeclaration {
  entries: (t.VariableDeclarator | ObjectBinding)[];
  /** Locals the expanded declarators read: runtime values the object already held. */
  locals: Map<string, Local>;
}

const identifier = (name: string): t.Identifier =>
  ({type: 'Identifier', name}) as t.Identifier;

const memberOf = (object: t.Expression, key: string): t.MemberExpression =>
  ({
    type: 'MemberExpression',
    object,
    property: {type: 'StringLiteral', value: key},
    computed: true,
  }) as t.MemberExpression;

const declaratorOf = (
  id: t.Identifier | t.ObjectPattern,
  init: t.Expression,
): t.VariableDeclarator =>
  ({type: 'VariableDeclarator', id, init}) as t.VariableDeclarator;

/**
 * The declarators an object pattern destructures into, one per name, each reading its field of `base`.
 *
 * @param pattern  The pattern.
 * @param base  What it destructures.
 * @param tree  The object's fields.
 *
 * @returns The declarators.
 */
function patternDeclarators(
  pattern: t.ObjectPattern,
  base: t.Expression,
  tree: ObjectTree,
): t.VariableDeclarator[] {
  const declarators: t.VariableDeclarator[] = [];
  const taken = new Set<string>();
  for (const property of pattern.properties) {
    if (property.type === 'RestElement') {
      if (property.argument.type !== 'Identifier') {
        throw aotError('AOT: a rest element must be a plain name');
      }
      const rest = Object.keys(tree).filter(key => !taken.has(key));
      declarators.push(
        declaratorOf(property.argument, {
          type: 'ObjectExpression',
          properties: rest.map(key => ({
            type: 'ObjectProperty',
            key: {type: 'StringLiteral', value: key},
            value: memberOf(base, key),
            computed: false,
            shorthand: false,
          })),
        } as t.ObjectExpression),
      );
      continue;
    }
    const key = propertyKey(property);
    taken.add(key);
    if (!Object.hasOwn(tree, key)) {
      throw aotError(
        `AOT: the object has no field "${key}" to destructure`,
        'destructure only the fields the object holds.',
      );
    }
    const {value} = property;
    if (value.type === 'Identifier' || value.type === 'ObjectPattern') {
      declarators.push(declaratorOf(value, memberOf(base, key)));
      continue;
    }
    throw aotError(
      'AOT: a destructured field must be a plain name or a nested object pattern',
      'defaults never apply, since every field holds a value; drop the `= …`.',
    );
  }
  return declarators;
}

/**
 * Expands a handler's `const` of an object, or a destructuring of one, into a local per field: what JS would
 * read from the object is worked out where the `const` is.
 *
 * @param declarator  The declarator.
 * @param environment  The expression environment.
 *
 * @returns The expansion, or null when it declares no object.
 */
export function expandObjectDeclarator(
  declarator: t.VariableDeclarator,
  environment: Env,
): ObjectDeclaration | null {
  const {id, init} = declarator;
  if (!init) return null;

  if (id.type === 'ObjectPattern') {
    const tree = objectTreeOf(init, environment);
    if (!tree) {
      throw aotError(
        'AOT: only an object can be destructured',
        'destructure an object state, or an object built from fields.',
      );
    }
    if (init.type === 'Identifier' || init.type === 'MemberExpression') {
      return {
        entries: patternDeclarators(id, init, tree),
        locals: new Map(),
      };
    }
    const temporary = identifier(`object$${syntheticCount++}`);
    return {
      entries: [
        declaratorOf(temporary, init),
        ...patternDeclarators(id, temporary, tree),
      ],
      locals: new Map(),
    };
  }

  if (id.type !== 'Identifier') return null;
  const tree = objectTreeOf(init, environment);
  if (!tree) return null;
  const entries: ObjectDeclaration['entries'] = [];
  const locals = new Map<string, Local>();
  for (const [key, value] of flattenTree(tree)) {
    const fieldName = identifier(`${id.name}.${key}`);
    if (value instanceof AstLeaf) {
      for (const [name, local] of value.locals) locals.set(name, local);
      entries.push(declaratorOf(fieldName, value.node as t.Expression));
    } else if (value instanceof RuntimeLeaf) {
      const name = `field$${syntheticCount++}`;
      locals.set(name, value.local);
      entries.push(declaratorOf(fieldName, identifier(name)));
    }
  }
  entries.push({kind: 'bindObject', name: id.name, tree});
  return {entries, locals};
}

/**
 * An object whose fields are read from the locals its declaration expanded to; a constant field stays one.
 *
 * @param binding  The object and its name.
 * @param locals  The locals in reach.
 *
 * @returns The object.
 */
export function boundObjectOf(
  {name, tree}: ObjectBinding,
  locals: Map<string, Local>,
): ObjectTree {
  const rebuild = (object: ObjectTree, path: string[]): ObjectTree =>
    Object.fromEntries(
      Object.entries(object).map(([key, value]) => {
        const fieldPath = [...path, key];
        if (isObjectTree(value)) return [key, rebuild(value, fieldPath)];
        if (value instanceof AstLeaf || value instanceof RuntimeLeaf) {
          const fieldName = `${name}.${fieldPath.join('.')}`;
          const local = locals.get(fieldName);
          if (!local) {
            throw new Error(`AOT internal: no local for ${fieldName}`);
          }
          return [key, new RuntimeLeaf(local)];
        }
        return [key, value];
      }),
    );
  return rebuild(tree, []);
}
