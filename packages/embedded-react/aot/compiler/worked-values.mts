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
 * Values worked out with a loop: a block-bodied `useMemo`, or a helper used as a value, whose body has `let` locals
 * or loops. Such a body can't be one expression, so it runs as statements, each `return` setting a local and leaving
 * the body. In a handler, effect or timer it runs just before the statement that reads it. In render it has a state
 * of its own, which the update works out again, before anything that reads it, when what it reads changes.
 */

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {aotError} from './diagnostics.mts';
import {buildsText, lowerExpr, lowerText} from './expressions.mts';
import {lowerStmts} from './handlers.mts';
import {helperEnvironment} from './helper-values.mts';
import {BOOL, FLOAT, INT, STRING} from './ir/types.mts';
import {Out} from './out.mts';
import {checkValueBody} from './updaters.mts';
import type * as t from '@babel/types';
import type {IrExpr} from './ir/expressions.mts';
import type {IrSource} from './ir/source.mts';
import type {IrStmt} from './ir/statements.mts';
import type {IrText} from './ir/text.mts';
import type {IrType} from './ir/types.mts';
import type {
  Env,
  FunctionNode,
  Local,
  ScalarState,
  StatementContext,
  StateTable,
} from './types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Types
 ---------------------------------------------------------------------------------------------------------------------*/

/** A body run as statements: them, and the local its value is left in. */
interface ValueStatements {
  statements: IrStmt[];
  result: Local;
  type: IrType;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Variables
 ---------------------------------------------------------------------------------------------------------------------*/

/** Numbers each value and each list worked out in render as it is made, so the update works out each in turn. */
let rebuilds = 0;

/** Numbers the locals a value is left in. */
let results = 0;

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * The next number in the order the update works out what render makes: a value worked out with a loop, or a list
 * filtered, sorted or split.
 *
 * @returns The number.
 */
export const nextRebuild = (): number => rebuilds++;

/**
 * Whether statements need to run as statements: they declare a `let` or loop, not counting a function inside them.
 *
 * @param statements  The statements.
 *
 * @returns Whether they do.
 */
function loopsIn(statements: (t.Statement | null | undefined)[]): boolean {
  return statements.some(statement => {
    switch (statement?.type) {
      case 'VariableDeclaration':
        return statement.kind !== 'const';
      case 'ForStatement':
      case 'ForOfStatement':
      case 'ForInStatement':
      case 'WhileStatement':
      case 'DoWhileStatement':
        return true;
      case 'BlockStatement':
        return loopsIn(statement.body);
      case 'IfStatement':
        return loopsIn([statement.consequent, statement.alternate]);
      case 'SwitchStatement':
        return statement.cases.some(({consequent}) => loopsIn(consequent));
      case 'LabeledStatement':
        return loopsIn([statement.body]);
      default:
        return false;
    }
  });
}

/**
 * Whether a helper's body loops or declares a `let`, so it runs as statements rather than as one expression.
 *
 * @param helper  The helper.
 *
 * @returns Whether it does.
 */
export const runsAsStatements = (helper: FunctionNode): boolean =>
  helper.body.type === 'BlockStatement' && loopsIn(helper.body.body);

/**
 * What a value-giving body is, for errors: `the useMemo for "total"` or `helper "sum"`.
 *
 * @param name  The helper's name.
 *
 * @returns The phrase, and the memo's name for a memo.
 */
function describe(name: string): {what: string; memo: string | null} {
  const memo = /^useMemo\((.*)\)$/.exec(name)?.[1] ?? null;
  return {
    what: memo === null ? `helper "${name}"` : `the useMemo for "${memo}"`,
    memo,
  };
}

/**
 * The type a local holding every value a body returns has: a string, a boolean, or a number, a float when any is.
 *
 * @param types  The returned values' types.
 * @param what  What returns them, for errors.
 *
 * @returns The type.
 */
function joinedType(types: IrType[], what: string): IrType {
  const kinds = new Set(types.map(type => type.kind));
  for (const kind of kinds) {
    if (!['bool', 'int', 'float', 'string'].includes(kind)) {
      throw aotError(
        `AOT: ${what} loops, so it must return a number, a boolean or a string`,
        'return the value itself, e.g. a field of an item rather than the item.',
      );
    }
  }
  const isNumber = (kind: string) => kind === 'int' || kind === 'float';
  const sorts = new Set(
    [...kinds].map(kind => (isNumber(kind) ? 'number' : kind)),
  );
  if (sorts.size > 1) {
    throw aotError(
      `AOT: ${what} loops, so each \`return\` must give the same kind of value`,
      `it returns a ${[...sorts].join(' on one path and a ')} on another. Return one kind on every path.`,
    );
  }
  if (kinds.has('string')) return STRING;
  if (kinds.has('bool')) return BOOL;
  return kinds.has('float') ? FLOAT : INT;
}

/**
 * A value of a type that a local starts at before a `return` sets it.
 *
 * @param type  The type.
 *
 * @returns The value.
 */
const zeroOf = (type: IrType): IrExpr =>
  type.kind === 'string'
    ? {kind: 'string', value: '', type}
    : type.kind === 'bool'
      ? {kind: 'boolean', value: false, type}
      : {kind: 'number', value: 0, type};

/**
 * Lowers a looping body to statements, each `return` leaving its value in a local of its own and the body.
 *
 * @param call  The call.
 * @param name  The helper's name.
 * @param helper  The helper.
 * @param environment  The caller's environment.
 * @param state  The component's state table.
 * @param output  Everything emitted so far.
 *
 * @returns The statements and the local.
 */
function lowerValueStatements(
  call: t.CallExpression,
  name: string,
  helper: FunctionNode & {body: t.BlockStatement},
  environment: Env,
  state: StateTable,
  output: Out,
): ValueStatements {
  const {what, memo} = describe(name);
  checkValueBody(helper.body, what, memo !== null);

  const resultName = `value@${name}#${results++}`;
  const returned: {value: IrExpr; text?: IrText}[] = [];
  const context: StatementContext = {
    stateChanged: false,
    output,
    helperList: helper.body.body,
    returnsValue: (argument, bodyEnvironment) => {
      if (buildsText(argument, bodyEnvironment)) {
        const text = lowerText(argument, bodyEnvironment);
        const value: IrExpr = {kind: 'string', value: '', type: STRING};
        returned.push({value, text});
        return [{kind: 'assignLocal', name: resultName, value, text}];
      }
      const value = lowerExpr(argument, bodyEnvironment);
      returned.push({value});
      return [{kind: 'assignLocal', name: resultName, value}];
    },
  };
  const body = lowerStmts(
    helper.body.body,
    helperEnvironment(name, helper, call.arguments, environment),
    state,
    context,
  );

  const type = joinedType(
    returned.map(({value, text}) => (text ? STRING : value.type)),
    what,
  );
  const result: Local = {
    name: resultName,
    slotKind:
      type.kind === 'string'
        ? 'string'
        : type.kind === 'float'
          ? 'float'
          : 'int',
    ...(type.kind === 'bool' ? {isBoolean: true} : {}),
  };
  return {
    statements: [
      {
        kind: 'declareLocal',
        name: resultName,
        initializer: zeroOf(type),
        isHoisted: false,
      },
      ...(context.usedReturn ? [{kind: 'block', body} as IrStmt] : body),
    ],
    result,
    type,
  };
}

/**
 * A call to a helper that loops, used as a value. In a handler, effect or timer, the body runs before the statement
 * that reads it, into a local. In render it is a state of its own, which the update works out again before anything
 * that reads it.
 *
 * @param call  The call.
 * @param name  The helper's name.
 * @param helper  The helper.
 * @param environment  The caller's environment.
 *
 * @returns The value.
 */
export function workedValueOf(
  call: t.CallExpression,
  name: string,
  helper: FunctionNode & {body: t.BlockStatement},
  environment: Env,
): IrSource {
  const location = call.loc?.start;
  const {what, memo} = describe(name);

  // In a handler, effect or timer: worked out just before the statement.
  const sink = environment.before;
  if (sink) {
    const {statements, result, type} = lowerValueStatements(
      call,
      name,
      helper,
      environment,
      sink.state,
      sink.output,
    );
    sink.statements.push(...statements);
    return {
      kind: 'fixed',
      expression: {kind: 'local', local: result, type},
      location,
    };
  }

  if (environment.outsideRender) {
    throw aotError(
      `AOT: ${what} loops, so it can't be used as a value here`,
      'call it from a statement of a handler, an effect or a timer, or from render.',
    );
  }
  if (environment.inListRow) {
    throw aotError(
      `AOT: ${what} loops, so it can't be used inside a list row`,
      'a value worked out with a loop is worked out once, outside the rows. Work it out for the whole list in a ' +
        'useMemo, or keep it in each item.',
    );
  }
  const values = environment.workedValues;
  if (!values) {
    throw new Error('AOT internal: no table for values worked out with a loop');
  }

  // In render: a state of its own, one per call and component instance. A call inside a helper being inlined may be
  // given other arguments each time, so it gets one each time.
  const prefix = environment.callbackPrefix ?? '';
  const shared = !environment.inlining?.size;
  const known = shared ? values.get(call)?.get(prefix) : undefined;
  if (known) return {kind: 'state', state: known, location};

  const {statements, result, type} = lowerValueStatements(
    call,
    name,
    helper,
    environment,
    {byName: new Map(environment.state), bySetter: new Map()},
    new Out(new Map(), null as unknown as t.Program),
  );
  const count = [...values.values()].reduce((sum, map) => sum + map.size, 0);
  const label = memo ?? name;
  const worked: ScalarState = {
    kind: 'scalar',
    name: label,
    slotName: `${prefix}w${count}_${label}`,
    setter: undefined,
    slotKind: result.slotKind,
    isBoolean: Boolean(result.isBoolean),
    initial: type.kind === 'string' ? '' : 0,
    host: false,
    initialAt: location,
  };
  const read: IrExpr = {kind: 'local', local: result, type};
  worked.worked = {
    rebuild: [
      ...statements,
      type.kind === 'string'
        ? {
            kind: 'setStateText',
            state: worked,
            text: {parts: [{kind: 'value', value: read}]},
          }
        : {kind: 'setState', state: worked, value: read},
    ],
    order: nextRebuild(),
  };

  if (!values.has(call)) values.set(call, new Map());
  values.get(call)!.set(shared ? prefix : `${prefix}#${count}`, worked);
  return {kind: 'state', state: worked, location};
}
