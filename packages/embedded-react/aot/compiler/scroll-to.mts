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

/**
 * @file compiler/scroll-to.mts
 * @brief Lowers `scrollTo(ref.current, {x, y})`: a scroll of the ScrollView the ref is on, and its position after.
 */

import {aotError} from './diagnostics.mts';
import {lowerExpr} from './expressions.mts';
import type * as t from '@babel/types';
import type {AotError} from './diagnostics.mts';
import type {IrExpr} from './ir/expressions.mts';
import type {IrScrollTo} from './ir/statements.mts';
import type {Env} from './types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** The fields of the position scrollTo returns, in the order the runtime gives them. */
export const SCROLL_POSITION_FIELDS = ['x', 'y', 'maxX', 'maxY'] as const;

const HINT =
  'call it as scrollTo(list.current, {y: 120}), where list is a useRef() on a <ScrollView>; leave an axis out ' +
  'to keep it, or the offset out to only read the position.';

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Whether an expression is a `scrollTo(…)` call.
 *
 * @param node  The expression.
 *
 * @returns True when it is.
 */
export function isScrollToCall(node: t.Node | null | undefined): boolean {
  return (
    node?.type === 'CallExpression' &&
    node.callee.type === 'Identifier' &&
    node.callee.name === 'scrollTo'
  );
}

/**
 * One axis of the offset, or null when the offset leaves it out.
 *
 * @param offset  The offset's object literal.
 * @param axis  The axis.
 * @param environment  The expression environment.
 *
 * @returns The axis's value.
 */
function axisOf(
  offset: t.ObjectExpression,
  axis: 'x' | 'y',
  environment: Env,
): IrExpr | null {
  // The last of a key written twice is the one JS keeps.
  const property = offset.properties.findLast(
    candidate =>
      candidate.type === 'ObjectProperty' &&
      candidate.key.type === 'Identifier' &&
      candidate.key.name === axis,
  ) as t.ObjectProperty | undefined;
  return property
    ? lowerExpr(property.value as t.Expression, environment)
    : null;
}

/**
 * Lowers a `scrollTo(ref.current, {x, y}?)` call.
 *
 * @param call  The call.
 * @param environment  The expression environment.
 * @param results  The locals its position is kept in, when the code reads it.
 *
 * @returns The statement.
 */
export function lowerScrollTo(
  call: t.CallExpression,
  environment: Env,
  results?: string[],
): IrScrollTo {
  const [handle, offset, ...rest] = call.arguments;
  const ref =
    handle?.type === 'MemberExpression' &&
    !handle.computed &&
    handle.object.type === 'Identifier' &&
    handle.property.type === 'Identifier' &&
    handle.property.name === 'current'
      ? environment.refs?.get(handle.object.name)
      : null;
  if (ref?.kind !== 'node') {
    throw aotError(
      'AOT: scrollTo() takes the `.current` of a node ref first',
      HINT,
    );
  }
  ref.used = true;

  const isOffset =
    offset?.type === 'ObjectExpression' &&
    offset.properties.every(
      property =>
        property.type === 'ObjectProperty' &&
        !property.computed &&
        property.key.type === 'Identifier' &&
        (property.key.name === 'x' || property.key.name === 'y'),
    );
  if (rest.length || (offset && !isOffset)) {
    throw aotError(
      'AOT: scrollTo() takes its offset as an object literal of x and y',
      HINT,
    );
  }
  return {
    kind: 'scrollTo',
    node: ref.slotName,
    x: offset ? axisOf(offset as t.ObjectExpression, 'x', environment) : null,
    y: offset ? axisOf(offset as t.ObjectExpression, 'y', environment) : null,
    results,
  };
}

/**
 * Refuses the position scrollTo returns where it can't be kept: anywhere but a const, or a field read in one.
 *
 * @param node  The expression.
 *
 * @returns The error, or null when the expression is no scrollTo call.
 */
export function unkeptScrollError(node: t.Node): AotError | null {
  return isScrollToCall(node)
    ? aotError(
        'AOT: scrollTo() returns a position, which only a const keeps',
        'keep it first, then read its fields: const position = scrollTo(list.current); or ' +
          'const {y, maxY} = scrollTo(list.current);',
      )
    : null;
}
