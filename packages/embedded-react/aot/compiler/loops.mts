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
 * Loops: `for` and `for…of` in handlers, effects, timers and the helpers they call. The VM only has a counted loop,
 * so each loop needs a most number of visits the compiler can work out: a `for` counts from a constant or a list's
 * length to another, and a `for…of` visits a list's rows, at most its cap. A test before each visit ends the loop
 * where JS's would.
 */

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {aotError} from './diagnostics.mts';
import {listValueOf, lowerExpr, staticInt} from './expressions.mts';
import {statementsOf} from './helper-values.mts';
import {BOOL, INT} from './ir/types.mts';
import {rowItemOf} from './list-order.mts';
import type * as t from '@babel/types';
import type {IrExpr} from './ir/expressions.mts';
import type {IrStmt} from './ir/statements.mts';
import type {
  Env,
  ListState,
  Local,
  StateTable,
  StatementContext,
} from './types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Types
 ---------------------------------------------------------------------------------------------------------------------*/

/** Lowers a loop's body (lowerStmts). */
type LowerStatements = (
  statements: t.Statement[],
  environment: Env,
  state: StateTable,
  context: StatementContext,
) => IrStmt[];

/** What a bound can read: a list's length, or an outer loop's counter. */
type Variable = ListState | Local;

/** A whole number the compiler can bound: worked out from constants, lists' lengths and outer loops' counters. */
interface Bounded {
  reads: Variable[];
  at: (values: Map<Variable, number>) => number;
}

type CounterTest = '<' | '<=' | '>' | '>=';

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** The most visits of one loop: LOOPSET's count is 16 bits. */
const MAX_VISITS = 0xffff;

/** The range of a 32-bit whole number, which a `for` loop's counter holds. */
const INT_MIN = -(2 ** 31);
const INT_MAX = 2 ** 31 - 1;

/** The most list lengths worked through to find a loop's most visits. */
const MAX_LENGTH_CASES = 100_000;

const FLIPPED: Record<CounterTest, CounterTest> = {
  '<': '>',
  '<=': '>=',
  '>': '<',
  '>=': '<=',
};

const BOUND_HINT =
  "count from a constant or a list's length to another, by a constant step: for (let i = 0; i < 8; i++) or " +
  'for (let i = 0; i < items.length; i++). To go over a list, write for (const item of items).';

/*----------------------------------------------------------------------------------------------------------------------
 - Variables
 ---------------------------------------------------------------------------------------------------------------------*/

/** Numbers the locals each loop's visits are read through. */
let loopVisits = 0;

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * An error located at a node.
 *
 * @param node  The node.
 * @param message  The message.
 * @param hint  The hint.
 *
 * @returns The error.
 */
function errorAt(node: t.Node, message: string, hint?: string): Error {
  const error = aotError(message, hint);
  if (node.loc) error.aotLoc = node.loc.start;
  return error;
}

/**
 * The local a loop's visit number is read through. Each has a name of its own, which no JS name can clash with.
 *
 * @returns The local.
 */
const visitLocal = (): Local => ({
  slotKind: 'int',
  name: `#visit${loopVisits++}`,
});

/**
 * A whole number worked out from constants and the lengths of lists, which the compiler can bound.
 *
 * @param node  The expression.
 * @param environment  The expression environment.
 *
 * @returns How to work it out for given lengths, or null when it reads anything else.
 */
function boundedOf(node: t.Node, environment: Env): Bounded | null {
  const constant = staticInt(node, environment);
  if (constant !== null) return {reads: [], at: () => constant};

  // An outer loop's counter, which goes through the values it was found to take.
  if (node.type === 'Identifier') {
    const local = environment.locals.get(node.name);
    return local?.range
      ? {reads: [local], at: values => values.get(local)!}
      : null;
  }

  if (
    node.type === 'MemberExpression' &&
    !node.computed &&
    node.property.type === 'Identifier' &&
    node.property.name === 'length'
  ) {
    const list = listValueOf(node.object, environment);
    return list ? {reads: [list], at: values => values.get(list)!} : null;
  }

  if (node.type === 'UnaryExpression' && node.operator === '-') {
    const operand = boundedOf(node.argument, environment);
    return operand && {reads: operand.reads, at: values => -operand.at(values)};
  }

  if (
    node.type === 'BinaryExpression' &&
    (node.operator === '+' || node.operator === '-' || node.operator === '*')
  ) {
    const left = boundedOf(node.left, environment);
    const right = boundedOf(node.right, environment);
    if (!left || !right) return null;
    const {operator} = node;
    return {
      reads: [...left.reads, ...right.reads],
      at: values => {
        const [first, second] = [left.at(values), right.at(values)];
        return operator === '+'
          ? first + second
          : operator === '-'
            ? first - second
            : first * second;
      },
    };
  }

  // Math.min / Math.max of bounded values.
  if (
    node.type === 'CallExpression' &&
    node.callee.type === 'MemberExpression' &&
    node.callee.object.type === 'Identifier' &&
    node.callee.object.name === 'Math' &&
    node.callee.property.type === 'Identifier' &&
    (node.callee.property.name === 'min' ||
      node.callee.property.name === 'max') &&
    node.arguments.length > 0
  ) {
    const parts = node.arguments.map(argument =>
      argument.type === 'SpreadElement' ||
      argument.type === 'ArgumentPlaceholder'
        ? null
        : boundedOf(argument, environment),
    );
    if (parts.some(part => !part)) return null;
    const pick = node.callee.property.name === 'min' ? Math.min : Math.max;
    return {
      reads: parts.flatMap(part => part!.reads),
      at: values => pick(...parts.map(part => part!.at(values))),
    };
  }

  return null;
}

/**
 * How many times a counter goes from `start` by `step` while `start test end` holds.
 *
 * @returns The count, or Infinity when the counter never gets there.
 */
function visitsOf(
  start: number,
  test: CounterTest,
  end: number,
  step: number,
): number {
  const holds = (value: number): boolean =>
    test === '<'
      ? value < end
      : test === '<='
        ? value <= end
        : test === '>'
          ? value > end
          : value >= end;
  if (!holds(start)) return 0;
  const goesUp = test === '<' || test === '<=';
  if (goesUp !== step > 0) return Infinity;
  const distance = Math.abs(end - start);
  const size = Math.abs(step);
  return test === '<' || test === '>'
    ? Math.ceil(distance / size)
    : Math.floor(distance / size) + 1;
}

/**
 * The values a bound's variable can have: a list holds from none to its cap (a constant array exactly its
 * length), and an outer counter goes through the values it was found to take.
 *
 * @param variable  The variable.
 *
 * @returns Its lowest and highest value.
 */
const rangeOf = (variable: Variable): [number, number] =>
  'kind' in variable
    ? [variable.constant ? variable.capacity : 0, variable.capacity]
    : variable.range!;

/**
 * The most times a loop can run, and the values its counter takes, over every value its bounds can read.
 *
 * @param reads  What its start and end read.
 * @param start  Its start.
 * @param test  How its test compares the counter with the end.
 * @param end  Its end.
 * @param step  What each visit adds.
 * @param node  The loop, for errors.
 *
 * @returns The count, and the counter's lowest and highest value, or null when it never runs.
 */
function mostVisits(
  reads: Variable[],
  start: Bounded,
  test: CounterTest,
  end: Bounded,
  step: number,
  node: t.Node,
): {visits: number; range: [number, number] | null} {
  const distinct = [...new Set(reads)];
  const ranges = distinct.map(rangeOf);
  const cases = ranges.reduce(
    (count, [low, high]) => count * (high - low + 1),
    1,
  );
  if (cases > MAX_LENGTH_CASES) {
    throw errorAt(
      node,
      "AOT: a for loop's bound reads too many values to count its visits",
      BOUND_HINT,
    );
  }

  let most = 0;
  let range: [number, number] | null = null;
  const values = new Map<Variable, number>();
  const visit = (at: number): void => {
    if (at === distinct.length) {
      const first = start.at(values);
      const visits = visitsOf(first, test, end.at(values), step);
      most = Math.max(most, visits);
      if (visits > 0 && visits !== Infinity) {
        const last = first + (visits - 1) * step;
        const [low, high] = range ?? [first, first];
        range = [Math.min(low, first, last), Math.max(high, first, last)];
      }
      return;
    }
    const [low, high] = ranges[at];
    for (let value = low; value <= high; value++) {
      values.set(distinct[at], value);
      visit(at + 1);
    }
  };
  visit(0);
  return {visits: most, range};
}

/**
 * A `for` loop's step: `i++`, `i--`, `i += k`, `i -= k`, `i = i + k` or `i = i - k`, with k a constant.
 *
 * @param update  The loop's update.
 * @param name  The counter's name.
 * @param environment  The expression environment.
 *
 * @returns How much each visit adds, or null when it is no such step.
 */
function stepOf(
  update: t.Expression | null | undefined,
  name: string,
  environment: Env,
): number | null {
  if (!update) return null;
  const isCounter = (node: t.Node): boolean =>
    node.type === 'Identifier' && node.name === name;
  if (update.type === 'UpdateExpression' && isCounter(update.argument)) {
    return update.operator === '++' ? 1 : -1;
  }
  if (update.type !== 'AssignmentExpression' || !isCounter(update.left)) {
    return null;
  }
  const sign = (operator: string): number | null =>
    operator === '+' ? 1 : operator === '-' ? -1 : null;
  if (update.operator === '+=' || update.operator === '-=') {
    const amount = staticInt(update.right, environment);
    return amount === null ? null : sign(update.operator[0])! * amount;
  }
  if (
    update.operator === '=' &&
    update.right.type === 'BinaryExpression' &&
    isCounter(update.right.left) &&
    sign(update.right.operator) !== null
  ) {
    const amount = staticInt(update.right.right, environment);
    return amount === null ? null : sign(update.right.operator)! * amount;
  }
  return null;
}

/**
 * Lowers a loop's body with the lists it visits or is bounded by on the context's stack, which a setter of one
 * of them inside it is checked against.
 *
 * @param body  The body.
 * @param lists  The lists.
 * @param environment  The body's environment.
 * @param state  The component's state table.
 * @param context  The statement compiler's context.
 * @param lowerStatements  lowerStmts.
 *
 * @returns The lowered body.
 */
function lowerLoopBody(
  body: t.Statement[],
  lists: ListState[],
  environment: Env,
  state: StateTable,
  context: StatementContext,
  lowerStatements: LowerStatements,
): IrStmt[] {
  const loops = (context.loops ??= []);
  // A filtered or sorted list holds rows of its source, so setting the source changes it too.
  loops.push(
    lists.flatMap(list =>
      list.derived?.source ? [list, list.derived.source] : [list],
    ),
  );
  try {
    return lowerStatements(body, environment, state, context);
  } finally {
    loops.pop();
  }
}

/**
 * Lowers `for (let i = start; i < end; i++) …`: the counter is a local the body reads but can't set. Start and end
 * are constants or lists' lengths, so the compiler can work out how many times it runs at most; one that reads a
 * length is tested before each visit, as JS tests it.
 *
 * @param statement  The loop.
 * @param environment  The expression environment.
 * @param state  The component's state table.
 * @param context  The statement compiler's context.
 * @param lowerStatements  lowerStmts.
 *
 * @returns The statements.
 */
export function lowerFor(
  statement: t.ForStatement,
  environment: Env,
  state: StateTable,
  context: StatementContext,
  lowerStatements: LowerStatements,
): IrStmt[] {
  const {init, test, update} = statement;
  const declarator =
    init?.type === 'VariableDeclaration' &&
    init.kind === 'let' &&
    init.declarations.length === 1
      ? init.declarations[0]
      : null;
  if (!declarator || declarator.id.type !== 'Identifier' || !declarator.init) {
    throw errorAt(
      statement,
      'AOT: a for loop must declare its counter with let',
      BOUND_HINT,
    );
  }
  const name = declarator.id.name;

  // `i < end`, or `end > i`.
  if (
    test?.type !== 'BinaryExpression' ||
    !(test.operator in FLIPPED) ||
    test.left.type === 'PrivateName'
  ) {
    throw errorAt(
      test ?? statement,
      `AOT: a for loop's test must compare ${name} with its end: ${name} < end`,
      BOUND_HINT,
    );
  }
  const isCounter = (node: t.Node): boolean =>
    node.type === 'Identifier' && node.name === name;
  const counterOnLeft = isCounter(test.left);
  if (!counterOnLeft && !isCounter(test.right)) {
    throw errorAt(
      test,
      `AOT: a for loop's test must compare ${name} with its end: ${name} < end`,
      BOUND_HINT,
    );
  }
  const operator = counterOnLeft
    ? (test.operator as CounterTest)
    : FLIPPED[test.operator as CounterTest];
  const endNode = (counterOnLeft ? test.right : test.left) as t.Expression;

  const step = stepOf(update, name, environment);
  if (!step) {
    throw errorAt(
      update ?? statement,
      `AOT: a for loop must step ${name} by a constant: ${name}++, ${name}-- or ${name} += 2`,
      BOUND_HINT,
    );
  }

  const start = boundedOf(declarator.init, environment);
  const end = boundedOf(endNode, environment);
  if (!start || !end) {
    throw errorAt(
      start ? endNode : declarator.init,
      "AOT: a for loop's start and end must be constants or lists' lengths",
      BOUND_HINT,
    );
  }
  const reads = [...start.reads, ...end.reads];
  const {visits: bound, range} = mostVisits(
    reads,
    start,
    operator,
    end,
    step,
    statement,
  );
  if (bound === Infinity) {
    throw errorAt(
      statement,
      `AOT: this for loop never ends: ${name} moves away from its end`,
      BOUND_HINT,
    );
  }
  if (bound > MAX_VISITS) {
    throw errorAt(
      statement,
      `AOT: a for loop runs at most ${MAX_VISITS} times, and this one can run ${bound}`,
      'split the work into smaller loops, or over several events.',
    );
  }
  // The counter is an int, and so is the value the last step leaves it at.
  if (
    range &&
    (Math.min(range[0], range[0] + step) < INT_MIN ||
      Math.max(range[1], range[1] + step) > INT_MAX)
  ) {
    throw errorAt(
      statement,
      `AOT: a for loop's counter must stay a 32-bit whole number, and ${name} can reach past one`,
      BOUND_HINT,
    );
  }

  // The counter is a local of its own, which the body reads and only the step sets.
  const counter: Local = {
    slotKind: 'int',
    name: `local_${name}@${statement.start ?? 0}`,
    counter: true,
    ...(range ? {range} : {}),
  };
  const startValue = lowerExpr(declarator.init, environment);
  const bodyEnvironment: Env = {
    ...environment,
    locals: new Map(environment.locals).set(name, counter),
  };
  const counterRead: IrExpr = {kind: 'local', local: counter, type: INT};
  const lists = reads.filter((read): read is ListState => 'kind' in read);
  const body = lowerLoopBody(
    statementsOf(statement.body),
    lists,
    bodyEnvironment,
    state,
    context,
    lowerStatements,
  );
  return [
    {
      kind: 'declareLocal',
      name: counter.name!,
      initializer: startValue,
      isHoisted: false,
    },
    {
      kind: 'loop',
      bound,
      visit: null,
      // A loop whose bounds read only constants runs exactly its bound.
      test: reads.length
        ? {
            kind: 'compare',
            operator,
            left: counterRead,
            right: lowerExpr(endNode, environment),
            type: BOOL,
          }
        : null,
      body,
      step: [
        {
          kind: 'assignLocal',
          name: counter.name!,
          value: {
            kind: 'helper',
            helper: 'add',
            bits: 32,
            callArguments: [
              counterRead,
              {kind: 'number', value: step, type: INT},
            ],
            type: INT,
          },
        },
      ],
    },
  ];
}

/**
 * The names a `for…of` binds for each row: `item`, `{a, b}`, or `[i, item]` over `list.entries()`.
 *
 * @param statement  The loop.
 *
 * @returns The list's expression, the item's pattern, and the index's name for entries().
 */
function forOfParts(statement: t.ForOfStatement): {
  iterable: t.Expression;
  item: t.LVal;
  indexName: string | null;
  kind: 'const' | 'let';
} {
  const {left, right} = statement;
  if (
    left.type !== 'VariableDeclaration' ||
    (left.kind !== 'const' && left.kind !== 'let') ||
    left.declarations.length !== 1 ||
    left.declarations[0].init
  ) {
    throw errorAt(
      statement,
      'AOT: a for…of must declare its item with const or let: for (const item of items)',
    );
  }
  const {kind} = left;
  const pattern = left.declarations[0].id;

  // `for (const [i, item] of items.entries())`.
  const isEntries =
    right.type === 'CallExpression' &&
    right.callee.type === 'MemberExpression' &&
    !right.callee.computed &&
    right.callee.property.type === 'Identifier' &&
    right.callee.property.name === 'entries' &&
    right.arguments.length === 0;
  if (isEntries) {
    const [index, item] =
      pattern.type === 'ArrayPattern' ? pattern.elements : [];
    if (
      pattern.type !== 'ArrayPattern' ||
      pattern.elements.length !== 2 ||
      index?.type !== 'Identifier' ||
      !item ||
      item.type === 'RestElement'
    ) {
      throw errorAt(
        pattern,
        'AOT: a for…of over entries() must name the index and the item: for (const [i, item] of items.entries())',
      );
    }
    return {
      iterable: (right.callee as t.MemberExpression).object as t.Expression,
      item: item as t.LVal,
      indexName: index.name,
      kind,
    };
  }
  return {iterable: right, item: pattern as t.LVal, indexName: null, kind};
}

/**
 * Binds a for…of's item pattern: a name for the row, or `{a, b: c}`, each field a local the body starts with,
 * declared as the loop declares its item.
 *
 * @param pattern  The pattern.
 * @param item  The row's item.
 * @param locals  The body's locals, which the item's name is added to.
 * @param kind  How the loop declares its item.
 *
 * @returns The declarations the body starts with.
 */
function bindItem(
  pattern: t.LVal,
  item: Local,
  locals: Map<string, Local>,
  kind: 'const' | 'let',
): t.Statement[] {
  if (pattern.type === 'Identifier') {
    locals.set(pattern.name, {...item, name: pattern.name});
    return [];
  }
  if (pattern.type !== 'ObjectPattern') {
    throw errorAt(
      pattern,
      'AOT: a for…of item must be a name or {field, …}',
      'name the item, then read its fields: for (const item of items) … item.title.',
    );
  }

  // The row is bound under a name no JS name can clash with, and each field read from it.
  const hidden: t.Identifier = {
    type: 'Identifier',
    name: `item@${pattern.start ?? 0}`,
  };
  locals.set(hidden.name, {...item, name: hidden.name});
  const declarators = pattern.properties.map(
    (property): t.VariableDeclarator => {
      if (
        property.type !== 'ObjectProperty' ||
        property.computed ||
        property.key.type !== 'Identifier' ||
        property.value.type !== 'Identifier'
      ) {
        throw errorAt(
          property,
          'AOT: a for…of item can only take plain fields apart: {title, done}',
          'read a nested or defaulted field in the body: const x = item.pos.x.',
        );
      }
      return {
        type: 'VariableDeclarator',
        id: property.value,
        init: {
          type: 'MemberExpression',
          object: hidden,
          property: property.key,
          computed: false,
          loc: property.loc,
        },
        loc: property.loc,
      };
    },
  );
  return declarators.length
    ? [
        {
          type: 'VariableDeclaration',
          kind,
          declarations: declarators,
          loc: pattern.loc,
        },
      ]
    : [];
}

/**
 * Lowers `for (const item of list) …`: the body runs for each row of a list state, a constant array, a list worked
 * out in render or a handler's split, at most the list's cap times, and stops at its count.
 *
 * @param statement  The loop.
 * @param environment  The expression environment.
 * @param state  The component's state table.
 * @param context  The statement compiler's context.
 * @param lowerStatements  lowerStmts.
 *
 * @returns The statements.
 */
export function lowerForOf(
  statement: t.ForOfStatement,
  environment: Env,
  state: StateTable,
  context: StatementContext,
  lowerStatements: LowerStatements,
): IrStmt[] {
  if (statement.await) {
    throw errorAt(statement, 'AOT: for await is not supported');
  }
  const {iterable, item: pattern, indexName, kind} = forOfParts(statement);
  const list = listValueOf(iterable, environment);
  if (!list) {
    throw errorAt(
      iterable,
      'AOT: a for…of must go over a list',
      'loop over a list state, a constant array, a filtered or sorted list, or a split: for (const item of items).',
    );
  }

  const visit = visitLocal();
  const row: IrExpr = {kind: 'local', local: visit, type: INT};
  const item = rowItemOf(list, {name: 'item', index: row} as Local);
  const locals = new Map(environment.locals);
  const declarations = bindItem(pattern, item, locals, kind);
  if (indexName) {
    locals.set(indexName, {...visit, counter: true});
  }
  const body = lowerLoopBody(
    [...declarations, ...statementsOf(statement.body)],
    [list],
    {...environment, locals},
    state,
    context,
    lowerStatements,
  );
  return [
    {
      kind: 'loop',
      bound: list.capacity,
      visit,
      // A constant array is always full.
      test: list.constant
        ? null
        : {
            kind: 'compare',
            operator: '<',
            left: row,
            right: {kind: 'listLength', list, type: INT},
            type: BOOL,
          },
      body,
      step: [],
    },
  ];
}
