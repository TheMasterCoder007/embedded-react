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
import type * as t from '@babel/types';
import type {Env, Scope} from './types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * evalStatic at a boundary that REQUIRES a compile-time constant. On a fold failure it rethrows as a
 * LOCATED aotError with a clear message (+ optional hint), instead of letting evalStatic's bare
 * control-flow error ("cannot statically resolve identifier …") leak to the user without a location.
 *
 * @param node  The expression to fold.
 * @param scope  Compile-time constants in reach.
 * @param message  The error to report when it does not fold.
 * @param hint  How to rewrite it, if there is a known way.
 *
 * @returns The folded value.
 */
export function evalStaticOrThrow(
  node: t.Node,
  scope: Scope,
  message: string,
  hint?: string,
): unknown {
  try {
    return evalStatic(node, scope);
  } catch {
    const error = aotError(message, hint);
    if (node && node.loc) {
      error.aotLoc = node.loc.start;
    }
    throw error;
  }
}

/**
 * Folds an expression to the value JS would give it when every name it reads is a compile-time constant.
 * Styles and state initial values fold through here. A caller that can also handle a runtime value catches
 * the error thrown on anything dynamic (a state reference, say) and emits C instead.
 *
 * @param node  The expression.
 * @param scope  Compile-time constants in reach.
 *
 * @returns The value: a number, string, boolean, null, or an object or array of those.
 *
 * @throws An error when any part of the expression is not a compile-time constant.
 */
export function evalStatic(node: t.Node, scope: Scope): unknown {
  switch (node.type) {
    case 'NumericLiteral':
    case 'StringLiteral':
    case 'BooleanLiteral':
      return node.value;
    case 'NullLiteral':
      return null;
    case 'UnaryExpression': {
      const operand: any = evalStatic(node.argument, scope);
      if (node.operator === '-') return -operand;
      if (node.operator === '+') return +operand;
      if (node.operator === '!') return !operand;
      break;
    }
    case 'BinaryExpression': {
      const left: any = evalStatic(node.left, scope);
      const right: any = evalStatic(node.right, scope);
      switch (node.operator) {
        case '+':
          return left + right;
        case '-':
          return left - right;
        case '*':
          return left * right;
        case '/':
          return left / right;
        case '%':
          return left % right;
        case '<':
          return left < right;
        case '>':
          return left > right;
        case '<=':
          return left <= right;
        case '>=':
          return left >= right;
        case '==':
        case '===':
          return left === right;
        case '!=':
        case '!==':
          return left !== right;
      }
      break;
    }
    case 'LogicalExpression': {
      const left = evalStatic(node.left, scope);
      if (node.operator === '&&') {
        return left ? evalStatic(node.right, scope) : left;
      }

      if (node.operator === '||') {
        return left ? left : evalStatic(node.right, scope);
      }
      break;
    }
    case 'ConditionalExpression':
      return evalStatic(node.test, scope)
        ? evalStatic(node.consequent, scope)
        : evalStatic(node.alternate, scope);
    case 'Identifier':
      if (node.name in scope) return scope[node.name];
      throw new Error(
        `AOT: cannot statically resolve identifier "${node.name}"`,
      );
    case 'MemberExpression': {
      const object: any = evalStatic(node.object, scope);
      const key: any = node.computed
        ? evalStatic(node.property, scope)
        : (node.property as t.Identifier).name;
      if (object == null) {
        throw new Error(`AOT: member access on null/undefined ("${key}")`);
      }
      return object[key];
    }
    case 'ObjectExpression': {
      const folded: Record<string, unknown> = {};
      for (const prop of node.properties) {
        if (prop.type !== 'ObjectProperty') {
          throw new Error(
            'AOT: object spreads/methods not supported in static objects',
          );
        }
        const key: any = prop.computed
          ? evalStatic(prop.key, scope)
          : ((prop.key as t.Identifier).name ??
            (prop.key as t.StringLiteral).value);
        folded[key] = evalStatic(prop.value, scope);
      }

      return folded;
    }
    case 'ArrayExpression':
      return node.elements.map(element =>
        element ? evalStatic(element, scope) : null,
      );
    case 'CallExpression': {
      // `StyleSheet.create({…})` folds to its argument; any other call is dynamic.
      const callee = node.callee;
      if (
        callee.type === 'MemberExpression' &&
        (callee.object as t.Identifier).name === 'StyleSheet' &&
        (callee.property as t.Identifier).name === 'create'
      ) {
        return evalStatic(node.arguments[0], scope);
      }

      throw new Error(`AOT: cannot statically evaluate call expression`);
    }
  }

  throw new Error(
    `AOT: unsupported expression "${node.type}" in static context`,
  );
}

/**
 * `scope` minus every name `env` binds at RUNTIME — locals, state, refs, animated values, and the event /
 * gesture params of the handler being compiled. emitExpr resolves those before env.consts, so a fold
 * that consulted the raw scope would silently swap a module const in for the value the code actually
 * uses. Every env-driven constant fold goes through this; the JSX-side folds keep the same rule by
 * deleting a name from their scope copy when they bind it (see emitComponent / emitDynamicMap).
 *
 * @param env  The expression environment.
 * @param scope  The constants a fold would otherwise see.
 *
 * @returns `scope` itself when nothing shadows it, else a copy without the shadowed names.
 */
export function foldScope(env: Env, scope: Scope): Scope {
  const runtimeNames = new Set([
    ...(env.locals?.keys() ?? []),
    ...(env.state?.keys() ?? []),
    ...(env.refs?.keys() ?? []),
    ...(env.animations?.keys() ?? []),
  ]);
  if (env.event) {
    runtimeNames.add(env.event);
  }

  if (env.gesture) {
    runtimeNames.add(env.gesture);
  }

  // Copy the scope only when a runtime name actually shadows one of its constants.
  let hasShadowedName = false;
  for (const name of runtimeNames) {
    if (name in scope) {
      hasShadowedName = true;
      break;
    }
  }

  if (!hasShadowedName) return scope;
  return Object.fromEntries(
    Object.entries(scope).filter(([name]) => !runtimeNames.has(name)),
  );
}

/**
 * `scope` with the global `undefined` bound, so a fold can resolve the name.
 *
 * @param scope  Compile-time constants in reach.
 *
 * @returns A scope that reads through to `scope` and also binds `undefined`.
 */
export const withUndefined = (scope: Scope): Scope =>
  Object.assign(Object.create(scope), {undefined});

/**
 * evalStatic with a fallback default when the node is absent or not foldable.
 *
 * @param node  The expression, if there is one.
 * @param env  The expression environment; its constants are what the fold sees.
 * @param defaultValue  The value to use when the node is absent or does not fold.
 *
 * @returns The folded value, or `defaultValue`.
 */
export function evalStaticOr(
  node: t.Node | null | undefined,
  env: Env,
  defaultValue: unknown,
): unknown {
  if (!node) return defaultValue;
  try {
    return evalStatic(node, foldScope(env, env.consts ?? {}));
  } catch {
    return defaultValue;
  }
}
