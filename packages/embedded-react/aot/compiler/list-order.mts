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

// list-order.mts — `filter` and `sort` on a list. In a setter they change its rows in place; in render they make a
// list of the source's row numbers, which the update rebuilds.

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {aotError} from './diagnostics.mts';
import {isFn} from './collect.mts';
import {constantListOf, fieldType, lowerExprWide} from './expressions.mts';
import {evalStatic, foldScope, rethrowFoldLimit} from './static-eval.mts';
import {INT} from './ir/types.mts';
import type * as t from '@babel/types';
import type {IrExpr} from './ir/expressions.mts';
import type {IrStmt} from './ir/statements.mts';
import type {Env, ListState, Local, OrderStep} from './types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** A function a list method is given, inline: its parameters and the value it returns. */
export interface ItemCallback {
  itemName?: string;
  indexName?: string;
  body: t.Expression;
}

/** A chain of filters and sorts on a list: `items.filter(…).toSorted(…)`. */
export interface ListChain {
  /** What the first step is called on. */
  base: t.Expression;
  steps: OrderStep[];
}

/** The rows a filter or sort visits. */
interface StepRows {
  /** The item at a position. */
  itemAt(position: Local): Local;
  /** What the code calls the list, for errors. */
  listName: string;
  /** The list the items are rows of. */
  source: ListState;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** One int field: a row number. */
const ROW_NUMBERS = {
  fields: [{key: '', path: [], kind: 'int' as const}],
  plain: true,
};

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

let visits = 0;

/**
 * A local a filter or sort reads a row's index through. Each has a name no JS name can clash with.
 *
 * @returns The local.
 */
const visitLocal = (): Local => ({
  slotKind: 'int',
  name: `#visit${visits++}`,
  visit: true,
});

/**
 * The inline function a list method is given.
 *
 * @param node  The argument.
 * @param what  The call, for errors: `items.map`.
 *
 * @returns Its parameters and what it returns.
 */
export function itemCallback(
  node: t.Node | undefined,
  what: string,
): ItemCallback {
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
 * The method a call makes on an object: `x.filter(…)` gives `filter`.
 *
 * @param node  The expression.
 *
 * @returns The method and what it is called on, or null when it is no such call.
 */
function methodCall(
  node: t.Node,
): {method: string; object: t.Expression; call: t.CallExpression} | null {
  if (
    node.type !== 'CallExpression' ||
    node.callee.type !== 'MemberExpression' ||
    node.callee.computed ||
    node.callee.property.type !== 'Identifier'
  ) {
    return null;
  }
  return {
    method: node.callee.property.name,
    object: node.callee.object,
    call: node,
  };
}

/**
 * The list a `sort` sorts a copy of: `[...items]`, `items.slice()`, or the result of a filter or sort, which is a
 * copy already. `items.sort(…)` would sort the list itself.
 *
 * @param node  What `sort` is called on.
 *
 * @returns The list it copies, the node itself for a result that is a copy, or null when it is no copy.
 */
function copied(node: t.Expression): t.Expression | null {
  if (
    node.type === 'ArrayExpression' &&
    node.elements.length === 1 &&
    node.elements[0]?.type === 'SpreadElement'
  ) {
    return node.elements[0].argument;
  }
  const call = methodCall(node);
  if (
    call?.method === 'slice' &&
    (call.call.arguments.length === 0 ||
      (call.call.arguments.length === 1 &&
        call.call.arguments[0].type === 'NumericLiteral' &&
        call.call.arguments[0].value === 0))
  ) {
    return call.object;
  }
  if (
    call &&
    (call.method === 'filter' ||
      call.method === 'toSorted' ||
      call.method === 'sort')
  ) {
    return node;
  }
  return null;
}

/**
 * Reads a chain of filters and sorts: `items.filter(…)`, `items.toSorted(…)`, `[...items].sort(…)` and those
 * one after another.
 *
 * @param node  The expression.
 *
 * @returns The chain, or null when the expression is none.
 */
export function listChainOf(node: t.Node): ListChain | null {
  const steps: OrderStep[] = [];
  let current = node as t.Expression;
  for (;;) {
    const call = methodCall(current);
    if (!call) break;
    const [callback] = call.call.arguments;
    if (call.call.arguments.length > 1 || callback?.type === 'SpreadElement') {
      break;
    }
    if (call.method === 'filter' || call.method === 'toSorted') {
      steps.unshift({
        kind: call.method === 'filter' ? 'filter' : 'sort',
        callback,
        what: `.${call.method}`,
      });
      current = call.object;
      continue;
    }
    if (call.method !== 'sort') break;
    const list = copied(call.object);
    if (!list) {
      throw aotError(
        'AOT: sort() sorts the list itself, which would change state in place',
        'sort a copy: [...items].sort((a, b) => a.n - b.n), or items.toSorted((a, b) => a.n - b.n).',
      );
    }
    steps.unshift({kind: 'sort', callback, what: '.sort'});
    current = list;
  }
  if (!steps.length) return null;
  const name =
    current.type === 'Identifier' ? current.name : steps[0].what.slice(1);
  steps.forEach(step => {
    step.what = `${name}${step.what}`;
  });
  return {base: current, steps};
}

/**
 * An environment where a function's parameters name what a step binds them to.
 *
 * @param environment  The environment around the call.
 * @param bindings  Each parameter's name, if it has one, and its local.
 *
 * @returns The environment.
 */
function boundEnvironment(
  environment: Env,
  bindings: [string | undefined, Local][],
): Env {
  const locals = new Map(environment.locals);
  let {state} = environment;
  for (const [name, local] of bindings) {
    if (!name) continue;
    locals.set(name, local);
    if (state.has(name)) {
      state = new Map(state);
      state.delete(name);
    }
  }
  return {...environment, locals, state};
}

/**
 * What a filter's function keeps a row for: `item => !item.done`, `(item, i) => i < 3`.
 *
 * @param step  The filter.
 * @param rows  The rows it visits.
 * @param position  The row's index.
 *
 * @returns The test.
 */
function filterTest(step: OrderStep, rows: StepRows, position: Local): IrExpr {
  const {itemName, indexName, body} = itemCallback(step.callback, step.what);
  return lowerExprWide(
    body,
    boundEnvironment(step.environment!, [
      [itemName, rows.itemAt(position)],
      [indexName, position],
    ]),
  );
}

/**
 * A compare function's result with each `x || y` written `x ? x : y`, which gives the number JS does: a tie broken
 * by another field, `a.done - b.done || a.n - b.n`.
 *
 * @param node  The result.
 *
 * @returns It, rewritten.
 */
function keepsNumber(node: t.Expression): t.Expression {
  if (node.type !== 'LogicalExpression' || node.operator !== '||') {
    return node;
  }
  const left = keepsNumber(node.left);
  return {
    type: 'ConditionalExpression',
    test: left,
    consequent: left,
    alternate: keepsNumber(node.right),
    loc: node.loc,
  } as t.ConditionalExpression;
}

/**
 * What a sort's compare function gives two rows: above zero when the first goes after the second. With no function,
 * a list of strings sorts by code point; JS sorts any other list by its text, which a package doesn't.
 *
 * @param step  The sort.
 * @param rows  The rows it visits.
 * @param first  The first row's index.
 * @param second  The second row's index.
 *
 * @returns The order.
 */
function sortOrder(
  step: OrderStep,
  rows: StepRows,
  first: Local,
  second: Local,
): IrExpr {
  const {struct} = rows.source;
  if (!step.callback) {
    const [field] = struct.fields;
    if (!struct.plain || field.kind !== 'string') {
      throw aotError(
        `AOT: ${step.what}() needs a compare function`,
        'without one JS sorts by text, which a package does only for a list of strings. Pass one: ' +
          `${step.what}((a, b) => a.n - b.n).`,
      );
    }
    const read = (position: Local): IrExpr => ({
      kind: 'itemField',
      item: rows.itemAt(position),
      field,
      type: fieldType(field),
    });
    return {
      kind: 'stringOrder',
      left: read(first),
      right: read(second),
      type: INT,
    };
  }
  const callback = step.callback;
  if (!isFn(callback) || callback.params.length !== 2) {
    throw aotError(
      `AOT: ${step.what} needs an inline compare function of two items`,
      `write it in the call: ${step.what}((a, b) => a.n - b.n).`,
    );
  }
  const {itemName, indexName, body} = itemCallback(callback, step.what);
  const order = lowerExprWide(
    keepsNumber(body),
    boundEnvironment(step.environment!, [
      [itemName, rows.itemAt(first)],
      [indexName, rows.itemAt(second)],
    ]),
  );
  if (!['int', 'float', 'bool'].includes(order.type.kind)) {
    throw aotError(
      `AOT: ${step.what}'s compare function must give a number`,
      'give a negative number when a goes first, a positive one when b does, and 0 to keep their order: ' +
        '(a, b) => a.n - b.n, or a.name.localeCompare(b.name).',
    );
  }
  return order;
}

/**
 * The statements that filter and sort a list's rows in place, one step after another.
 *
 * @param list  The list whose rows they change.
 * @param steps  The steps.
 * @param rows  The rows they visit.
 *
 * @returns The statements.
 */
export function orderStatements(
  list: ListState,
  steps: OrderStep[],
  rows: StepRows,
): IrStmt[] {
  return steps.map(step => {
    if (step.kind === 'filter') {
      const position = visitLocal();
      return {
        kind: 'filterItems',
        list,
        position,
        keep: filterTest(step, rows, position),
      };
    }
    const first = visitLocal();
    const second = visitLocal();
    return {
      kind: 'sortItems',
      list,
      first,
      second,
      order: sortOrder(step, rows, first, second),
    };
  });
}

/**
 * The tests and compare functions of a rebuild.
 *
 * @param statements  The rebuild.
 *
 * @returns What they work out.
 */
const testsOf = (statements: IrStmt[]): IrExpr[] =>
  statements.flatMap(statement =>
    statement.kind === 'fillOrder'
      ? statement.keep
        ? [statement.keep]
        : []
      : statement.kind === 'filterItems'
        ? [statement.keep]
        : statement.kind === 'sortItems'
          ? [statement.order]
          : [],
  );

/**
 * Refuses a test that reads what only a handler has, which the update can't work out.
 *
 * @param tests  The tests.
 * @param what  The call, for errors.
 */
function checkRenderReads(tests: IrExpr[], what: string): void {
  const seen = new Set<object>();
  const walk = (value: unknown): void => {
    if (!value || typeof value !== 'object' || seen.has(value)) return;
    seen.add(value);
    if (Array.isArray(value)) {
      value.forEach(walk);
      return;
    }
    const node = value as {kind?: string; local?: Local};
    if (node.kind === 'local' && node.local) {
      const {local} = node;
      if (!local.list && !local.visit && !local.expression) {
        throw aotError(
          `AOT: ${what}'s function reads a local of the handler`,
          'a filtered or sorted list is worked out with the render, from state and props. Filter in the ' +
            `setter instead, or test the rows in a loop of your own.`,
        );
      }
    }
    for (const [key, child] of Object.entries(value)) {
      if (key !== 'source' && key !== 'location') walk(child);
    }
  };
  tests.forEach(walk);
}

/**
 * The list a chain starts from: a list state or one worked out in render that a name holds, or a constant array.
 * A chain whose functions fold with a constant array folds whole, so it is no list here.
 *
 * @param chain  The chain.
 * @param node  The whole chain.
 * @param environment  The expression environment.
 *
 * @returns The list, or null.
 */
function baseListOf(
  chain: ListChain,
  node: t.Node,
  environment: Env,
): ListState | null {
  const {base} = chain;
  if (base.type === 'Identifier' && environment.state.has(base.name)) {
    const record = environment.state.get(base.name)!;
    return record.kind === 'list' ? record : null;
  }
  const scope = foldScope(environment, environment.constants ?? {});
  const fold = (part: t.Node): {value: unknown} | null => {
    try {
      return {value: evalStatic(part, scope, 'boot')};
    } catch (error) {
      rethrowFoldLimit(error);
      return null;
    }
  };
  const array = fold(base)?.value;
  if (!Array.isArray(array) || fold(node)) return null;
  return constantListOf(array, base, environment);
}

/**
 * The list a filter or sort chain makes in render: the row numbers of its source that the filters keep, in the
 * order the sorts leave them, rebuilt by the update when what it reads changes. A chain on a list that is one
 * goes on from its steps.
 *
 * @param node  The chain.
 * @param environment  The expression environment.
 *
 * @returns The list, or null when the expression is no such chain.
 */
export function derivedListOf(
  node: t.Node,
  environment: Env,
): ListState | null {
  const chain = listChainOf(node);
  if (!chain) return null;
  const base = baseListOf(chain, node, environment);
  if (!base) return null;
  if (environment.inListRow) {
    throw aotError(
      `AOT: ${chain.steps[0].what} inside a list row isn't supported`,
      'filter or sort the list once, outside the rows: const visible = items.filter(…).',
    );
  }

  const lists = environment.derivedLists;
  if (!lists) {
    throw new Error('AOT internal: no table for filtered and sorted lists');
  }
  const known = lists.get(node)?.get(base);
  if (known) return known;

  const source = base.derived?.source ?? base;
  const steps = [
    ...(base.derived?.steps ?? []),
    ...chain.steps.map(step => ({...step, environment})),
  ];
  const count = [...lists.values()].reduce((sum, map) => sum + map.size, 0);
  const list: ListState = {
    kind: 'list',
    name: steps.at(-1)!.what,
    slotName: `o${count}_${source.name}`,
    setter: undefined,
    struct: ROW_NUMBERS,
    items: [],
    capacity: source.capacity,
    initialAt: node.loc?.start,
  };

  // The first filter fills the list with the rows it keeps; the steps after it change those in place.
  const listName = source.name;
  const fill = visitLocal();
  const [first, ...rest] = steps;
  const sourceRows: StepRows = {
    itemAt: position =>
      ({
        name: `${listName}[]`,
        struct: source.struct,
        list: source,
        index: {kind: 'local', local: position, type: INT},
      }) as Local,
    listName,
    source,
  };
  const orderRows: StepRows = {
    ...sourceRows,
    itemAt: position => ({
      ...sourceRows.itemAt(position),
      through: list,
    }),
  };
  const filtersFirst = first.kind === 'filter';
  const rebuild: IrStmt[] = [
    {
      kind: 'fillOrder',
      list,
      row: fill,
      keep: filtersFirst ? filterTest(first, sourceRows, fill) : null,
    },
    ...orderStatements(list, filtersFirst ? rest : steps, orderRows),
  ];
  const tests = testsOf(rebuild);
  checkRenderReads(tests, chain.steps[0].what);
  list.derived = {source, steps, rebuild, tests};

  if (!lists.has(node)) lists.set(node, new Map());
  lists.get(node)!.set(base, list);
  return list;
}

/**
 * The item a row of a list names: a row of its own, or for a filtered or sorted list, the source row it holds.
 *
 * @param list  The list.
 * @param item  Where the row is: `row` or `index`, `guarded`, and its name.
 *
 * @returns The item.
 */
export function rowItemOf(list: ListState, item: Local): Local {
  return list.derived
    ? {
        ...item,
        struct: list.derived.source.struct,
        list: list.derived.source,
        through: list,
      }
    : {...item, struct: list.struct, list};
}

/**
 * A component's memos and the lists its body filters or sorts by name, in the order the body has them, so each
 * can read the ones before it.
 *
 * @param body  The component's body.
 * @param memos  Its useMemo bindings.
 *
 * @returns Each binding's name, its expression, and whether it is a memo.
 */
export function renderBindings(
  body: t.BlockStatement | t.Expression,
  memos: Map<string, t.Expression>,
): {name: string; expression: t.Expression; isMemo: boolean}[] {
  if (body.type !== 'BlockStatement') {
    return [...memos].map(([name, expression]) => ({
      name,
      expression,
      isMemo: true,
    }));
  }
  const bindings = [];
  for (const statement of body.body) {
    if (statement.type !== 'VariableDeclaration') continue;
    for (const {id, init} of statement.declarations) {
      if (id.type !== 'Identifier' || !init) continue;
      const memo = memos.get(id.name);
      const method = methodCall(init)?.method;
      if (memo) {
        bindings.push({name: id.name, expression: memo, isMemo: true});
      } else if (
        statement.kind === 'const' &&
        (method === 'filter' || method === 'toSorted' || method === 'sort')
      ) {
        bindings.push({name: id.name, expression: init, isMemo: false});
      }
    }
  }
  return bindings;
}

/**
 * Binds a name to the list it filters or sorts in render, which reads then find as a list.
 *
 * @param name  The name.
 * @param expression  What it is bound to.
 * @param environment  The component's environment, whose state table takes it.
 *
 * @returns Whether it named such a list.
 */
export function bindDerivedList(
  name: string,
  expression: t.Expression,
  environment: Env,
): boolean {
  const list = derivedListOf(expression, environment);
  if (!list) return false;
  environment.state = new Map(environment.state).set(name, list);
  delete environment.constants?.[name];
  return true;
}
