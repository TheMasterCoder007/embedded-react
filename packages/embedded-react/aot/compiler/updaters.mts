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
 * Updaters with a block body: `setN(v => { … return next; })`. The body runs in the handler as its statements do, and
 * each `return next` sets the state through an updater of its own, `setN(v => next)`, then leaves the body.
 */

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {aotError} from './diagnostics.mts';
import {exits} from './helper-values.mts';
import type * as t from '@babel/types';
import type {FunctionNode} from './types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/** Calls an updater can't make: they start or stop something. */
const SIDE_EFFECT_CALLS = new Set([
  'setInterval',
  'setTimeout',
  'clearInterval',
  'clearTimeout',
  'updateVector',
]);

/** The sets a `return` in an updater makes. */
const returnSets = new WeakSet<t.Node>();

/**
 * Whether a call is a set a `return` in an updater makes. It leaves any loop in the updater at once, and the setter
 * call the updater was given is checked against the loops outside it.
 *
 * @param node  The call.
 *
 * @returns Whether it is.
 */
export const isReturnSet = (node: t.Node): boolean => returnSets.has(node);

/**
 * An error located at a node.
 *
 * @param node  The node.
 * @param message  What is wrong.
 * @param hint  How to rewrite it.
 *
 * @returns The error.
 */
function locatedError(node: t.Node, message: string, hint: string): Error {
  const error = aotError(message, hint);
  if (node.loc) error.aotLoc = node.loc.start;
  return error;
}

/**
 * The names a pattern declares.
 *
 * @param pattern  The pattern.
 *
 * @returns The names.
 */
function patternNames(pattern: t.Node | null | undefined): string[] {
  if (!pattern) return [];
  switch (pattern.type) {
    case 'Identifier':
      return [pattern.name];
    case 'ObjectPattern':
      return pattern.properties.flatMap(property =>
        property.type === 'RestElement'
          ? patternNames(property.argument)
          : patternNames(property.value),
      );
    case 'ArrayPattern':
      return pattern.elements.flatMap(element => patternNames(element));
    case 'AssignmentPattern':
      return patternNames(pattern.left);
    case 'RestElement':
      return patternNames(pattern.argument);
    default:
      return [];
  }
}

/**
 * Each statement in a body, nested ones included, and not those of a function inside it.
 *
 * @param statements  The body.
 * @param visit  Called with each statement.
 */
function eachStatement(
  statements: t.Statement[],
  visit: (statement: t.Statement) => void,
): void {
  for (const statement of statements) {
    visit(statement);
    switch (statement.type) {
      case 'BlockStatement':
        eachStatement(statement.body, visit);
        break;
      case 'IfStatement':
        eachStatement([statement.consequent], visit);
        if (statement.alternate) eachStatement([statement.alternate], visit);
        break;
      case 'SwitchStatement':
        for (const switchCase of statement.cases) {
          eachStatement(switchCase.consequent, visit);
        }
        break;
      case 'ForStatement':
      case 'ForOfStatement':
      case 'ForInStatement':
      case 'WhileStatement':
      case 'DoWhileStatement':
      case 'LabeledStatement':
        eachStatement([statement.body], visit);
        break;
    }
  }
}

/**
 * The values a block body returns, from any branch. A function inside it returns its own.
 *
 * @param body  The body.
 *
 * @returns The returned values.
 */
export function returnedValues(body: t.BlockStatement): t.Expression[] {
  const values: t.Expression[] = [];
  eachStatement(body.body, statement => {
    if (statement.type === 'ReturnStatement' && statement.argument) {
      values.push(statement.argument);
    }
  });
  return values;
}

/**
 * Checks an updater's body only works out the new value: it writes only its own locals, and calls nothing as a
 * statement or to start a timer. Each way out returns the new value.
 *
 * @param setter  The setter's name.
 * @param updater  The updater.
 */
function checkUpdaterBody(
  setter: string,
  updater: FunctionNode & {body: t.BlockStatement},
): void {
  const what = `${setter}'s updater`;
  const statements = updater.body.body;
  const declared = new Set<string>();
  eachStatement(statements, statement => {
    if (statement.type === 'VariableDeclaration') {
      for (const declarator of statement.declarations) {
        patternNames(declarator.id).forEach(name => declared.add(name));
      }
    }
  });

  const pureHint =
    'an updater only works out the new value from the old one. Set other state, write refs and start timers ' +
    'in the handler, outside the updater.';
  eachStatement(statements, statement => {
    if (statement.type === 'ReturnStatement' && !statement.argument) {
      throw locatedError(
        statement,
        `AOT: each \`return\` in ${what} must give the new value`,
        'return the value to set, e.g. return next;',
      );
    }

    if (statement.type === 'VariableDeclaration') {
      for (const {init} of statement.declarations) {
        if (
          init?.type === 'CallExpression' &&
          init.callee.type === 'Identifier' &&
          SIDE_EFFECT_CALLS.has(init.callee.name)
        ) {
          throw locatedError(
            init,
            `AOT: ${what} can't call ${init.callee.name}`,
            pureHint,
          );
        }
      }
    }

    if (statement.type !== 'ExpressionStatement') return;
    const {expression} = statement;
    const written =
      expression.type === 'AssignmentExpression'
        ? expression.left
        : expression.type === 'UpdateExpression'
          ? expression.argument
          : null;
    if (written?.type === 'Identifier' && declared.has(written.name)) return;
    const called =
      expression.type === 'CallExpression' &&
      expression.callee.type === 'Identifier'
        ? expression.callee.name
        : null;
    throw locatedError(
      statement,
      called
        ? `AOT: ${what} can't call ${called}`
        : written
          ? `AOT: ${what} can only write its own locals`
          : `AOT: ${what} can only work out the new value`,
      pureHint,
    );
  });

  if (!exits(statements)) {
    throw locatedError(
      updater.body,
      `AOT: ${what} can end without returning the new value`,
      'return the value to set at the end of the updater, and from each branch that ends it.',
    );
  }
}

/**
 * A statement with each `return next` in it made `return setter(previous => next)`.
 *
 * @param statement  The statement.
 * @param setOf  The set call for a returned value.
 *
 * @returns The statement.
 */
function settingReturns(
  statement: t.Statement,
  setOf: (value: t.Expression) => t.CallExpression,
): t.Statement {
  const each = (inner: t.Statement): t.Statement =>
    settingReturns(inner, setOf);
  switch (statement.type) {
    case 'ReturnStatement':
      return statement.argument
        ? {...statement, argument: setOf(statement.argument)}
        : statement;
    case 'BlockStatement':
      return {...statement, body: statement.body.map(each)};
    case 'IfStatement':
      return {
        ...statement,
        consequent: each(statement.consequent),
        alternate: statement.alternate ? each(statement.alternate) : null,
      };
    case 'SwitchStatement':
      return {
        ...statement,
        cases: statement.cases.map(switchCase => ({
          ...switchCase,
          consequent: switchCase.consequent.map(each),
        })),
      };
    case 'ForStatement':
    case 'ForOfStatement':
    case 'ForInStatement':
    case 'WhileStatement':
    case 'DoWhileStatement':
    case 'LabeledStatement':
      return {...statement, body: each(statement.body)} as t.Statement;
    default:
      return statement;
  }
}

/**
 * The statements a block-bodied updater runs in the handler: its body, with each `return next` setting the state
 * through an updater of its own, so the setter lowers it as it does any updater. That updater reads the parameter as
 * the body has it bound (narrowed by a test on a tag), except a list's, which a list op needs by name.
 *
 * @param setter  The setter's name.
 * @param updater  The updater.
 * @param keepsParameter  Whether each set names the parameter again.
 *
 * @returns The statements.
 */
export function updaterStatements(
  setter: string,
  updater: FunctionNode & {body: t.BlockStatement},
  keepsParameter: boolean,
): t.Statement[] {
  checkUpdaterBody(setter, updater);
  const setOf = (value: t.Expression): t.CallExpression => {
    const call: t.CallExpression = {
      type: 'CallExpression',
      callee: {type: 'Identifier', name: setter},
      arguments: [
        {
          type: 'ArrowFunctionExpression',
          params: keepsParameter ? updater.params : [],
          body: value,
          expression: true,
          async: false,
          loc: value.loc,
        } as t.ArrowFunctionExpression,
      ],
      loc: value.loc,
    };
    returnSets.add(call);
    return call;
  };
  return updater.body.body.map(statement => settingReturns(statement, setOf));
}
