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

import {
  BootReadError,
  BootValue,
  buildValueOf,
  containsBoot,
  foldBoot,
  isBoot,
  isContainer,
} from './boot.mts';
import {aotError} from './diagnostics.mts';
import type * as t from '@babel/types';
import type {Env, Scope} from './types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * How a value derived from the panel size folds: at the build's panel (`fold`), as a boot value (`boot`), or
 * not at all (`strict`). See evalStatic.
 */
export type BootMode = 'fold' | 'boot' | 'strict';

/** One evalStatic call: the scope it folds in, and that scope's copy, made once a boot value needs it. */
interface Fold {
  scope: Scope;
  snapshot: Scope | null;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** The Math functions a fold works out, as JS does. */
const STATIC_MATH: Record<string, (...args: any[]) => number> = {
  min: Math.min,
  max: Math.max,
  floor: Math.floor,
  ceil: Math.ceil,
  round: Math.round,
  trunc: Math.trunc,
  abs: Math.abs,
};

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
 * A value derived from the panel size (see BootValue) folds by `mode`: `fold` takes what it is on the build's
 * panel and records that as an assumption the panel at boot must meet; `boot` keeps it as a boot value, which
 * boot works out; `strict` throws a BootReadError, so the caller lowers it as a runtime value instead.
 *
 * @param node  The expression.
 * @param scope  Compile-time constants in reach.
 * @param mode  How a value derived from the panel size folds.
 *
 * @returns The value: a number, string, boolean, null, or an object or array of those.
 *
 * @throws An error when any part of the expression is not a compile-time constant.
 */
export function evalStatic(
  node: t.Node,
  scope: Scope,
  mode: BootMode = 'fold',
): unknown {
  const value = evaluate(node, scope, {scope, snapshot: null});
  if (mode === 'boot') return value;
  if (mode === 'fold') return foldBoot(value);
  if (containsBoot(value)) {
    throw new BootReadError();
  }

  return value;
}

/**
 * The names a boot value's expression reads, copied once per fold: the scope a const was folded in can lose
 * or rebind names afterward.
 *
 * @param fold  The fold.
 *
 * @returns The copy.
 */
function snapshotOf(fold: Fold): Scope {
  if (!fold.snapshot) {
    const copy: Scope = {};
    for (const name in fold.scope) {
      copy[name] = fold.scope[name];
    }
    fold.snapshot = copy;
  }

  return fold.snapshot;
}

/**
 * A boot value for an expression the fold could only take so far because it reads one.
 *
 * @param node  The expression.
 * @param value  What it is on the build's panel.
 * @param fold  The fold.
 * @param lowerable  Boot can work it out from the expression.
 * @param basis  What a fold of it assumes when boot cannot work it out.
 *
 * @returns The boot value.
 */
const bootOf = (
  node: t.Node,
  value: unknown,
  fold: Fold,
  lowerable = true,
  basis: BootValue[] = [],
): BootValue =>
  new BootValue(
    node as t.Expression,
    snapshotOf(fold),
    buildValueOf(value),
    lowerable,
    null,
    null,
    basis,
  );

/**
 * `test ? consequent : alternate` with a test that depends on the panel. Two objects with the same keys merge
 * into one whose differing entries each pick at boot, so a branch that changes only values stays one value;
 * anything else is one boot value.
 *
 * @param node  The expression.
 * @param test  The test, as a boot value.
 * @param fold  The fold.
 *
 * @returns The value.
 */
function bootConditional(
  node: t.ConditionalExpression,
  test: BootValue,
  fold: Fold,
): unknown {
  const taken = buildValueOf(test) ? node.consequent : node.alternate;
  const takenValue = evaluate(taken, fold.scope, fold);
  let otherValue: unknown;
  try {
    otherValue = evaluate(
      taken === node.consequent ? node.alternate : node.consequent,
      fold.scope,
      fold,
    );
  } catch {
    // The branch the build's panel does not take does not fold, so boot cannot pick between them.
    return bootOf(node, takenValue, fold, false, [test]);
  }

  const [consequent, alternate] =
    taken === node.consequent
      ? [takenValue, otherValue]
      : [otherValue, takenValue];
  return mergeBranches(node, node.consequent, node.alternate, [
    consequent,
    alternate,
    takenValue,
  ]);

  /**
   * Merges two branch values at `at`, entry by entry where both are objects. An entry only one branch has is
   * taken at the build's panel, which assumes the test.
   *
   * @param at  The expression the merged value stands for.
   * @param consequentNode  The consequent branch's expression.
   * @param alternateNode  The alternate branch's expression.
   * @param values  The consequent's value, the alternate's, and the one the build's panel takes.
   *
   * @returns The merged value.
   */
  function mergeBranches(
    at: t.Node,
    consequentNode: t.Expression,
    alternateNode: t.Expression,
    [whenTrue, whenFalse, built]: unknown[],
  ): unknown {
    if (whenTrue === whenFalse) return whenTrue;
    const mergeable =
      isContainer(whenTrue) &&
      isContainer(whenFalse) &&
      !Array.isArray(whenTrue) &&
      !Array.isArray(whenFalse);
    if (!mergeable) {
      return bootOf(at, built, fold, !isContainer(buildValueOf(built)), [test]);
    }

    // Each entry is `test ? (consequent).key : (alternate).key`, which boot works out on its own.
    const merged: Record<string, unknown> = {};
    const keys = new Set([...Object.keys(whenTrue), ...Object.keys(whenFalse)]);
    for (const key of keys) {
      if (!Object.hasOwn(whenTrue, key) || !Object.hasOwn(whenFalse, key)) {
        const builtEntry = (built as Record<string, unknown>)[key];
        merged[key] = bootOf(at, builtEntry, fold, false, [test]);
        continue;
      }

      const entryOf = (object: t.Expression): t.Expression =>
        ({
          type: 'MemberExpression',
          object,
          property: {type: 'StringLiteral', value: key},
          computed: true,
          loc: object.loc,
        }) as t.MemberExpression;
      const consequentEntry = entryOf(consequentNode);
      const alternateEntry = entryOf(alternateNode);
      merged[key] = mergeBranches(
        {
          type: 'ConditionalExpression',
          test: node.test,
          consequent: consequentEntry,
          alternate: alternateEntry,
          loc: at.loc,
        } as t.ConditionalExpression,
        consequentEntry,
        alternateEntry,
        [
          (whenTrue as Record<string, unknown>)[key],
          (whenFalse as Record<string, unknown>)[key],
          (built as Record<string, unknown>)[key],
        ],
      );
    }

    return merged;
  }
}

/**
 * evalStatic's fold, which keeps a value derived from the panel size as a boot value.
 *
 * @param node  The expression.
 * @param scope  Compile-time constants in reach.
 * @param fold  The fold this expression is part of.
 *
 * @returns The value.
 */
function evaluate(node: t.Node, scope: Scope, fold: Fold): unknown {
  switch (node.type) {
    case 'NumericLiteral':
    case 'StringLiteral':
    case 'BooleanLiteral':
      return node.value;
    case 'NullLiteral':
      return null;
    case 'UnaryExpression': {
      const operand: any = evaluate(node.argument, scope, fold);
      const apply = (value: any): unknown => {
        if (node.operator === '-') return -value;
        if (node.operator === '+') return +value;
        if (node.operator === '!') return !value;
        throw new Error(
          'AOT: unsupported expression "UnaryExpression" in static context',
        );
      };
      if (isBoot(operand)) {
        const result = bootOf(node, apply(buildValueOf(operand)), fold);
        // `!flag` holds exactly when `flag` does not, so a fold of it assumes the flag itself.
        if (
          node.operator === '!' &&
          typeof buildValueOf(operand) === 'boolean'
        ) {
          result.equivalent = operand.equivalent ?? operand;
        }

        return result;
      }

      return apply(operand);
    }
    case 'BinaryExpression': {
      const left: any = evaluate(node.left, scope, fold);
      const right: any = evaluate(node.right, scope, fold);
      const result = binary(
        node.operator,
        buildValueOf(left),
        buildValueOf(right),
      );
      return isBoot(left) || isBoot(right)
        ? bootOf(node, result, fold)
        : result;
    }
    case 'LogicalExpression': {
      const left = evaluate(node.left, scope, fold);
      if (node.operator !== '&&' && node.operator !== '||') break;
      if (!isBoot(left)) {
        const takesRight = node.operator === '&&' ? left : !left;
        return takesRight ? evaluate(node.right, scope, fold) : left;
      }

      // A test that depends on the panel: boot picks a side, so both have to fold.
      const takesRight =
        node.operator === '&&' ? buildValueOf(left) : !buildValueOf(left);
      try {
        const right = evaluate(node.right, scope, fold);
        return bootOf(node, takesRight ? right : left.value, fold);
      } catch {
        if (takesRight) throw new Error('AOT: the right side does not fold');
        return bootOf(node, left.value, fold, false, [left]);
      }
    }
    case 'ConditionalExpression': {
      const test = evaluate(node.test, scope, fold);
      if (isBoot(test)) return bootConditional(node, test, fold);
      return test
        ? evaluate(node.consequent, scope, fold)
        : evaluate(node.alternate, scope, fold);
    }
    case 'Identifier':
      if (node.name in scope) return scope[node.name];
      throw new Error(
        `AOT: cannot statically resolve identifier "${node.name}"`,
      );
    case 'MemberExpression': {
      const object: any = evaluate(node.object, scope, fold);
      const key: any = node.computed
        ? evaluate(node.property, scope, fold)
        : (node.property as t.Identifier).name;
      if (isBoot(key)) {
        throw new Error('AOT: a member key that depends on the panel size');
      }

      // A member of a boot value is one too, which only a fold can take.
      if (isBoot(object)) {
        const objectValue: any = buildValueOf(object);
        if (objectValue == null) {
          throw new Error(`AOT: member access on null/undefined ("${key}")`);
        }

        return bootOf(node, objectValue[key], fold, false, [object]);
      }

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
          ? evaluate(prop.key, scope, fold)
          : ((prop.key as t.Identifier).name ??
            (prop.key as t.StringLiteral).value);
        if (isBoot(key)) {
          throw new Error('AOT: an object key that depends on the panel size');
        }

        folded[key] = evaluate(prop.value, scope, fold);
      }

      return folded;
    }
    case 'ArrayExpression':
      return node.elements.map(element =>
        element ? evaluate(element, scope, fold) : null,
      );
    case 'CallExpression': {
      // `StyleSheet.create({…})` folds to its argument; any other call is dynamic.
      const callee = node.callee;
      if (
        callee.type === 'MemberExpression' &&
        (callee.object as t.Identifier).name === 'StyleSheet' &&
        (callee.property as t.Identifier).name === 'create'
      ) {
        return evaluate(node.arguments[0], scope, fold);
      }

      // Math.min / max / floor / ceil / round / trunc / abs fold as JS works them out; others are dynamic.
      const mathFn =
        callee.type === 'MemberExpression' &&
        !callee.computed &&
        callee.object.type === 'Identifier' &&
        callee.object.name === 'Math' &&
        !('Math' in scope)
          ? (callee.property as t.Identifier).name
          : null;
      if (mathFn && Object.hasOwn(STATIC_MATH, mathFn)) {
        const args = node.arguments.map(arg => {
          if (arg.type === 'SpreadElement') {
            throw new Error('AOT: cannot statically evaluate a spread');
          }

          return evaluate(arg, scope, fold);
        });
        const result = STATIC_MATH[mathFn](...args.map(buildValueOf));
        return args.some(isBoot) ? bootOf(node, result, fold) : result;
      }

      throw new Error(`AOT: cannot statically evaluate call expression`);
    }
  }

  throw new Error(
    `AOT: unsupported expression "${node.type}" in static context`,
  );
}

/**
 * A binary operator applied as JS applies it.
 *
 * @param operator  The operator.
 * @param left  The left operand.
 * @param right  The right operand.
 *
 * @returns The result.
 */
function binary(operator: string, left: any, right: any): unknown {
  switch (operator) {
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

  throw new Error(
    'AOT: unsupported expression "BinaryExpression" in static context',
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
