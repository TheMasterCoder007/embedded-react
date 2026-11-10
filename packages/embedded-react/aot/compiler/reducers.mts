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
 * useReducer: each `dispatch(action)` runs the reducer in place, as an updater with a block body. The action is bound
 * where it is dispatched: its constant fields fold, so the branches its type rules out are dropped before the body is
 * lowered, and a `throw` for an action no dispatch sends never runs.
 */

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {lowerSourcedWide} from './expressions.mts';
import {exits, statementsOf} from './helper-values.mts';
import {AstLeaf, isObjectTree, objectTreeOf} from './records.mts';
import {
  RuntimeLeaf,
  evalStatic,
  foldScope,
  rethrowFoldLimit,
  withoutNames,
} from './static-eval.mts';
import {switchAsIf} from './switch-cases.mts';
import {declaredIn} from './updaters.mts';
import type * as t from '@babel/types';
import type {ObjectTree} from './records.mts';
import type {Env, Local, Scope} from './types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * What an expression folds to, if it does.
 *
 * @param node  The expression.
 * @param scope  The constants a fold may read.
 *
 * @returns The value, or undefined when it does not fold.
 */
function folded(node: t.Node, scope: Scope): {value: unknown} | undefined {
  try {
    return {value: evalStatic(node, scope, 'strict')};
  } catch (error) {
    rethrowFoldLimit(error);
    return undefined;
  }
}

/**
 * The local a runtime value is held in, worked out where it is written.
 *
 * @param node  The value.
 * @param environment  The environment it is written in.
 *
 * @returns The local.
 */
function localOf(node: t.Node, environment: Env): Local {
  const value = lowerSourcedWide(node, environment);
  return {
    slotKind: value.slotKind,
    isBoolean: value.isBoolean,
    isLiteral: value.isLiteral,
    wide: value.wide,
    source: value.source,
    expression: value.expression,
  };
}

/**
 * An object's fields as the reducer reads them: a constant stays one, and anything else is worked out at the
 * dispatch, so names the reducer declares can't change what it reads.
 *
 * @param tree  The object.
 * @param environment  The dispatch's environment.
 * @param scope  The constants a fold may read there.
 *
 * @returns The object.
 */
function settledTree(
  tree: ObjectTree,
  environment: Env,
  scope: Scope,
): ObjectTree {
  return Object.fromEntries(
    Object.entries(tree).map(([key, value]) => {
      if (isObjectTree(value)) {
        return [key, settledTree(value, environment, scope)];
      }
      if (!(value instanceof AstLeaf)) return [key, value];
      const constant = value.locals.size
        ? undefined
        : folded(value.node, scope);
      if (constant && typeof constant.value !== 'object') {
        return [key, constant.value];
      }
      const fieldEnvironment = value.locals.size
        ? {
            ...environment,
            locals: new Map([...environment.locals, ...value.locals]),
          }
        : environment;
      return [key, new RuntimeLeaf(localOf(value.node, fieldEnvironment))];
    }),
  );
}

/**
 * An action that picks between two at run time: a ternary of actions, or an object with a field that is a ternary of
 * two constants, `{type: up ? 'inc' : 'dec'}`. Each side is dispatched on its own, so its constant fields fold.
 *
 * @param action  What is dispatched.
 * @param scope  The constants a fold may read.
 *
 * @returns The test and the action each way, or null when the action picks nothing.
 */
export function actionChoice(
  action: t.Expression,
  scope: Scope,
): {
  test: t.Expression;
  whenTrue: t.Expression;
  whenFalse: t.Expression;
} | null {
  if (action.type === 'ConditionalExpression' && !folded(action.test, scope)) {
    return {
      test: action.test,
      whenTrue: action.consequent,
      whenFalse: action.alternate,
    };
  }
  if (action.type !== 'ObjectExpression') return null;
  for (const [index, property] of action.properties.entries()) {
    if (
      property.type !== 'ObjectProperty' ||
      property.value.type !== 'ConditionalExpression' ||
      folded(property.value.test, scope)
    ) {
      continue;
    }
    const {test, consequent, alternate} = property.value;
    const isConstant = (node: t.Node) => {
      const value = folded(node, scope);
      return value !== undefined && typeof value.value !== 'object';
    };
    if (!isConstant(consequent) || !isConstant(alternate)) continue;
    const withValue = (value: t.Expression): t.ObjectExpression => ({
      ...action,
      properties: action.properties.map((other, at) =>
        at === index ? {...property, value} : other,
      ),
    });
    return {
      test,
      whenTrue: withValue(consequent),
      whenFalse: withValue(alternate),
    };
  }
  return null;
}

/**
 * The environment a reducer's body runs in for one dispatch: the action parameter bound to what is dispatched. A
 * constant folds, an object is its fields, and anything else is a local holding the value.
 *
 * @param parameter  The action parameter's name, if the reducer has one.
 * @param action  What the dispatch passes.
 * @param environment  The dispatch's environment.
 *
 * @returns The body's environment.
 */
export function actionEnvironment(
  parameter: string | undefined,
  action: t.Expression,
  environment: Env,
): Env {
  if (!parameter) return environment;
  const scope = foldScope(environment, environment.constants ?? {});
  const locals = new Map(environment.locals);
  const state = new Map(environment.state);
  const constants = {...environment.constants};
  locals.delete(parameter);
  state.delete(parameter);
  delete constants[parameter];

  const constant = folded(action, scope);
  const tree = constant ? null : objectTreeOf(action, environment);
  if (constant) {
    constants[parameter] = constant.value;
  } else if (tree) {
    constants[parameter] = settledTree(tree, environment, scope);
  } else {
    locals.set(parameter, localOf(action, environment));
  }
  return {...environment, locals, state, constants};
}

/**
 * A value with each ternary whose test folds made the side it picks.
 *
 * @param node  The value.
 * @param scope  The constants a fold may read.
 *
 * @returns The value.
 */
function prunedValue(node: t.Expression, scope: Scope): t.Expression {
  if (node.type !== 'ConditionalExpression') return node;
  const test = folded(node.test, scope);
  if (test) {
    return prunedValue(test.value ? node.consequent : node.alternate, scope);
  }
  return {
    ...node,
    consequent: prunedValue(node.consequent, scope),
    alternate: prunedValue(node.alternate, scope),
  };
}

/**
 * Statements with each branch whose test folds made the one it takes, in loops too. A branch taken that always ends
 * drops the statements after it, which never run. Tests on the action's constant fields fold, so a `switch
 * (action.type)` keeps only the case a dispatch takes. A name a block or a loop declares hides what it names outside,
 * so no test folds through it.
 *
 * @param statements  The statements.
 * @param scope  The constants a fold may read.
 *
 * @returns The statements.
 */
export function prunedStatements(
  statements: t.Statement[],
  scope: Scope,
): t.Statement[] {
  const pruned: t.Statement[] = [];
  scope = withoutNames(scope, declaredIn(statements));
  for (const original of statements) {
    const statement =
      original.type === 'SwitchStatement' ? switchAsIf(original) : original;
    switch (statement.type) {
      case 'IfStatement': {
        const test = folded(statement.test, scope);
        if (!test) {
          pruned.push({
            ...statement,
            consequent: {
              type: 'BlockStatement',
              body: prunedStatements(statementsOf(statement.consequent), scope),
              directives: [],
            },
            alternate: statement.alternate
              ? {
                  type: 'BlockStatement',
                  body: prunedStatements(
                    statementsOf(statement.alternate),
                    scope,
                  ),
                  directives: [],
                }
              : null,
          });
          continue;
        }

        // The branch taken runs in place. One that always ends is the last thing that runs.
        const branch = test.value ? statement.consequent : statement.alternate;
        const taken = branch
          ? prunedStatements(statementsOf(branch), scope)
          : [];
        if (exits(taken)) {
          pruned.push(...taken);
          return pruned;
        }
        if (taken.length) {
          pruned.push({type: 'BlockStatement', body: taken, directives: []});
        }
        continue;
      }
      case 'BlockStatement': {
        const body = prunedStatements(statement.body, scope);
        pruned.push({...statement, body});
        if (exits(body)) return pruned;
        continue;
      }
      case 'ForStatement':
      case 'ForOfStatement': {
        const head =
          statement.type === 'ForStatement' ? statement.init : statement.left;
        pruned.push({
          ...statement,
          body: {
            type: 'BlockStatement',
            body: prunedStatements(
              statementsOf(statement.body),
              withoutNames(scope, declaredIn([head])),
            ),
            directives: [],
          },
        });
        continue;
      }
      case 'ReturnStatement':
        pruned.push(
          statement.argument
            ? {...statement, argument: prunedValue(statement.argument, scope)}
            : statement,
        );
        return pruned;
      default:
        pruned.push(statement);
    }
  }
  return pruned;
}
