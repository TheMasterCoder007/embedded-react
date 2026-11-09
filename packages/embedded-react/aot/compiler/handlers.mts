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

import {parseColor, parsePath} from '../../src/embedded-react/svg-ops.js';
import {
  aotError,
  constantText,
  ENGINE_STRING_MAX,
  withLoc,
} from './diagnostics.mts';
import type {SourcePosition} from './diagnostics.mts';
import {evalStatic} from './static-eval.mts';
import {fieldsUnder} from './list-items.mts';
import {
  mix64Error,
  staticInt,
  isTime64,
  constDivisor,
  lowerSourcedWide,
  lowerSource,
  slotValueOf,
  lowerExpr,
  lowerExprWide,
  lowerText,
  buildsText,
} from './expressions.mts';
import {isFn} from './collect.mts';
import {
  isAnimatedCall,
  resolveAnim,
  animValues,
  lowerAnimateStart,
  stopWaitOwner,
} from './animations.mts';
import {
  svgNum,
  CAP_MAP,
  JOIN_MAP,
  arcTape,
  circleTape,
  rectTape,
  lineTape,
  checkedTapeNumber,
  checkVectorPaints,
  vectorPaintOf,
  DEFAULT_MITER_LIMIT,
} from './svg.mts';
import {armedFlag} from './ir/functions.mts';
import type * as t from '@babel/types';
import type {Out} from './out.mts';
import type {IrExpr} from './ir/expressions.mts';
import {
  INT,
  isConvertible64,
  isIntSlot,
  isTime,
  refType,
  slotOf,
  stateType,
  STRING,
} from './ir/types.mts';
import type {IrDepEffect} from './ir/functions.mts';
import type {IrSource} from './ir/source.mts';
import type {WideSlot} from './ir/types.mts';
import type {
  IrCallback,
  IrItemValue,
  IrStmt,
  IrTimer,
  IrUpdateVector,
} from './ir/statements.mts';
import type {IrText} from './ir/text.mts';
import type {IrGeometry, IrTapeValue, IrVectorPaint} from './ir/vectors.mts';
import type {
  SlotKind,
  EffectRecord,
  Env,
  FunctionNode,
  ItemField,
  ItemStruct,
  ListState,
  Local,
  PanCallback,
  PanResponderRecord,
  RefRecord,
  ScalarState,
  StateTable,
  StatementContext,
} from './types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** The constant 0, which a remainder by 0 or ±1 stores. */
const ZERO: IrExpr = {kind: 'number', value: 0, type: INT};

/** Keys an AST node keeps besides what it says: where it is, and its comments. */
const POSITION_KEYS = new Set([
  'loc',
  'start',
  'end',
  'range',
  'extra',
  'leadingComments',
  'trailingComments',
  'innerComments',
]);

/** Update locals made so far, which numbers the next one's name. */
let updateRows = 0;

/** Each updateVector shape key and the numbers its array takes, for errors. */
const UPDATE_VECTOR_ARGUMENTS: Record<string, string> = {
  arc: '[cx, cy, r, startAngle, endAngle]',
  circle: '[cx, cy, r]',
  rect: '[x, y, width, height] or [x, y, width, height, rx, ry]',
  line: '[x1, y1, x2, y2]',
};

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Whether a value is left out of an item: null or undefined.
 *
 * @param node  The value.
 *
 * @returns Whether it is.
 */
const leavesOut = (node: t.Node): boolean =>
  node.type === 'NullLiteral' ||
  (node.type === 'Identifier' && node.name === 'undefined');

/**
 * Whether two expressions are written the same, wherever they are.
 *
 * @param first  One.
 * @param second  The other.
 *
 * @returns Whether they are.
 */
function sameNode(first: unknown, second: unknown): boolean {
  if (first === second) return true;
  if (
    !first ||
    !second ||
    typeof first !== 'object' ||
    typeof second !== 'object' ||
    Array.isArray(first) !== Array.isArray(second)
  ) {
    return false;
  }
  const keysOf = (value: object): string[] =>
    Object.keys(value).filter(key => !POSITION_KEYS.has(key));
  const firstKeys = keysOf(first);
  return (
    firstKeys.length === keysOf(second).length &&
    firstKeys.every(key =>
      sameNode(
        (first as Record<string, unknown>)[key],
        (second as Record<string, unknown>)[key],
      ),
    )
  );
}

/**
 * Whether an expression reads any of some names.
 *
 * @param node  The expression.
 * @param names  The names.
 *
 * @returns Whether it does.
 */
function mentions(node: unknown, names: ReadonlySet<string>): boolean {
  if (!node || typeof node !== 'object') return false;
  if (Array.isArray(node)) return node.some(child => mentions(child, names));
  const astNode = node as t.Node;
  if (astNode.type === 'Identifier') return names.has(astNode.name);
  return Object.entries(astNode).some(
    ([key, child]) =>
      !POSITION_KEYS.has(key) &&
      !(
        key === 'property' &&
        (astNode.type === 'MemberExpression' ||
          astNode.type === 'OptionalMemberExpression') &&
        !astNode.computed
      ) &&
      !(
        key === 'key' &&
        astNode.type === 'ObjectProperty' &&
        !astNode.computed
      ) &&
      mentions(child, names),
  );
}

/**
 * The value an item's field is set to, checked against the field.
 *
 * @param field  The field.
 * @param valueNode  Its value.
 * @param listState  The list.
 * @param environment  The expression environment.
 *
 * @returns The value.
 */
function itemValue(
  field: ItemField,
  valueNode: t.Node,
  listState: ListState,
  environment: Env,
): IrItemValue {
  if (field.kind === 'string') {
    const text = lowerText(valueNode, environment);
    checkConstantFits(
      text,
      `field "${field.key}" of list state "${listState.name}"`,
    );
    return {field, text};
  }
  const value = lowerExpr(valueNode, environment);
  if ((field.kind === 'bool') !== (value.type.kind === 'bool')) {
    throw aotError(
      `AOT: field "${field.key || 'value'}" of list state "${listState.name}" holds ${field.kind === 'bool' ? 'a boolean' : 'a number'}`,
      'give it a value of the same kind as the initial ones.',
    );
  }
  return {
    field,
    value: field.kind === 'bool' ? value : storeValue(value, field.kind),
  };
}

/**
 * The fields an appended item sets, by key: an object literal's, nested ones flattened into dotted keys, or a plain
 * value's one field. A field set to null or undefined is left out.
 *
 * @param item  The appended item.
 * @param struct  The list's fields.
 * @param listName  The list, for errors.
 *
 * @returns Each field's value.
 */
function appendedFields(
  item: t.Expression,
  struct: ItemStruct,
  listName: string,
): Map<string, t.Node> {
  const given = new Map<string, t.Node>();
  if (struct.plain) {
    if (item.type === 'ObjectExpression') {
      throw aotError(
        `AOT: list state "${listName}" holds plain values, not objects`,
        'append a value of the same kind as the initial ones.',
      );
    }
    if (!leavesOut(item)) {
      given.set('', item);
    }
    return given;
  }

  const visit = (object: t.ObjectExpression, path: string[]): void => {
    for (const property of object.properties) {
      if (property.type !== 'ObjectProperty' || property.computed) {
        throw aotError(
          'AOT: an appended item must be an object literal of plain fields',
          'write each field out: {title: t, n: 1}. Spreads, methods and computed keys are not lowered.',
        );
      }
      const fieldPath = [...path, propertyKey(property)];
      if (property.value.type === 'ObjectExpression') {
        visit(property.value, fieldPath);
        continue;
      }
      // A field set to null or undefined, or an object of fields set to one, is left out.
      const name = fieldPath.join('.');
      const isField = struct.fields.some(field => field.key === name);
      if (
        !isField &&
        !(leavesOut(property.value) && fieldsUnder(struct, fieldPath).length)
      ) {
        throw noField(listName, name);
      }
      if (!leavesOut(property.value)) {
        given.set(name, property.value);
      }
    }
  };
  if (item.type !== 'ObjectExpression') {
    throw new Error('AOT: appended list items must be object literals');
  }
  visit(item, []);
  return given;
}

/**
 * An object property's key.
 *
 * @param property  The property.
 *
 * @returns The key.
 */
const propertyKey = (property: t.ObjectProperty): string =>
  (property.key as t.Identifier).name ??
  String((property.key as t.StringLiteral).value);

/**
 * The error for a field a list's items don't have.
 *
 * @param listName  The list.
 * @param name  The field.
 *
 * @returns The error.
 */
const noField = (listName: string, name: string): Error =>
  aotError(
    `AOT: list state "${listName}" has no field "${name}"`,
    "a list's fields are those of its initial items. Give one of them the field, and leave it out of the " +
      'others to make it optional.',
  );

/** A function a list method is given, inline: its parameters and the value it returns. */
interface ItemCallback {
  itemName?: string;
  indexName?: string;
  body: t.Expression;
}

/**
 * The inline function a list method is given.
 *
 * @param node  The argument.
 * @param what  The call, for errors: `items.map`.
 *
 * @returns Its parameters and what it returns.
 */
function itemCallback(node: t.Node | undefined, what: string): ItemCallback {
  if (!isFn(node)) {
    throw aotError(
      `AOT: ${what} needs an inline function`,
      `write the function in the call: ${what}((item, i) => …).`,
    );
  }
  const names = node.params.map(parameter => {
    if (parameter.type !== 'Identifier') {
      throw aotError(
        `AOT: ${what}'s function must name its parameters`,
        'name the item and its index, e.g. (item, i) => …, and read fields as item.done.',
      );
    }
    return parameter.name;
  });
  let {body} = node;
  if (body.type === 'BlockStatement') {
    const [statement] = body.body;
    if (
      body.body.length !== 1 ||
      statement.type !== 'ReturnStatement' ||
      !statement.argument
    ) {
      throw aotError(
        `AOT: ${what}'s function must return a single expression`,
        'return one value, e.g. (item, i) => (i === n ? {...item, done: true} : item).',
      );
    }
    body = statement.argument;
  }
  return {itemName: names[0], indexName: names[1], body};
}

/**
 * A list row index: a whole number.
 *
 * @param node  The index.
 * @param listName  The list, for errors.
 * @param environment  The expression environment.
 *
 * @returns The index.
 */
function rowIndexOf(node: t.Node, listName: string, environment: Env): IrExpr {
  const index = lowerExpr(node, environment);
  if (index.type.kind !== 'int') {
    throw aotError(
      `AOT: an index into "${listName}" must be a whole number`,
      'an index is an int: Math.floor(x) or a whole-number state.',
    );
  }
  return index;
}

/**
 * The index `items.filter((_, i) => i !== n)` removes: n.
 *
 * @param callback  The filter's function.
 * @param listName  The list, for errors.
 *
 * @returns n.
 */
function filteredIndex(callback: ItemCallback, listName: string): t.Node {
  const {itemName, indexName, body} = callback;
  const isIndex = (node: t.Node): boolean =>
    node.type === 'Identifier' && node.name === indexName;
  const other =
    indexName &&
    body.type === 'BinaryExpression' &&
    (body.operator === '!==' || body.operator === '!=')
      ? isIndex(body.left)
        ? body.right
        : isIndex(body.right)
          ? body.left
          : null
      : null;
  const names = new Set([itemName, indexName].filter(Boolean) as string[]);
  if (!other || mentions(other, names)) {
    throw aotError(
      `AOT: only ${listName}.filter((_, i) => i !== n) is supported on a list`,
      'it removes item n. A filter on the items themselves is not lowered yet.',
    );
  }
  return other;
}

/** What an update reads: the list, and which item is the row it replaces. */
interface RowUpdate {
  list: ListState;
  listName: string;
  environment: Env;
  /**
   * Whether an expression is the replaced item, or the object at a path in it: `item`, `item.pos`.
   *
   * @param node  The expression.
   * @param path  The path.
   *
   * @returns Whether it is.
   */
  isRow(node: t.Node, path: string[]): boolean;
}

/**
 * Whether an expression is the object at a path in an item: `base.pos.x` for ['pos', 'x'].
 *
 * @param node  The expression.
 * @param path  The path.
 * @param isItem  Whether an expression is the item.
 *
 * @returns Whether it is.
 */
function isPathOf(
  node: t.Node,
  path: string[],
  isItem: (node: t.Node) => boolean,
): boolean {
  let current = node;
  for (let index = path.length - 1; index >= 0; index--) {
    if (
      current.type !== 'MemberExpression' ||
      current.computed ||
      (current.property as t.Identifier).name !== path[index]
    ) {
      return false;
    }
    current = current.object;
  }
  return isItem(current);
}

/**
 * The statements an update runs on a row: `if`s for a ternary's tests, and a `setRow` for each item it builds.
 * A branch that gives back the item itself changes nothing.
 *
 * @param node  What the update gives the row.
 * @param update  The update.
 *
 * @returns The statements.
 */
function updateBody(node: t.Node, update: RowUpdate): IrStmt[] {
  if (update.isRow(node, [])) {
    return [];
  }
  if (node.type === 'ConditionalExpression') {
    const consequent = updateBody(node.consequent, update);
    const alternate = updateBody(node.alternate, update);
    if (!consequent.length && !alternate.length) {
      return [];
    }
    return [
      {
        kind: 'if',
        test: lowerExprWide(node.test, update.environment),
        consequent,
        alternate: alternate.length ? alternate : null,
      },
    ];
  }
  return [setRow(node, update)];
}

/**
 * Sets a row to an item: a spread of the row's own item keeps the fields it doesn't set; without one, every field
 * it leaves out is undefined from then on.
 *
 * @param node  The item.
 * @param update  The update.
 *
 * @returns The statement.
 */
function setRow(node: t.Node, update: RowUpdate): IrStmt {
  const {list, listName, environment} = update;
  const {struct} = list;
  const given = new Map<ItemField, t.Node>();
  const cleared = new Set<ItemField>();
  if (struct.plain) {
    if (node.type === 'ObjectExpression') {
      throw aotError(
        `AOT: list state "${listName}" holds plain values, not objects`,
        'give a value of the same kind as the initial ones.',
      );
    }
    if (leavesOut(node)) {
      cleared.add(struct.fields[0]);
    } else {
      given.set(struct.fields[0], node);
    }
  } else {
    if (node.type !== 'ObjectExpression') {
      throw aotError(
        `AOT: an item of "${listName}" must be replaced with an object literal`,
        'spread the item and set the fields that change: {...item, done: true}.',
      );
    }
    const visit = (object: t.ObjectExpression, path: string[]): void => {
      const seen = new Set<ItemField>();
      let keeps = false;
      object.properties.forEach((property, position) => {
        if (property.type === 'SpreadElement') {
          if (position !== 0 || !update.isRow(property.argument, path)) {
            throw aotError(
              'AOT: an updated item can spread only the item it replaces, first',
              path.length
                ? `spread the same object: {...item, ${path[0]}: {...item.${path.join('.')}, x: 1}}.`
                : 'spread the item, then set the fields that change: {...item, done: true}.',
            );
          }
          keeps = true;
          return;
        }
        if (property.type !== 'ObjectProperty' || property.computed) {
          throw aotError(
            'AOT: an updated item must be an object literal of plain fields',
            'write each field out: {...item, title: t, n: 1}. Methods and computed keys are not lowered.',
          );
        }
        const fieldPath = [...path, propertyKey(property)];
        const fields = fieldsUnder(struct, fieldPath);
        if (!fields.length) {
          throw noField(listName, fieldPath.join('.'));
        }
        fields.forEach(field => seen.add(field));
        const {value} = property;
        if (value.type === 'ObjectExpression') {
          visit(value, fieldPath);
        } else if (leavesOut(value)) {
          fields.forEach(field => cleared.add(field));
        } else if (
          fields.length === 1 &&
          fields[0].key === fieldPath.join('.')
        ) {
          given.set(fields[0], value);
        } else if (!update.isRow(value, fieldPath)) {
          throw aotError(
            `AOT: field "${fieldPath.join('.')}" of list state "${listName}" holds an object`,
            `set its fields with an object literal: {...item, ${fieldPath[0]}: {...item.${fieldPath.join('.')}, x: 1}}.`,
          );
        }
      });
      if (!keeps) {
        for (const field of fieldsUnder(struct, path)) {
          if (!seen.has(field)) {
            cleared.add(field);
          }
        }
      }
    };
    visit(node, []);
  }

  const values: IrItemValue[] = [];
  for (const field of struct.fields) {
    const valueNode = given.get(field);
    if (cleared.has(field)) {
      field.optional = true;
    } else if (valueNode) {
      values.push(itemValue(field, valueNode, list, environment));
    }
  }
  return {kind: 'setRow', list, values, cleared: [...cleared]};
}

/**
 * The local an update's row index is read through. Each has a name of its own, which no JS name can clash with.
 *
 * @returns The local.
 */
function updateRowLocal(): Local {
  return {slotKind: 'int', name: `#row${updateRows++}`};
}

/**
 * Lowers `items.map(…)` as a list's next value: each row is visited, and set to what the function gives it.
 *
 * @param listState  The list.
 * @param listName  What the setter's argument calls it.
 * @param argument  The map's function.
 * @param environment  The expression environment.
 *
 * @returns The statements.
 */
function lowerItemsMap(
  listState: ListState,
  listName: string,
  argument: t.Node | undefined,
  environment: Env,
): IrStmt[] {
  const {itemName, indexName, body} = itemCallback(argument, `${listName}.map`);
  const row = updateRowLocal();
  const locals = new Map(environment.locals);
  if (itemName) {
    locals.set(itemName, {
      name: itemName,
      struct: listState.struct,
      list: listState,
      index: {kind: 'local', local: row, type: INT},
    } as Local);
  }
  if (indexName) {
    locals.set(indexName, row);
  }
  const statements = updateBody(body, {
    list: listState,
    listName,
    environment: {...environment, locals},
    isRow: (node, path) =>
      isPathOf(
        node,
        path,
        item => item.type === 'Identifier' && item.name === itemName,
      ),
  });
  return statements.length
    ? [
        {
          kind: 'updateItems',
          list: listState,
          row,
          index: null,
          body: statements,
        },
      ]
    : [];
}

/**
 * Lowers `items.with(n, item)` as a list's next value: row n is set to the item. A negative n counts back from the
 * end, and a row past either end stops the app, as JS throws there.
 *
 * @param listState  The list.
 * @param listName  What the setter's argument calls it.
 * @param argumentNodes  The call's arguments.
 * @param environment  The expression environment.
 *
 * @returns The statements.
 */
function lowerItemsWith(
  listState: ListState,
  listName: string,
  argumentNodes: t.CallExpression['arguments'],
  environment: Env,
): IrStmt[] {
  const [indexNode, itemNode] = argumentNodes;
  if (
    argumentNodes.length !== 2 ||
    indexNode.type === 'SpreadElement' ||
    itemNode.type === 'SpreadElement'
  ) {
    throw aotError(
      `AOT: ${listName}.with takes an index and an item`,
      `e.g. ${listName}.with(n, {...${listName}[n], done: true}).`,
    );
  }
  const index = rowIndexOf(indexNode, listName, environment);
  const constantIndex = staticInt(indexNode, environment);
  // The replaced item: items[n], or a row's own item when n is that row's index.
  const isItem = (node: t.Node): boolean => {
    if (
      node.type === 'MemberExpression' &&
      node.computed &&
      node.object.type === 'Identifier' &&
      node.object.name === listName
    ) {
      return sameNode(node.property, indexNode);
    }
    const local =
      node.type === 'Identifier'
        ? environment.locals.get(node.name)
        : undefined;
    return (
      local?.list === listState &&
      local.row !== undefined &&
      local.row === constantIndex
    );
  };
  const row = updateRowLocal();
  const statements = updateBody(itemNode, {
    list: listState,
    listName,
    environment,
    isRow: (node, path) => isPathOf(node, path, isItem),
  });
  return statements.length
    ? [{kind: 'updateItems', list: listState, row, index, body: statements}]
    : [];
}

/**
 * The arguments of a call to a method of the list: `items.slice(…)`, or null when it is no such call.
 *
 * @param node  The expression.
 * @param listName  The list's name.
 * @param method  The method.
 *
 * @returns The arguments.
 */
function listCall(
  node: t.Node | null | undefined,
  listName: string,
  method: string,
): t.CallExpression['arguments'] | null {
  return node?.type === 'CallExpression' &&
    node.callee.type === 'MemberExpression' &&
    !node.callee.computed &&
    node.callee.object.type === 'Identifier' &&
    node.callee.object.name === listName &&
    (node.callee.property as t.Identifier).name === method
    ? node.arguments
    : null;
}

/**
 * The index `[...items.slice(0, n), ...items.slice(n + 1)]` removes, or null when the list literal is not that.
 *
 * @param nextList  The list literal.
 * @param listName  The list's name.
 * @param environment  The expression environment.
 *
 * @returns n.
 */
function slicedOutIndex(
  nextList: t.ArrayExpression,
  listName: string,
  environment: Env,
): t.Node | null {
  const [first, second] = nextList.elements;
  if (
    nextList.elements.length !== 2 ||
    first?.type !== 'SpreadElement' ||
    second?.type !== 'SpreadElement'
  ) {
    return null;
  }
  const before = listCall(first.argument, listName, 'slice');
  const after = listCall(second.argument, listName, 'slice');
  if (before?.length !== 2 || after?.length !== 1) {
    return null;
  }
  const [start, end] = before;
  const [next] = after;
  const constantEnd = staticInt(end, environment);
  const constantNext = staticInt(next, environment);
  const followsOn =
    (next.type === 'BinaryExpression' &&
      next.operator === '+' &&
      sameNode(next.left, end) &&
      staticInt(next.right, environment) === 1) ||
    (constantEnd !== null && constantNext === constantEnd + 1);
  if (
    staticInt(start, environment) !== 0 ||
    !followsOn ||
    (constantEnd !== null && constantEnd < 0)
  ) {
    throw aotError(
      `AOT: only [...${listName}.slice(0, n), ...${listName}.slice(n + 1)] is supported to splice a list`,
      'it removes item n, which must not be negative.',
    );
  }
  return end;
}

/**
 * Lowers a list-state setter call (`setItems(...)`) to bounded list operations.
 *
 * @param listState  The list state.
 * @param nextList  The setter's argument: the list's next value.
 * @param environment  The expression environment.
 * @param listName  What the argument calls the list: its name, or an updater's parameter.
 *
 * @returns The statements.
 */
function lowerListOp(
  listState: ListState,
  nextList: t.CallExpression['arguments'][number],
  environment: Env,
  listName = listState.name,
): IrStmt[] {
  const {capacity: capacity, struct} = listState;

  // setItems([...items, a, b]) — append; setItems([]) — clear; [...items.slice(0, n), ...items.slice(n + 1)] — remove.
  if (nextList.type === 'ArrayExpression') {
    if (nextList.elements.length === 0) {
      return [{kind: 'clearList', list: listState}];
    }

    const slicedOut = slicedOutIndex(nextList, listName, environment);
    if (slicedOut) {
      return [
        {
          kind: 'removeItem',
          list: listState,
          index: rowIndexOf(slicedOut, listName, environment),
          countsBack: false,
        },
      ];
    }

    const [firstElement, ...appendedItems] = nextList.elements;
    if (
      firstElement?.type !== 'SpreadElement' ||
      (firstElement.argument as t.Identifier).name !== listName
    ) {
      throw new Error(
        `AOT: a list literal must spread the current list first: [...${listName}, item]`,
      );
    }

    // Each appended item fills the next free row; a full list drops it. A field it leaves out is 0 or "", and
    // optional from then on, so a read of it sees undefined.
    const statements: IrStmt[] = [];
    for (const item of appendedItems) {
      if (!item) {
        throw aotError('AOT: a list literal has an empty slot');
      }
      if (item.type === 'SpreadElement') {
        throw aotError(
          `AOT: a list literal can spread only the current list, first: [...${listName}, item]`,
        );
      }

      const given = appendedFields(item, struct, listName);
      const values: IrItemValue[] = [];
      for (const field of struct.fields) {
        const valueNode = given.get(field.key);
        if (valueNode) {
          values.push(itemValue(field, valueNode, listState, environment));
        } else {
          field.optional = true;
        }
      }

      statements.push({kind: 'appendItem', list: listState, values});
    }

    return statements;
  }

  // filter((_, i) => i !== n) and toSpliced(n, 1) remove item n; toSpliced counts a negative n back from the end.
  const filterArguments = listCall(nextList, listName, 'filter');
  if (filterArguments) {
    const index = filteredIndex(
      itemCallback(filterArguments[0], `${listName}.filter`),
      listName,
    );
    return [
      {
        kind: 'removeItem',
        list: listState,
        index: rowIndexOf(index, listName, environment),
        countsBack: false,
      },
    ];
  }
  const splicedArguments = listCall(nextList, listName, 'toSpliced');
  if (splicedArguments) {
    const [start, count] = splicedArguments;
    if (
      splicedArguments.length !== 2 ||
      start.type === 'SpreadElement' ||
      staticInt(count, environment) !== 1
    ) {
      throw aotError(
        `AOT: only ${listName}.toSpliced(n, 1) is supported on a list`,
        'it removes item n.',
      );
    }
    return [
      {
        kind: 'removeItem',
        list: listState,
        index: rowIndexOf(start, listName, environment),
        countsBack: true,
      },
    ];
  }

  // map(item => …) and with(n, item) set items in place.
  const mapArguments = listCall(nextList, listName, 'map');
  if (mapArguments) {
    return lowerItemsMap(listState, listName, mapArguments[0], environment);
  }
  const withArguments = listCall(nextList, listName, 'with');
  if (withArguments) {
    return lowerItemsWith(listState, listName, withArguments, environment);
  }

  // slice(0, end) keeps the first items by JS rules: a negative end counts back, a large or missing one keeps all.
  const sliceArguments = listCall(nextList, listName, 'slice');
  if (sliceArguments) {
    const [start, end] = sliceArguments;
    if (start && staticInt(start, environment) !== 0) {
      throw aotError(
        `AOT: only ${listName}.slice(0, end) is supported on a list`,
        `a list is a fixed pool and a count, so a slice can only drop items from the end — ` +
          `e.g. ${listName}.slice(0, -1) drops the last one.`,
      );
    }

    if (!end) return [];

    // Settle a constant end here, clamped to ±capacity (past which nothing changes), so it needs no 64-bit math.
    const staticEnd = staticInt(end, environment);
    if (staticEnd !== null) {
      const clampedEnd = Math.max(-capacity, Math.min(capacity, staticEnd));
      if (clampedEnd === -1) {
        return [{kind: 'dropLastItem', list: listState}];
      }
      if (clampedEnd >= 0) {
        return [{kind: 'keepFirstItems', list: listState, count: clampedEnd}];
      }
      return [
        {
          kind: 'sliceItems',
          list: listState,
          end: {kind: 'number', value: clampedEnd, type: INT},
        },
      ];
    }

    // A runtime end applies the same JS rules when it runs.
    return [
      {
        kind: 'sliceItems',
        list: listState,
        end: asInt(lowerExpr(end, environment)),
      },
    ];
  }

  throw aotError(
    `AOT: unsupported list operation on "${listName}"`,
    `use [...${listName}, item], ${listName}.slice(0, n), [], ${listName}.filter((_, i) => i !== n), ` +
      `${listName}.toSpliced(n, 1), ${listName}.map(item => …) or ${listName}.with(n, item).`,
  );
}

/**
 * Returns a statement node's body list: a BlockStatement's contents, or the lone statement wrapped.
 *
 * @param node  An `if` branch.
 *
 * @returns Its statements.
 */
function blockList(node: t.Statement): t.Statement[] {
  return node.type === 'BlockStatement' ? node.body : [node];
}

/**
 * Checks a value written to a numeric state or ref slot, whose kind came from its initial value. Every value
 * written to an int slot is recorded in environment.stores, from which the typing pass infers the slots that hold a
 * 64-bit timestamp, and compileWidened compiles again with those slots widened to 64 bits. A timestamp cannot
 * go into a float or boolean slot (a 64-bit constant can go into a float one), and a widened slot takes no
 * floats.
 *
 * @param lowered  The value: its source, and its type.
 * @param slotType  The slot's kind.
 * @param isBoolean  The slot holds a boolean.
 * @param widened  What the slot holds, if it was widened to 64 bits.
 * @param slotName  The slot's name, for environment.stores.
 * @param slotDescription  The slot, for the message.
 * @param environment  The expression environment.
 */
function storeCheck(
  lowered: {source: IrSource; expression: IrExpr},
  slotType: SlotKind,
  isBoolean: boolean,
  widened: WideSlot | undefined,
  slotName: string,
  slotDescription: string,
  environment: Env,
): void {
  const value = lowered.expression;
  if ((slotType === 'int' && !isBoolean) || widened === 'number') {
    environment.stores.push({
      slot: slotName,
      value: lowered.source,
      math64: Boolean(environment.math64),
    });
  }

  if (value.type.kind === 'int64' && slotType !== 'i64') {
    if (slotType === 'int' && !isBoolean) return;

    if (isConvertible64(value.type) && slotType === 'float') return;

    throw aotError(
      `AOT: a 64-bit time value cannot be stored in ${slotDescription}`,
      'give it a whole-number initial value — useState(0) or useRef(0) — and it widens to hold the timestamp.',
    );
  }

  if (slotType === 'i64' && value.type.kind === 'float') {
    throw mix64Error();
  }
}

/**
 * `value` for an int destination. A float goes through a checked conversion: a plain one is undefined for
 * NaN or a value past the int range.
 *
 * @param value  The lowered value.
 *
 * @returns The value as an int.
 */
const asInt = (value: IrExpr): IrExpr =>
  value.type.kind === 'float'
    ? {kind: 'toInt', operand: value, type: INT}
    : value;

/**
 * `value` for a slot of `slotType`: an int slot takes a float the way asInt does.
 *
 * @param value  The lowered value.
 * @param slotType  The slot's kind.
 *
 * @returns The value the slot stores.
 */
const storeValue = (value: IrExpr, slotType: SlotKind): IrExpr =>
  slotType === 'int' ? asInt(value) : value;

/**
 * A read of a ref's current value.
 *
 * @param ref  The ref.
 *
 * @returns The read.
 */
const refRead = (ref: RefRecord): IrExpr => ({
  kind: 'ref',
  ref,
  type: refType(ref),
});

/**
 * Refuses a constant string too long for the buffer a write is built in, which would cut it short. A string
 * built at run time is checked when it is written instead.
 *
 * @param text  The text written.
 * @param what  What it is written into, for the error.
 */
function checkConstantFits(text: IrText, what: string): void {
  const constant = constantText(text);
  if (constant === null) {
    return;
  }

  const bytes = Buffer.byteLength(constant, 'utf8');
  if (bytes > ENGINE_STRING_MAX.ER_TEXT_MAX) {
    throw aotError(
      `AOT: ${what} is set to a ${bytes}-byte string, but a write holds ${ENGINE_STRING_MAX.ER_TEXT_MAX} bytes (ER_TEXT_MAX)`,
      'shorten the string: a write is built in a buffer that ER_TEXT_MAX in er_scene.h sizes.',
    );
  }
}

/**
 * Lowers a write of an expression into a scalar state slot: text for a string buffer (so a `+` chain keeps its
 * parts), a plain value otherwise.
 *
 * @param scalarState  The state slot.
 * @param valueNode  The value.
 * @param environment  The expression environment.
 *
 * @returns The write.
 */
function lowerScalarSet(
  scalarState: ScalarState,
  valueNode: t.Node,
  environment: Env,
): IrStmt {
  if (scalarState.slotKind !== 'string') {
    // A 64-bit slot takes int `+ - *` worked out in 64 bits.
    const lowered = lowerSource(
      valueNode,
      scalarState.slotKind === 'i64'
        ? {...environment, math64: true}
        : environment,
    );
    const value = lowered.expression;
    storeCheck(
      lowered,
      scalarState.slotKind,
      scalarState.isBoolean,
      scalarState.widened,
      scalarState.slotName,
      `state "${scalarState.name}"`,
      environment,
    );

    return {
      kind: 'setState',
      state: scalarState,
      value: storeValue(value, scalarState.slotKind),
    };
  }

  const text = lowerText(valueNode, environment);
  checkConstantFits(text, `state "${scalarState.name}"`);
  return {
    kind: 'setStateText',
    state: scalarState,
    text,
  };
}

/**
 * Resolves `node` to its ref when it is `<ref>.current` member access on a known value ref.
 *
 * @param node  An assignment or update target.
 * @param environment  The expression environment.
 *
 * @returns The ref, marked used, or null when the node is not such an access.
 */
function refTarget(
  node: t.Node | null | undefined,
  environment: Env,
): RefRecord | null {
  if (
    node?.type === 'MemberExpression' &&
    !node.computed &&
    node.object.type === 'Identifier' &&
    (node.property as t.Identifier).name === 'current' &&
    environment.refs?.has(node.object.name)
  ) {
    const ref = environment.refs.get(node.object.name)!;
    ref.used = true;
    return ref;
  }

  return null;
}

/**
 * An imperative updateVector shape `{ arc:[…]|circle:[…]|rect:[x, y, w, h, rx?, ry?]|line:[…]|path:'…', fill, … }`:
 * its tape slots, the locals they need, and its static paint. Geometry coords may reference state, refs or event
 * fields; the paint must be static. The geometry is the same as the JSX path's. `tag` names any locals and must
 * be unique in the handler block they are written into.
 *
 * @param shapeNode  The shape's object literal.
 * @param environment  The expression environment.
 * @param tag  Names the shape's locals.
 *
 * @returns The shape's tape slots, the locals they need, and its paint.
 */
function imperativeShape(
  shapeNode: t.Node | null | undefined,
  environment: Env,
  tag: string,
): IrGeometry & {paint: IrVectorPaint} {
  // Index the shape's properties by name.
  if (shapeNode?.type !== 'ObjectExpression') {
    throw new Error('AOT: each updateVector shape must be an object literal');
  }

  const props: Record<string, t.Node> = {};
  for (const prop of shapeNode.properties) {
    if (prop.type !== 'ObjectProperty') {
      throw new Error(
        'AOT: spread/method in an updateVector shape not supported',
      );
    }
    props[
      (prop.key as t.Identifier).name ?? (prop.key as t.StringLiteral).value
    ] = prop.value;
  }

  // Lower whichever geometry key is present; array numbers may be runtime values, a path string must be static.
  const geometryArgs = (
    key: string,
    count: number,
    required = count,
  ): IrTapeValue[] => {
    if (props[key].type !== 'ArrayExpression') {
      throw new Error(`AOT: updateVector "${key}" must be an array literal`);
    }

    const elements = (props[key] as t.ArrayExpression).elements;
    const shapeHint = `${key}: ${UPDATE_VECTOR_ARGUMENTS[key]}.`;
    if (elements.length < required) {
      throw aotError(
        `AOT: updateVector "${key}" needs ${required} numbers (got ${elements.length})`,
        shapeHint,
      );
    }

    // A hole (`[1, , 3]`) has no value to lower; an optional rect radius left out is just a shorter array.
    return elements.slice(0, count).map((valueNode, position) => {
      if (!valueNode) {
        throw aotError(
          `AOT: updateVector "${key}" has an empty slot at position ${position}`,
          shapeHint,
        );
      }

      return {kind: 'float', value: lowerExpr(valueNode, environment)};
    });
  };
  let geometry: IrGeometry;
  if (props.arc) {
    const [cx, cy, radius, startDeg, endDeg] = geometryArgs('arc', 5);
    geometry = {entries: arcTape(cx, cy, radius, startDeg, endDeg), locals: []};
  } else if (props.circle) {
    const [cx, cy, radius] = geometryArgs('circle', 3);
    geometry = {entries: circleTape(cx, cy, radius), locals: []};
  } else if (props.rect) {
    const [x, y, width, height, rx = null, ry = null] = geometryArgs(
      'rect',
      6,
      4,
    );
    geometry = rectTape(x, y, width, height, rx, ry, tag);
  } else if (props.line) {
    const [x1, y1, x2, y2] = geometryArgs('line', 4);
    geometry = {entries: lineTape(x1, y1, x2, y2), locals: []};
  } else if (props.path) {
    geometry = {
      entries: parsePath(
        String(evalStatic(props.path, environment.constants ?? {})),
      ).map((value: number) => checkedTapeNumber(value)),
      locals: [],
    };
  } else {
    throw new Error(
      'AOT: an updateVector shape needs one of arc / circle / rect / line / path',
    );
  }

  // Fold the paint to its static record: fill, stroke, stroke width, miter limit, cap, join, fill rule.
  const paintValue = (key: string, fallback: unknown): unknown => {
    if (props[key] == null) return fallback;
    try {
      return evalStatic(props[key], environment.constants ?? {});
    } catch {
      throw new Error(`AOT: updateVector paint "${key}" must be static`);
    }
  };
  const paint = vectorPaintOf([
    parseColor(paintValue('fill', 'none')),
    parseColor(paintValue('stroke', 'none')),
    svgNum(paintValue('strokeWidth', 1), 1),
    svgNum(paintValue('miter', DEFAULT_MITER_LIMIT), DEFAULT_MITER_LIMIT),
    CAP_MAP[paintValue('cap', 'butt') as string] ?? 0,
    JOIN_MAP[paintValue('join', 'miter') as string] ?? 0,
    paintValue('fillRule', 'nonzero') === 'evenodd' ? 1 : 0,
  ]);

  return {...geometry, paint};
}

/**
 * Lowers `updateVector(nodeRef, shapes, [x,y,w,h]?)`: fill a mutable op-tape, push it to the node, and
 * (optionally) hint the dirty sub-rect — the imperative fast path (drag) that bypasses the update. The call's
 * tables are declared here.
 *
 * @param expression  The updateVector call.
 * @param environment  The expression environment.
 * @param context  The statement compiler's context.
 *
 * @returns The statement.
 */
function lowerUpdateVector(
  expression: t.CallExpression,
  environment: Env,
  context: StatementContext,
): IrUpdateVector {
  // The first argument must be a node ref, and the second an array literal of shapes.
  const output = context.output;
  const [refArgument, shapesArgument, dirtyArgument] = expression.arguments;
  const ref =
    refArgument?.type === 'Identifier'
      ? environment.refs?.get(refArgument.name)
      : null;
  if (ref?.kind !== 'node') {
    throw new Error(
      'AOT: updateVector(ref, …) first arg must be a node ref (const r = useRef())',
    );
  }

  ref.used = true;
  if (shapesArgument?.type !== 'ArrayExpression') {
    throw new Error(
      'AOT: updateVector(ref, shapes, …) shapes must be an array literal',
    );
  }

  // Claim the id first so two updateVector calls in one handler get distinct locals, then lower each shape.
  const vectorId = output.svgCount++;
  const update: IrUpdateVector = {
    kind: 'updateVector',
    id: vectorId,
    node: ref.slotName,
    locals: [],
    tape: [],
    paints: [],
    dirty: null,
  };
  for (const shapeNode of shapesArgument.elements) {
    const shape = imperativeShape(
      shapeNode,
      environment,
      `uv${vectorId}_${update.paints.length}`,
    );
    update.locals.push(...shape.locals);
    update.tape.push(
      {kind: 'op', operation: 'ER_VECTOR_OP_SHAPE'},
      checkedTapeNumber(update.paints.length),
      ...shape.entries,
    );
    update.paints.push(shape.paint);
  }

  // Declare the op-tape and its paint table.
  checkVectorPaints(update.paints);
  output.vectors.push({kind: 'updateVector', update});

  // Hint the dirty sub-rect when the call gives one.
  if (dirtyArgument) {
    if (
      dirtyArgument.type !== 'ArrayExpression' ||
      dirtyArgument.elements.length < 4
    ) {
      throw new Error(
        'AOT: updateVector dirtyRect must be a [x, y, w, h] array literal',
      );
    }

    update.dirty = dirtyArgument.elements.slice(0, 4).map((edgeNode, edge) => {
      if (!edgeNode) {
        throw aotError(
          `AOT: updateVector dirtyRect has an empty slot at position ${edge}`,
        );
      }

      return lowerExpr(edgeNode, environment);
    });
  }

  return update;
}

/**
 * Lowers setInterval/setTimeout(cb, ms) to a timer, and registers cb as a timer function of its own.
 *
 * @param expression  The setInterval / setTimeout call.
 * @param environment  The expression environment.
 * @param state  The component's state table.
 * @param context  The statement compiler's context.
 *
 * @returns The timer.
 */
function lowerTimer(
  expression: t.CallExpression,
  environment: Env,
  state: StateTable,
  context: StatementContext,
): IrTimer {
  const callback = expression.arguments[0];
  if (!isFn(callback)) {
    throw aotError(
      'AOT: a setInterval/setTimeout callback must be an inline function',
      'pass an inline arrow, e.g. setInterval(() => setTick((t) => t + 1), 1000).',
    );
  }

  // Any delay is accepted here; the printer converts a timestamp or float one the way Flow A's setTimeout does.
  const delay = expression.arguments[1]
    ? lowerExprWide(expression.arguments[1], environment)
    : null;
  const isRepeating =
    (expression.callee as t.Identifier).name === 'setInterval';

  // Reserve the timer's slot before compiling its body, which may add timers of its own.
  const slot = context.output.timerFunctions.length;
  const timerFunctionName = `timer_${slot}`;
  context.output.timerFunctions.push({name: timerFunctionName, callback: null});
  context.output.timerFunctions[slot].callback = lowerHandler(
    callback,
    environment,
    state,
    context.output,
  );
  return {delay, isRepeating, functionName: timerFunctionName};
}

/**
 * Inlines a handler-statement call to a helper / useCallback: binds the call's args to the helper's params
 * as locals, then lowers the helper body here in the current env/state/ctx. Guards against recursion.
 *
 * @param helperName  The helper's name.
 * @param helper  The helper.
 * @param callArguments  The call's arguments.
 * @param environment  The expression environment.
 * @param state  The component's state table.
 * @param context  The statement compiler's context.
 *
 * @returns The helper's body as statements.
 */
function inlineHelperCall(
  helperName: string,
  helper: FunctionNode,
  callArguments: t.CallExpression['arguments'],
  environment: Env,
  state: StateTable,
  context: StatementContext,
): IrStmt[] {
  // A helper already being inlined further up this call chain is recursive.
  context.inlining = context.inlining ?? new Set();
  if (context.inlining.has(helperName)) {
    throw aotError(
      `AOT: helper "${helperName}" is recursive — can't be inlined into a handler`,
      'handlers are flattened to straight-line code, so a helper that calls itself (directly ' +
        'or via another helper) has no base case to unroll. Move recursive logic out of the ' +
        'handler, or precompute the value.',
    );
  }

  // Bind each passed argument to its parameter as a local holding the argument's expression.
  const locals = new Map<string, Local>(environment.locals);
  helper.params.forEach((parameter, parameterIndex) => {
    if (parameter.type !== 'Identifier') {
      throw aotError(
        `AOT: helper "${helperName}" must take simple (identifier) params to be inlined`,
        'a helper called from a handler must use plain positional params (e.g. `(a, b) => …`) ' +
          '— destructuring or default params in the signature are not supported.',
      );
    }

    if (callArguments[parameterIndex]) {
      const argumentExpression = lowerSourcedWide(
        callArguments[parameterIndex],
        environment,
      );
      locals.set(parameter.name, {
        slotKind: argumentExpression.slotKind,
        isBoolean: argumentExpression.isBoolean,
        isLiteral: argumentExpression.isLiteral,
        wide: argumentExpression.wide,
        source: argumentExpression.source,
        expression: argumentExpression.expression,
      });
    }
  });

  // An expression body is a single statement.
  const body = helper.body;
  const statements: t.Statement[] =
    body.type === 'BlockStatement'
      ? body.body
      : [{type: 'ExpressionStatement', expression: body}];

  // The body is spliced into the caller, so reject its `return`, even in an effect body where one is allowed.
  const outerAllowReturn = context.allowReturn;
  context.inlining.add(helperName);
  context.allowReturn = false;
  try {
    return lowerStmts(statements, {...environment, locals}, state, context);
  } finally {
    context.allowReturn = outerAllowReturn;
    context.inlining.delete(helperName);
  }
}

/**
 * Lowers one handler ExpressionStatement: a state setter, ref mutation, timer, updateVector, helper call, or
 * Animated.*(...).start() / .stop() (see lowerHandlerExpr, the located entry point).
 *
 * @param expression  The statement's expression.
 * @param environment  The expression environment.
 * @param state  The component's state table.
 * @param context  The statement compiler's context.
 *
 * @returns The statements.
 */
function lowerHandlerExprImpl(
  expression: t.Expression,
  environment: Env,
  state: StateTable,
  context: StatementContext,
): IrStmt[] {
  // updateVector(ref, shapes, dirtyRect?) — imperative vector redraw (no update).
  if (
    expression.type === 'CallExpression' &&
    expression.callee.type === 'Identifier' &&
    expression.callee.name === 'updateVector'
  ) {
    return [lowerUpdateVector(expression, environment, context)];
  }

  // setInterval / setTimeout(cb, ms) → register a host-tick timer (the returned id is discarded here).
  if (
    expression.type === 'CallExpression' &&
    expression.callee.type === 'Identifier' &&
    (expression.callee.name === 'setInterval' ||
      expression.callee.name === 'setTimeout')
  ) {
    return [
      {
        kind: 'startTimer',
        timer: lowerTimer(expression, environment, state, context),
        idLocal: null,
      },
    ];
  }

  // clearInterval / clearTimeout(id) → deactivate the timer slot.
  if (
    expression.type === 'CallExpression' &&
    expression.callee.type === 'Identifier' &&
    (expression.callee.name === 'clearInterval' ||
      expression.callee.name === 'clearTimeout')
  ) {
    return [
      {
        kind: 'clearTimer',
        id: asInt(lowerExpr(expression.arguments[0], environment)),
      },
    ];
  }

  // `ref.current = expr` / `ref.current += expr` — a value ref write; does NOT trigger a re-render.
  if (expression.type === 'AssignmentExpression') {
    const ref = refTarget(expression.left, environment);
    if (!ref) {
      throw new Error(
        'AOT: the only assignment allowed in a handler is `ref.current = ...`',
      );
    }

    // Lower the right side in the ref's width and check it fits; a 64-bit `%=` keeps its constant divisor.
    const refEnvironment =
      ref.slotKind === 'i64' ? {...environment, math64: true} : environment;
    const loweredValue = lowerSource(expression.right, refEnvironment);
    const valueExpression = loweredValue.expression;
    const refHoldsTime = isTime(refType(ref));
    if (
      expression.operator === '/=' &&
      (refHoldsTime || isTime64(valueExpression))
    ) {
      throw aotError(
        'AOT: `/=` on a 64-bit time value is not supported',
        'JS division gives a fraction; write ref.current = Math.floor(ref.current / n) for whole units.',
      );
    }

    const wideModuloDivisor =
      expression.operator === '%=' &&
      (refHoldsTime || isTime64(valueExpression))
        ? constDivisor(expression.right, environment)
        : null;
    storeCheck(
      loweredValue,
      ref.slotKind as SlotKind,
      false,
      ref.kind === 'value' ? ref.widened : undefined,
      ref.slotName,
      `ref "${((expression.left as t.MemberExpression).object as t.Identifier).name}"`,
      environment,
    );

    // Lower whole-ref `+= -= *=`, float `/=` and float `%=` as the binary op to share its checked and float math.
    const isWholeRef = ref.slotKind === 'int' || ref.slotKind === 'i64';
    const binaryOperation = (
      {'+=': '+', '-=': '-', '*=': '*'} as Record<string, string>
    )[expression.operator];
    const isFloatModulo =
      expression.operator === '%=' &&
      (ref.slotKind === 'float' || valueExpression.type.kind === 'float');
    if (
      (isWholeRef &&
        (binaryOperation ||
          (expression.operator === '/=' &&
            valueExpression.type.kind === 'float'))) ||
      isFloatModulo
    ) {
      const resultExpression = lowerExprWide(
        {
          type: 'BinaryExpression',
          operator: binaryOperation ?? expression.operator.slice(0, -1),
          left: expression.left,
          right: expression.right,
          loc: expression.loc,
        } as t.BinaryExpression,
        refEnvironment,
      );

      return [
        {
          kind: 'writeRef',
          ref,
          operator: '=',
          value: storeValue(resultExpression, ref.slotKind as SlotKind),
        },
      ];
    }

    // Whole-number `/=` and `%=` use the checked helpers for JS's answer, in 64 bits on a 64-bit ref that holds no
    // time; a constant divisor other than 0 or -1 needs no check.
    const isWholeValue =
      isIntSlot(valueExpression.type) ||
      (valueExpression.type.kind === 'int64' && !isTime64(valueExpression));
    const refBits = ref.slotKind === 'i64' ? 64 : 32;
    if (
      ((ref.slotKind === 'int' && isIntSlot(valueExpression.type)) ||
        (ref.slotKind === 'i64' && !refHoldsTime && isWholeValue)) &&
      (expression.operator === '/=' || expression.operator === '%=')
    ) {
      const staticDivisor = staticInt(expression.right, environment);
      if (
        expression.operator === '/=' &&
        (staticDivisor === null || staticDivisor === 0 || staticDivisor === -1)
      ) {
        return [
          {
            kind: 'writeRef',
            ref,
            operator: '=',
            value: {
              kind: 'helper',
              helper: 'div',
              bits: refBits,
              callArguments: [refRead(ref), valueExpression],
              type: refType(ref),
            },
          },
        ];
      }

      if (
        expression.operator === '%=' &&
        (staticDivisor === 0 || staticDivisor === -1)
      ) {
        return [{kind: 'writeRef', ref, operator: '=', value: ZERO}];
      }

      if (expression.operator === '%=' && staticDivisor === null) {
        return [
          {
            kind: 'writeRef',
            ref,
            operator: '=',
            value: {
              kind: 'helper',
              helper: 'mod',
              bits: refBits,
              callArguments: [refRead(ref), valueExpression],
              type: refType(ref),
            },
          },
        ];
      }
    }

    // A 64-bit `%=` by 0 or ±1 stores 0: no remainder, JS's NaN as 0, and no overflow on INT64_MIN % -1.
    if (
      wideModuloDivisor === 0 ||
      wideModuloDivisor === 1 ||
      wideModuloDivisor === -1
    ) {
      return [{kind: 'writeRef', ref, operator: '=', value: ZERO}];
    }

    // Everything else is a plain assignment or compound assignment.
    return [
      {
        kind: 'writeRef',
        ref,
        operator: expression.operator,
        value:
          expression.operator === '='
            ? storeValue(valueExpression, ref.slotKind as SlotKind)
            : valueExpression,
      },
    ];
  }

  // `ref.current++` / `ref.current--`; a whole-number ref steps through the checked `+ 1` / `- 1`.
  if (expression.type === 'UpdateExpression') {
    const ref = refTarget(expression.argument, environment);
    if (!ref) {
      throw new Error(
        'AOT: the only ++/-- allowed in a handler is on `ref.current`',
      );
    }

    if (ref.slotKind === 'int' || ref.slotKind === 'i64') {
      return [
        {
          kind: 'writeRef',
          ref,
          operator: '=',
          value: {
            kind: 'helper',
            helper: expression.operator === '++' ? 'add' : 'sub',
            bits: ref.slotKind === 'i64' ? 64 : 32,
            callArguments: [
              refRead(ref),
              {kind: 'number', value: 1, type: INT},
            ],
            type: refType(ref),
          },
        },
      ];
    }

    return [{kind: 'stepRef', ref, operator: expression.operator}];
  }

  // `anim.stop()` freezes each driven value; the cancel reports !finished, which ends any sequence or loop chain.
  if (
    expression.type === 'CallExpression' &&
    expression.callee.type === 'MemberExpression' &&
    (expression.callee.property as t.Identifier).name === 'stop' &&
    isAnimatedCall(resolveAnim(expression.callee.object, environment))
  ) {
    return [
      {
        kind: 'stopAnimations',
        handles: [...animValues(expression.callee.object, environment)],
        waitOwner: stopWaitOwner(
          expression.callee.object,
          environment,
          context,
        ),
      },
    ];
  }

  // Animated.*(…).start(), atomic or composed, is native-driven, so it sets no state and needs no update.
  if (
    expression.type === 'CallExpression' &&
    expression.callee.type === 'MemberExpression' &&
    (expression.callee.property as t.Identifier).name === 'start'
  ) {
    return [lowerAnimateStart(expression, environment, state, context)];
  }

  // What is left must be a plain call: to a helper or a state setter.
  if (
    expression.type !== 'CallExpression' ||
    expression.callee.type !== 'Identifier'
  ) {
    throw aotError(
      'AOT: a handler statement must be a state setter, a ref write, or Animated.timing/spring(...).start()',
      'each statement in a handler must be one of: setX(value) / setX(prev => …), a ' +
        '`ref.current = …` write, an `updateVector(…)` call, or `Animated.timing|spring(v, ' +
        '…).start()`. Wrap conditional logic in `if (…) { … }`.',
    );
  }

  // A call to a helper / useCallback (e.g. `reset();`) → inline its body here so handlers can compose logic.
  const helperFunction =
    environment.helpers?.get(expression.callee.name) ??
    environment.callbacks?.get(expression.callee.name);
  if (helperFunction) {
    return inlineHelperCall(
      expression.callee.name,
      helperFunction,
      expression.arguments,
      environment,
      state,
      context,
    );
  }

  // A state setter. Flag the state change, which tells the caller to re-apply state once the body is done.
  const targetState = state.bySetter.get(expression.callee.name);
  if (!targetState) {
    throw aotError(
      `AOT: "${expression.callee.name}" is not a known state setter`,
      `a handler can only call a setter from this component's own useState (e.g. setCount), ` +
        `a ref write, updateVector(…), or Animated…start(). "${expression.callee.name}" isn't one ` +
        `of those — arbitrary functions (fetch, console.*, helpers) can't be lowered.`,
    );
  }

  context.stateChanged = true;
  const setterArgument = expression.arguments[0];
  if (targetState.kind === 'list') {
    // setItems(prev => …): the parameter names the list.
    if (isFn(setterArgument)) {
      const {itemName: previousName, body} = itemCallback(
        setterArgument,
        `${expression.callee.name}'s updater`,
      );
      if (!previousName) {
        return lowerListOp(targetState, body, environment);
      }
      return lowerListOp(
        targetState,
        body,
        {
          ...environment,
          state: new Map(environment.state).set(previousName, targetState),
        },
        previousName,
      );
    }
    return lowerListOp(targetState, setterArgument, environment);
  }

  // A scalar setter takes either an updater function or the new value itself.
  if (
    setterArgument &&
    (setterArgument.type === 'ArrowFunctionExpression' ||
      setterArgument.type === 'FunctionExpression')
  ) {
    // setState(prev => expr): bind the param to the current value, typed as a read of it, and assign the result.
    const previousParameter = (
      setterArgument.params[0] as t.Identifier | undefined
    )?.name;
    const locals = new Map<string, Local>(environment.locals);
    if (previousParameter) {
      locals.set(previousParameter, {
        slotKind: targetState.slotKind,
        isBoolean: targetState.isBoolean,
        ...(targetState.widened === 'number' ? {isLiteral: true} : {}),
        source: {kind: 'state', state: targetState, location: undefined},
        expression: {
          kind: 'state',
          state: targetState,
          type: stateType(targetState),
        },
      });
    }

    if (setterArgument.body.type === 'BlockStatement') {
      throw new Error(
        'AOT: updater function must be a single expression (for now)',
      );
    }

    return [
      lowerScalarSet(targetState, setterArgument.body, {
        ...environment,
        locals,
      }),
    ];
  }

  return [lowerScalarSet(targetState, setterArgument, environment)];
}

/** lowerHandlerExprImpl, with the expression's source location attached to any AOT error it throws. */
const lowerHandlerExpression = withLoc(lowerHandlerExprImpl);

/**
 * Lowers a list of handler statements to the IR. This one-statement lowering runs event handlers, effect bodies
 * (useEffect), timer callbacks, and inlined helper calls. Supports: `const x = expr` (a local, visible to later
 * statements), `if (cond) {...} else {...}`, state setters (list setters included), ref writes, timers,
 * updateVector() and Animated.*(...).start(). `context` accumulates `stateChanged` (→ a trailing update)
 * and `usedReturn` (an early `return` was lowered, so the body needs a
 * function of its own). `context.allowReturn` and `context.bodyList` mark which body a `return` may exit and which
 * statement of it is the tail.
 *
 * @param statements  The statements.
 * @param environment  The expression environment.
 * @param state  The component's state table.
 * @param context  The statement compiler's context.
 *
 * @returns The lowered statements.
 */
export function lowerStmts(
  statements: t.Statement[],
  environment: Env,
  state: StateTable,
  context: StatementContext,
): IrStmt[] {
  const lowered: IrStmt[] = [];
  // What each statement lowers to is marked with where it is, once the next one starts.
  let from = 0;
  let location: SourcePosition | undefined;
  for (
    let statementIndex = 0;
    statementIndex < statements.length;
    statementIndex++
  ) {
    markLoc(lowered, from, location);
    const statement = statements[statementIndex];
    from = lowered.length;
    location = startOf(statement);

    // `const x = …` declares a local that the statements after it can read.
    if (statement.type === 'VariableDeclaration') {
      for (const declarator of statement.declarations) {
        if (declarator.id.type !== 'Identifier') {
          throw new Error(
            'AOT: destructuring a handler local is not supported',
          );
        }

        if (!declarator.init) {
          throw new Error('AOT: a handler local must have an initializer');
        }

        // `const anim = Animated.…` holds no value; `anim.start()`/`anim.stop()` compile the animation it names.
        if (isAnimatedCall(declarator.init)) {
          const locals = new Map(environment.locals);
          locals.delete(declarator.id.name);
          environment = {
            ...environment,
            locals,
            animationLocals: new Map(environment.animationLocals).set(
              declarator.id.name,
              declarator.init,
            ),
          };
          continue;
        }

        // A dep-driven effect's cleanup outlives the call, so the body's locals become slots.
        const isHoisted = Boolean(
          context.hoist && statements === context.bodyList,
        );
        const localName = isHoisted
          ? `${context.hoist!.prefix}${declarator.id.name}`
          : `local_${declarator.id.name}`;

        // `const id = setInterval/setTimeout(…)` is an int timer-id local, so a later clearInterval(id) resolves.
        if (
          declarator.init.type === 'CallExpression' &&
          declarator.init.callee.type === 'Identifier' &&
          (declarator.init.callee.name === 'setInterval' ||
            declarator.init.callee.name === 'setTimeout')
        ) {
          // The id is only needed for a later clear*(); a mount effect drops its cleanup, so mark it used.
          if (isHoisted) {
            context.hoist!.declarations.push({
              kind: 'local',
              name: localName,
              slotKind: 'int',
            });
          }
          lowered.push({
            kind: 'startTimer',
            timer: lowerTimer(declarator.init, environment, state, context),
            idLocal: {name: localName, isHoisted},
          });
          environment = {
            ...environment,
            locals: new Map(environment.locals).set(declarator.id.name, {
              name: localName,
              slotKind: 'int',
            }),
          };

          continue;
        }

        // Any other initializer must lower to a number, a boolean or a string. A built string is text.
        const text = buildsText(declarator.init, environment)
          ? lowerText(declarator.init, environment)
          : undefined;
        const {source: initialSource, expression: initialExpression} = text
          ? {
              source: undefined,
              expression: {kind: 'string', value: '', type: STRING} as IrExpr,
            }
          : lowerSource(declarator.init, environment);
        if (initialExpression.type.kind === 'string') {
          if (initialExpression.kind === 'string') {
            checkConstantFits(
              {parts: [{kind: 'literal', text: initialExpression.value}]},
              `local "${declarator.id.name}"`,
            );
          }

          // Copy into a buffer of its own; aliasing a state slot would hide a self-read like `setLabel(t + '!')`.
          if (isHoisted) {
            context.hoist!.declarations.push({
              kind: 'local',
              name: localName,
              slotKind: 'string',
            });
          }
        } else {
          const slot = slotOf(initialExpression.type);
          if (slot !== 'int' && slot !== 'float' && slot !== 'i64') {
            throw aotError(
              'AOT: a handler local must hold a number, a boolean or a string',
              'bind the value itself — e.g. `const id = item.id` — rather than a list item or a node ref.',
            );
          }

          if (isHoisted) {
            context.hoist!.declarations.push({
              kind: 'local',
              name: localName,
              slotKind: slot,
            });
          }
        }

        lowered.push({
          kind: 'declareLocal',
          name: localName,
          initializer: initialExpression,
          ...(text ? {text} : {}),
          isHoisted,
        });

        // Later statements read the name as the local.
        environment = {
          ...environment,
          locals: new Map(environment.locals).set(declarator.id.name, {
            ...slotValueOf(initialExpression),
            name: localName,
            source: initialSource,
          }),
        };
      }

      continue;
    }

    // A tail `return` is the cleanup, dropped on mount and armed if dep-driven; an earlier one is a guard.
    if (statement.type === 'ReturnStatement') {
      if (!context.allowReturn) {
        const error = aotError(
          'AOT: `return` is only supported inside a useEffect body',
          'a handler, timer, animation or inlined-helper body cannot return early — flatten the logic into if/else.',
        );
        if (statement.loc) {
          error.aotLoc = statement.loc.start;
        }

        throw error;
      }

      const isTail =
        statements === context.bodyList &&
        statementIndex === statements.length - 1;
      if (context.dependencyDriven && isFn(statement.argument) && !isTail) {
        const error = aotError(
          'AOT: a useEffect cleanup must be the last statement of the effect body',
          'a cleanup returned from inside an `if` would be dropped. Return `undefined` from the ' +
            'guard and put the single `return () => …` at the end of the body.',
        );

        if (statement.loc) {
          error.aotLoc = statement.loc.start;
        }

        throw error;
      }

      if (isTail) {
        if (context.cleanup && isFn(statement.argument)) {
          context.cleanup.emit(statement.argument, environment);
          lowered.push({kind: 'armCleanup', flag: context.cleanup.armed});
        }

        continue;
      }

      context.usedReturn = true;
      lowered.push({kind: 'return'});
      continue;
    }

    // `if (…) { … } else { … }` lowers each branch in order: the test, then the consequent, then the alternate.
    if (statement.type === 'IfStatement') {
      const test = lowerExprWide(statement.test, environment);
      const consequent = lowerStmts(
        blockList(statement.consequent),
        environment,
        state,
        context,
      );
      const alternate = statement.alternate
        ? lowerStmts(
            blockList(statement.alternate),
            environment,
            state,
            context,
          )
        : null;
      lowered.push({kind: 'if', test, consequent, alternate});
      continue;
    }

    // Anything else must be an expression statement: a setter call, a ref write, a timer, and so on.
    if (statement.type !== 'ExpressionStatement') {
      throw aotError(
        `AOT: unsupported statement "${statement.type}" in event handler`,
        'a handler supports only `const x = …` locals, `if (…) { … } else { … }`, and ' +
          'expression statements (setters / ref writes / updateVector / Animated…start). Loops ' +
          '(for/while), switch and try/catch are not lowered — precompute values or flatten the ' +
          'logic into if/else.',
      );
    }

    lowered.push(
      ...lowerHandlerExpression(
        statement.expression,
        environment,
        state,
        context,
      ),
    );
  }

  markLoc(lowered, from, location);
  return lowered;
}

/**
 * Where a statement starts. An expression body made into a statement has no location of its own.
 *
 * @param statement  The statement.
 *
 * @returns Its start, if it has one.
 */
function startOf(statement: t.Statement): SourcePosition | undefined {
  return (
    statement.loc?.start ??
    (statement.type === 'ExpressionStatement'
      ? statement.expression.loc?.start
      : undefined)
  );
}

/**
 * Marks the statements lowered from one source statement with its location. One nested inside it, or inlined
 * from a helper, keeps its own.
 *
 * @param lowered  The statements lowered so far.
 * @param from  The first one the source statement lowered to.
 * @param location  Where the source statement is.
 */
function markLoc(
  lowered: IrStmt[],
  from: number,
  location: SourcePosition | undefined,
): void {
  for (let index = from; index < lowered.length; index++) {
    lowered[index].location ??= location;
  }
}

/**
 * Lowers a useEffect (App or child). Two shapes:
 *  - `useEffect(fn, [])` — MOUNT-ONCE: body runs after the initial the update (in the build).
 *  - `useEffect(fn, [dep…])` — DEP-DRIVEN: body becomes a function of its own; runs once at mount,
 *    then again from the update whenever a SCALAR dep changes (compared against a stored prev). The body is
 *    compiled WITHOUT a trailing update — it runs INSIDE an update / at mount, so re-applying state it
 *    sets happens on the next update (one-frame), and it can never re-enter an update (no infinite loop).
 *    A dep-driven `return () => …` becomes `effect_N_cleanup()`, run at the top of the next re-run.
 *
 * @param effect  The effect.
 * @param environment  The expression environment.
 * @param state  The component's state table.
 * @param output  Everything emitted so far; the effect is added to it.
 */
export function lowerEffect(
  effect: EffectRecord,
  environment: Env,
  state: StateTable,
  output: Out,
): void {
  // An expression body is a single statement.
  const body = effect.functionNode.body;
  const statements: t.Statement[] =
    body.type === 'BlockStatement'
      ? body.body
      : [{type: 'ExpressionStatement', expression: body}];

  // A mount-once effect (`[]`) runs once, from the build.
  const isMount =
    effect.dependencies.type === 'ArrayExpression' &&
    effect.dependencies.elements.length === 0;
  if (isMount) {
    const context: StatementContext = {
      stateChanged: false,
      output,
      allowReturn: true,
      bodyList: statements,
    };
    const lowered = lowerStmts(statements, environment, state, context);
    if (!context.usedReturn) {
      output.mountEffects.push({
        kind: 'inline',
        callback: {body: lowered, updatesState: context.stateChanged},
      });

      return;
    }

    // With an early `return` the body gets its own function, so it cannot skip later mount effects.
    const name = `effect_${output.effectCount++}`;
    output.effectFunctions.push({kind: 'mount', name, body: lowered});
    output.mountEffects.push({
      kind: 'call',
      name,
      updatesState: context.stateChanged,
    });

    return;
  }

  // A dep-driven effect gets its own function; each dep must be scalar, so a change shows against a stored copy.
  if (effect.dependencies.type !== 'ArrayExpression') {
    throw aotError(
      'AOT: a useEffect dependency list must be an array literal',
      'pass `[]` (run once) or `[a, b]` (re-run when a/b change).',
    );
  }
  const effectId = output.effectCount++;
  const dependencies = (effect.dependencies as t.ArrayExpression).elements.map(
    dependencyNode => {
      if (!dependencyNode) {
        throw aotError('AOT: a useEffect dependency must be an expression');
      }

      const dependencyExpression = lowerExprWide(dependencyNode, environment);
      if (
        !['int', 'float', 'i64', 'string'].includes(
          slotValueOf(dependencyExpression).slotKind,
        )
      ) {
        throw aotError(
          'AOT: useEffect dependencies must be scalar (number / bool / string)',
          'depend on scalar state values; object/array dependencies are not yet supported.',
        );
      }

      return dependencyExpression;
    },
  );

  // React runs the previous cleanup before a re-run; an `armed` flag records whether the last run reached it.
  const lastStatement = statements[statements.length - 1];
  const hasCleanup =
    lastStatement?.type === 'ReturnStatement' && isFn(lastStatement.argument);
  const dependencyEffect: IrDepEffect = {
    id: effectId,
    dependencies,
    body: [],
    hasCleanup,
    cleanup: null,
  };
  const context: StatementContext = {
    stateChanged: false,
    output,
    allowReturn: true,
    bodyList: statements,
    dependencyDriven: true,
  };

  // With a cleanup, hoist the body's locals and lower the tail `return () => …` into `<effect>_cleanup()`.
  if (hasCleanup) {
    context.hoist = {
      prefix: `effect${effectId}_local_`,
      declarations: output.effectDeclarations,
    };
    context.cleanup = {
      armed: armedFlag(effectId),
      emit: (cleanupFunction, cleanupEnvironment) => {
        const cleanupBody = cleanupFunction.body;
        const cleanupStatements: t.Statement[] =
          cleanupBody.type === 'BlockStatement'
            ? cleanupBody.body
            : [{type: 'ExpressionStatement', expression: cleanupBody}];
        const cleanupContext: StatementContext = {
          stateChanged: false,
          output,
        };
        dependencyEffect.cleanup = lowerStmts(
          cleanupStatements,
          cleanupEnvironment,
          state,
          cleanupContext,
        );
        output.effectFunctions.push({
          kind: 'cleanup',
          effect: dependencyEffect,
        });
      },
    };

    output.effectDeclarations.push({kind: 'armed', effect: dependencyEffect});
  }

  // The effect function runs the last run's cleanup first, if that run armed it, then the body.
  dependencyEffect.body = lowerStmts(statements, environment, state, context);
  output.effectFunctions.push({kind: 'effect', effect: dependencyEffect});

  // A copy of each dep, taken at mount; the update re-runs the effect when one no longer matches its copy.
  output.effectDeclarations.push({kind: 'deps', effect: dependencyEffect});
  output.mountEffects.push({kind: 'depEffect', effect: dependencyEffect});
  output.dependencyEffects.push(dependencyEffect);
}

/**
 * Lowers an event handler to a callback. Its first parameter is the event, whose touch fields read
 * EREventData; a PanResponder callback's second is RN's gestureState (see lowerGestureField).
 *
 * @param functionNode  The handler.
 * @param environment  The expression environment.
 * @param state  The component's state table.
 * @param output  Everything emitted so far.
 * @param pan  The PanResponder the handler belongs to, if any.
 *
 * @returns The callback.
 */
export function lowerHandler(
  functionNode: PanCallback,
  environment: Env,
  state: StateTable,
  output: Out,
  pan: PanResponderRecord | null = null,
): IrCallback {
  // An expression body is a single statement.
  const body = functionNode.body;
  const statements: t.Statement[] =
    body.type === 'BlockStatement'
      ? body.body
      : [{type: 'ExpressionStatement', expression: body}];

  // The first parameter is the event (fields map to EREventData); a PanResponder's second is RN's gestureState.
  const eventParameter =
    functionNode.params[0]?.type === 'Identifier'
      ? functionNode.params[0].name
      : null;
  const gestureParameter =
    pan && functionNode.params[1]?.type === 'Identifier'
      ? functionNode.params[1].name
      : null;
  let handlerEnvironment = environment;
  if (eventParameter) {
    handlerEnvironment = {...handlerEnvironment, event: eventParameter};
  }

  if (gestureParameter) {
    handlerEnvironment = {
      ...handlerEnvironment,
      gesture: gestureParameter,
      pan,
    };
  }

  // The body re-applies state-dependent props once at its end if it set state.
  const context: StatementContext = {stateChanged: false, output};
  return {
    body: lowerStmts(statements, handlerEnvironment, state, context),
    updatesState: context.stateChanged,
  };
}

/**
 * Lowers a value-callback (e.g., Switch onValueChange) — binds its first param to `valueCode`, not an event.
 *
 * @param functionNode  The callback.
 * @param environment  The expression environment.
 * @param state  The component's state table.
 * @param output  Everything emitted so far.
 * @param slotKind  The value's kind.
 * @param isBoolean  The value is a boolean, so text prints it as true/false.
 * @param valueExpression  The value as a typed expression.
 * @param secondValueExpression  The second parameter's value as a typed expression, if any.
 *
 * @returns The callback.
 */
export function lowerValueHandler(
  functionNode: FunctionNode,
  environment: Env,
  state: StateTable,
  output: Out,
  slotKind: SlotKind,
  isBoolean: boolean,
  valueExpression: IrExpr,
  secondValueExpression?: IrExpr,
): IrCallback {
  // A <Switch> hands its callback a boolean; carry that so `'on=' + v` prints true/false, not 1/0.
  const valueParameter =
    functionNode.params[0]?.type === 'Identifier'
      ? functionNode.params[0].name
      : null;
  const locals = new Map<string, Local>(environment.locals);
  if (valueParameter) {
    locals.set(valueParameter, {
      slotKind,
      isBoolean,
      expression: valueExpression,
    });
  }

  // A RANGE <Dial>'s second parameter binds the same way, lowering to the event's value_start with no object on device.
  const secondParameter =
    functionNode.params[1]?.type === 'Identifier'
      ? functionNode.params[1].name
      : null;
  if (secondParameter && secondValueExpression) {
    locals.set(secondParameter, {slotKind, expression: secondValueExpression});
  }

  // Lower the body (an expression body is a single statement); it re-applies state once if it set any.
  const context: StatementContext = {stateChanged: false, output};
  const body = functionNode.body;
  const statements: t.Statement[] =
    body.type === 'BlockStatement'
      ? body.body
      : [{type: 'ExpressionStatement', expression: body}];
  return {
    body: lowerStmts(statements, {...environment, locals}, state, context),
    updatesState: context.stateChanged,
  };
}
