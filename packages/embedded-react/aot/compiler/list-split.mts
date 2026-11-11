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

// list-split.mts — `split` of a string into a list of strings. A setter fills its list with the pieces; in render
// the pieces make a list the update rebuilds; a handler's `const` fills a list of its own.

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {aotError} from './diagnostics.mts';
import {buildsText, lowerExpr, lowerExprWide} from './expressions.mts';
import {expressionReads} from './ir/reads.mts';
import {
  evalStatic,
  foldScope,
  rethrowFoldLimit,
  splitProblem,
} from './static-eval.mts';
import {nextRebuild} from './worked-values.mts';
import type * as t from '@babel/types';
import type {IrExpr} from './ir/expressions.mts';
import type {IrSplitItems, IrStmt} from './ir/statements.mts';
import type {Env, ItemStruct, ListState} from './types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** A `text.split(…)` call. */
interface SplitCall {
  /** What it splits. */
  text: t.Expression;
  call: t.CallExpression;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * One string field: a piece.
 *
 * @returns The struct.
 */
const pieces = (): ItemStruct => ({
  fields: [{key: '', path: [], kind: 'string'}],
  plain: true,
});

/**
 * Reads `text.split(…)`.
 *
 * @param node  The expression.
 *
 * @returns The call, or null when the expression is none.
 */
export function splitCallOf(node: t.Node | null | undefined): SplitCall | null {
  if (
    node?.type !== 'CallExpression' ||
    node.callee.type !== 'MemberExpression' ||
    node.callee.computed ||
    node.callee.property.type !== 'Identifier' ||
    node.callee.property.name !== 'split'
  ) {
    return null;
  }
  return {text: node.callee.object, call: node};
}

/**
 * A string a split reads, kept somewhere: a string built in place lives in the format buffer, not in a register.
 *
 * @param node  The string.
 * @param environment  The expression environment.
 * @param what  What it is to the split, for errors.
 *
 * @returns The string.
 */
function keptString(node: t.Node, environment: Env, what: string): IrExpr {
  if (
    what === 'the separator' &&
    (node.type === 'RegExpLiteral' ||
      (node.type === 'NewExpression' &&
        node.callee.type === 'Identifier' &&
        node.callee.name === 'RegExp'))
  ) {
    throw aotError(...splitProblem(/,/, undefined, false)!);
  }
  if (buildsText(node, environment)) {
    throw aotError(
      `AOT: split needs ${what} kept somewhere, not built in place`,
      'build it into a local or state first: `const line = `${a},${b}`;` then line.split(",").',
    );
  }
  const value = lowerExprWide(node, environment);
  if (value.type.kind !== 'string') {
    throw what === 'the separator'
      ? aotError(...splitProblem(0, undefined, false)!)
      : aotError(
          'AOT: split is only supported on a string',
          'JS has split on strings only; convert first: String(n).split("").',
        );
  }
  return value;
}

/**
 * The statement that fills a list of strings with a string's pieces. `split()` with no separator keeps the whole
 * string as one piece.
 *
 * @param list  The list.
 * @param split  The split.
 * @param environment  The expression environment.
 *
 * @returns The statement.
 */
function splitStatement(
  list: ListState,
  split: SplitCall,
  environment: Env,
): IrSplitItems {
  const [separatorNode, limitNode, ...rest] = split.call.arguments;
  if ([separatorNode, limitNode].some(node => node?.type === 'SpreadElement')) {
    throw aotError(
      'AOT: split cannot take a spread',
      'pass the separator and the limit: text.split(",", 3).',
    );
  }
  const problem = splitProblem(
    separatorNode ? '' : undefined,
    limitNode ? 0 : undefined,
    rest.length > 0,
  );
  if (problem) {
    throw aotError(...problem);
  }

  const text = keptString(split.text, environment, 'the string it splits');
  if (!separatorNode) {
    return {kind: 'splitItems', list, text, separator: null, limit: null};
  }
  const separator = keptString(separatorNode, environment, 'the separator');
  let limit: IrExpr | null = null;
  if (limitNode) {
    limit = lowerExpr(limitNode, environment);
    if (limit.type.kind !== 'int') {
      throw aotError(
        "AOT: split's limit must be a whole number",
        'pass a whole number, or leave it out to keep every piece.',
      );
    }
  }
  return {kind: 'splitItems', list, text, separator, limit};
}

/**
 * What a split reads: its string, its separator and its limit.
 *
 * @param statement  The split.
 *
 * @returns The values.
 */
const splitValuesOf = (statement: IrSplitItems): IrExpr[] => [
  statement.text,
  ...(statement.separator ? [statement.separator] : []),
  ...(statement.limit ? [statement.limit] : []),
];

/**
 * Lowers a split as a list state's next value, `setWords(text.split(','))`: its pieces replace the rows.
 *
 * @param listState  The list.
 * @param nextList  The setter's argument.
 * @param environment  The expression environment.
 * @param listName  What the setter's argument calls the list.
 *
 * @returns The statements, or null when the argument is no split.
 */
export function lowerSplitInto(
  listState: ListState,
  nextList: t.Node,
  environment: Env,
  listName: string,
): IrStmt[] | null {
  const split = splitCallOf(nextList);
  if (!split) return null;
  const {struct} = listState;
  if (!struct.plain || struct.fields[0].kind !== 'string') {
    throw aotError(
      `AOT: only a list of strings takes a split, and "${listName}" holds ${struct.plain ? `${struct.fields[0].kind}s` : 'objects'}`,
      'keep the pieces in a list of strings: const [words, setWords] = useState([""]).',
    );
  }
  const statement = splitStatement(listState, split, environment);
  const {separator} = statement;
  if (separator && expressionReads(separator)?.has(listState)) {
    throw aotError(
      `AOT: a split into "${listName}" can't split at one of its own items`,
      `the pieces replace its rows as they are found. Copy the separator into a const first.`,
    );
  }
  return [statement];
}

/**
 * The list a split makes in render: the pieces of its string, rebuilt by the update when what it reads changes.
 * A split of a constant folds to an array, so it is no list here.
 *
 * @param node  The expression.
 * @param environment  The expression environment.
 *
 * @returns The list, or null when the expression is no such split.
 */
export function splitListOf(node: t.Node, environment: Env): ListState | null {
  const split = splitCallOf(node);
  if (!split) return null;
  try {
    evalStatic(
      node,
      foldScope(environment, environment.constants ?? {}),
      'boot',
    );
    return null;
  } catch (error) {
    rethrowFoldLimit(error);
  }
  if (environment.outsideRender) {
    throw aotError(
      'AOT: a split in a handler, effect or timer must be a const',
      'split into a const first: const parts = text.split(","); then read parts[0] or parts.length.',
    );
  }
  if (environment.inListRow) {
    throw aotError(
      "AOT: split inside a list row isn't supported",
      'split once, outside the rows: const parts = text.split(",").',
    );
  }

  const lists = environment.derivedLists;
  if (!lists) {
    throw new Error('AOT internal: no table for lists worked out in render');
  }
  // Each instance of a component reads its own props, so it gets a list of its own.
  const key = environment.callbackPrefix ?? '';
  const known = lists.get(node)?.get(key);
  if (known) return known;

  const count = [...lists.values()].reduce((sum, map) => sum + map.size, 0);
  const list: ListState = {
    kind: 'list',
    name: 'split',
    slotName: `o${count}_split`,
    setter: undefined,
    struct: pieces(),
    items: [],
    capacity: environment.caps.listCap,
    initialAt: node.loc?.start,
  };
  const statement = splitStatement(list, split, environment);
  list.derived = {
    steps: [],
    rebuild: [statement],
    tests: splitValuesOf(statement),
    order: nextRebuild(),
  };

  if (!lists.has(node)) lists.set(node, new Map());
  lists.get(node)!.set(key, list);
  return list;
}

/**
 * A handler's `const parts = text.split(',')`: a list of its own, filled where the const is.
 *
 * @param name  The const's name.
 * @param init  What it is set to.
 * @param kind  `const`, `let` or `var`.
 * @param environment  The expression environment.
 *
 * @returns The list and the statements that fill it, or null when the const is no split.
 */
export function handlerSplitList(
  name: string,
  init: t.Expression,
  kind: t.VariableDeclaration['kind'],
  environment: Env,
): {list: ListState; statements: IrStmt[]} | null {
  const split = splitCallOf(init);
  if (!split) return null;
  if (kind !== 'const') {
    throw aotError(
      'AOT: a split in a handler, effect or timer must be a const',
      `its list can't be set again, so declare it with const: const ${name} = text.split(",").`,
    );
  }
  const lists = environment.splitLists;
  if (!lists) {
    throw new Error('AOT internal: no table for split lists');
  }
  const key = environment.callbackPrefix ?? '';
  let list = lists.get(init)?.get(key);
  if (!list) {
    const count = [...lists.values()].reduce((sum, map) => sum + map.size, 0);
    list = {
      kind: 'list',
      name,
      slotName: `h${count}_${name}`,
      setter: undefined,
      struct: pieces(),
      items: [],
      capacity: environment.caps.listCap,
      initialAt: init.loc?.start,
      handlerSplit: true,
    };
    if (!lists.has(init)) lists.set(init, new Map());
    lists.get(init)!.set(key, list);
  }
  return {list, statements: [splitStatement(list, split, environment)]};
}
