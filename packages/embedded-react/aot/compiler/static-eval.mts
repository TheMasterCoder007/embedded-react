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
import {ENGINE_STRING_MAX, aotError} from './diagnostics.mts';
import type {AotError} from './diagnostics.mts';
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

/** A fold refused because a string helper is past what a package can run. */
interface FoldLimitError extends AotError {
  isFoldLimit?: boolean;
}

/** One evalStatic call: the scope it folds in, and that scope's copy, made once a boot value needs it. */
interface Fold {
  scope: Scope;
  snapshot: Scope | null;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** The Math functions a fold works out, as JS does. */
const STATIC_MATH: Record<string, (...values: any[]) => number> = {
  min: Math.min,
  max: Math.max,
  floor: Math.floor,
  ceil: Math.ceil,
  round: Math.round,
  trunc: Math.trunc,
  abs: Math.abs,
};

/** What a fold sees for a global the code shadows with a runtime value: bound, so not the global, and not constant. */
const RUNTIME_BINDING = Symbol('runtime binding');

/** Globals a fold calls as JS does, unless the code shadows them. */
const FOLDED_GLOBALS = ['String'];

/** The most decimals toFixed prints in a package, as ERB_FIXED_DIGITS_MAX allows. */
export const MAX_FIXED_DIGITS = 20;

/** The hint for a helper's number that isn't a whole number. */
const WHOLE_NUMBER_HINT =
  'the package bakes it into the instruction, so write a literal or a module constant.';

/**
 * The string helpers a fold works out, as JS does, and the type each one is called on. A call past what the package
 * could do at run time is refused, so a constant and a runtime value accept the same code.
 */
const STATIC_METHODS: Record<
  string,
  {
    receiver: 'number' | 'string';
    problem: (...values: any[]) => [string, string] | null;
    apply: (...values: any[]) => unknown;
  }
> = {
  toFixed: {
    receiver: 'number',
    problem: (_value: number, digits: unknown = 0) =>
      fixedDigitsProblem(digits),
    apply: (value: number, digits?: number) => value.toFixed(digits),
  },
  padStart: {
    receiver: 'string',
    problem: (_value: string, width: unknown = 0, fill: unknown = ' ') =>
      padWidthProblem(width) ?? padFillProblem(fill),
    apply: (value: string, width: number, fill?: string) =>
      value.padStart(width, fill),
  },
  slice: {
    receiver: 'string',
    problem: (_value: string, ...positions: unknown[]) =>
      slicePositionProblem(positions),
    apply: (value: string, start?: number, end?: number) =>
      value.slice(start, end),
  },
  toUpperCase: {
    receiver: 'string',
    problem: () => null,
    apply: (value: string) => value.toUpperCase(),
  },
  includes: {
    receiver: 'string',
    problem: (_value: string, needle: unknown, ...rest: unknown[]) =>
      includesProblem(needle, rest.length > 0),
    apply: (value: string, needle: string) => value.includes(needle),
  },
};

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Why toFixed can't take these digits in a package, or null when it can.
 *
 * @param digits  The digits.
 *
 * @returns The error's message and hint.
 */
export function fixedDigitsProblem(digits: unknown): [string, string] | null {
  if (!Number.isInteger(digits)) {
    return [
      "AOT: toFixed's digits must be a compile-time whole number",
      WHOLE_NUMBER_HINT,
    ];
  }

  if ((digits as number) < 0 || (digits as number) > MAX_FIXED_DIGITS) {
    return [
      `AOT: toFixed(${digits}) is out of range`,
      `a package prints 0 to ${MAX_FIXED_DIGITS} decimals.`,
    ];
  }

  return null;
}

/**
 * Why padStart can't take this length in a package, or null when it can.
 *
 * @param width  The length.
 *
 * @returns The error's message and hint.
 */
export function padWidthProblem(width: unknown): [string, string] | null {
  if (!Number.isInteger(width)) {
    return [
      "AOT: padStart's length must be a compile-time whole number",
      WHOLE_NUMBER_HINT,
    ];
  }

  if ((width as number) > ENGINE_STRING_MAX.ER_TEXT_MAX) {
    return [
      `AOT: padStart(${width}) is longer than any text`,
      `a text holds at most ${ENGINE_STRING_MAX.ER_TEXT_MAX} bytes.`,
    ];
  }

  return null;
}

/**
 * Why padStart can't take this fill in a package, or null when it can: one printable ASCII character, or none.
 *
 * @param fill  The fill.
 *
 * @returns The error's message and hint.
 */
export function padFillProblem(fill: unknown): [string, string] | null {
  if (fill === '' || (typeof fill === 'string' && /^[ -~]$/.test(fill))) {
    return null;
  }

  return [
    "AOT: padStart's fill must be one printable ASCII character",
    'write a constant such as "0" or " "; a longer fill is not supported.',
  ];
}

/**
 * Why slice can't take these positions in a package, or null when it can: numbers, or left out.
 *
 * @param positions  The start and end, as given.
 *
 * @returns The error's message and hint.
 */
export function slicePositionProblem(
  positions: unknown[],
): [string, string] | null {
  if (positions.every(position => typeof position === 'number')) {
    return null;
  }

  return [
    "AOT: slice's start and end must be numbers",
    'pass whole numbers, or leave the end out to cut to the end.',
  ];
}

/**
 * Why includes can't take this search in a package, or null when it can: a string, and no position.
 *
 * @param needle  What it searches for.
 * @param hasPosition  Whether the call gives a position to search from.
 *
 * @returns The error's message and hint.
 */
export function includesProblem(
  needle: unknown,
  hasPosition: boolean,
): [string, string] | null {
  if (hasPosition) {
    return [
      "AOT: includes' position is not supported",
      'slice the string into a local first: `const rest = name.slice(2);` then `rest.includes(x)`.',
    ];
  }

  if (typeof needle !== 'string') {
    return [
      'AOT: includes only searches for a string',
      'JS converts anything else to a string first; convert it yourself: name.includes(String(n)).',
    ];
  }

  return null;
}

/**
 * Rethrows an error that refuses a fold because a string helper is past what a package can run, so a caller that
 * treats a fold that fails as a runtime value doesn't swallow it.
 *
 * @param error  What a fold threw.
 */
export function rethrowFoldLimit(error: unknown): void {
  if ((error as FoldLimitError | null)?.isFoldLimit) {
    throw error;
  }
}

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
  } catch (foldError) {
    rethrowFoldLimit(foldError);
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
 * the error thrown on anything dynamic (a state reference, say) and lowers it instead.
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
      setBinding(copy, name, fold.scope[name]);
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
      if (
        hasBinding(scope, node.name) &&
        scope[node.name] !== RUNTIME_BINDING
      ) {
        return scope[node.name];
      }
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
      const mathFunction =
        callee.type === 'MemberExpression' &&
        !callee.computed &&
        callee.object.type === 'Identifier' &&
        callee.object.name === 'Math' &&
        !hasBinding(scope, 'Math')
          ? (callee.property as t.Identifier).name
          : null;
      if (mathFunction && Object.hasOwn(STATIC_MATH, mathFunction)) {
        const argumentValues = node.arguments.map(argument => {
          if (argument.type === 'SpreadElement') {
            throw new Error('AOT: cannot statically evaluate a spread');
          }

          return evaluate(argument, scope, fold);
        });
        const result = STATIC_MATH[mathFunction](
          ...argumentValues.map(buildValueOf),
        );
        return argumentValues.some(isBoot)
          ? bootOf(node, result, fold)
          : result;
      }

      // String(x), and the string helpers on a constant.
      const isString =
        callee.type === 'Identifier' &&
        callee.name === 'String' &&
        !hasBinding(scope, 'String');
      const methodName =
        callee.type === 'MemberExpression' && !callee.computed
          ? (callee.property as t.Identifier).name
          : '';
      const method = Object.hasOwn(STATIC_METHODS, methodName)
        ? STATIC_METHODS[methodName]
        : undefined;
      if (isString || method) {
        const values = [
          ...(isString ? [] : [(callee as t.MemberExpression).object]),
          ...node.arguments,
        ].map(argument => {
          if (argument.type === 'SpreadElement') {
            throw new Error('AOT: cannot statically evaluate a spread');
          }

          return evaluate(argument, scope, fold);
        });
        const built = values.map(buildValueOf);
        if (method && typeof built[0] !== method.receiver) {
          throw new Error('AOT: a string helper on the wrong type');
        }

        const problem = method?.problem(...built);
        if (problem) {
          const error: FoldLimitError = aotError(...problem);
          error.isFoldLimit = true;
          if (node.loc) {
            error.aotLoc = node.loc.start;
          }
          throw error;
        }

        const result = isString
          ? built.length === 0
            ? ''
            : String(built[0])
          : method!.apply(...built);
        return values.some(isBoot) ? bootOf(node, result, fold) : result;
      }

      throw new Error(`AOT: cannot statically evaluate call expression`);
    }
    case 'TemplateLiteral': {
      const values = node.expressions.map(expression =>
        evaluate(expression, scope, fold),
      );
      const result = node.quasis
        .map(
          (quasi, index) =>
            quasi.value.cooked +
            (index < values.length ? String(buildValueOf(values[index])) : ''),
        )
        .join('');
      return values.some(isBoot) ? bootOf(node, result, fold) : result;
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
      return left == right;
    case '===':
      return left === right;
    case '!=':
      return left != right;
    case '!==':
      return left !== right;
  }

  throw new Error(
    'AOT: unsupported expression "BinaryExpression" in static context',
  );
}

/**
 * `scope` minus every name `environment` binds at RUNTIME — locals, state, refs, animated values, and the event /
 * gesture params of the handler being compiled. lowerExpr resolves those before environment.constants, so a fold
 * that consulted the raw scope would silently swap a module const in for the value the code actually
 * uses. Every env-driven constant fold goes through this; the JSX-side folds keep the same rule by
 * deleting a name from their scope copy when they bind it (see emitComponent / emitDynamicMap). A runtime name that
 * hides a global the fold calls, such as `String`, is kept as bound but not constant, so the global isn't called.
 *
 * @param environment  The expression environment.
 * @param scope  The constants a fold would otherwise see.
 *
 * @returns `scope` itself when nothing shadows it, else a copy without the shadowed names.
 */
export function foldScope(environment: Env, scope: Scope): Scope {
  const runtimeNames = new Set([
    ...(environment.locals?.keys() ?? []),
    ...(environment.state?.keys() ?? []),
    ...(environment.refs?.keys() ?? []),
    ...(environment.animations?.keys() ?? []),
  ]);
  if (environment.event) {
    runtimeNames.add(environment.event);
  }

  if (environment.gesture) {
    runtimeNames.add(environment.gesture);
  }

  // Copy the scope only when a runtime name actually shadows one of its constants.
  let hasShadowedName = false;
  for (const name of runtimeNames) {
    if (hasBinding(scope, name)) {
      hasShadowedName = true;
      break;
    }
  }

  const kept = hasShadowedName
    ? Object.fromEntries(
        Object.entries(scope).filter(([name]) => !runtimeNames.has(name)),
      )
    : scope;

  // A runtime value named like a global the fold calls hides that global.
  const shadowedGlobals = FOLDED_GLOBALS.filter(name => runtimeNames.has(name));
  if (shadowedGlobals.length === 0) return kept;
  return Object.assign(
    Object.create(kept),
    Object.fromEntries(shadowedGlobals.map(name => [name, RUNTIME_BINDING])),
  );
}

/**
 * Whether `scope` binds `name`, through the scopes it reads through to but not Object.prototype.
 *
 * @param scope  Compile-time constants in reach.
 * @param name  The identifier.
 *
 * @returns True when a scope in the chain has it as its own.
 */
export function hasBinding(scope: Scope, name: string): boolean {
  for (
    let layer: object | null = scope;
    layer && layer !== Object.prototype;
    layer = Object.getPrototypeOf(layer)
  ) {
    if (Object.hasOwn(layer, name)) return true;
  }

  return false;
}

/**
 * Binds `name` in `scope` as its own property, so `__proto__` is a name rather than Object.prototype's setter.
 *
 * @param scope  The scope to bind in.
 * @param name  The identifier.
 * @param value  Its value.
 */
export function setBinding(scope: Scope, name: string, value: unknown): void {
  Object.defineProperty(scope, name, {
    value,
    writable: true,
    enumerable: true,
    configurable: true,
  });
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
 * @param environment  The expression environment; its constants are what the fold sees.
 * @param defaultValue  The value to use when the node is absent or does not fold.
 *
 * @returns The folded value, or `defaultValue`.
 */
export function evalStaticOr(
  node: t.Node | null | undefined,
  environment: Env,
  defaultValue: unknown,
): unknown {
  if (!node) return defaultValue;
  try {
    return evalStatic(
      node,
      foldScope(environment, environment.constants ?? {}),
    );
  } catch {
    return defaultValue;
  }
}
