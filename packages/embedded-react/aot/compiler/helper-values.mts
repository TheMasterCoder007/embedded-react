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
import {lowerSource, lowerSourcedWide} from './expressions.mts';
import {objectTreeOf} from './records.mts';
import {evalStatic, foldScope} from './static-eval.mts';
import {switchAsIf} from './switch-cases.mts';
import {narrowOn, narrowed} from './tagged-records.mts';
import type * as t from '@babel/types';
import type {IrSource} from './ir/source.mts';
import type {Env, FunctionNode} from './types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * The statements a branch runs.
 *
 * @param node  The branch.
 *
 * @returns The statements.
 */
export const statementsOf = (node: t.Statement): t.Statement[] =>
  node.type === 'BlockStatement' ? node.body : [node];

/**
 * The statements a helper's body runs. An expression body returns its value.
 *
 * @param helper  The helper.
 *
 * @returns The statements.
 */
const bodyOf = ({body}: FunctionNode): t.Statement[] =>
  body.type === 'BlockStatement'
    ? body.body
    : [{type: 'ReturnStatement', argument: body, loc: body.loc}];

/**
 * Whether statements always end in a `return`, so nothing after them runs. A switch does when each case and
 * its default do.
 *
 * @param statements  The statements.
 * @param inLoop  In a loop's body, where a `break` or `continue` leaves too.
 *
 * @returns Whether they do.
 */
export function exits(statements: t.Statement[], inLoop = false): boolean {
  const last = statements.at(-1);
  if (!last) return false;
  if (last.type === 'ReturnStatement') return true;
  if (
    inLoop &&
    (last.type === 'BreakStatement' || last.type === 'ContinueStatement') &&
    !last.label
  ) {
    return true;
  }
  if (last.type === 'BlockStatement') return exits(last.body, inLoop);
  if (last.type === 'SwitchStatement') return exits([switchAsIf(last)], inLoop);
  return (
    last.type === 'IfStatement' &&
    last.alternate !== null &&
    last.alternate !== undefined &&
    exits(statementsOf(last.consequent), inLoop) &&
    exits(statementsOf(last.alternate), inLoop)
  );
}

/**
 * What a value-giving body is, for messages: a helper used as a value, or a block-bodied memo.
 *
 * @param name  The helper's name.
 *
 * @returns The phrase.
 */
function valueGiver(name: string): string {
  const memo = /^useMemo\((.*)\)$/.exec(name);
  return memo
    ? `the useMemo for "${memo[1]}" works out a value`
    : `helper "${name}" is used as a value`;
}

/**
 * The helper a call names, if it is one: a plain function or a useCallback, and not a name a local or a state
 * hides.
 *
 * @param node  The call.
 * @param environment  The expression environment.
 *
 * @returns The helper's name and function, or null.
 */
export function helperOf(
  node: t.Node,
  environment: Env,
): {name: string; helper: FunctionNode} | null {
  if (node.type !== 'CallExpression' || node.callee.type !== 'Identifier') {
    return null;
  }

  const {name} = node.callee;
  if (environment.locals.has(name) || environment.state.has(name)) return null;
  const helper =
    environment.helpers?.get(name) ?? environment.callbacks?.get(name);
  return helper ? {name, helper} : null;
}

/**
 * The environment a helper's body is inlined in: each parameter bound to what the call passes. A constant is
 * bound as a constant, so it folds; an object as the object; anything else as a local holding the value. A helper
 * already being inlined further up is recursive, and refused.
 *
 * @param name  The helper's name.
 * @param helper  The helper.
 * @param callArguments  The call's arguments.
 * @param environment  The caller's environment.
 *
 * @returns The body's environment.
 */
export function helperEnvironment(
  name: string,
  helper: FunctionNode,
  callArguments: t.CallExpression['arguments'],
  environment: Env,
): Env {
  if (environment.inlining?.has(name)) {
    throw aotError(
      `AOT: helper "${name}" is recursive — can't be inlined`,
      'each call to a helper is compiled in place, so a helper that calls itself (directly ' +
        'or through another helper) never ends. Write the steps out, or precompute the value.',
    );
  }

  const locals = new Map(environment.locals);
  const state = new Map(environment.state);
  const constants = {...environment.constants};
  const scope = foldScope(environment, environment.constants ?? {});
  helper.params.forEach((parameter, index) => {
    if (parameter.type !== 'Identifier') {
      throw aotError(
        `AOT: helper "${name}" must take simple (identifier) params to be inlined`,
        'a helper must use plain positional params (e.g. `(a, b) => …`) — destructuring or ' +
          'default params in the signature are not supported.',
      );
    }

    const argument = callArguments[index];
    if (!argument || argument.type === 'SpreadElement') {
      throw aotError(
        `AOT: helper "${name}" must be passed each of its params`,
        `pass a value for "${parameter.name}".`,
      );
    }

    // The parameter hides any name it shares with the caller's.
    locals.delete(parameter.name);
    state.delete(parameter.name);
    delete constants[parameter.name];
    let folded: unknown;
    try {
      folded = evalStatic(argument, scope, 'strict');
    } catch {
      folded = undefined;
    }
    if (['number', 'string', 'boolean'].includes(typeof folded)) {
      constants[parameter.name] = folded;
      return;
    }

    const object = objectTreeOf(argument, environment);
    if (object) {
      constants[parameter.name] = object;
      return;
    }

    const value = lowerSourcedWide(argument, environment);
    locals.set(parameter.name, {
      slotKind: value.slotKind,
      isBoolean: value.isBoolean,
      isLiteral: value.isLiteral,
      wide: value.wide,
      source: value.source,
      expression: value.expression,
    });
  });

  return {
    ...environment,
    locals,
    state,
    constants,
    inlining: new Set([...(environment.inlining ?? []), name]),
  };
}

/**
 * Binds a helper's `const x = …` as a local holding the value.
 *
 * @param declaration  The declaration.
 * @param name  The helper's name.
 * @param environment  The environment before it.
 *
 * @returns The environment after it.
 */
function bindConst(
  declaration: t.VariableDeclaration,
  name: string,
  environment: Env,
): Env {
  const locals = new Map(environment.locals);
  for (const declarator of declaration.declarations) {
    if (
      declaration.kind !== 'const' ||
      declarator.id.type !== 'Identifier' ||
      !declarator.init
    ) {
      throw aotError(
        `AOT: ${valueGiver(name)}, so its locals must be \`const x = …\``,
        'declare each local once with const and a value.',
      );
    }
    const value = lowerSourcedWide(declarator.init, {...environment, locals});
    locals.set(declarator.id.name, {
      slotKind: value.slotKind,
      isBoolean: value.isBoolean,
      isLiteral: value.isLiteral,
      wide: value.wide,
      source: value.source,
      expression: value.expression,
    });
  }

  return {...environment, locals};
}

/**
 * The value statements give: each `if` a ternary over what each branch gives, and a branch that does not return
 * goes on to the statements after it.
 *
 * @param statements  The statements.
 * @param name  The helper's name.
 * @param bodyEnvironment  The environment they run in.
 * @param fallThrough  The value when they end without a `return`.
 *
 * @returns The value.
 */
function valueOf(
  statements: t.Statement[],
  name: string,
  bodyEnvironment: Env,
  fallThrough: () => IrSource,
): IrSource {
  let environment = bodyEnvironment;
  for (let index = 0; index < statements.length; index++) {
    let statement = statements[index];
    const rest = statements.slice(index + 1);
    if (statement.type === 'SwitchStatement') statement = switchAsIf(statement);
    switch (statement.type) {
      case 'VariableDeclaration':
        environment = bindConst(statement, name, environment);
        continue;
      case 'ReturnStatement':
        if (!statement.argument) break;
        return lowerSource(statement.argument, environment).source;
      case 'BlockStatement': {
        const after = environment;
        return valueOf(statement.body, name, environment, () =>
          valueOf(rest, name, after, fallThrough),
        );
      }
      case 'IfStatement': {
        // The statements after a branch that returns read what the other branch's test leaves.
        const {whenTrue, whenFalse} = narrowOn(statement.test, environment);
        const consequent = statementsOf(statement.consequent);
        const alternate = statement.alternate
          ? statementsOf(statement.alternate)
          : [];
        const after = exits(consequent)
          ? narrowed(environment, whenFalse)
          : exits(alternate)
            ? narrowed(environment, whenTrue)
            : environment;
        const afterwards = () => valueOf(rest, name, after, fallThrough);
        return {
          kind: 'conditional',
          test: lowerSource(statement.test, environment).source,
          consequent: valueOf(
            consequent,
            name,
            narrowed(environment, whenTrue),
            afterwards,
          ),
          alternate: valueOf(
            alternate,
            name,
            narrowed(environment, whenFalse),
            afterwards,
          ),
          location: statement.test.loc?.start,
        };
      }
    }

    const isLoop =
      statement.type === 'ForStatement' ||
      statement.type === 'ForOfStatement' ||
      statement.type === 'WhileStatement' ||
      statement.type === 'DoWhileStatement';
    const error = aotError(
      statement.type === 'ReturnStatement'
        ? `AOT: ${valueGiver(name)}, so each \`return\` must give one`
        : isLoop
          ? `AOT: ${valueGiver(name)}, so it can't loop yet`
          : `AOT: ${valueGiver(name)}, so it can only declare consts, branch and return`,
      name.startsWith('useMemo(')
        ? isLoop
          ? 'keep the value in state, and update it in the handler that changes what it is worked out from.'
          : 'a memo works out a value without side effects. Set state in a handler or an effect.'
        : isLoop
          ? 'loop in the handler instead, into a let: let total = 0; for (const item of items) total += item.n;'
          : 'a helper whose result is used works out a value without side effects. Call one that sets ' +
            'state as a statement of its own.',
    );
    if (statement.loc) error.aotLoc = statement.loc.start;
    throw error;
  }

  return fallThrough();
}

/**
 * A helper call used as a value: the helper's body worked out as one expression, with a ternary for each branch.
 *
 * @param node  The call.
 * @param environment  The expression environment.
 *
 * @returns The value, or null when the call is not to a helper.
 */
export function helperValueOf(node: t.Node, environment: Env): IrSource | null {
  const called = helperOf(node, environment);
  if (!called) return null;
  const {name, helper} = called;
  return valueOf(
    bodyOf(helper),
    name,
    helperEnvironment(
      name,
      helper,
      (node as t.CallExpression).arguments,
      environment,
    ),
    () => {
      throw aotError(
        `AOT: ${valueGiver(name)}, but can end without returning one`,
        `return a value at the end of the ${name.startsWith('useMemo(') ? 'memo' : 'helper'}, and from each branch ` +
          'that ends it.',
      );
    },
  );
}

/**
 * The expression a helper call returns when the helper returns it on its only path, after any consts, and the
 * environment it is read in. Text a helper builds lowers through this.
 *
 * @param node  The call.
 * @param environment  The expression environment.
 *
 * @returns The expression and its environment, or null when the call is not to such a helper.
 */
export function returnedExpression(
  node: t.Node,
  environment: Env,
): {node: t.Expression; environment: Env} | null {
  const called = helperOf(node, environment);
  if (!called) return null;
  const statements = bodyOf(called.helper);
  const last = statements.at(-1);
  if (
    last?.type !== 'ReturnStatement' ||
    !last.argument ||
    statements
      .slice(0, -1)
      .some(statement => statement.type !== 'VariableDeclaration')
  ) {
    return null;
  }

  let bodyEnvironment = helperEnvironment(
    called.name,
    called.helper,
    (node as t.CallExpression).arguments,
    environment,
  );
  for (const statement of statements.slice(0, -1)) {
    bodyEnvironment = bindConst(
      statement as t.VariableDeclaration,
      called.name,
      bodyEnvironment,
    );
  }

  return {node: last.argument, environment: bodyEnvironment};
}
